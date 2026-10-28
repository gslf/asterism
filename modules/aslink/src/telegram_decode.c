/*
 * telegram_decode.c — pure decoding of Bot API replies (no network).
 *
 * Everything here reads attacker-reachable data: any Telegram user can make
 * the bot receive a message. Identifiers must have the documented type,
 * range and a conservative charset or the whole update is marked invalid;
 * display strings (names, titles, file names) are truncated on a code point
 * boundary. An invalid update keeps its update_id and kind_name so the
 * poller can confirm it and move on; only a reply whose envelope or
 * update_id is broken fails as a whole (that data comes from Telegram, not
 * from users).
 */

#include "aslink_internal.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef const asmodel_json_value jv;

/* Telegram ids have at most 52 significant bits. */
#define ID_MAX ((int64_t)9007199254740991LL) /* 2^53 - 1 */

/* ── field access: 1 present and valid, 0 absent (or null), -1 invalid ── */

static int get_int(jv *o, const char *k, int64_t lo, int64_t hi, int64_t *out) {
  jv *v = asmodel_json_object_get(o, k);
  long long x;
  if (!v || asmodel_json_typeof(v) == ASMODEL_JSON_NULL) return 0;
  if (!asmodel_json_is_int(v)) return -1;
  x = asmodel_json_int_value(v);
  if (x < lo || x > hi) return -1;
  *out = (int64_t)x;
  return 1;
}

static int get_bool(jv *o, const char *k, int *out) {
  jv *v = asmodel_json_object_get(o, k);
  if (!v || asmodel_json_typeof(v) == ASMODEL_JSON_NULL) return 0;
  if (asmodel_json_typeof(v) != ASMODEL_JSON_BOOL) return -1;
  *out = asmodel_json_bool_value(v) ? 1 : 0;
  return 1;
}

/* Strings never carry NULs into C: the parser already guarantees UTF-8. */
static int get_str(jv *o, const char *k, const char **s, size_t *len) {
  jv *v = asmodel_json_object_get(o, k);
  if (!v || asmodel_json_typeof(v) == ASMODEL_JSON_NULL) return 0;
  if (asmodel_json_typeof(v) != ASMODEL_JSON_STRING) return -1;
  *s = asmodel_json_string_value(v);
  *len = asmodel_json_string_length(v);
  if (memchr(*s, 0, *len)) return -1;
  return 1;
}

static jv *get_obj(jv *o, const char *k, int *bad) {
  jv *v = asmodel_json_object_get(o, k);
  if (!v || asmodel_json_typeof(v) == ASMODEL_JSON_NULL) return NULL;
  if (asmodel_json_typeof(v) != ASMODEL_JSON_OBJECT) {
    *bad = 1;
    return NULL;
  }
  return v;
}

/* Opaque identifier (file_id, callback id): printable ASCII, no spaces,
 * fits dst. Returns 1 copied, 0 absent, -1 invalid. */
static int copy_id(char *dst, size_t cap, jv *o, const char *k) {
  const char *s;
  size_t n, i;
  int r = get_str(o, k, &s, &n);
  if (r <= 0) return r;
  if (n == 0 || n >= cap) return -1;
  for (i = 0; i < n; i++)
    if ((unsigned char)s[i] <= 0x20 || (unsigned char)s[i] >= 0x7F) return -1;
  memcpy(dst, s, n);
  dst[n] = 0;
  return 1;
}

/* Display string: truncated on a code point boundary; -1 on wrong type. */
static int copy_display(char *dst, size_t cap, jv *o, const char *k) {
  const char *s;
  size_t n;
  int r = get_str(o, k, &s, &n);
  if (r <= 0) return r;
  aslink_copy_display(dst, cap, s, n);
  return 1;
}

/* Username: [A-Za-z0-9_] only, else left empty (it is cosmetic: identity
 * is the numeric id). */
static int copy_username(char *dst, size_t cap, jv *o) {
  const char *s;
  size_t n, i;
  int r = get_str(o, "username", &s, &n);
  if (r <= 0) return r;
  if (n == 0 || n >= cap) return 1;
  for (i = 0; i < n; i++)
    if (!isalnum((unsigned char)s[i]) && s[i] != '_') return 1;
  memcpy(dst, s, n);
  dst[n] = 0;
  return 1;
}

/* "first last" of a User object into dst. */
static int user_name(char *dst, size_t cap, jv *user) {
  const char *first = "", *last = "";
  size_t nf = 0, nl = 0;
  core_buf b;
  int ok;
  if (get_str(user, "first_name", &first, &nf) < 0 ||
      get_str(user, "last_name", &last, &nl) < 0)
    return -1;
  core_buf_init(&b);
  ok = core_buf_append(&b, first, nf) == CORE_OK &&
       (!nl || !nf || core_buf_appendc(&b, ' ') == CORE_OK) &&
       core_buf_append(&b, last, nl) == CORE_OK &&
       core_buf_appends(&b, "") == CORE_OK;
  if (ok) aslink_copy_display(dst, cap, b.data, b.len);
  core_buf_free(&b);
  return ok ? 1 : -2;
}

/* ── building blocks ──────────────────────────────────────────────────── */

typedef enum { D_OK = 0, D_INVALID = -1, D_NOMEM = -2 } dres;

static dres decode_from(jv *from, aslink_update *u) {
  int r;
  if (get_int(from, "id", 1, ID_MAX, &u->from_id) != 1) return D_INVALID;
  if (get_bool(from, "is_bot", &u->from_is_bot) < 0) return D_INVALID;
  if (copy_username(u->from_username, sizeof u->from_username, from) < 0)
    return D_INVALID;
  r = user_name(u->from_name, sizeof u->from_name, from);
  return r == -2 ? D_NOMEM : r < 0 ? D_INVALID : D_OK;
}

static dres decode_chat(jv *chat, aslink_update *u) {
  const char *type;
  size_t n, i;
  if (!chat || get_int(chat, "id", -ID_MAX, ID_MAX, &u->chat_id) != 1 ||
      u->chat_id == 0)
    return D_INVALID;
  if (get_str(chat, "type", &type, &n) == 1 && n < sizeof u->chat_type) {
    for (i = 0; i < n; i++)
      if (!islower((unsigned char)type[i]) && type[i] != '_') break;
    if (i == n) {
      memcpy(u->chat_type, type, n);
      u->chat_type[n] = 0;
    }
  }
  return D_OK;
}

static dres decode_origin(jv *origin, aslink_update *u) {
  const char *type;
  size_t n, i;
  char name[160] = {0};
  jv *who;
  int bad = 0;
  core_buf b;
  dres res = D_OK;
  if (get_str(origin, "type", &type, &n) != 1) return D_INVALID;
  if (n == 4 && memcmp(type, "user", 4) == 0) {
    who = get_obj(origin, "sender_user", &bad);
    if (bad || (who && user_name(name, sizeof name, who) < 0)) return D_INVALID;
  } else if (n == 11 && memcmp(type, "hidden_user", 11) == 0) {
    if (copy_display(name, sizeof name, origin, "sender_user_name") < 0)
      return D_INVALID;
  } else if (n == 4 && memcmp(type, "chat", 4) == 0) {
    who = get_obj(origin, "sender_chat", &bad);
    if (bad || (who && copy_display(name, sizeof name, who, "title") < 0))
      return D_INVALID;
  } else if (n == 7 && memcmp(type, "channel", 7) == 0) {
    who = get_obj(origin, "chat", &bad);
    if (bad || (who && copy_display(name, sizeof name, who, "title") < 0))
      return D_INVALID;
  } else {
    /* A future origin kind: keep its name when it looks like one. */
    for (i = 0; i < n && n < 24; i++)
      if (!islower((unsigned char)type[i]) && type[i] != '_') break;
    if (n == 0 || n >= 24 || i != n) {
      type = "unknown";
      n = 7;
    }
  }
  u->forwarded = 1;
  core_buf_init(&b);
  if (core_buf_append(&b, type, n) != CORE_OK || core_buf_appendc(&b, ':') != CORE_OK ||
      core_buf_appends(&b, name) != CORE_OK) {
    res = D_NOMEM;
  } else {
    aslink_copy_display(u->forward_origin, sizeof u->forward_origin, b.data, b.len);
  }
  core_buf_free(&b);
  return res;
}

static int get_duration(jv *o, int *out) {
  int64_t d = 0;
  int r = get_int(o, "duration", 0, INT_MAX, &d);
  if (r < 0) return -1;
  *out = (int)d;
  return 0;
}

static int get_size(jv *o, int64_t *out) {
  return get_int(o, "file_size", 0, ID_MAX, out) < 0 ? -1 : 0;
}

/* The ASCII-only MIME type, else left empty. */
static int copy_mime(char *dst, size_t cap, jv *o) {
  char tmp[128] = {0};
  size_t i;
  if (copy_display(tmp, sizeof tmp < cap ? sizeof tmp : cap, o, "mime_type") < 0)
    return -1;
  for (i = 0; tmp[i]; i++)
    if ((unsigned char)tmp[i] <= 0x20 || (unsigned char)tmp[i] >= 0x7F) return 0;
  memcpy(dst, tmp, i + 1);
  return 0;
}

static dres decode_media(jv *m, aslink_update *u) {
  int bad = 0;
  jv *voice = get_obj(m, "voice", &bad);
  jv *audio = get_obj(m, "audio", &bad);
  jv *doc = get_obj(m, "document", &bad);
  jv *photo = asmodel_json_object_get(m, "photo");
  if (bad) return D_INVALID;
  if (voice) {
    if (copy_id(u->voice_file_id, sizeof u->voice_file_id, voice, "file_id") != 1 ||
        get_duration(voice, &u->voice_duration_s) < 0 ||
        get_size(voice, &u->file_size) < 0)
      return D_INVALID;
  }
  if (audio) {
    if (copy_id(u->audio_file_id, sizeof u->audio_file_id, audio, "file_id") != 1 ||
        get_duration(audio, &u->audio_duration_s) < 0 ||
        copy_mime(u->audio_mime, sizeof u->audio_mime, audio) < 0 ||
        get_size(audio, &u->file_size) < 0)
      return D_INVALID;
  }
  if (doc) {
    if (copy_id(u->document_file_id, sizeof u->document_file_id, doc, "file_id") != 1 ||
        copy_display(u->document_name, sizeof u->document_name, doc, "file_name") < 0 ||
        copy_mime(u->document_mime, sizeof u->document_mime, doc) < 0 ||
        get_size(doc, &u->file_size) < 0)
      return D_INVALID;
  }
  if (photo && asmodel_json_typeof(photo) != ASMODEL_JSON_NULL) {
    size_t i, n = asmodel_json_array_len(photo);
    int64_t best = -1;
    jv *pick = NULL;
    if (asmodel_json_typeof(photo) != ASMODEL_JSON_ARRAY || n == 0) return D_INVALID;
    for (i = 0; i < n; i++) {
      jv *ps = asmodel_json_array_at(photo, i);
      int64_t w = 0, h = 0;
      if (asmodel_json_typeof(ps) != ASMODEL_JSON_OBJECT ||
          get_int(ps, "width", 0, 1000000, &w) < 0 ||
          get_int(ps, "height", 0, 1000000, &h) < 0)
        return D_INVALID;
      if (w * h >= best) { /* sizes come smallest first; ties keep the last */
        best = w * h;
        pick = ps;
      }
    }
    if (copy_id(u->photo_file_id, sizeof u->photo_file_id, pick, "file_id") != 1 ||
        get_size(pick, &u->file_size) < 0)
      return D_INVALID;
  }
  return D_OK;
}

static dres decode_message(jv *m, aslink_update *u) {
  jv *from, *reply, *origin;
  const char *text;
  size_t n;
  int bad = 0, r;
  dres d;
  if (asmodel_json_typeof(m) != ASMODEL_JSON_OBJECT) return D_INVALID;
  if (get_int(m, "message_id", 0, ID_MAX, &u->message_id) != 1) return D_INVALID;
  if ((d = decode_chat(get_obj(m, "chat", &bad), u)) != D_OK || bad) return D_INVALID;
  if (get_int(m, "message_thread_id", 1, ID_MAX, &u->thread_id) < 0 ||
      get_bool(m, "is_topic_message", &u->is_topic) < 0 ||
      get_int(m, "date", 0, ID_MAX, &u->date) < 0)
    return D_INVALID;
  from = get_obj(m, "from", &bad);
  if (bad) return D_INVALID;
  if (from && (d = decode_from(from, u)) != D_OK) return d;
  r = get_str(m, "text", &text, &n);
  if (r == 0) r = get_str(m, "caption", &text, &n);
  if (r < 0 || (r == 1 && n > ASLINK_RAW_TEXT_MAX)) return D_INVALID;
  if (r == 1) {
    u->text = core_strndup(text, n);
    if (!u->text) return D_NOMEM;
  }
  origin = get_obj(m, "forward_origin", &bad);
  if (bad) return D_INVALID;
  if (origin && (d = decode_origin(origin, u)) != D_OK) return d;
  reply = get_obj(m, "reply_to_message", &bad);
  if (bad) return D_INVALID;
  /* In topics every message "replies" to the topic's creation message;
   * that is not a reply the user made. */
  if (reply && !asmodel_json_object_get(reply, "forum_topic_created") &&
      get_int(reply, "message_id", 1, ID_MAX, &u->reply_to_message_id) < 0)
    return D_INVALID;
  return decode_media(m, u);
}

static dres decode_callback(jv *q, aslink_update *u) {
  jv *from, *msg;
  const char *data;
  size_t n;
  int bad = 0, r;
  dres d;
  if (asmodel_json_typeof(q) != ASMODEL_JSON_OBJECT) return D_INVALID;
  if (copy_id(u->callback_id, sizeof u->callback_id, q, "id") != 1) return D_INVALID;
  from = get_obj(q, "from", &bad);
  if (bad || !from) return D_INVALID;
  if ((d = decode_from(from, u)) != D_OK) return d;
  msg = get_obj(q, "message", &bad);
  if (bad) return D_INVALID;
  if (msg) {
    /* Message or InaccessibleMessage (date 0): chat and message_id only. */
    if (decode_chat(get_obj(msg, "chat", &bad), u) != D_OK || bad ||
        get_int(msg, "message_id", 0, ID_MAX, &u->callback_message_id) != 1 ||
        get_int(msg, "message_thread_id", 1, ID_MAX, &u->thread_id) < 0 ||
        get_bool(msg, "is_topic_message", &u->is_topic) < 0)
      return D_INVALID;
  }
  r = get_str(q, "data", &data, &n);
  if (r < 0 || (r == 1 && n > 256)) return D_INVALID;
  if (r == 1) {
    u->callback_data = core_strndup(data, n);
    if (!u->callback_data) return D_NOMEM;
  }
  return D_OK;
}

static dres decode_stop(jv *s, aslink_update *u) {
  int bad = 0;
  if (asmodel_json_typeof(s) != ASMODEL_JSON_OBJECT) return D_INVALID;
  if (decode_chat(get_obj(s, "chat", &bad), u) != D_OK || bad ||
      get_int(s, "message_thread_id", 1, ID_MAX, &u->thread_id) < 0 ||
      get_int(s, "draft_id", -ID_MAX, ID_MAX, &u->draft_id) != 1 || u->draft_id == 0)
    return D_INVALID;
  return D_OK;
}

static void update_clear(aslink_update *u) {
  free(u->text);
  free(u->callback_data);
  u->text = NULL;
  u->callback_data = NULL;
}

/* Decode one update into u (zeroed). D_INVALID only for a missing or bad
 * update_id; payload problems set u->invalid instead. */
static dres decode_update(jv *o, aslink_update *u) {
  size_t i, count;
  const char *key = NULL;
  jv *payload = NULL;
  dres d = D_OK;
  if (asmodel_json_typeof(o) != ASMODEL_JSON_OBJECT ||
      get_int(o, "update_id", 0, ID_MAX, &u->update_id) != 1)
    return D_INVALID;
  count = asmodel_json_object_count(o);
  for (i = 0; i < count; i++) {
    const char *k = asmodel_json_object_key_at(o, i);
    if (strcmp(k, "update_id") != 0) {
      key = k;
      payload = asmodel_json_object_value_at(o, i);
      break;
    }
  }
  u->kind = ASLINK_UPDATE_OTHER;
  if (!key) return D_OK;
  aslink_copy_display(u->kind_name, sizeof u->kind_name, key, strlen(key));
  if (strcmp(key, "message") == 0) {
    u->kind = ASLINK_UPDATE_MESSAGE;
    d = decode_message(payload, u);
  } else if (strcmp(key, "callback_query") == 0) {
    u->kind = ASLINK_UPDATE_CALLBACK;
    d = decode_callback(payload, u);
  } else if (strcmp(key, "stopped_message_generation") == 0) {
    u->kind = ASLINK_UPDATE_STOP;
    d = decode_stop(payload, u);
  }
  if (d == D_NOMEM) return D_NOMEM;
  if (d == D_INVALID) {
    int64_t id = u->update_id;
    char name[sizeof u->kind_name];
    memcpy(name, u->kind_name, sizeof name);
    update_clear(u);
    memset(u, 0, sizeof *u);
    u->update_id = id;
    memcpy(u->kind_name, name, sizeof name);
    u->kind = ASLINK_UPDATE_OTHER;
    u->invalid = 1;
  }
  return D_OK;
}

/* ── envelope ─────────────────────────────────────────────────────────── */

static int contains_ci(const char *hay, const char *needle) {
  size_t n = strlen(needle);
  for (; hay && *hay; hay++) {
    size_t i;
    for (i = 0; i < n && hay[i] && tolower((unsigned char)hay[i]) == needle[i]; i++) {}
    if (i == n) return 1;
  }
  return 0;
}

static aslink_err map_code(long code, const char *desc) {
  if (code == 401 || code == 403) return ASLINK_ERR_UNAUTHORIZED;
  if (code == 404) return ASLINK_ERR_NOT_FOUND;
  if (code == 413) return ASLINK_ERR_TOO_LARGE;
  if (code == 429) return ASLINK_ERR_RATE_LIMITED;
  if (code >= 500 && code <= 599) return ASLINK_ERR_IO;
  if (code == 400 && desc) {
    if (contains_ci(desc, "not found")) return ASLINK_ERR_NOT_FOUND;
    if (contains_ci(desc, "too big") || contains_ci(desc, "too large"))
      return ASLINK_ERR_TOO_LARGE;
  }
  return ASLINK_ERR_PROTOCOL;
}

aslink_err aslink_decode_envelope(const char *body, size_t len, long http_status,
                                  const char *token, asmodel_json_value **out_root,
                                  const asmodel_json_value **out_result,
                                  aslink_status *st) {
  asmodel_json_value *root = NULL;
  jv *ok, *result, *params;
  const char *desc = "";
  size_t dlen = 0;
  int64_t code = 0, retry = 0;
  char shown[200];
  aslink_err e;
  *out_root = NULL;
  *out_result = NULL;
  if (st) st->http_status = (int)http_status;
  if (!body || asmodel_json_parse(body, len, &root) != 0 ||
      asmodel_json_typeof(root) != ASMODEL_JSON_OBJECT ||
      asmodel_json_typeof(ok = asmodel_json_object_get(root, "ok")) != ASMODEL_JSON_BOOL) {
    asmodel_json_free(root);
    e = map_code(http_status, NULL);
    aslink_status_msg(st, token, "HTTP %ld with a body that is not a Bot API reply",
                      http_status);
    return e;
  }
  if (asmodel_json_bool_value(ok)) {
    result = asmodel_json_object_get(root, "result");
    if (!result || http_status < 200 || http_status > 299) {
      asmodel_json_free(root);
      aslink_status_msg(st, token, "HTTP %ld: ok reply without a result", http_status);
      return ASLINK_ERR_PROTOCOL;
    }
    *out_root = root;
    *out_result = result;
    return ASLINK_OK;
  }
  /* ok:false — error_code is "subject to change", so the HTTP status
   * decides when it is missing. */
  get_int(root, "error_code", 0, 999, &code);
  if (get_str(root, "description", &desc, &dlen) != 1) {
    desc = "";
    dlen = 0;
  }
  params = asmodel_json_object_get(root, "parameters");
  if (params) get_int(params, "retry_after", 0, 86400 * 7, &retry);
  aslink_copy_display(shown, sizeof shown, desc, dlen);
  if (st) {
    st->telegram_code = (int)code;
    st->retry_after_s = (int)retry;
  }
  e = map_code(code ? (long)code : http_status, shown);
  aslink_status_msg(st, token, "Telegram error %d: %s",
                    (int)(code ? code : http_status), shown[0] ? shown : "(no description)");
  asmodel_json_free(root);
  return e;
}

aslink_err aslink_decode_update_list(const asmodel_json_value *result, aslink_update **out,
                                     size_t *out_n, aslink_status *st) {
  aslink_update *v = NULL;
  size_t i, n;
  if (!out || !out_n) return ASLINK_ERR_INVALID;
  *out = NULL;
  *out_n = 0;
  if (asmodel_json_typeof(result) != ASMODEL_JSON_ARRAY) {
    aslink_status_msg(st, NULL, "getUpdates result is not an array");
    return ASLINK_ERR_PROTOCOL;
  }
  n = asmodel_json_array_len(result);
  if (n) {
    v = (aslink_update *)calloc(n, sizeof *v);
    if (!v) return ASLINK_ERR_NOMEM;
  }
  for (i = 0; i < n; i++) {
    dres d = decode_update(asmodel_json_array_at(result, i), &v[i]);
    if (d != D_OK) {
      aslink_updates_free(v, i + 1);
      if (d == D_NOMEM) return ASLINK_ERR_NOMEM;
      aslink_status_msg(st, NULL, "update %u has no valid update_id", (unsigned)i);
      return ASLINK_ERR_PROTOCOL;
    }
  }
  *out = v;
  *out_n = n;
  return ASLINK_OK;
}

aslink_err aslink_decode_updates(const char *json, size_t len, aslink_update **out,
                                 size_t *out_n, aslink_status *st) {
  asmodel_json_value *root = NULL;
  const asmodel_json_value *result = NULL;
  aslink_err e;
  if (!out || !out_n) return ASLINK_ERR_INVALID;
  *out = NULL;
  *out_n = 0;
  e = aslink_decode_envelope(json, len, 200, NULL, &root, &result, st);
  if (e == ASLINK_OK) e = aslink_decode_update_list(result, out, out_n, st);
  asmodel_json_free(root);
  return e;
}

void aslink_updates_free(aslink_update *u, size_t n) {
  size_t i;
  if (!u) return;
  for (i = 0; i < n; i++) update_clear(&u[i]);
  free(u);
}

/* ── method results ───────────────────────────────────────────────────── */

aslink_err aslink_decode_message_id(const asmodel_json_value *result, int64_t *out) {
  int64_t id = 0;
  if (asmodel_json_typeof(result) != ASMODEL_JSON_OBJECT ||
      get_int(result, "message_id", 0, ID_MAX, &id) != 1)
    return ASLINK_ERR_PROTOCOL;
  if (out) *out = id;
  return ASLINK_OK;
}

aslink_err aslink_decode_topic(const asmodel_json_value *result, int64_t *out_thread_id) {
  int64_t id = 0;
  if (asmodel_json_typeof(result) != ASMODEL_JSON_OBJECT ||
      get_int(result, "message_thread_id", 1, ID_MAX, &id) != 1)
    return ASLINK_ERR_PROTOCOL;
  if (out_thread_id) *out_thread_id = id;
  return ASLINK_OK;
}

aslink_err aslink_decode_file(const asmodel_json_value *result, char **out_path,
                              int64_t *out_size) {
  const char *path;
  size_t n;
  int64_t size = -1;
  *out_path = NULL;
  if (asmodel_json_typeof(result) != ASMODEL_JSON_OBJECT ||
      get_str(result, "file_path", &path, &n) != 1 || n == 0 || n > 1024 ||
      get_int(result, "file_size", 0, ID_MAX, &size) < 0)
    return ASLINK_ERR_PROTOCOL;
  *out_path = core_strndup(path, n);
  if (!*out_path) return ASLINK_ERR_NOMEM;
  if (out_size) *out_size = size;
  return ASLINK_OK;
}

aslink_err aslink_decode_bot(const asmodel_json_value *result, aslink_bot_info *out) {
  aslink_bot_info b;
  memset(&b, 0, sizeof b);
  if (asmodel_json_typeof(result) != ASMODEL_JSON_OBJECT ||
      get_int(result, "id", 1, ID_MAX, &b.id) != 1 ||
      copy_username(b.username, sizeof b.username, result) < 0 ||
      get_bool(result, "has_topics_enabled", &b.has_topics_enabled) < 0 ||
      get_bool(result, "allows_users_to_create_topics",
               &b.allows_users_to_create_topics) < 0)
    return ASLINK_ERR_PROTOCOL;
  if (out) *out = b;
  return ASLINK_OK;
}

aslink_err aslink_decode_true(const asmodel_json_value *result) {
  return asmodel_json_typeof(result) == ASMODEL_JSON_BOOL &&
                 asmodel_json_bool_value(result)
             ? ASLINK_OK
             : ASLINK_ERR_PROTOCOL;
}
