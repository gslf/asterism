/*
 * util.c — growable buffer, strings, UTF-8, FNV-1a, Base64, token heuristic.
 */

#include "core.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

const char *core_err_name(core_err e) {
  switch (e) {
    case CORE_OK: return "CORE_OK";
    case CORE_ERR_IO: return "CORE_ERR_IO";
    case CORE_ERR_NOT_FOUND: return "CORE_ERR_NOT_FOUND";
    case CORE_ERR_INVALID: return "CORE_ERR_INVALID";
    case CORE_ERR_NOMEM: return "CORE_ERR_NOMEM";
    case CORE_ERR_BUSY: return "CORE_ERR_BUSY";
  }
  return "CORE_ERR_UNKNOWN";
}

/* ---- growable buffer ---------------------------------------------------- */

void core_buf_init(core_buf *b) {
  if (!b) return;
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

void core_buf_free(core_buf *b) {
  if (!b) return;
  free(b->data);
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

static core_err buf_reserve(core_buf *b, size_t extra) {
  size_t need, cap;
  char *p;
  if (extra > SIZE_MAX - b->len - 1) return CORE_ERR_NOMEM;
  need = b->len + extra + 1;
  if (need <= b->cap) return CORE_OK;
  cap = b->cap ? b->cap : 64;
  while (cap < need) {
    if (cap > SIZE_MAX / 2) {
      cap = need;
      break;
    }
    cap *= 2;
  }
  p = realloc(b->data, cap);
  if (!p) return CORE_ERR_NOMEM;
  b->data = p;
  b->cap = cap;
  return CORE_OK;
}

core_err core_buf_append(core_buf *b, const void *data, size_t len) {
  core_err e;
  if (!b || (!data && len > 0)) return CORE_ERR_INVALID;
  e = buf_reserve(b, len);
  if (e != CORE_OK) return e;
  if (len > 0) memcpy(b->data + b->len, data, len);
  b->len += len;
  b->data[b->len] = '\0';
  return CORE_OK;
}

core_err core_buf_appends(core_buf *b, const char *s) {
  if (!s) return CORE_ERR_INVALID;
  return core_buf_append(b, s, strlen(s));
}

core_err core_buf_appendc(core_buf *b, char ch) {
  return core_buf_append(b, &ch, 1);
}

core_err core_buf_vprintf(core_buf *b, const char *fmt, va_list ap) {
  va_list copy;
  int need;
  core_err e;
  if (!b || !fmt) return CORE_ERR_INVALID;
  va_copy(copy, ap);
  need = vsnprintf(NULL, 0, fmt, copy);
  va_end(copy);
  if (need < 0) return CORE_ERR_INVALID;
  e = buf_reserve(b, (size_t)need);
  if (e != CORE_OK) return e;
  vsnprintf(b->data + b->len, (size_t)need + 1, fmt, ap);
  b->len += (size_t)need;
  return CORE_OK;
}

core_err core_buf_printf(core_buf *b, const char *fmt, ...) {
  va_list ap;
  core_err e;
  va_start(ap, fmt);
  e = core_buf_vprintf(b, fmt, ap);
  va_end(ap);
  return e;
}

char *core_buf_detach(core_buf *b) {
  char *p;
  if (!b) return NULL;
  p = b->data;
  if (!p) {
    p = malloc(1);
    if (p) p[0] = '\0';
  }
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
  return p;
}

/* ---- strings ------------------------------------------------------------ */

char *core_strdup(const char *s) {
  size_t n;
  char *p;
  if (!s) return NULL;
  n = strlen(s);
  p = malloc(n + 1);
  if (!p) return NULL;
  memcpy(p, s, n + 1);
  return p;
}

char *core_strndup(const char *s, size_t n) {
  size_t len = 0;
  char *p;
  if (!s) return NULL;
  while (len < n && s[len] != '\0') len++;
  p = malloc(len + 1);
  if (!p) return NULL;
  memcpy(p, s, len);
  p[len] = '\0';
  return p;
}

int core_strcasecmp(const char *a, const char *b) {
  if (a == b) return 0;
  if (!a) return -1;
  if (!b) return 1;
  for (;; a++, b++) {
    unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
    if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
    if (ca != cb) return (int)ca - (int)cb;
    if (ca == '\0') return 0;
  }
}

bool core_str_has_prefix(const char *s, const char *prefix) {
  if (!s || !prefix) return false;
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool is_ascii_space(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' ||
         ch == '\f';
}

char *core_str_trim(char *s) {
  size_t len, start = 0;
  if (!s) return NULL;
  len = strlen(s);
  while (len > 0 && is_ascii_space(s[len - 1])) len--;
  while (start < len && is_ascii_space(s[start])) start++;
  if (start > 0) memmove(s, s + start, len - start);
  s[len - start] = '\0';
  return s;
}

bool core_str_blank(const char *s) {
  if (!s) return true;
  for (; *s; s++)
    if (!is_ascii_space(*s)) return false;
  return true;
}

bool core_slug_valid(const char *s) {
  size_t i;
  if (!s) return false;
  if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9')))
    return false;
  for (i = 1; s[i] != '\0'; i++) {
    char ch = s[i];
    if (i > 63) return false;
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-'))
      return false;
  }
  return true;
}

/* ---- UTF-8 validation ---------------------------------------------------
 * Strict: overlong encodings, UTF-16 surrogates (U+D800..U+DFFF) and code
 * points above U+10FFFF are rejected, as are truncated sequences. */

bool core_utf8_valid(const char *s, size_t len) {
  const unsigned char *p = (const unsigned char *)s;
  size_t i = 0;
  if (!s) return len == 0;
  while (i < len) {
    unsigned char b = p[i];
    if (b < 0x80) { /* ASCII */
      i += 1;
    } else if (b >= 0xC2 && b <= 0xDF) { /* U+0080..U+07FF */
      if (len - i < 2 || (p[i + 1] & 0xC0) != 0x80) return false;
      i += 2;
    } else if (b == 0xE0) { /* U+0800..U+0FFF (no overlongs) */
      if (len - i < 3 || p[i + 1] < 0xA0 || p[i + 1] > 0xBF ||
          (p[i + 2] & 0xC0) != 0x80)
        return false;
      i += 3;
    } else if (b >= 0xE1 && b <= 0xEC) { /* U+1000..U+CFFF */
      if (len - i < 3 || (p[i + 1] & 0xC0) != 0x80 ||
          (p[i + 2] & 0xC0) != 0x80)
        return false;
      i += 3;
    } else if (b == 0xED) { /* U+D000..U+D7FF (no surrogates) */
      if (len - i < 3 || p[i + 1] < 0x80 || p[i + 1] > 0x9F ||
          (p[i + 2] & 0xC0) != 0x80)
        return false;
      i += 3;
    } else if (b >= 0xEE && b <= 0xEF) { /* U+E000..U+FFFF */
      if (len - i < 3 || (p[i + 1] & 0xC0) != 0x80 ||
          (p[i + 2] & 0xC0) != 0x80)
        return false;
      i += 3;
    } else if (b == 0xF0) { /* U+10000..U+3FFFF (no overlongs) */
      if (len - i < 4 || p[i + 1] < 0x90 || p[i + 1] > 0xBF ||
          (p[i + 2] & 0xC0) != 0x80 || (p[i + 3] & 0xC0) != 0x80)
        return false;
      i += 4;
    } else if (b >= 0xF1 && b <= 0xF3) { /* U+40000..U+FFFFF */
      if (len - i < 4 || (p[i + 1] & 0xC0) != 0x80 ||
          (p[i + 2] & 0xC0) != 0x80 || (p[i + 3] & 0xC0) != 0x80)
        return false;
      i += 4;
    } else if (b == 0xF4) { /* U+100000..U+10FFFF (cap) */
      if (len - i < 4 || p[i + 1] < 0x80 || p[i + 1] > 0x8F ||
          (p[i + 2] & 0xC0) != 0x80 || (p[i + 3] & 0xC0) != 0x80)
        return false;
      i += 4;
    } else { /* 0x80..0xC1, 0xF5..0xFF: stray continuation / invalid lead */
      return false;
    }
  }
  return true;
}

bool core_utf8_count(const char *s, size_t *out_chars) {
  const unsigned char *p = (const unsigned char *)s;
  size_t n = 0;

  if (out_chars) *out_chars = 0;
  if (!s) return false;
  while (*p) {
    uint32_t cp;
    size_t need;
    if (*p < 0x80) {
      cp = *p;
      need = 1;
    } else if (*p >= 0xc2 && *p <= 0xdf) {
      cp = (uint32_t)(*p & 0x1f);
      need = 2;
    } else if (*p >= 0xe0 && *p <= 0xef) {
      cp = (uint32_t)(*p & 0x0f);
      need = 3;
    } else if (*p >= 0xf0 && *p <= 0xf4) {
      cp = (uint32_t)(*p & 0x07);
      need = 4;
    } else {
      return false;
    }
    for (size_t i = 1; i < need; i++) {
      if ((p[i] & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (uint32_t)(p[i] & 0x3f);
    }
    if ((need == 3 && cp < 0x800) || (need == 4 && cp < 0x10000) ||
        (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff)
      return false;
    p += need;
    n++;
  }
  if (out_chars) *out_chars = n;
  return true;
}

/* ---- FNV-1a 64 ---------------------------------------------------------- */

uint64_t core_fnv1a64(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint64_t h = UINT64_C(14695981039346656037);
  size_t i;
  if (!p) return h;
  for (i = 0; i < len; i++) {
    h ^= p[i];
    h *= UINT64_C(1099511628211);
  }
  return h;
}

/* ---- Base64 (RFC 4648, strict) ------------------------------------------ */

static const char b64_alpha[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *core_base64_encode(const uint8_t *data, size_t len) {
  size_t olen, i, o = 0;
  char *out;
  if (!data && len > 0) return NULL;
  if (len > (SIZE_MAX / 4) * 3 - 2) return NULL;
  olen = ((len + 2) / 3) * 4;
  out = malloc(olen + 1);
  if (!out) return NULL;
  for (i = 0; i + 3 <= len; i += 3) {
    uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) |
                 (uint32_t)data[i + 2];
    out[o++] = b64_alpha[(v >> 18) & 63];
    out[o++] = b64_alpha[(v >> 12) & 63];
    out[o++] = b64_alpha[(v >> 6) & 63];
    out[o++] = b64_alpha[v & 63];
  }
  if (len - i == 1) {
    uint32_t v = (uint32_t)data[i] << 16;
    out[o++] = b64_alpha[(v >> 18) & 63];
    out[o++] = b64_alpha[(v >> 12) & 63];
    out[o++] = '=';
    out[o++] = '=';
  } else if (len - i == 2) {
    uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
    out[o++] = b64_alpha[(v >> 18) & 63];
    out[o++] = b64_alpha[(v >> 12) & 63];
    out[o++] = b64_alpha[(v >> 6) & 63];
    out[o++] = '=';
  }
  out[o] = '\0';
  return out;
}

static int b64_val(char ch) {
  if (ch >= 'A' && ch <= 'Z') return ch - 'A';
  if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
  if (ch >= '0' && ch <= '9') return ch - '0' + 52;
  if (ch == '+') return 62;
  if (ch == '/') return 63;
  return -1;
}

bool core_base64_decode(const char *s, uint8_t **out, size_t *out_len) {
  size_t n, pad = 0, i, o = 0;
  uint8_t *buf;
  if (!s || !out || !out_len) return false;
  n = strlen(s);
  if (n % 4 != 0) return false;
  if (n == 0) {
    buf = malloc(1);
    if (!buf) return false;
    *out = buf;
    *out_len = 0;
    return true;
  }
  if (s[n - 1] == '=') {
    pad = 1;
    if (s[n - 2] == '=') pad = 2;
  }
  for (i = 0; i < n - pad; i++)
    if (b64_val(s[i]) < 0) return false; /* also rejects interior '=' */
  buf = malloc((n / 4) * 3);
  if (!buf) return false;
  for (i = 0; i < n; i += 4) {
    int a = b64_val(s[i]), b = b64_val(s[i + 1]);
    if (i + 4 < n || pad == 0) {
      int c3 = b64_val(s[i + 2]), c4 = b64_val(s[i + 3]);
      uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12) |
                   ((uint32_t)c3 << 6) | (uint32_t)c4;
      buf[o++] = (uint8_t)(v >> 16);
      buf[o++] = (uint8_t)(v >> 8);
      buf[o++] = (uint8_t)v;
    } else if (pad == 1) {
      int c3 = b64_val(s[i + 2]);
      uint32_t v;
      if (c3 & 0x03) { /* non-canonical trailing bits */
        free(buf);
        return false;
      }
      v = ((uint32_t)a << 18) | ((uint32_t)b << 12) | ((uint32_t)c3 << 6);
      buf[o++] = (uint8_t)(v >> 16);
      buf[o++] = (uint8_t)(v >> 8);
    } else { /* pad == 2 */
      if (b & 0x0f) {
        free(buf);
        return false;
      }
      buf[o++] = (uint8_t)((((uint32_t)a << 18) | ((uint32_t)b << 12)) >> 16);
    }
  }
  *out = buf;
  *out_len = o;
  return true;
}

int core_token_heuristic(const char *text) {
  size_t len, t;
  if (!text || text[0] == '\0') return 0;
  len = strlen(text);
  t = len / 4;
  if (t == 0) t = 1;
  if (t > (size_t)INT_MAX) t = (size_t)INT_MAX;
  return (int)t;
}
