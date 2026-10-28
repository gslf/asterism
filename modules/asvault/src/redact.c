/*
 * redact.c — stored values back to {{secret:NAME}} in tool output and
 * outbound messages.
 *
 * Two kinds of matchers per secret (values of at least ASVAULT_REDACT_MIN
 * bytes; a value equal to one of an earlier name is matched once, as that
 * name):
 *  - a decoding matcher that accepts, byte by byte, the value as-is or
 *    URL-encoded (%XX in either hex case, '+' for space) or JSON-escaped
 *    (\" \\ \/ \b \f \n \r \t and \uXXXX, surrogate pairs included), in any
 *    mix. It runs twice, preferring escapes and preferring raw bytes, so a
 *    value that itself contains "%25" or "\\" is still found;
 *  - literal base64 patterns, standard and URL-safe alphabets: the whole
 *    encoding padded and unpadded, plus, for each of the three byte
 *    alignments, the run of characters determined by the value alone, which
 *    finds it inside a longer encoding (HTTP Basic "user:password").
 * At every position the longest match is taken; matches that overlap merge
 * into one run, which becomes the placeholders of the secrets that extended
 * it (in order), so no byte of a matched value survives next to a
 * placeholder. A value nested inside a longer matched value adds no
 * placeholder of its own.
 *
 * Decrypted values and the patterns derived from them are zeroized before
 * return.
 */

#include "asvault_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *name;
  char       *value;
  size_t      len;
} rd_secret;

typedef struct {
  uint8_t *s;
  size_t   len;
  size_t   owner; /* index into the secret array */
} rd_lit;

typedef struct {
  rd_secret *sec;
  size_t     ns;
  rd_lit    *lit;
  size_t     nl, cap;
  size_t     first[257]; /* lit sorted by first byte: bucket offsets */
} rd_set;

static void rd_free(rd_set *s) {
  size_t i;
  for (i = 0; i < s->ns; i++) asvault_free_secret(s->sec[i].value);
  for (i = 0; i < s->nl; i++) {
    asvault_wipe(s->lit[i].s, s->lit[i].len);
    free(s->lit[i].s);
  }
  free(s->sec);
  free(s->lit);
  memset(s, 0, sizeof *s);
}

/* Copy a pattern in unless it is short or already present. */
static asvault_err rd_add(rd_set *s, const char *p, size_t len, size_t owner) {
  size_t i;
  uint8_t *copy;
  if (len < ASVAULT_REDACT_MIN) return ASVAULT_OK;
  for (i = 0; i < s->nl; i++)
    if (s->lit[i].len == len && memcmp(s->lit[i].s, p, len) == 0)
      return ASVAULT_OK;
  if (s->nl == s->cap) {
    size_t ncap = s->cap ? s->cap * 2 : 16;
    rd_lit *grown = (rd_lit *)realloc(s->lit, ncap * sizeof *grown);
    if (!grown) return ASVAULT_ERR_NOMEM;
    s->lit = grown;
    s->cap = ncap;
  }
  copy = (uint8_t *)malloc(len);
  if (!copy) return ASVAULT_ERR_NOMEM;
  memcpy(copy, p, len);
  s->lit[s->nl].s = copy;
  s->lit[s->nl].len = len;
  s->lit[s->nl].owner = owner;
  s->nl++;
  return ASVAULT_OK;
}

/* ---- base64 patterns ---------------------------------------------------- */

static const char B64_STD[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char B64_URL[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* Byte x of zeros(k) || val || zeros. */
static unsigned rd_byte(const uint8_t *val, size_t len, size_t k, size_t x) {
  return x < k || x - k >= len ? 0u : (unsigned)val[x - k];
}

static asvault_err rd_base64(rd_set *s, const uint8_t *val, size_t len,
                             size_t owner) {
  const char *alpha[2] = {B64_STD, B64_URL};
  size_t a, k, c, nchars, first, last;
  char *buf;
  asvault_err e = ASVAULT_OK;

  if (len > ((size_t)-1 - 16) / 2) return ASVAULT_ERR_NOMEM;
  nchars = ((len + 2) * 8 + 5) / 6;
  buf = (char *)malloc(nchars + 4);
  if (!buf) return ASVAULT_ERR_NOMEM;
  for (a = 0; a < 2 && e == ASVAULT_OK; a++) {
    for (k = 0; k < 3 && e == ASVAULT_OK; k++) {
      size_t total = k + len;
      size_t n = (total * 8 + 5) / 6;
      first = (size_t)-1;
      last = 0;
      for (c = 0; c < n; c++) {
        size_t bit = 6 * c, b = bit / 8;
        unsigned w = (rd_byte(val, len, k, b) << 8) |
                     rd_byte(val, len, k, b + 1);
        buf[c] = alpha[a][(w >> (10 - bit % 8)) & 63u];
        /* determined by the value alone: both source bytes inside it */
        if (b >= k && (bit + 5) / 8 < total) {
          if (first == (size_t)-1) first = c;
          last = c;
        }
      }
      if (first != (size_t)-1)
        e = rd_add(s, buf + first, last - first + 1, owner);
      if (e == ASVAULT_OK && k == 0) {
        e = rd_add(s, buf, n, owner); /* whole, unpadded */
        while (e == ASVAULT_OK && n % 4) buf[n++] = '=';
        if (e == ASVAULT_OK) e = rd_add(s, buf, n, owner); /* padded */
      }
    }
  }
  asvault_wipe(buf, nchars + 4);
  free(buf);
  return e;
}

static int lit_cmp(const void *a, const void *b) {
  const rd_lit *x = (const rd_lit *)a, *y = (const rd_lit *)b;
  if (x->s[0] != y->s[0]) return x->s[0] < y->s[0] ? -1 : 1;
  if (x->len != y->len) return x->len > y->len ? -1 : 1;
  return 0;
}

static int sec_cmp(const void *a, const void *b) {
  return strcmp(((const rd_secret *)a)->name, ((const rd_secret *)b)->name);
}

/* Decrypt every value and derive the patterns. */
static asvault_err rd_build(asvault *v, const asvault_doc *d, rd_set *s) {
  size_t i, j, kept = 0;
  asvault_err e;

  memset(s, 0, sizeof *s);
  s->sec = (rd_secret *)calloc(d->n ? d->n : 1, sizeof *s->sec);
  if (!s->sec) return asvault_seterr(v, ASVAULT_ERR_NOMEM, "out of memory");
  for (i = 0; i < d->n; i++) {
    rd_secret *x = &s->sec[s->ns];
    x->name = d->recs[i].name;
    e = asvault_rec_open(v, &d->recs[i], &x->value, &x->len);
    if (e != ASVAULT_OK) return e;
    s->ns++;
  }
  qsort(s->sec, s->ns, sizeof *s->sec, sec_cmp);
  /* drop short values and repeats of an earlier name's value */
  for (i = 0; i < s->ns; i++) {
    bool drop = s->sec[i].len < ASVAULT_REDACT_MIN;
    for (j = 0; !drop && j < kept; j++)
      drop = s->sec[j].len == s->sec[i].len &&
             memcmp(s->sec[j].value, s->sec[i].value, s->sec[i].len) == 0;
    if (drop) {
      asvault_free_secret(s->sec[i].value);
      s->sec[i].value = NULL;
      continue;
    }
    s->sec[kept++] = s->sec[i];
  }
  s->ns = kept;
  for (i = 0; i < s->ns; i++) {
    e = rd_base64(s, (const uint8_t *)s->sec[i].value, s->sec[i].len, i);
    if (e != ASVAULT_OK)
      return asvault_seterr(v, ASVAULT_ERR_NOMEM, "out of memory");
  }
  if (s->nl > 1) qsort(s->lit, s->nl, sizeof *s->lit, lit_cmp);
  for (i = 0, j = 0; i < 257; i++) {
    while (j < s->nl && s->lit[j].s[0] < i) j++;
    s->first[i] = j;
  }
  return ASVAULT_OK;
}

/* ---- decoding matcher --------------------------------------------------- */

static int hexval(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

static long hex4(const char *t, size_t tn, size_t p) {
  long v = 0;
  size_t i;
  if (p + 4 > tn) return -1;
  for (i = 0; i < 4; i++) {
    int h = hexval(t[p + i]);
    if (h < 0) return -1;
    v = v * 16 + h;
  }
  return v;
}

static size_t utf8_put(unsigned long cp, uint8_t out[4]) {
  if (cp < 0x80) {
    out[0] = (uint8_t)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (uint8_t)(0xC0 | (cp >> 6));
    out[1] = (uint8_t)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (uint8_t)(0xE0 | (cp >> 12));
    out[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (uint8_t)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (uint8_t)(0xF0 | (cp >> 18));
  out[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (uint8_t)(0x80 | (cp & 0x3F));
  return 4;
}

/* An escape at t[p] decoding to bytes that continue val at j: the number of
 * text bytes it spans and the value bytes it covers, or 0. */
static size_t escape_at(const char *t, size_t tn, size_t p, const uint8_t *val,
                        size_t vlen, size_t j, size_t *covered) {
  uint8_t dec[4];
  size_t span = 0, m = 0;
  if (t[p] == '%' && p + 2 < tn) {
    int h = hexval(t[p + 1]), l = hexval(t[p + 2]);
    if (h >= 0 && l >= 0) {
      dec[0] = (uint8_t)(h * 16 + l);
      span = 3;
      m = 1;
    }
  } else if (t[p] == '+') {
    dec[0] = ' ';
    span = 1;
    m = 1;
  } else if (t[p] == '\\' && p + 1 < tn) {
    char ch = t[p + 1];
    const char *from = "\"\\/bfnrt", *to = "\"\\/\b\f\n\r\t";
    const char *hit = ch ? strchr(from, ch) : NULL;
    if (hit) {
      dec[0] = (uint8_t)to[hit - from];
      span = 2;
      m = 1;
    } else if (ch == 'u') {
      long cp = hex4(t, tn, p + 2);
      span = 6;
      if (cp >= 0xD800 && cp <= 0xDBFF) { /* surrogate pair */
        long lo = -1;
        if (p + 7 < tn && t[p + 6] == '\\' && t[p + 7] == 'u')
          lo = hex4(t, tn, p + 8);
        cp = lo >= 0xDC00 && lo <= 0xDFFF
                 ? 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00)
                 : -1;
        span = 12;
      } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
        cp = -1;
      }
      if (cp >= 0) m = utf8_put((unsigned long)cp, dec);
    }
  }
  if (m == 0 || j + m > vlen || memcmp(dec, val + j, m) != 0) return 0;
  *covered = m;
  return span;
}

/* Text bytes matched at i for the whole value, or 0. */
static size_t decode_match(const char *t, size_t tn, size_t i,
                           const uint8_t *val, size_t vlen, int prefer_raw) {
  size_t p = i, j = 0, covered, span;
  while (j < vlen) {
    if (p >= tn) return 0;
    if (prefer_raw && (uint8_t)t[p] == val[j]) {
      p++;
      j++;
      continue;
    }
    span = escape_at(t, tn, p, val, vlen, j, &covered);
    if (span) {
      p += span;
      j += covered;
      continue;
    }
    if ((uint8_t)t[p] != val[j]) return 0;
    p++;
    j++;
  }
  return p - i;
}

/* ---- sweep -------------------------------------------------------------- */

typedef struct {
  core_buf    out;
  const char *t;
  size_t      copy_from;
  int         active;
  size_t      rs, re;
  size_t     *own;
  size_t      nown;
} rd_run;

/* The longest match starting at `start`: opens a run, or extends the
 * current one (a match nested inside it adds nothing). */
static void run_note(rd_run *r, size_t start, size_t end, size_t owner) {
  size_t i;
  if (!r->active) {
    r->active = 1;
    r->rs = start;
    r->re = end;
    r->nown = 0;
  } else if (end > r->re) {
    r->re = end;
  } else {
    return;
  }
  for (i = 0; i < r->nown; i++)
    if (r->own[i] == owner) return;
  r->own[r->nown++] = owner;
}

static core_err run_flush(rd_run *r, const rd_set *s) {
  core_err ce = core_buf_append(&r->out, r->t + r->copy_from,
                                r->rs - r->copy_from);
  size_t i;
  for (i = 0; ce == CORE_OK && i < r->nown; i++)
    ce = core_buf_printf(&r->out, "{{secret:%s}}", s->sec[r->own[i]].name);
  r->copy_from = r->re;
  r->active = 0;
  r->nown = 0;
  return ce;
}

static asvault_err rd_apply(asvault *v, const rd_set *s, const char *t,
                            char **out_text) {
  size_t tn = strlen(t), i, k, m, x, best, best_owner;
  rd_run r;
  core_err ce = CORE_OK;

  memset(&r, 0, sizeof r);
  core_buf_init(&r.out);
  r.t = t;
  r.own = (size_t *)calloc(s->ns ? s->ns : 1, sizeof *r.own);
  if (!r.own) return asvault_seterr(v, ASVAULT_ERR_NOMEM, "out of memory");
  for (i = 0; ce == CORE_OK && i <= tn; i++) {
    uint8_t b;
    if (r.active && i >= r.re) ce = run_flush(&r, s);
    if (ce != CORE_OK || i == tn) break;
    b = (uint8_t)t[i];
    best = 0;
    best_owner = 0;
    for (k = s->first[b]; k < s->first[b + 1]; k++) {
      const rd_lit *l = &s->lit[k];
      if (l->len > best && l->len <= tn - i &&
          memcmp(t + i, l->s, l->len) == 0) {
        best = l->len;
        best_owner = l->owner;
      }
    }
    for (x = 0; x < s->ns; x++) {
      const uint8_t *val = (const uint8_t *)s->sec[x].value;
      if (b != val[0] && b != '%' && b != '\\' && b != '+') continue;
      m = decode_match(t, tn, i, val, s->sec[x].len, 0);
      if (m > best) {
        best = m;
        best_owner = x;
      }
      m = decode_match(t, tn, i, val, s->sec[x].len, 1);
      if (m > best) {
        best = m;
        best_owner = x;
      }
    }
    if (best) run_note(&r, i, i + best, best_owner);
  }
  if (ce == CORE_OK)
    ce = core_buf_append(&r.out, t + r.copy_from, tn - r.copy_from);
  free(r.own);
  if (ce != CORE_OK) {
    core_buf_free(&r.out);
    return asvault_seterr(v, ASVAULT_ERR_NOMEM, "out of memory");
  }
  *out_text = core_buf_detach(&r.out);
  if (!*out_text) return asvault_seterr(v, ASVAULT_ERR_NOMEM, "out of memory");
  return ASVAULT_OK;
}

static asvault_err redact_locked(asvault *v, const char *text,
                                 char **out_text) {
  asvault_doc d;
  rd_set s;
  asvault_err e;

  e = asvault_doc_load(v, &d);
  if (e != ASVAULT_OK) return e;
  memset(&s, 0, sizeof s);
  if (d.n > 0) {
    e = asvault_key_unlock(v, &d);
    if (e == ASVAULT_OK) e = rd_build(v, &d, &s);
  } else {
    s.sec = (rd_secret *)calloc(1, sizeof *s.sec); /* nothing to match */
    if (!s.sec) e = asvault_seterr(v, ASVAULT_ERR_NOMEM, "out of memory");
  }
  if (e == ASVAULT_OK) e = rd_apply(v, &s, text, out_text);
  rd_free(&s);
  asvault_doc_free(&d);
  return e;
}

asvault_err asvault_redact(asvault *v, const char *text, char **out_text) {
  asvault_err e;
  if (out_text) *out_text = NULL;
  if (!v || !text || !out_text) return ASVAULT_ERR_INVALID;
  os_mutex_lock(&v->mu);
  e = redact_locked(v, text, out_text);
  os_mutex_unlock(&v->mu);
  return e;
}
