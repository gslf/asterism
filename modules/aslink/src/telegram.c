/*
 * telegram.c — Bot API methods over the transport.
 *
 * Each method validates and builds its body (telegram_request.c), POSTs it
 * to <base>/bot<token>/<method> on a fresh handle (transport_*.c) and checks
 * the reply (telegram_decode.c). URLs embedding the token are wiped before
 * they are freed; messages reaching the caller are redacted.
 *
 * Downloads stream into a temporary sibling of the destination (random
 * name, exclusive create, owner-only on POSIX) and are moved into place
 * with os_file_replace only once complete, synced and within the cap.
 */

#include "aslink_internal.h"

#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static void wipe_free(char *s) {
  if (s) {
    volatile char *p = s;
    while (*p) *p++ = 0;
  }
  free(s);
}

static char *bot_url(const aslink_ctx *c, const char *prefix, const char *tail) {
  core_buf b;
  core_buf_init(&b);
  if (core_buf_printf(&b, "%s%s%s/%s", c->base, prefix, c->token, tail) != CORE_OK) {
    if (b.data) wipe_free(core_buf_detach(&b));
    return NULL;
  }
  return core_buf_detach(&b);
}

static aslink_err check_ctx(aslink_ctx *c, aslink_status *st) {
  if (!c) return ASLINK_ERR_INVALID;
  if (ASLINK_FLAG_GET(&c->cancelled)) {
    aslink_status_msg(st, NULL, "cancelled");
    return ASLINK_ERR_CANCELLED;
  }
  return ASLINK_OK;
}

/* POST one method; on ASLINK_OK *root owns the reply and *result points
 * into it. body is JSON, or NULL with a multipart form. */
static aslink_err call(aslink_ctx *c, const char *method, const char *body,
                       const aslink_form_field *form, size_t form_n, long timeout_ms,
                       asmodel_json_value **root, const asmodel_json_value **result,
                       aslink_status *st) {
  aslink_xfer x;
  char *url, *reply = NULL;
  size_t len = 0;
  long status = 0;
  aslink_err e;
  *root = NULL;
  *result = NULL;
  url = bot_url(c, "/bot", method);
  if (!url) return ASLINK_ERR_NOMEM;
  memset(&x, 0, sizeof x);
  x.url = url;
  x.json_body = body;
  x.form = form;
  x.form_n = form_n;
  x.connect_timeout_ms = c->connect_timeout_ms;
  x.timeout_ms = timeout_ms;
  x.max_body = ASLINK_RESPONSE_MAX;
  x.cancel = &c->cancelled;
  x.token = c->token;
  e = aslink_perform(&x, &reply, &len, &status, st);
  wipe_free(url);
  if (e != ASLINK_OK) return e;
  e = aslink_decode_envelope(reply, len, status, c->token, root, result, st);
  free(reply);
  return e;
}

/* call() for a JSON body that a builder produced (freed here). */
static aslink_err call_json(aslink_ctx *c, const char *method, char *body,
                            long timeout_ms, asmodel_json_value **root,
                            const asmodel_json_value **result, aslink_status *st) {
  aslink_err e = call(c, method, body, NULL, 0, timeout_ms, root, result, st);
  free(body);
  return e;
}

/* Methods that answer True. */
static aslink_err call_true(aslink_ctx *c, const char *method, char *body,
                            aslink_status *st) {
  asmodel_json_value *root;
  const asmodel_json_value *result;
  aslink_err e = call_json(c, method, body, c->request_timeout_ms, &root, &result, st);
  if (e == ASLINK_OK && (e = aslink_decode_true(result)) != ASLINK_OK)
    aslink_status_msg(st, NULL, "%s: unexpected result", method);
  asmodel_json_free(root);
  return e;
}

/* Edits answer the edited Message, or True for inline messages. */
static aslink_err call_edit(aslink_ctx *c, const char *method, char *body,
                            aslink_status *st) {
  asmodel_json_value *root;
  const asmodel_json_value *result;
  aslink_err e = call_json(c, method, body, c->request_timeout_ms, &root, &result, st);
  if (e == ASLINK_OK && asmodel_json_typeof(result) != ASMODEL_JSON_OBJECT &&
      aslink_decode_true(result) != ASLINK_OK) {
    aslink_status_msg(st, NULL, "%s: unexpected result", method);
    e = ASLINK_ERR_PROTOCOL;
  }
  asmodel_json_free(root);
  return e;
}

/* ── updates ──────────────────────────────────────────────────────────── */

aslink_err aslink_get_updates(aslink_ctx *c, int64_t offset, int timeout_s,
                              aslink_update **out, size_t *out_n, aslink_status *st) {
  asmodel_json_value *root;
  const asmodel_json_value *result;
  char *body = NULL;
  aslink_err e;
  aslink_status_reset(st);
  if (out) *out = NULL;
  if (out_n) *out_n = 0;
  if (!out || !out_n) return ASLINK_ERR_INVALID;
  if ((e = check_ctx(c, st)) != ASLINK_OK ||
      (e = aslink_req_get_updates(offset, timeout_s, &body, st)) != ASLINK_OK)
    return e;
  e = call_json(c, "getUpdates", body, c->request_timeout_ms + 1000L * timeout_s, &root,
                &result, st);
  if (e == ASLINK_OK) e = aslink_decode_update_list(result, out, out_n, st);
  asmodel_json_free(root);
  return e;
}

/* ── sending ──────────────────────────────────────────────────────────── */

aslink_err aslink_send_message(aslink_ctx *c, int64_t chat_id, int64_t thread_id,
                               const char *text, const aslink_send_opts *opts,
                               int64_t *out_message_id, aslink_status *st) {
  asmodel_json_value *root;
  const asmodel_json_value *result;
  char *body = NULL;
  aslink_err e;
  aslink_status_reset(st);
  if (out_message_id) *out_message_id = 0;
  if ((e = check_ctx(c, st)) != ASLINK_OK ||
      (e = aslink_req_send_message(chat_id, thread_id, text, opts, &body, st)) !=
          ASLINK_OK)
    return e;
  e = call_json(c, "sendMessage", body, c->request_timeout_ms, &root, &result, st);
  if (e == ASLINK_OK && (e = aslink_decode_message_id(result, out_message_id)) != ASLINK_OK)
    aslink_status_msg(st, NULL, "sendMessage: no message_id in the result");
  asmodel_json_free(root);
  return e;
}

aslink_err aslink_edit_message(aslink_ctx *c, int64_t chat_id, int64_t message_id,
                               const char *text, const aslink_send_opts *opts,
                               aslink_status *st) {
  char *body = NULL;
  aslink_err e;
  aslink_status_reset(st);
  if ((e = check_ctx(c, st)) != ASLINK_OK ||
      (e = aslink_req_edit_message(chat_id, message_id, text, opts, &body, st)) !=
          ASLINK_OK)
    return e;
  return call_edit(c, "editMessageText", body, st);
}

aslink_err aslink_edit_keyboard(aslink_ctx *c, int64_t chat_id, int64_t message_id,
                                const aslink_keyboard *keyboard, aslink_status *st) {
  char *body = NULL;
  aslink_err e;
  aslink_status_reset(st);
  if ((e = check_ctx(c, st)) != ASLINK_OK ||
      (e = aslink_req_edit_keyboard(chat_id, message_id, keyboard, &body, st)) !=
          ASLINK_OK)
    return e;
  return call_edit(c, "editMessageReplyMarkup", body, st);
}

aslink_err aslink_send_draft(aslink_ctx *c, int64_t chat_id, int64_t thread_id,
                             int64_t draft_id, const char *text,
                             const aslink_draft_opts *opts, aslink_status *st) {
  char *body = NULL;
  aslink_err e;
  aslink_status_reset(st);
  if ((e = check_ctx(c, st)) != ASLINK_OK ||
      (e = aslink_req_send_draft(chat_id, thread_id, draft_id, text, opts, &body, st)) !=
          ASLINK_OK)
    return e;
  return call_true(c, "sendMessageDraft", body, st);
}

aslink_err aslink_send_chat_action(aslink_ctx *c, int64_t chat_id, int64_t thread_id,
                                   const char *action, aslink_status *st) {
  char *body = NULL;
  aslink_err e;
  aslink_status_reset(st);
  if ((e = check_ctx(c, st)) != ASLINK_OK ||
      (e = aslink_req_chat_action(chat_id, thread_id, action, &body, st)) != ASLINK_OK)
    return e;
  return call_true(c, "sendChatAction", body, st);
}

aslink_err aslink_answer_callback(aslink_ctx *c, const char *callback_id,
                                  const char *text, int alert, aslink_status *st) {
  char *body = NULL;
  aslink_err e;
  aslink_status_reset(st);
  if ((e = check_ctx(c, st)) != ASLINK_OK ||
      (e = aslink_req_answer_callback(callback_id, text, alert, &body, st)) != ASLINK_OK)
    return e;
  return call_true(c, "answerCallbackQuery", body, st);
}

aslink_err aslink_create_topic(aslink_ctx *c, int64_t chat_id, const char *name,
                               int64_t *out_thread_id, aslink_status *st) {
  asmodel_json_value *root;
  const asmodel_json_value *result;
  char *body = NULL;
  aslink_err e;
  aslink_status_reset(st);
  if (out_thread_id) *out_thread_id = 0;
  if ((e = check_ctx(c, st)) != ASLINK_OK ||
      (e = aslink_req_create_topic(chat_id, name, &body, st)) != ASLINK_OK)
    return e;
  e = call_json(c, "createForumTopic", body, c->request_timeout_ms, &root, &result, st);
  if (e == ASLINK_OK && (e = aslink_decode_topic(result, out_thread_id)) != ASLINK_OK)
    aslink_status_msg(st, NULL, "createForumTopic: no message_thread_id in the result");
  asmodel_json_free(root);
  return e;
}

/* ── bot ──────────────────────────────────────────────────────────────── */

aslink_err aslink_set_commands(aslink_ctx *c, const aslink_command *cmds, size_t n,
                               aslink_status *st) {
  char *body = NULL;
  aslink_err e;
  aslink_status_reset(st);
  if ((e = check_ctx(c, st)) != ASLINK_OK ||
      (e = aslink_req_set_commands(cmds, n, &body, st)) != ASLINK_OK)
    return e;
  return call_true(c, "setMyCommands", body, st);
}

aslink_err aslink_get_me(aslink_ctx *c, aslink_bot_info *out, aslink_status *st) {
  asmodel_json_value *root;
  const asmodel_json_value *result;
  char *body;
  aslink_err e;
  aslink_status_reset(st);
  if (out) memset(out, 0, sizeof *out);
  if ((e = check_ctx(c, st)) != ASLINK_OK) return e;
  if (!(body = core_strdup("{}"))) return ASLINK_ERR_NOMEM;
  e = call_json(c, "getMe", body, c->request_timeout_ms, &root, &result, st);
  if (e == ASLINK_OK && (e = aslink_decode_bot(result, out)) != ASLINK_OK)
    aslink_status_msg(st, NULL, "getMe: the result is not a bot user");
  asmodel_json_free(root);
  return e;
}

/* ── files ────────────────────────────────────────────────────────────── */

/* Telegram file paths look like "documents/file_3.pdf": relative, plain
 * ASCII segments. Anything else (an absolute path from a local Bot API
 * server, "..", odd bytes) is refused rather than escaped. */
static int file_path_ok(const char *p) {
  size_t i;
  if (!p[0] || p[0] == '/' || strstr(p, "..") || strstr(p, "//")) return 0;
  for (i = 0; p[i]; i++) {
    char ch = p[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-' || ch == '/'))
      return 0;
  }
  return 1;
}

static char *temp_sibling(const char *dest) {
  unsigned char rnd[8];
  char hex[17];
  size_t i;
  core_buf b;
  os_random_bytes(rnd, sizeof rnd);
  for (i = 0; i < sizeof rnd; i++) {
    static const char digits[] = "0123456789abcdef";
    hex[2 * i] = digits[rnd[i] >> 4];
    hex[2 * i + 1] = digits[rnd[i] & 15];
  }
  hex[16] = 0;
  core_buf_init(&b);
  if (core_buf_printf(&b, "%s.part-%s", dest, hex) != CORE_OK) {
    core_buf_free(&b);
    return NULL;
  }
  return core_buf_detach(&b);
}

/* Create a new file that only the owner can read: downloads are user
 * content. Never reuses or follows an existing entry at that name. */
static FILE *create_private(const char *path) {
#if defined(_WIN32)
  return os_fopen(path, "wbx");
#else
  FILE *f;
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
  if (fd < 0) return NULL;
  f = fdopen(fd, "wb");
  if (!f) {
    close(fd);
    remove(path);
  }
  return f;
#endif
}

aslink_err aslink_download_file(aslink_ctx *c, const char *file_id,
                                const char *dest_path, uint64_t max_bytes,
                                aslink_status *st) {
  asmodel_json_value *root;
  const asmodel_json_value *result;
  char *body = NULL, *path = NULL, *url = NULL, *tmp = NULL, *reply = NULL;
  int64_t size = -1;
  long status = 0;
  size_t len = 0;
  FILE *f = NULL;
  aslink_xfer x;
  aslink_err e;
  aslink_status_reset(st);
  if ((e = check_ctx(c, st)) != ASLINK_OK) return e;
  if (!dest_path || !*dest_path) {
    aslink_status_msg(st, NULL, "dest_path is required");
    return ASLINK_ERR_INVALID;
  }
  if (!max_bytes) max_bytes = ASLINK_DOWNLOAD_DEFAULT;
  if ((e = aslink_req_get_file(file_id, &body, st)) != ASLINK_OK) return e;
  e = call_json(c, "getFile", body, c->request_timeout_ms, &root, &result, st);
  if (e == ASLINK_OK && (e = aslink_decode_file(result, &path, &size)) != ASLINK_OK)
    aslink_status_msg(st, NULL, "getFile: no downloadable file_path in the result");
  asmodel_json_free(root);
  if (e != ASLINK_OK) return e;
  if (size >= 0 && (uint64_t)size > max_bytes) {
    aslink_status_msg(st, NULL, "file is %lld bytes, the cap is %llu", (long long)size,
                      (unsigned long long)max_bytes);
    e = ASLINK_ERR_TOO_LARGE;
    goto done;
  }
  if (!file_path_ok(path)) {
    aslink_status_msg(st, NULL, "getFile: unexpected file_path");
    e = ASLINK_ERR_PROTOCOL;
    goto done;
  }
  if (!(url = bot_url(c, "/file/bot", path)) || !(tmp = temp_sibling(dest_path))) {
    e = ASLINK_ERR_NOMEM;
    goto done;
  }
  f = create_private(tmp);
  if (!f) {
    aslink_status_msg(st, NULL, "cannot create a temporary file next to dest_path");
    e = ASLINK_ERR_IO;
    goto done;
  }
  memset(&x, 0, sizeof x);
  x.url = url;
  x.connect_timeout_ms = c->connect_timeout_ms;
  x.timeout_ms = c->transfer_timeout_ms;
  x.sink = f;
  x.sink_max = max_bytes;
  x.cancel = &c->cancelled;
  x.token = c->token;
  e = aslink_perform(&x, &reply, &len, &status, st);
  if (e == ASLINK_OK && (status < 200 || status > 299)) {
    asmodel_json_value *err_root = NULL;
    const asmodel_json_value *err_result = NULL;
    e = aslink_decode_envelope(reply, len, status, c->token, &err_root, &err_result, st);
    asmodel_json_free(err_root);
    if (e == ASLINK_OK) e = ASLINK_ERR_PROTOCOL;
  }
  if (e == ASLINK_OK && os_fsync(f) != CORE_OK) {
    aslink_status_msg(st, NULL, "cannot sync the downloaded file");
    e = ASLINK_ERR_IO;
  }
  if (fclose(f) != 0 && e == ASLINK_OK) {
    aslink_status_msg(st, NULL, "cannot close the downloaded file");
    e = ASLINK_ERR_IO;
  }
  f = NULL;
  if (e == ASLINK_OK && os_file_replace(tmp, dest_path) != CORE_OK) {
    aslink_status_msg(st, NULL, "cannot move the downloaded file into place");
    e = ASLINK_ERR_IO;
  }
  if (e != ASLINK_OK) os_remove_file(tmp);

done:
  if (f) {
    fclose(f);
    os_remove_file(tmp);
  }
  free(reply);
  free(tmp);
  wipe_free(url);
  free(path);
  return e;
}

/* The announced upload name: the path's last segment, without bytes that
 * could bend a Content-Disposition header. */
static char *upload_name(const char *path) {
  const char *base = path, *p;
  char *name;
  size_t i;
  for (p = path; *p; p++)
    if (*p == '/' || *p == '\\') base = p + 1;
  name = core_strdup(*base ? base : "file");
  if (!name) return NULL;
  for (i = 0; name[i]; i++) {
    unsigned char ch = (unsigned char)name[i];
    if (ch < 0x20 || ch == 0x7F || ch == '"' || ch == ';') name[i] = '_';
  }
  if (!core_utf8_valid(name, strlen(name))) {
    free(name);
    name = core_strdup("file");
  }
  return name;
}

aslink_err aslink_send_document(aslink_ctx *c, int64_t chat_id, int64_t thread_id,
                                const char *path, const char *caption,
                                aslink_status *st) {
  asmodel_json_value *root;
  const asmodel_json_value *result;
  aslink_form_field form[4];
  char chat[32], thread[32], *name = NULL;
  uint64_t size = 0;
  size_t n = 0;
  core_err ce;
  aslink_err e;
  aslink_status_reset(st);
  if ((e = check_ctx(c, st)) != ASLINK_OK ||
      (e = aslink_check_ids(chat_id, thread_id, st)) != ASLINK_OK ||
      (e = aslink_check_text("caption", caption, ASLINK_CAPTION_MAX, 1, st)) != ASLINK_OK)
    return e;
  if (!path || !*path) {
    aslink_status_msg(st, NULL, "path is required");
    return ASLINK_ERR_INVALID;
  }
  ce = os_file_size(path, &size);
  if (ce != CORE_OK) {
    aslink_status_msg(st, NULL, "cannot read the file to upload");
    return aslink_from_core(ce);
  }
  if (size == 0) {
    aslink_status_msg(st, NULL, "the file to upload is empty");
    return ASLINK_ERR_INVALID;
  }
  if (size > ASLINK_UPLOAD_MAX) {
    aslink_status_msg(st, NULL, "file is %llu bytes, the upload cap is 50 MB",
                      (unsigned long long)size);
    return ASLINK_ERR_TOO_LARGE;
  }
  if (!(name = upload_name(path))) return ASLINK_ERR_NOMEM;
  memset(form, 0, sizeof form);
  snprintf(chat, sizeof chat, "%lld", (long long)chat_id);
  form[n].name = "chat_id";
  form[n++].value = chat;
  if (thread_id) {
    snprintf(thread, sizeof thread, "%lld", (long long)thread_id);
    form[n].name = "message_thread_id";
    form[n++].value = thread;
  }
  if (caption && *caption) {
    form[n].name = "caption";
    form[n++].value = caption;
  }
  form[n].name = "document";
  form[n].file_path = path;
  form[n++].file_name = name;
  e = call(c, "sendDocument", NULL, form, n, c->transfer_timeout_ms, &root, &result, st);
  if (e == ASLINK_OK && aslink_decode_message_id(result, NULL) != ASLINK_OK) {
    aslink_status_msg(st, NULL, "sendDocument: no message_id in the result");
    e = ASLINK_ERR_PROTOCOL;
  }
  asmodel_json_free(root);
  free(name);
  return e;
}
