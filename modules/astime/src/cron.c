/*
 * cron.c — five-field cron expressions and their fire times.
 *
 * Parsing builds one bit mask per field. Searching walks local calendar
 * days (bounded by ASTIME_CRON_HORIZON_DAYS) and, on matching days, the
 * hour/minute masks in order. Each wall time maps to its first instant, so
 * a repeated wall time (fall back) fires once and a missing one (spring
 * forward) is skipped. That mapping is monotonic, so the first wall time
 * whose instant passes the reference is the answer.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "astime_internal.h"

/* ── parsing ──────────────────────────────────────────────────────────── */

static const char *const MONTH_NAMES[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                          "jul", "aug", "sep", "oct", "nov", "dec"};
static const char *const DAY_NAMES[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};

typedef struct {
  const char *label;
  int lo, hi;                /* accepted values (day-of-week: 0..7)  */
  int star_hi;               /* upper bound of "*"                   */
  const char *const *names;  /* NULL: numbers only                   */
  int names_n, names_base;
} field_spec;

static const field_spec FIELDS[5] = {
    {"minute", 0, 59, 59, NULL, 0, 0},
    {"hour", 0, 23, 23, NULL, 0, 0},
    {"day-of-month", 1, 31, 31, NULL, 0, 0},
    {"month", 1, 12, 12, MONTH_NAMES, 12, 1},
    {"day-of-week", 0, 7, 6, DAY_NAMES, 7, 0},
};

typedef struct {
  const char *s;
  size_t len, pos;
  const field_spec *f;
  char *err;
} field_cur;

static int cur_peek(const field_cur *c) {
  return c->pos < c->len ? (unsigned char)c->s[c->pos] : -1;
}

static bool field_err(field_cur *c, const char *fmt, ...) {
  char msg[ASTIME_ERR_SIZE];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  astime__seterr(c->err, "%s: %s", c->f->label, msg);
  return false;
}

/* A number or (month/day-of-week) a three-letter name. */
static bool parse_value(field_cur *c, int *out) {
  int ch = cur_peek(c);
  if (ch >= '0' && ch <= '9') {
    int v = 0, nd = 0;
    while ((ch = cur_peek(c)) >= '0' && ch <= '9') {
      if (++nd > 3) return field_err(c, "number too long");
      v = v * 10 + (ch - '0');
      c->pos++;
    }
    if (v < c->f->lo || v > c->f->hi)
      return field_err(c, "value %d out of range %d-%d", v, c->f->lo, c->f->hi);
    *out = v;
    return true;
  }
  if (isalpha(ch)) {
    char word[16];
    size_t n = 0;
    int i;
    while ((ch = cur_peek(c)) >= 0 && isalpha(ch)) {
      if (n + 1 < sizeof word) word[n++] = (char)tolower(ch);
      c->pos++;
    }
    word[n] = '\0';
    if (!c->f->names)
      return field_err(c, "names are not allowed here (\"%s\")", word);
    for (i = 0; i < c->f->names_n; i++)
      if (strcmp(word, c->f->names[i]) == 0) {
        *out = i + c->f->names_base;
        return true;
      }
    return field_err(c, "unknown name \"%s\"", word);
  }
  if (ch < 0) return field_err(c, "missing value");
  return field_err(c, "unexpected character '%c'", (char)ch);
}

static bool parse_item(field_cur *c, uint64_t *mask) {
  const field_spec *f = c->f;
  int lo = 0, hi = 0, step = 1, v;
  bool single = false;
  if (cur_peek(c) == '*') {
    c->pos++;
    lo = f->lo;
    hi = f->star_hi;
  } else {
    if (!parse_value(c, &lo)) return false;
    hi = lo;
    single = true;
    if (cur_peek(c) == '-') {
      c->pos++;
      if (!parse_value(c, &hi)) return false;
      single = false;
      /* "fri-sun": a day-of-week range may end on Sunday written as 0. */
      if (f->names == DAY_NAMES && hi == 0 && lo > 0) hi = 7;
      if (lo > hi) return field_err(c, "range %d-%d is reversed", lo, hi);
    }
  }
  if (cur_peek(c) == '/') {
    int nd = 0;
    c->pos++;
    step = 0;
    while (cur_peek(c) >= '0' && cur_peek(c) <= '9') {
      if (++nd > 3) return field_err(c, "step too long");
      step = step * 10 + (cur_peek(c) - '0');
      c->pos++;
    }
    if (nd == 0) return field_err(c, "missing step after '/'");
    if (step < 1 || step > f->hi)
      return field_err(c, "step %d out of range 1-%d", step, f->hi);
    if (single) hi = f->star_hi; /* "a/n" means a-max/n */
  }
  if (cur_peek(c) >= 0 && cur_peek(c) != ',')
    return field_err(c, "unexpected character '%c'", (char)cur_peek(c));
  for (v = lo; v <= hi; v += step) {
    int bit = (f->names == DAY_NAMES && v == 7) ? 0 : v;
    *mask |= UINT64_C(1) << bit;
  }
  return true;
}

static bool parse_field(const char *s, size_t len, const field_spec *f,
                        uint64_t *mask, bool *star, char *err) {
  field_cur c;
  c.s = s;
  c.len = len;
  c.pos = 0;
  c.f = f;
  c.err = err;
  *mask = 0;
  *star = len > 0 && s[0] == '*';
  for (;;) {
    if (cur_peek(&c) == ',' || cur_peek(&c) < 0)
      return field_err(&c, "empty list item");
    if (!parse_item(&c, mask)) return false;
    if (cur_peek(&c) < 0) break;
    c.pos++; /* ',' */
  }
  return true;
}

static const struct {
  const char *name;
  const char *expr;
} MACROS[] = {
    {"@hourly", "0 * * * *"},  {"@daily", "0 0 * * *"},   {"@midnight", "0 0 * * *"},
    {"@weekly", "0 0 * * 0"},  {"@monthly", "0 0 1 * *"}, {"@yearly", "0 0 1 1 *"},
    {"@annually", "0 0 1 1 *"},
};

astime_err astime_cron_parse(const char *expr, astime_cron *out, char err[ASTIME_ERR_SIZE]) {
  const char *p, *start[5];
  size_t len[5], nf = 0, i, total;
  astime_cron c;
  if (!expr || !out) {
    astime__seterr(err, "cron: missing expression");
    return ASTIME_ERR_INVALID;
  }
  while (*expr == ' ' || *expr == '\t') expr++;
  total = strlen(expr);
  while (total > 0 && (expr[total - 1] == ' ' || expr[total - 1] == '\t')) total--;
  if (total == 0) {
    astime__seterr(err, "cron: empty expression");
    return ASTIME_ERR_INVALID;
  }
  if (total >= ASTIME_CRON_TEXT_MAX) {
    astime__seterr(err, "cron: expression longer than %d characters",
                   ASTIME_CRON_TEXT_MAX - 1);
    return ASTIME_ERR_INVALID;
  }
  if (expr[0] == '@') {
    char word[16];
    size_t n = 0;
    for (i = 0; i < total && n + 1 < sizeof word; i++) word[n++] = (char)tolower((unsigned char)expr[i]);
    word[n] = '\0';
    if (total < sizeof word) {
      for (i = 0; i < sizeof MACROS / sizeof MACROS[0]; i++)
        if (strcmp(word, MACROS[i].name) == 0) return astime_cron_parse(MACROS[i].expr, out, err);
    }
    if (total == 7 && strncmp(word, "@reboot", 7) == 0)
      astime__seterr(err, "cron: @reboot is not supported (use a one-shot \"at\" trigger)");
    else
      astime__seterr(err, "cron: unknown macro \"%.*s\" (use @hourly, @daily, @weekly, "
                          "@monthly or @yearly)", (int)(total > 32 ? 32 : total), expr);
    return ASTIME_ERR_INVALID;
  }
  p = expr;
  while ((size_t)(p - expr) < total) {
    const char *q = p;
    while ((size_t)(q - expr) < total && *q != ' ' && *q != '\t') q++;
    if (nf < 5) {
      start[nf] = p;
      len[nf] = (size_t)(q - p);
    }
    nf++;
    p = q;
    while ((size_t)(p - expr) < total && (*p == ' ' || *p == '\t')) p++;
  }
  if (nf != 5) {
    astime__seterr(err, "cron: expected 5 fields (minute hour day-of-month month "
                        "day-of-week), got %zu", nf);
    return ASTIME_ERR_INVALID;
  }
  memset(&c, 0, sizeof c);
  for (i = 0; i < 5; i++) {
    uint64_t mask;
    bool star;
    char ferr[ASTIME_ERR_SIZE];
    if (!parse_field(start[i], len[i], &FIELDS[i], &mask, &star, ferr)) {
      astime__seterr(err, "cron: %s", ferr);
      return ASTIME_ERR_INVALID;
    }
    switch (i) {
    case 0: c.minute = mask; break;
    case 1: c.hour = (uint32_t)mask; break;
    case 2:
      c.dom = (uint32_t)mask;
      if (star) c.flags |= ASTIME_CRON_DOM_STAR;
      break;
    case 3: c.month = (uint16_t)mask; break;
    default:
      c.dow = (uint8_t)mask;
      if (star) c.flags |= ASTIME_CRON_DOW_STAR;
      break;
    }
  }
  *out = c;
  return ASTIME_OK;
}

astime_err astime_trigger_set_cron(astime_trigger *t, const char *expr, astime_tz tz,
                                   char err[ASTIME_ERR_SIZE]) {
  astime_cron c;
  astime_err e;
  size_t n;
  if (!t || !expr) return ASTIME_ERR_INVALID;
  e = astime_cron_parse(expr, &c, err);
  if (e != ASTIME_OK) return e;
  memset(t, 0, sizeof *t);
  t->kind = ASTIME_TRIGGER_CRON;
  t->tz = tz;
  while (*expr == ' ' || *expr == '\t') expr++;
  n = strlen(expr);
  while (n > 0 && (expr[n - 1] == ' ' || expr[n - 1] == '\t')) n--;
  memcpy(t->cron_text, expr, n);
  t->cron_text[n] = '\0';
  t->cron = c;
  return ASTIME_OK;
}

/* ── matching ─────────────────────────────────────────────────────────── */

static bool day_matches(const astime_cron *c, int month, int dom, int dow) {
  bool dm, wm;
  if (!(c->month & (1u << month))) return false;
  dm = (c->dom >> dom) & 1u;
  wm = (c->dow >> dow) & 1u;
  if (c->flags & (ASTIME_CRON_DOM_STAR | ASTIME_CRON_DOW_STAR)) return dm && wm;
  return dm || wm;
}

bool astime__cron_satisfiable(const astime_cron *c) {
  int m, d;
  if (!c->minute || !c->hour || !c->month) return false;
  if (!(c->flags & (ASTIME_CRON_DOM_STAR | ASTIME_CRON_DOW_STAR)) && c->dow) return true;
  if (!c->dow) return false;
  for (m = 1; m <= 12; m++) {
    if (!(c->month & (1u << m))) continue;
    for (d = 1; d <= astime__days_in_month(2000, m); d++) /* 2000: leap */
      if ((c->dom >> d) & 1u) return true;
  }
  return false;
}

/* ── search ───────────────────────────────────────────────────────────── */

/* Direction +1: first instant > ref. Direction -1: last instant <= ref. */
static astime_err cron_search(const astime_cron *c, int64_t ref, astime_tz tz,
                              int dir, int64_t *out) {
  int64_t start_day, day, k;
  if (!c || !out) return ASTIME_ERR_INVALID;
  if (!c->minute || !c->hour || !c->month || !c->dom || !c->dow) return ASTIME_ERR_NOT_FOUND;
  astime__tz_refresh();
  start_day = astime__floor_div(ref + astime__offset(ref, tz), 86400) - dir;
  for (k = 0; k <= ASTIME_CRON_HORIZON_DAYS + 2; k++) {
    int64_t y, oa, ob;
    int m, d, h, mi;
    bool fixed;
    day = start_day + dir * k;
    astime__civil_from_days(day, &y, &m, &d);
    if (!day_matches(c, m, d, astime__weekday(day))) continue;
    /* One offset for the whole day unless a transition is near. */
    oa = astime__offset(day * 86400 - 86400, tz);
    ob = astime__offset(day * 86400 + 2 * 86400, tz);
    fixed = oa == ob;
    for (h = dir > 0 ? 0 : 23; h >= 0 && h <= 23; h += dir) {
      if (!((c->hour >> h) & 1u)) continue;
      for (mi = dir > 0 ? 0 : 59; mi >= 0 && mi <= 59; mi += dir) {
        int64_t local, t;
        if (!((c->minute >> mi) & 1u)) continue;
        local = day * 86400 + (int64_t)h * 3600 + (int64_t)mi * 60;
        if (fixed) {
          t = local - oa;
        } else if (astime__from_wall(local, tz, &t, NULL) == 0) {
          continue; /* DST gap: this wall time does not exist today */
        }
        if (dir > 0 ? t > ref : t <= ref) {
          *out = t;
          return ASTIME_OK;
        }
      }
    }
  }
  return ASTIME_ERR_NOT_FOUND;
}

astime_err astime_cron_next(const astime_cron *cron, int64_t after, astime_tz tz,
                            int64_t *out_next) {
  return cron_search(cron, after, tz, +1, out_next);
}

astime_err astime_cron_prev(const astime_cron *cron, int64_t at_or_before, astime_tz tz,
                            int64_t *out_prev) {
  return cron_search(cron, at_or_before, tz, -1, out_prev);
}
