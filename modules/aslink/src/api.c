/*
 * api.c — error names, status messages with token redaction, and the
 * Telegram context (open, close, cancel).
 *
 * The bot token is a bearer secret embedded in every request path
 * (/bot<token>/METHOD). Every message that reaches a caller goes through
 * aslink_redact, which removes the literal token and any path segment
 * shaped like one, before it is truncated to the status buffer: a token cut
 * in half by truncation would otherwise slip past the match.
 */

#include "aslink_internal.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static const char *const err_names[] = {
  "ASLINK_OK",
  "ASLINK_ERR_INVALID",
  "ASLINK_ERR_NOMEM",
  "ASLINK_ERR_IO",
  "ASLINK_ERR_PROTOCOL",
  "ASLINK_ERR_UNAUTHORIZED",
  "ASLINK_ERR_RATE_LIMITED",
  "ASLINK_ERR_NOT_FOUND",
  "ASLINK_ERR_TOO_LARGE",
  "ASLINK_ERR_TIMEOUT",
  "ASLINK_ERR_CANCELLED",
  "ASLINK_ERR_UNSUPPORTED",
};

const char *aslink_err_name(aslink_err e) {
  if ((int)e >= 0 && (size_t)e < sizeof err_names / sizeof err_names[0])
    return err_names[e];
  return "ASLINK_ERR_UNKNOWN";
}

aslink_err aslink_from_core(core_err e) {
  switch (e) {
    case CORE_OK: return ASLINK_OK;
    case CORE_ERR_NOT_FOUND: return ASLINK_ERR_NOT_FOUND;
    case CORE_ERR_INVALID: return ASLINK_ERR_INVALID;
    case CORE_ERR_NOMEM: return ASLINK_ERR_NOMEM;
    case CORE_ERR_BUSY:
    case CORE_ERR_IO: break;
  }
  return ASLINK_ERR_IO;
}

/* ── status and redaction ─────────────────────────────────────────────── */

void aslink_status_reset(aslink_status *st) {
  if (st) memset(st, 0, sizeof *st);
}

static int token_char(unsigned char ch) {
  return isalnum(ch) || ch == '_' || ch == '-';
}

/* Length of a "bot<digits>:<token chars>" match at s, else 0. */
static size_t bot_segment(const char *s) {
  size_t i = 3, digits, secret;
  if (strncmp(s, "bot", 3) != 0) return 0;
  for (digits = 0; isdigit((unsigned char)s[i]); i++) digits++;
  if (!digits || s[i] != ':') return 0;
  i++;
  for (secret = 0; token_char((unsigned char)s[i]); i++) secret++;
  return secret ? i : 0;
}

/* Redacted copy of s in a fresh core_buf; 0 on allocation failure. */
static int redact_into(core_buf *b, const char *s, const char *token) {
  size_t tlen = token ? strlen(token) : 0;
  while (*s) {
    size_t seg;
    if (tlen && strncmp(s, token, tlen) == 0) {
      if (core_buf_appends(b, "<redacted>") != CORE_OK) return 0;
      s += tlen;
    } else if ((seg = bot_segment(s)) != 0) {
      if (core_buf_appends(b, "bot<redacted>") != CORE_OK) return 0;
      s += seg;
    } else {
      if (core_buf_appendc(b, *s) != CORE_OK) return 0;
      s++;
    }
  }
  return core_buf_appends(b, "") == CORE_OK;
}

/* Copy src into dst (cap bytes) on a code point boundary; invalid UTF-8
 * bytes become '?' so the message stays printable. */
static void copy_message(char *dst, size_t cap, const char *src) {
  size_t i, n = strlen(src);
  char *tmp = (char *)malloc(n + 1);
  if (!tmp) {
    dst[0] = 0;
    return;
  }
  memcpy(tmp, src, n + 1);
  for (i = 0; i < n;) {
    size_t step = 1, units;
    unsigned char c0 = (unsigned char)tmp[i];
    if (c0 >= 0x80) {
      step = c0 >= 0xF0 ? 4 : c0 >= 0xE0 ? 3 : c0 >= 0xC0 ? 2 : 1;
      if (i + step > n || !aslink_utf16_units(tmp + i, step, &units)) {
        tmp[i] = '?';
        step = 1;
      }
    } else if (c0 < 0x20 && c0 != '\t') {
      tmp[i] = ' ';
    }
    i += step;
  }
  n = aslink_utf8_prefix(tmp, n, cap);
  memcpy(dst, tmp, n);
  dst[n] = 0;
  free(tmp);
}

void aslink_redact(char *msg, size_t cap, const char *token) {
  core_buf b;
  if (!msg || !cap) return;
  core_buf_init(&b);
  if (redact_into(&b, msg, token)) {
    copy_message(msg, cap, b.data ? b.data : "");
  } else {
    /* Out of memory: drop the message rather than risk the secret. */
    msg[0] = 0;
  }
  core_buf_free(&b);
}

void aslink_status_msg(aslink_status *st, const char *token, const char *fmt, ...) {
  va_list ap;
  core_buf raw, clean;
  if (!st) return;
  core_buf_init(&raw);
  core_buf_init(&clean);
  va_start(ap, fmt);
  if (core_buf_vprintf(&raw, fmt, ap) == CORE_OK &&
      redact_into(&clean, raw.data ? raw.data : "", token)) {
    copy_message(st->message, sizeof st->message, clean.data ? clean.data : "");
  } else {
    st->message[0] = 0;
  }
  va_end(ap);
  core_buf_free(&raw);
  core_buf_free(&clean);
}

/* ── context ──────────────────────────────────────────────────────────── */

static int token_valid(const char *t) {
  size_t i = 0, digits = 0, secret = 0;
  if (!t) return 0;
  for (; isdigit((unsigned char)t[i]); i++) digits++;
  if (digits == 0 || digits > 20 || t[i] != ':') return 0;
  for (i++; t[i]; i++) {
    if (!token_char((unsigned char)t[i])) return 0;
    secret++;
  }
  return secret > 0 && secret <= 256;
}

/* https://<anything>, or http:// to a loopback host. No userinfo, query,
 * fragment, whitespace or control characters. */
static int base_valid(const char *b) {
  const char *host;
  size_t i, hlen;
  if (!b || strlen(b) > 512) return 0;
  for (i = 0; b[i]; i++) {
    unsigned char ch = (unsigned char)b[i];
    if (ch <= 0x20 || ch >= 0x7F || ch == '?' || ch == '#' || ch == '@' ||
        ch == '\\' || ch == '"')
      return 0;
  }
  if (strncmp(b, "https://", 8) == 0) {
    host = b + 8;
  } else if (strncmp(b, "http://", 7) == 0) {
    host = b + 7;
  } else {
    return 0;
  }
  if (host[0] == '[') {
    const char *end = strchr(host, ']');
    if (!end) return 0;
    hlen = (size_t)(end - host) + 1;
  } else {
    hlen = strcspn(host, ":/");
  }
  if (hlen == 0) return 0;
  if (b[4] == ':') {
    /* plain http: loopback only, the token must not cross a network. */
    char name[10] = {0};
    if (hlen < sizeof name) {
      for (i = 0; i < hlen; i++) name[i] = (char)tolower((unsigned char)host[i]);
    }
    if (strcmp(name, "127.0.0.1") != 0 && strcmp(name, "localhost") != 0 &&
        strcmp(name, "[::1]") != 0)
      return 0;
  }
  return 1;
}

aslink_err aslink_telegram_open(const aslink_telegram_params *p, aslink_ctx **out) {
  aslink_ctx *c;
  const char *base;
  size_t n;
  if (!out) return ASLINK_ERR_INVALID;
  *out = NULL;
  if (!p || !token_valid(p->token) || p->connect_timeout_ms < 0 ||
      p->request_timeout_ms < 0 || p->transfer_timeout_ms < 0)
    return ASLINK_ERR_INVALID;
  base = p->api_base ? p->api_base : ASLINK_DEFAULT_BASE;
  if (!base_valid(base)) return ASLINK_ERR_INVALID;
  c = (aslink_ctx *)calloc(1, sizeof *c);
  if (!c) return ASLINK_ERR_NOMEM;
  c->base = core_strdup(base);
  c->token = core_strdup(p->token);
  if (!c->base || !c->token) {
    aslink_close(c);
    return ASLINK_ERR_NOMEM;
  }
  n = strlen(c->base);
  while (n > 0 && c->base[n - 1] == '/') c->base[--n] = 0;
  c->connect_timeout_ms = p->connect_timeout_ms ? p->connect_timeout_ms : 10000;
  c->request_timeout_ms = p->request_timeout_ms ? p->request_timeout_ms : 30000;
  c->transfer_timeout_ms = p->transfer_timeout_ms ? p->transfer_timeout_ms : 600000;
  *out = c;
  return ASLINK_OK;
}

void aslink_close(aslink_ctx *c) {
  if (!c) return;
  if (c->token) {
    /* Best effort: do not leave the secret in freed heap memory. */
    volatile char *t = c->token;
    while (*t) *t++ = 0;
  }
  free(c->token);
  free(c->base);
  free(c);
}

void aslink_cancel(aslink_ctx *c) {
  if (c) ASLINK_FLAG_SET(&c->cancelled);
}
