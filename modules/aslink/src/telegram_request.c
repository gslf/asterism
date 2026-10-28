/*
 * telegram_request.c — validation and JSON bodies of Bot API requests.
 *
 * Pure functions: no network, so the exact wire shape is unit-tested.
 * Limits are the documented ones (sendMessage text 1-4096 characters,
 * caption 0-1024, callback_data 1-64 bytes, topic name 1-128, callback
 * answer 0-200, commands 1-32 of [a-z0-9_] with 1-256 character
 * descriptions, at most 100 commands); characters are counted as UTF-16
 * code units (see text.c). A text with a parse_mode is only capped in bytes
 * locally: its length after entity parsing is the server's to judge.
 */

#include "aslink_internal.h"

#include <stdlib.h>
#include <string.h>

typedef asmodel_json_value jval;

/* Report a rejected argument and evaluate to the error. */
#define fail(st, e, ...) (aslink_status_msg((st), NULL, __VA_ARGS__), (e))

aslink_err aslink_check_text(const char *field, const char *text, size_t max_units,
                             int allow_empty, aslink_status *st) {
  size_t len, units = 0;
  if (!text) {
    return allow_empty ? ASLINK_OK
                       : fail(st, ASLINK_ERR_INVALID, "%s is required", field);
  }
  len = strlen(text);
  if (len > ASLINK_RAW_TEXT_MAX)
    return fail(st, ASLINK_ERR_TOO_LARGE, "%s exceeds 65536 bytes", field);
  if (!aslink_utf16_units(text, len, &units))
    return fail(st, ASLINK_ERR_INVALID, "%s is not valid UTF-8", field);
  if (!len && !allow_empty)
    return fail(st, ASLINK_ERR_INVALID, "%s is empty", field);
  if (max_units && units > max_units) {
    aslink_status_msg(st, NULL, "%s has %lu characters, the limit is %lu", field,
                      (unsigned long)units, (unsigned long)max_units);
    return ASLINK_ERR_TOO_LARGE;
  }
  return ASLINK_OK;
}

aslink_err aslink_check_ids(int64_t chat_id, int64_t thread_id, aslink_status *st) {
  if (chat_id == 0) return fail(st, ASLINK_ERR_INVALID, "%s must be non-zero", "chat_id");
  if (thread_id < 0)
    return fail(st, ASLINK_ERR_INVALID, "%s must not be negative", "thread_id");
  return ASLINK_OK;
}

static aslink_err check_parse_mode(const char *mode, aslink_status *st) {
  if (!mode || strcmp(mode, "HTML") == 0 || strcmp(mode, "MarkdownV2") == 0)
    return ASLINK_OK;
  return fail(st, ASLINK_ERR_INVALID, "%s must be NULL, HTML or MarkdownV2",
              "parse_mode");
}

/* Printable ASCII without spaces, 1..max bytes. */
static int opaque_id(const char *s, size_t max) {
  size_t i;
  if (!s || !*s) return 0;
  for (i = 0; s[i]; i++)
    if ((unsigned char)s[i] <= 0x20 || (unsigned char)s[i] >= 0x7F || i >= max) return 0;
  return 1;
}

/* Collects construction failures: every set takes ownership of its value. */
typedef struct {
  jval *obj;
  int bad;
} builder;

static void put(builder *b, const char *key, jval *v) {
  if (asmodel_json_object_set(b->obj, key, v) != 0) b->bad = 1;
}
static void put_int(builder *b, const char *key, int64_t x) {
  put(b, key, asmodel_json_int((long long)x));
}
static void put_str(builder *b, const char *key, const char *s) {
  put(b, key, asmodel_json_string(s));
}
static void put_true(builder *b, const char *key) {
  put(b, key, asmodel_json_bool(1));
}

static aslink_err finish(builder *b, char **out) {
  *out = NULL;
  if (!b->obj || b->bad) {
    asmodel_json_free(b->obj);
    return ASLINK_ERR_NOMEM;
  }
  *out = asmodel_json_write(b->obj, 0);
  asmodel_json_free(b->obj);
  return *out ? ASLINK_OK : ASLINK_ERR_NOMEM;
}

static aslink_err start(builder *b, char **out) {
  if (out) *out = NULL;
  b->obj = asmodel_json_object();
  b->bad = 0;
  return b->obj ? ASLINK_OK : ASLINK_ERR_NOMEM;
}

static int url_allowed(const char *u) {
  return core_str_has_prefix(u, "https://") || core_str_has_prefix(u, "http://") ||
         core_str_has_prefix(u, "tg://");
}

static aslink_err check_keyboard(const aslink_keyboard *k, aslink_status *st) {
  size_t i;
  aslink_err e;
  if (!k->buttons || k->n == 0 || k->n > ASLINK_BUTTONS_MAX)
    return fail(st, ASLINK_ERR_INVALID, "%s must hold 1-100 buttons", "keyboard");
  for (i = 0; i < k->n; i++) {
    const aslink_button *bt = &k->buttons[i];
    if ((e = aslink_check_text("button text", bt->text, 0, 0, st)) != ASLINK_OK) return e;
    if (!bt->callback_data == !bt->url)
      return fail(st, ASLINK_ERR_INVALID, "%s needs exactly one of callback_data or url",
                  "button");
    if (bt->callback_data) {
      size_t n = strlen(bt->callback_data);
      if (n == 0 || n > ASLINK_CALLBACK_DATA_MAX ||
          !core_utf8_valid(bt->callback_data, n))
        return fail(st, ASLINK_ERR_INVALID, "%s must be 1-64 bytes of UTF-8",
                    "callback_data");
    } else if (!url_allowed(bt->url) || strlen(bt->url) > 2048 ||
               !core_utf8_valid(bt->url, strlen(bt->url))) {
      return fail(st, ASLINK_ERR_INVALID, "%s must be an http, https or tg URL",
                  "button url");
    }
  }
  return ASLINK_OK;
}

/* {"inline_keyboard":[[{"text":..,"callback_data"|"url":..},..],..]};
 * NULL on allocation failure. The keyboard was checked. */
static jval *keyboard_json(const aslink_keyboard *k) {
  jval *markup = asmodel_json_object(), *rows = asmodel_json_array(), *row = NULL;
  size_t i, per = k->per_row ? k->per_row : k->n;
  int bad = !markup || !rows;
  for (i = 0; i < k->n && !bad; i++) {
    builder btn;
    if (!row && !(row = asmodel_json_array())) {
      bad = 1;
      break;
    }
    btn.obj = asmodel_json_object();
    btn.bad = !btn.obj;
    put_str(&btn, "text", k->buttons[i].text);
    if (k->buttons[i].callback_data)
      put_str(&btn, "callback_data", k->buttons[i].callback_data);
    else
      put_str(&btn, "url", k->buttons[i].url);
    if (btn.bad) {
      asmodel_json_free(btn.obj);
      bad = 1;
      break;
    }
    if (asmodel_json_array_push(row, btn.obj) != 0) { /* frees btn.obj */
      bad = 1;
      break;
    }
    if ((i + 1) % per == 0 || i + 1 == k->n) {
      /* push owns row from here on, also when it fails */
      if (asmodel_json_array_push(rows, row) != 0) bad = 1;
      row = NULL;
    }
  }
  asmodel_json_free(row);
  if (bad) {
    asmodel_json_free(rows);
    asmodel_json_free(markup);
    return NULL;
  }
  if (asmodel_json_object_set(markup, "inline_keyboard", rows) != 0) {
    asmodel_json_free(markup);
    return NULL;
  }
  return markup;
}

static aslink_err put_keyboard(builder *b, const aslink_keyboard *k) {
  jval *m = keyboard_json(k);
  if (!m) return ASLINK_ERR_NOMEM;
  put(b, "reply_markup", m);
  return ASLINK_OK;
}

static void put_no_preview(builder *b) {
  jval *lp = asmodel_json_object();
  if (!lp || asmodel_json_object_set(lp, "is_disabled", asmodel_json_bool(1)) != 0) {
    asmodel_json_free(lp);
    b->bad = 1;
    return;
  }
  put(b, "link_preview_options", lp);
}

static aslink_err check_message_text(const char *text, const char *parse_mode,
                                     aslink_status *st) {
  aslink_err e = check_parse_mode(parse_mode, st);
  if (e != ASLINK_OK) return e;
  return aslink_check_text("text", text, parse_mode ? 0 : ASLINK_MESSAGE_MAX, 0, st);
}

/* ── methods ──────────────────────────────────────────────────────────── */

aslink_err aslink_req_get_updates(int64_t offset, int timeout_s, char **out,
                                  aslink_status *st) {
  static const char *const kinds[] = {"message", "callback_query",
                                      "stopped_message_generation"};
  builder b;
  jval *allowed;
  size_t i;
  if (timeout_s < 0 || timeout_s > ASLINK_POLL_TIMEOUT_MAX)
    return fail(st, ASLINK_ERR_INVALID, "%s must be in [0, 600]", "timeout_s");
  if (start(&b, out) != ASLINK_OK) return ASLINK_ERR_NOMEM;
  if (offset != 0) put_int(&b, "offset", offset);
  put_int(&b, "timeout", timeout_s);
  allowed = asmodel_json_array();
  for (i = 0; allowed && i < sizeof kinds / sizeof kinds[0]; i++)
    if (asmodel_json_array_push(allowed, asmodel_json_string(kinds[i])) != 0) b.bad = 1;
  put(&b, "allowed_updates", allowed);
  return finish(&b, out);
}

aslink_err aslink_req_send_message(int64_t chat_id, int64_t thread_id, const char *text,
                                   const aslink_send_opts *o, char **out,
                                   aslink_status *st) {
  builder b;
  aslink_err e;
  if ((e = aslink_check_ids(chat_id, thread_id, st)) != ASLINK_OK ||
      (e = check_message_text(text, o ? o->parse_mode : NULL, st)) != ASLINK_OK)
    return e;
  if (o && o->reply_to < 0)
    return fail(st, ASLINK_ERR_INVALID, "%s must not be negative", "reply_to");
  if (o && o->keyboard && (e = check_keyboard(o->keyboard, st)) != ASLINK_OK) return e;
  if (start(&b, out) != ASLINK_OK) return ASLINK_ERR_NOMEM;
  put_int(&b, "chat_id", chat_id);
  if (thread_id) put_int(&b, "message_thread_id", thread_id);
  put_str(&b, "text", text);
  if (o && o->parse_mode) put_str(&b, "parse_mode", o->parse_mode);
  if (o && o->no_preview) put_no_preview(&b);
  if (o && o->silent) put_true(&b, "disable_notification");
  if (o && o->reply_to) {
    builder r;
    r.obj = asmodel_json_object();
    r.bad = !r.obj;
    put_int(&r, "message_id", o->reply_to);
    put_true(&r, "allow_sending_without_reply");
    if (r.bad) {
      asmodel_json_free(r.obj);
      b.bad = 1;
    } else {
      put(&b, "reply_parameters", r.obj);
    }
  }
  if (o && o->keyboard && put_keyboard(&b, o->keyboard) != ASLINK_OK) b.bad = 1;
  return finish(&b, out);
}

aslink_err aslink_req_edit_message(int64_t chat_id, int64_t message_id, const char *text,
                                   const aslink_send_opts *o, char **out,
                                   aslink_status *st) {
  builder b;
  aslink_err e;
  if ((e = aslink_check_ids(chat_id, 0, st)) != ASLINK_OK ||
      (e = check_message_text(text, o ? o->parse_mode : NULL, st)) != ASLINK_OK)
    return e;
  if (message_id <= 0)
    return fail(st, ASLINK_ERR_INVALID, "%s must be positive", "message_id");
  if (o && o->keyboard && (e = check_keyboard(o->keyboard, st)) != ASLINK_OK) return e;
  if (start(&b, out) != ASLINK_OK) return ASLINK_ERR_NOMEM;
  put_int(&b, "chat_id", chat_id);
  put_int(&b, "message_id", message_id);
  put_str(&b, "text", text);
  if (o && o->parse_mode) put_str(&b, "parse_mode", o->parse_mode);
  if (o && o->no_preview) put_no_preview(&b);
  if (o && o->keyboard && put_keyboard(&b, o->keyboard) != ASLINK_OK) b.bad = 1;
  return finish(&b, out);
}

aslink_err aslink_req_edit_keyboard(int64_t chat_id, int64_t message_id,
                                    const aslink_keyboard *k, char **out,
                                    aslink_status *st) {
  builder b;
  aslink_err e;
  if ((e = aslink_check_ids(chat_id, 0, st)) != ASLINK_OK) return e;
  if (message_id <= 0)
    return fail(st, ASLINK_ERR_INVALID, "%s must be positive", "message_id");
  if (k && (e = check_keyboard(k, st)) != ASLINK_OK) return e;
  if (start(&b, out) != ASLINK_OK) return ASLINK_ERR_NOMEM;
  put_int(&b, "chat_id", chat_id);
  put_int(&b, "message_id", message_id);
  /* Without reply_markup the inline keyboard is removed. */
  if (k && put_keyboard(&b, k) != ASLINK_OK) b.bad = 1;
  return finish(&b, out);
}

aslink_err aslink_req_send_draft(int64_t chat_id, int64_t thread_id, int64_t draft_id,
                                 const char *text, const aslink_draft_opts *o,
                                 char **out, aslink_status *st) {
  builder b;
  aslink_err e;
  const char *mode = o ? o->parse_mode : NULL;
  if (chat_id <= 0)
    return fail(st, ASLINK_ERR_INVALID, "%s must be a private chat", "chat_id");
  if (thread_id < 0)
    return fail(st, ASLINK_ERR_INVALID, "%s must not be negative", "thread_id");
  if (draft_id == 0)
    return fail(st, ASLINK_ERR_INVALID, "%s must be non-zero", "draft_id");
  if ((e = check_parse_mode(mode, st)) != ASLINK_OK ||
      (e = aslink_check_text("text", text, mode ? 0 : ASLINK_MESSAGE_MAX, 1, st)) !=
          ASLINK_OK)
    return e;
  if (start(&b, out) != ASLINK_OK) return ASLINK_ERR_NOMEM;
  put_int(&b, "chat_id", chat_id);
  if (thread_id) put_int(&b, "message_thread_id", thread_id);
  put_int(&b, "draft_id", draft_id);
  put_str(&b, "text", text ? text : "");
  if (mode) put_str(&b, "parse_mode", mode);
  if (o && o->can_stop) put_true(&b, "can_stop");
  if (o && o->keep_on_stop) put_true(&b, "keep_on_stop");
  return finish(&b, out);
}

aslink_err aslink_req_chat_action(int64_t chat_id, int64_t thread_id, const char *action,
                                  char **out, aslink_status *st) {
  static const char *const actions[] = {
    "typing", "upload_photo", "record_video", "upload_video", "record_voice",
    "upload_voice", "upload_document", "choose_sticker", "find_location",
    "record_video_note", "upload_video_note"};
  builder b;
  size_t i;
  aslink_err e;
  if ((e = aslink_check_ids(chat_id, thread_id, st)) != ASLINK_OK) return e;
  for (i = 0; action && i < sizeof actions / sizeof actions[0]; i++)
    if (strcmp(action, actions[i]) == 0) break;
  if (!action || i == sizeof actions / sizeof actions[0])
    return fail(st, ASLINK_ERR_INVALID, "%s is not a documented chat action", "action");
  if (start(&b, out) != ASLINK_OK) return ASLINK_ERR_NOMEM;
  put_int(&b, "chat_id", chat_id);
  if (thread_id) put_int(&b, "message_thread_id", thread_id);
  put_str(&b, "action", action);
  return finish(&b, out);
}

aslink_err aslink_req_answer_callback(const char *callback_id, const char *text,
                                      int alert, char **out, aslink_status *st) {
  builder b;
  aslink_err e;
  if (!opaque_id(callback_id, 127))
    return fail(st, ASLINK_ERR_INVALID, "%s is not a callback query id", "callback_id");
  if ((e = aslink_check_text("text", text, ASLINK_CALLBACK_TEXT_MAX, 1, st)) != ASLINK_OK)
    return e;
  if (start(&b, out) != ASLINK_OK) return ASLINK_ERR_NOMEM;
  put_str(&b, "callback_query_id", callback_id);
  if (text) put_str(&b, "text", text);
  if (alert) put_true(&b, "show_alert");
  return finish(&b, out);
}

aslink_err aslink_req_create_topic(int64_t chat_id, const char *name, char **out,
                                   aslink_status *st) {
  builder b;
  aslink_err e;
  if ((e = aslink_check_ids(chat_id, 0, st)) != ASLINK_OK ||
      (e = aslink_check_text("name", name, ASLINK_TOPIC_NAME_MAX, 0, st)) != ASLINK_OK)
    return e;
  if (start(&b, out) != ASLINK_OK) return ASLINK_ERR_NOMEM;
  put_int(&b, "chat_id", chat_id);
  put_str(&b, "name", name);
  return finish(&b, out);
}

static int command_valid(const char *c) {
  size_t i;
  if (!c || !*c) return 0;
  for (i = 0; c[i]; i++) {
    if (i >= 32) return 0;
    if (!((c[i] >= 'a' && c[i] <= 'z') || (c[i] >= '0' && c[i] <= '9') || c[i] == '_'))
      return 0;
  }
  return 1;
}

aslink_err aslink_req_set_commands(const aslink_command *cmds, size_t n, char **out,
                                   aslink_status *st) {
  builder b;
  jval *list;
  size_t i;
  aslink_err e;
  if (!cmds || n == 0 || n > ASLINK_COMMANDS_MAX)
    return fail(st, ASLINK_ERR_INVALID, "%s must hold 1-100 commands", "commands");
  for (i = 0; i < n; i++) {
    if (!command_valid(cmds[i].command))
      return fail(st, ASLINK_ERR_INVALID, "%s must be 1-32 of [a-z0-9_]", "command");
    if ((e = aslink_check_text("description", cmds[i].description,
                               ASLINK_COMMAND_DESC_MAX, 0, st)) != ASLINK_OK)
      return e;
  }
  if (start(&b, out) != ASLINK_OK) return ASLINK_ERR_NOMEM;
  list = asmodel_json_array();
  for (i = 0; list && i < n; i++) {
    builder c;
    c.obj = asmodel_json_object();
    c.bad = !c.obj;
    put_str(&c, "command", cmds[i].command);
    put_str(&c, "description", cmds[i].description);
    if (c.bad) {
      asmodel_json_free(c.obj);
      b.bad = 1;
      break;
    }
    if (asmodel_json_array_push(list, c.obj) != 0) b.bad = 1;
  }
  put(&b, "commands", list);
  return finish(&b, out);
}

aslink_err aslink_req_get_file(const char *file_id, char **out, aslink_status *st) {
  builder b;
  if (!opaque_id(file_id, 255))
    return fail(st, ASLINK_ERR_INVALID, "%s is not a file id", "file_id");
  if (start(&b, out) != ASLINK_OK) return ASLINK_ERR_NOMEM;
  put_str(&b, "file_id", file_id);
  return finish(&b, out);
}
