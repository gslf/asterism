/*
 * util.c — error names, string lists, enum names, hashes, signatures and
 * the key locator that turns a key path into a source line.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astime_internal.h"

astime_err astime_from_core(core_err e) {
  switch (e) {
  case CORE_OK: return ASTIME_OK;
  case CORE_ERR_NOT_FOUND: return ASTIME_ERR_NOT_FOUND;
  case CORE_ERR_INVALID: return ASTIME_ERR_INVALID;
  case CORE_ERR_NOMEM: return ASTIME_ERR_NOMEM;
  case CORE_ERR_BUSY: return ASTIME_ERR_BUSY;
  case CORE_ERR_IO: break;
  }
  return ASTIME_ERR_IO;
}

const char *astime_err_name(astime_err e) {
  switch (e) {
  case ASTIME_OK: return "ASTIME_OK";
  case ASTIME_ERR_IO: return "ASTIME_ERR_IO";
  case ASTIME_ERR_PARSE: return "ASTIME_ERR_PARSE";
  case ASTIME_ERR_INVALID: return "ASTIME_ERR_INVALID";
  case ASTIME_ERR_NOT_FOUND: return "ASTIME_ERR_NOT_FOUND";
  case ASTIME_ERR_EXISTS: return "ASTIME_ERR_EXISTS";
  case ASTIME_ERR_BUSY: return "ASTIME_ERR_BUSY";
  case ASTIME_ERR_NOMEM: return "ASTIME_ERR_NOMEM";
  }
  return "ASTIME_ERR_UNKNOWN";
}

void astime_free(void *p) { free(p); }

void astime_strings_free(astime_strings *l) {
  size_t i;
  if (!l) return;
  for (i = 0; i < l->n; i++) free(l->items[i]);
  free(l->items);
  l->items = NULL;
  l->n = 0;
}

astime_err astime__strings_push(astime_strings *l, char *owned) {
  char **items;
  if (!owned) return ASTIME_ERR_NOMEM;
  items = (char **)realloc(l->items, (l->n + 1) * sizeof *items);
  if (!items) {
    free(owned);
    return ASTIME_ERR_NOMEM;
  }
  items[l->n++] = owned;
  l->items = items;
  return ASTIME_OK;
}

astime_err astime__strings_pushf(astime_strings *l, const char *fmt, ...) {
  core_buf b;
  va_list ap;
  core_err ce;
  core_buf_init(&b);
  va_start(ap, fmt);
  ce = core_buf_vprintf(&b, fmt, ap);
  va_end(ap);
  if (ce != CORE_OK) {
    core_buf_free(&b);
    return ASTIME_ERR_NOMEM;
  }
  return astime__strings_push(l, core_buf_detach(&b));
}

void astime__seterr(char *err, const char *fmt, ...) {
  va_list ap;
  if (!err) return;
  va_start(ap, fmt);
  vsnprintf(err, ASTIME_ERR_SIZE, fmt, ap);
  va_end(ap);
}

void astime__copy_short(char *dst, size_t cap, const char *src) {
  size_t n = 0, i = 0;
  if (!dst || cap == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  while (src[i] && n + 1 < cap) {
    unsigned char ch = (unsigned char)src[i];
    size_t len = ch < 0x80 ? 1 : (ch >> 5) == 6 ? 2 : (ch >> 4) == 14 ? 3 : (ch >> 3) == 30 ? 4 : 1;
    size_t k;
    if (n + len >= cap) break;
    for (k = 1; k < len; k++)
      if (((unsigned char)src[i + k] & 0xC0) != 0x80) break;
    if (k < len) { /* broken sequence: replace the lead byte */
      dst[n++] = '?';
      i++;
      continue;
    }
    if (len == 1) {
      dst[n++] = (ch < 0x20 || ch == 0x7F) ? ' ' : (char)ch;
    } else {
      memcpy(dst + n, src + i, len);
      n += len;
    }
    i += len;
  }
  dst[n] = '\0';
}

/* ── enum names ───────────────────────────────────────────────────────── */

const char *astime_mode_name(astime_mode m) {
  switch (m) {
  case ASTIME_MODE_CODING: return "coding";
  case ASTIME_MODE_AUTOMATE: return "automate";
  case ASTIME_MODE_CHAT: break;
  }
  return "chat";
}

const char *astime_profile_name(astime_profile p) {
  switch (p) {
  case ASTIME_PROFILE_CODING_READONLY: return "coding-readonly";
  case ASTIME_PROFILE_CODING_SANDBOXED: return "coding-sandboxed";
  case ASTIME_PROFILE_AUTOMATION_CI: return "automation-ci";
  case ASTIME_PROFILE_CHAT: break;
  }
  return "chat";
}

const char *astime_notify_name(astime_notify n) {
  switch (n) {
  case ASTIME_NOTIFY_ON_CHANGE: return "on_change";
  case ASTIME_NOTIFY_NEVER: return "never";
  case ASTIME_NOTIFY_ALWAYS: break;
  }
  return "always";
}

const char *astime_provider_name(unsigned bit) {
  switch (bit) {
  case ASTIME_PROVIDER_LOCAL: return "local";
  case ASTIME_PROVIDER_CHATGPT: return "chatgpt";
  case ASTIME_PROVIDER_CODEX: return "codex";
  case ASTIME_PROVIDER_CLAUDE_CODE: return "claude-code";
  default: break;
  }
  return NULL;
}

const char *astime_status_name(astime_status s) {
  switch (s) {
  case ASTIME_STATUS_OK: return "ok";
  case ASTIME_STATUS_FAILED: return "failed";
  case ASTIME_STATUS_SKIPPED: return "skipped";
  case ASTIME_STATUS_DEFERRED: return "deferred";
  case ASTIME_STATUS_NONE: break;
  }
  return "none";
}

const char *astime_due_reason_name(astime_due_reason r) {
  switch (r) {
  case ASTIME_DUE_CATCH_UP: return "catch_up";
  case ASTIME_DUE_WATCH: return "watch";
  case ASTIME_DUE_SCHEDULE: break;
  }
  return "schedule";
}

/* ── hashes and signatures ────────────────────────────────────────────── */

astime_err astime_hash_file(const char *path, char out[ASTIME_HASH_MAX]) {
  uint8_t h[32];
  astime_err e;
  if (!path || !out) return ASTIME_ERR_INVALID;
  e = astime_from_core(core_sha256_file(path, h));
  if (e != ASTIME_OK) return e;
  core_sha256_hex(h, 32, out);
  return ASTIME_OK;
}

void astime_hash_bytes(const void *data, size_t len, char out[ASTIME_HASH_MAX]) {
  uint8_t h[32];
  if (!out) return;
  core_sha256(data ? data : "", data ? len : 0, h);
  core_sha256_hex(h, 32, out);
}

void astime__trigger_sig(const astime_routine *r, char out[ASTIME_SIG_MAX]) {
  const astime_trigger *t = &r->trigger;
  const char *tz = astime_tz_name(t->tz);
  switch (t->kind) {
  case ASTIME_TRIGGER_CRON:
    snprintf(out, ASTIME_SIG_MAX, "cron:%llx:%lx:%lx:%x:%x:%x:%s",
             (unsigned long long)t->cron.minute, (unsigned long)t->cron.hour,
             (unsigned long)t->cron.dom, (unsigned)t->cron.month, (unsigned)t->cron.dow,
             (unsigned)t->cron.flags, tz);
    break;
  case ASTIME_TRIGGER_EVERY:
    snprintf(out, ASTIME_SIG_MAX, "every:%lld:%s", (long long)t->every_s, tz);
    break;
  case ASTIME_TRIGGER_AT:
    snprintf(out, ASTIME_SIG_MAX, "at:%lld", (long long)t->at);
    break;
  case ASTIME_TRIGGER_NONE:
  default:
    snprintf(out, ASTIME_SIG_MAX, "none:%s", tz);
    break;
  }
}

void astime__watch_sig(const astime_routine *r, char out[ASTIME_SIG_MAX]) {
  char hex[65];
  if (r->watch_kind == ASTIME_WATCH_NONE || !r->watch_target) {
    out[0] = '\0';
    return;
  }
  astime_hash_bytes(r->watch_target, strlen(r->watch_target), hex);
  hex[32] = '\0';
  snprintf(out, ASTIME_SIG_MAX, "%s:%s", r->watch_kind == ASTIME_WATCH_URL ? "url" : "file", hex);
}

/* ── key locator ──────────────────────────────────────────────────────── */

/* A tolerant scan of xCDN text: strings and comments are skipped, braces
 * and brackets tracked, and each "key:" recorded with the keys of the
 * enclosing objects. The first key whose dotted path equals `path` wins. */

#define LOC_DEPTH 32
#define LOC_KEY 64

typedef struct {
  bool object;
  char parent[LOC_KEY]; /* key of the enclosing object holding this frame */
  char key[LOC_KEY];    /* last key read in this object                   */
} loc_frame;

static bool loc_path_equals(const loc_frame *fr, int depth, const char *key, const char *path) {
  char built[LOC_DEPTH * LOC_KEY];
  size_t n = 0;
  int i;
  built[0] = '\0';
  for (i = 0; i <= depth; i++) {
    if (!fr[i].object && i > 0) return false; /* keys inside arrays are not addressed */
    if (fr[i].parent[0]) n += (size_t)snprintf(built + n, sizeof built - n, "%s%s", n ? "." : "", fr[i].parent);
    if (n >= sizeof built) return false;
  }
  snprintf(built + n, sizeof built - n, "%s%s", n ? "." : "", key);
  return strcmp(built, path) == 0;
}

size_t astime__locate_line(const char *text, const char *path) {
  loc_frame fr[LOC_DEPTH];
  int depth = 0;
  size_t line = 1, i = 0;
  char pending[LOC_KEY];
  size_t pending_line = 0;
  bool have_pending = false;
  if (!text || !path || !*path) return 0;
  memset(fr, 0, sizeof fr);
  fr[0].object = true; /* the implicit top-level object */
  pending[0] = '\0';
  while (text[i]) {
    char ch = text[i];
    if (ch == '\n') {
      line++;
      i++;
      continue;
    }
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == ',') {
      i++;
      continue;
    }
    if (ch == '/' && text[i + 1] == '/') {
      while (text[i] && text[i] != '\n') i++;
      continue;
    }
    if (ch == '/' && text[i + 1] == '*') {
      i += 2;
      while (text[i] && !(text[i] == '*' && text[i + 1] == '/')) {
        if (text[i] == '\n') line++;
        i++;
      }
      if (text[i]) i += 2;
      continue;
    }
    if (ch == ':' && have_pending) {
      if (fr[depth].object) {
        snprintf(fr[depth].key, LOC_KEY, "%s", pending);
        if (loc_path_equals(fr, depth, pending, path)) return pending_line;
      }
      have_pending = false;
      i++;
      continue;
    }
    have_pending = false;
    if (ch == '{' || ch == '[') {
      if (depth + 1 < LOC_DEPTH) {
        depth++;
        fr[depth].object = ch == '{';
        snprintf(fr[depth].parent, LOC_KEY, "%s", fr[depth - 1].object ? fr[depth - 1].key : "");
        fr[depth].key[0] = '\0';
        /* A key belongs to one value: forget it once the value opens. */
        if (fr[depth - 1].object) fr[depth - 1].key[0] = '\0';
      }
      i++;
      continue;
    }
    if (ch == '}' || ch == ']') {
      if (depth > 0) depth--;
      i++;
      continue;
    }
    if (ch == '"') {
      size_t n = 0, start_line = line;
      i++;
      while (text[i] && text[i] != '"') {
        if (text[i] == '\\' && text[i + 1]) {
          i++;
        }
        if (text[i] == '\n') line++;
        if (n + 1 < LOC_KEY) pending[n++] = text[i];
        i++;
      }
      if (text[i]) i++;
      pending[n] = '\0';
      have_pending = true;
      pending_line = start_line;
      continue;
    }
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_') {
      size_t n = 0;
      while ((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z') ||
             (text[i] >= '0' && text[i] <= '9') || text[i] == '_' || text[i] == '-') {
        if (n + 1 < LOC_KEY) pending[n++] = text[i];
        i++;
      }
      pending[n] = '\0';
      if (text[i] == '"') continue; /* typed literal prefix: t"..", r".." */
      have_pending = true;
      pending_line = line;
      continue;
    }
    if (ch == '#' || ch == '@') { /* tags and annotations */
      i++;
      while ((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z') ||
             (text[i] >= '0' && text[i] <= '9') || text[i] == '_' || text[i] == '-')
        i++;
      continue;
    }
    i++;
  }
  return 0;
}
