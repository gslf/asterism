/*
 * text.c — UTF-8 / UTF-16 counting, HTML escaping and message splitting.
 *
 * The Bot API states message limits in "characters" and entity offsets in
 * UTF-16 code units. Counting UTF-16 units is the safe reading: it is never
 * smaller than the code point count, so a part that fits here fits there.
 */

#include "aslink_internal.h"

#include <stdlib.h>
#include <string.h>

/* Length of the strict UTF-8 sequence at p (n bytes available) and its code
 * point; 0 when invalid (overlong, surrogate, above U+10FFFF, truncated). */
static size_t utf8_decode(const unsigned char *p, size_t n, uint32_t *cp) {
  uint32_t c;
  size_t len, i;
  if (n == 0) return 0;
  if (p[0] < 0x80) {
    *cp = p[0];
    return 1;
  }
  if (p[0] >= 0xC2 && p[0] <= 0xDF) {
    len = 2; c = p[0] & 0x1Fu;
  } else if (p[0] >= 0xE0 && p[0] <= 0xEF) {
    len = 3; c = p[0] & 0x0Fu;
  } else if (p[0] >= 0xF0 && p[0] <= 0xF4) {
    len = 4; c = p[0] & 0x07u;
  } else {
    return 0;
  }
  if (n < len) return 0;
  for (i = 1; i < len; i++) {
    if ((p[i] & 0xC0) != 0x80) return 0;
    c = (c << 6) | (p[i] & 0x3Fu);
  }
  if ((len == 3 && c < 0x800) || (len == 4 && (c < 0x10000 || c > 0x10FFFF)) ||
      (c >= 0xD800 && c <= 0xDFFF))
    return 0;
  *cp = c;
  return len;
}

int aslink_utf16_units(const char *s, size_t len, size_t *out_units) {
  const unsigned char *p = (const unsigned char *)s;
  size_t i = 0, units = 0;
  if (!s && len) return 0;
  while (i < len) {
    uint32_t cp;
    size_t step = utf8_decode(p + i, len - i, &cp);
    if (!step) return 0;
    units += cp >= 0x10000 ? 2 : 1;
    i += step;
  }
  if (out_units) *out_units = units;
  return 1;
}

size_t aslink_utf8_prefix(const char *s, size_t len, size_t cap) {
  size_t n;
  if (!s || cap == 0) return 0;
  if (len < cap) return len;
  n = cap - 1;
  while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
  return n;
}

/* Display strings end up in logs, prompts and chat lines: control
 * characters become spaces and bidirectional overrides (U+202A-U+202E,
 * U+2066-U+2069, the "invoice\u202Efdp.exe" trick) become U+FFFD. Every
 * replacement keeps the byte length, so the string stays valid UTF-8. */
static void neutralise(char *s, size_t n) {
  unsigned char *p = (unsigned char *)s;
  size_t i = 0;
  while (i < n) {
    if (p[i] < 0x20 || p[i] == 0x7F) {
      p[i++] = ' ';
    } else if (p[i] == 0xC2 && i + 1 < n && p[i + 1] >= 0x80 && p[i + 1] <= 0x9F) {
      p[i] = p[i + 1] = ' '; /* C1 controls */
      i += 2;
    } else if (p[i] == 0xE2 && i + 2 < n &&
               ((p[i + 1] == 0x80 && p[i + 2] >= 0xAA && p[i + 2] <= 0xAE) ||
                (p[i + 1] == 0x81 && p[i + 2] >= 0xA6 && p[i + 2] <= 0xA9))) {
      p[i] = 0xEF;
      p[i + 1] = 0xBF;
      p[i + 2] = 0xBD;
      i += 3;
    } else {
      i++;
    }
  }
}

void aslink_copy_display(char *dst, size_t cap, const char *src, size_t len) {
  size_t n;
  if (!dst || cap == 0) return;
  dst[0] = 0;
  if (!src || memchr(src, 0, len) || !core_utf8_valid(src, len)) return;
  n = aslink_utf8_prefix(src, len, cap);
  memcpy(dst, src, n);
  dst[n] = 0;
  neutralise(dst, n);
}

char *aslink_html_escape(const char *utf8) {
  core_buf b;
  const char *p;
  if (!utf8 || !core_utf8_valid(utf8, strlen(utf8))) return NULL;
  core_buf_init(&b);
  if (core_buf_appends(&b, "") != CORE_OK) return NULL;
  for (p = utf8; *p; p++) {
    const char *rep = NULL;
    core_err e;
    switch (*p) {
      case '&': rep = "&amp;"; break;
      case '<': rep = "&lt;"; break;
      case '>': rep = "&gt;"; break;
      case '"': rep = "&quot;"; break;
      default: break;
    }
    e = rep ? core_buf_appends(&b, rep) : core_buf_appendc(&b, *p);
    if (e != CORE_OK) {
      core_buf_free(&b);
      return NULL;
    }
  }
  return core_buf_detach(&b);
}

/* ── splitting ────────────────────────────────────────────────────────── */

typedef struct {
  char **v;
  size_t n, cap;
} parts;

static int parts_push(parts *ps, const char *s, size_t len) {
  char *copy;
  if (ps->n == ps->cap) {
    size_t cap = ps->cap ? ps->cap * 2 : 4;
    char **v = (char **)realloc(ps->v, cap * sizeof *v);
    if (!v) return 0;
    ps->v = v;
    ps->cap = cap;
  }
  copy = core_strndup(s, len);
  if (!copy) return 0;
  ps->v[ps->n++] = copy;
  return 1;
}

static int trailing_space(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

/* Pick where the part [p, q) should end, q < len being the first byte that
 * does not fit. Returns the part end; *next is where the following part
 * starts (after the dropped separator). */
static size_t pick_break(const char *s, size_t len, size_t p, size_t q,
                         size_t *next) {
  /* Candidates are separator starts i in (p, q]: the part is [p, i). */
  size_t para = 0, line = 0, space = 0, half = p + (q - p) / 2, i;
  for (i = q; i > p; i--) {
    if (!para && s[i] == '\n' && i + 1 < len && s[i + 1] == '\n') para = i;
    if (!line && s[i] == '\n') line = i;
    if (!space && s[i] == ' ') space = i;
    if (para && line && space) break;
  }
  if (para && para >= half) { *next = para + 2; return para; }
  if (line && line >= half) { *next = line + 1; return line; }
  if (space && space >= half) { *next = space + 1; return space; }
  /* Nothing in the second half: the latest boundary of any kind. */
  if (line >= space && line) { *next = line + 1; return line; }
  if (space) { *next = space + 1; return space; }
  *next = q;
  return q;
}

aslink_err aslink_split_text(const char *utf8, size_t limit_chars,
                             char ***out_parts, size_t *out_n) {
  parts ps = {NULL, 0, 0};
  const unsigned char *u = (const unsigned char *)utf8;
  size_t len, p = 0;
  if (out_parts) *out_parts = NULL;
  if (out_n) *out_n = 0;
  if (!utf8 || !out_parts || !out_n) return ASLINK_ERR_INVALID;
  if (limit_chars == 0) limit_chars = ASLINK_MESSAGE_MAX;
  if (limit_chars < 2) return ASLINK_ERR_INVALID;
  len = strlen(utf8);
  if (!core_utf8_valid(utf8, len)) return ASLINK_ERR_INVALID;
  while (p < len) {
    size_t q, units = 0, end, next;
    while (p < len && (utf8[p] == '\n' || utf8[p] == '\r')) p++;
    if (p >= len) break;
    for (q = p; q < len;) {
      uint32_t cp;
      size_t step = utf8_decode(u + q, len - q, &cp);
      size_t w = cp >= 0x10000 ? 2 : 1;
      if (!step || units + w > limit_chars) break;
      units += w;
      q += step;
    }
    if (q >= len) {
      end = next = len;
    } else {
      end = pick_break(utf8, len, p, q, &next);
    }
    while (end > p && trailing_space(utf8[end - 1])) end--;
    if (end > p && !parts_push(&ps, utf8 + p, end - p)) {
      aslink_strings_free(ps.v, ps.n);
      return ASLINK_ERR_NOMEM;
    }
    p = next;
  }
  *out_parts = ps.v;
  *out_n = ps.n;
  return ASLINK_OK;
}

void aslink_strings_free(char **s, size_t n) {
  size_t i;
  if (!s) return;
  for (i = 0; i < n; i++) free(s[i]);
  free(s);
}
