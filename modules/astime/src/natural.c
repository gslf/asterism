/*
 * natural.c — schedules from everyday English, no model.
 *
 * The text is split into lowercase words at spaces and punctuation (a
 * non-ASCII character never matches) and read by a small backtracking
 * grammar:
 *
 *   schedule := cron | iso-time
 *             | interval                      "every 30 minutes", "hourly"
 *             | relative                      "in 30 minutes"
 *             | dayspec time | time dayspec   "every monday at 6pm", "weekdays at 9"
 *             | date time | time date         "tomorrow at 3pm", "on 2026-11-03 at 09:00"
 *
 * Recurring phrasings become cron, intervals "every", dates one-shot "at"
 * resolved in the requested zone.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astime_internal.h"

#define NTOK 40
#define TOKLEN 32

/* ── folding ──────────────────────────────────────────────────────────── */

/* Latin-1 supplement letters (second byte after 0xC3) folded to ASCII. */
static const char *fold_c3(unsigned char b) {
  if ((b >= 0x80 && b <= 0x85) || (b >= 0xA0 && b <= 0xA5)) return "a";
  if (b == 0x87 || b == 0xA7) return "c";
  if ((b >= 0x88 && b <= 0x8B) || (b >= 0xA8 && b <= 0xAB)) return "e";
  if ((b >= 0x8C && b <= 0x8F) || (b >= 0xAC && b <= 0xAF)) return "i";
  if (b == 0x91 || b == 0xB1) return "n";
  if ((b >= 0x92 && b <= 0x96) || (b >= 0xB2 && b <= 0xB6)) return "o";
  if ((b >= 0x99 && b <= 0x9C) || (b >= 0xB9 && b <= 0xBC)) return "u";
  if (b == 0x9D || b == 0xBD || b == 0xBF) return "y";
  if (b == 0x9F) return "ss";
  return NULL;
}

void astime_slug_from_text(const char *text, char out[ASTIME_NAME_MAX]) {
  char buf[256];
  size_t n = 0, i = 0, cut;
  if (!out) return;
  while (text && text[i] && n + 3 < sizeof buf) {
    unsigned char ch = (unsigned char)text[i];
    const char *f = NULL;
    if (ch == 0xC3 && text[i + 1]) {
      f = fold_c3((unsigned char)text[i + 1]);
      i += 2;
    } else if (ch >= 0x80) {
      i++;
      while (((unsigned char)text[i] & 0xC0) == 0x80) i++;
    } else {
      i++;
    }
    if (f) {
      for (; *f; f++) buf[n++] = *f;
    } else if (isalnum(ch) && ch < 0x80) {
      buf[n++] = (char)tolower(ch);
    } else if (n > 0 && buf[n - 1] != '-') {
      buf[n++] = '-';
    }
  }
  while (n > 0 && buf[n - 1] == '-') n--;
  buf[n] = '\0';
  cut = n;
  if (cut > 40) {
    cut = 40;
    while (cut > 10 && buf[cut] != '-') cut--;
    if (buf[cut] != '-') cut = 40;
    while (cut > 0 && buf[cut - 1] == '-') cut--;
  }
  if (cut == 0) {
    snprintf(out, ASTIME_NAME_MAX, "routine");
    return;
  }
  snprintf(out, ASTIME_NAME_MAX, "%.*s", (int)cut, buf);
}

/* ── tokens ───────────────────────────────────────────────────────────── */

typedef struct {
  char t[NTOK][TOKLEN];
  int n, pos;
  int64_t now;
  astime_tz tz;
  char hint[ASTIME_ERR_SIZE];
} np;

static bool tokenize(np *p, const char *text) {
  size_t i = 0, len = 0;
  char cur[TOKLEN];
  bool overflow = false;
  p->n = 0;
#define FLUSH()                                                    \
  do {                                                             \
    while (len > 0 && cur[len - 1] == '.') len--;                  \
    if (len > 0) {                                                 \
      if (p->n >= NTOK) return false;                              \
      memcpy(p->t[p->n], cur, len);                                \
      p->t[p->n][len] = '\0';                                      \
      p->n++;                                                      \
    }                                                              \
    len = 0;                                                       \
  } while (0)
  while (text[i]) {
    unsigned char ch = (unsigned char)text[i++];
    if (ch >= 0x80) { /* a non-ASCII character: kept as "?", matches nothing */
      while (((unsigned char)text[i] & 0xC0) == 0x80) i++;
      ch = '?';
    } else if (isspace(ch) || ch == ',' || ch == ';' || ch == '!' || ch == '?' || ch == '\'' ||
               ch == '"' || ch == '(' || ch == ')') {
      FLUSH();
      continue;
    }
    if (len + 1 < TOKLEN) cur[len++] = (char)tolower(ch);
    else overflow = true;
  }
  FLUSH();
#undef FLUSH
  return !overflow;
}

static const char *peek(const np *p, int k) {
  return p->pos + k < p->n ? p->t[p->pos + k] : NULL;
}

static bool at_end(const np *p) { return p->pos >= p->n; }

static bool accept(np *p, const char *w) {
  const char *s = peek(p, 0);
  if (s && strcmp(s, w) == 0) {
    p->pos++;
    return true;
  }
  return false;
}

/* Accept a whole sequence "w1 w2 ..." or nothing. */
static bool seq(np *p, const char *words) {
  int save = p->pos;
  char buf[96], *w, *rest;
  snprintf(buf, sizeof buf, "%s", words);
  for (w = buf; w && *w; w = rest) {
    rest = strchr(w, ' ');
    if (rest) *rest++ = '\0';
    if (!accept(p, w)) {
      p->pos = save;
      return false;
    }
  }
  return true;
}

static bool one_of(np *p, const char *const *words) {
  for (; *words; words++)
    if (seq(p, *words)) return true;
  return false;
}

static void hint(np *p, const char *msg) {
  if (!p->hint[0]) snprintf(p->hint, sizeof p->hint, "%s", msg);
}

/* ── numbers, units, names ────────────────────────────────────────────── */

static const struct {
  const char *w;
  int v;
} NUM_WORDS[] = {
    {"a", 1}, {"an", 1}, {"one", 1}, {"two", 2}, {"three", 3}, {"four", 4}, {"five", 5},
    {"six", 6}, {"seven", 7}, {"eight", 8}, {"nine", 9}, {"ten", 10}, {"eleven", 11},
    {"twelve", 12}, {"fifteen", 15}, {"twenty", 20}, {"thirty", 30}, {"forty", 40},
    {"forty-five", 45},
};

static bool digits(const char *s, int maxd, int *out) {
  int v = 0, n = 0;
  if (!s || !*s) return false;
  for (; *s; s++, n++) {
    if (*s < '0' || *s > '9' || n >= maxd) return false;
    v = v * 10 + (*s - '0');
  }
  *out = v;
  return true;
}

/* Digits or a number word, consuming one token. */
static bool num(np *p, int *out, bool words) {
  const char *s = peek(p, 0);
  size_t i;
  if (!s) return false;
  if (digits(s, 4, out)) {
    p->pos++;
    return true;
  }
  if (!words) return false;
  for (i = 0; i < sizeof NUM_WORDS / sizeof NUM_WORDS[0]; i++)
    if (strcmp(s, NUM_WORDS[i].w) == 0) {
      *out = NUM_WORDS[i].v;
      p->pos++;
      return true;
    }
  return false;
}

/* Day of month: "3", "3rd", "1st", "first". */
static bool ordinal(np *p, int *out) {
  const char *s = peek(p, 0);
  char buf[TOKLEN];
  size_t n;
  if (!s) return false;
  if (strcmp(s, "first") == 0) {
    *out = 1;
    p->pos++;
    return true;
  }
  n = strlen(s);
  snprintf(buf, sizeof buf, "%s", s);
  if (n > 2 && (strcmp(s + n - 2, "st") == 0 || strcmp(s + n - 2, "nd") == 0 ||
                strcmp(s + n - 2, "rd") == 0 || strcmp(s + n - 2, "th") == 0))
    buf[n - 2] = '\0';
  if (!digits(buf, 2, out) || *out < 1 || *out > 31) return false;
  p->pos++;
  return true;
}

static int64_t unit_of(const char *s, bool allow_days) {
  static const char *const MIN[] = {"min", "mins", "minute", "minutes", "m", NULL};
  static const char *const HOUR[] = {"h", "hour", "hours", "hr", "hrs", NULL};
  static const char *const DAY[] = {"day", "days", "d", NULL};
  int i;
  if (!s) return 0;
  for (i = 0; MIN[i]; i++)
    if (strcmp(s, MIN[i]) == 0) return 60;
  for (i = 0; HOUR[i]; i++)
    if (strcmp(s, HOUR[i]) == 0) return 3600;
  for (i = 0; allow_days && DAY[i]; i++)
    if (strcmp(s, DAY[i]) == 0) return 86400;
  return 0;
}

/* "30 minutes", "2h", "30min", "an hour", "half an hour". */
static bool amount(np *p, int64_t *out, bool days) {
  int save = p->pos, n;
  const char *s = peek(p, 0);
  int64_t u;
  if (seq(p, "half an hour") || seq(p, "half hour")) {
    *out = 1800;
    return true;
  }
  if (seq(p, "quarter of an hour") || seq(p, "quarter hour") || seq(p, "a quarter of an hour") ||
      seq(p, "a quarter hour")) {
    *out = 900;
    return true;
  }
  if (s) { /* attached unit: "30min", "2h" */
    char d[TOKLEN];
    size_t k = 0;
    while (s[k] >= '0' && s[k] <= '9' && k + 1 < sizeof d) {
      d[k] = s[k];
      k++;
    }
    d[k] = '\0';
    if (k > 0 && s[k] && (u = unit_of(s + k, days)) > 0 && digits(d, 4, &n)) {
      p->pos++;
      *out = (int64_t)n * u;
      return true;
    }
  }
  if (num(p, &n, true) && (u = unit_of(peek(p, 0), days)) > 0) {
    p->pos++;
    *out = (int64_t)n * u;
    return true;
  }
  p->pos = save;
  return false;
}

static int weekday_word(const char *s, bool *plural) {
  static const char *const W[7][5] = {
      {"sunday", "sun", NULL},
      {"monday", "mon", NULL},
      {"tuesday", "tue", "tues", NULL},
      {"wednesday", "wed", NULL},
      {"thursday", "thu", "thur", "thurs", NULL},
      {"friday", "fri", NULL},
      {"saturday", "sat", NULL},
  };
  static const char *const PLURAL[7] = {"sundays", "mondays", "tuesdays", "wednesdays",
                                        "thursdays", "fridays", "saturdays"};
  int d, k;
  *plural = false;
  if (!s) return -1;
  for (d = 0; d < 7; d++) {
    if (strcmp(s, PLURAL[d]) == 0) {
      *plural = true;
      return d;
    }
    for (k = 0; W[d][k]; k++)
      if (strcmp(s, W[d][k]) == 0) return d;
  }
  return -1;
}

static int month_word(const char *s) {
  static const char *const M[12][4] = {
      {"january", "jan", NULL},   {"february", "feb", NULL}, {"march", "mar", NULL},
      {"april", "apr", NULL},     {"may", NULL},             {"june", "jun", NULL},
      {"july", "jul", NULL},      {"august", "aug", NULL},   {"september", "sep", "sept", NULL},
      {"october", "oct", NULL},   {"november", "nov", NULL}, {"december", "dec", NULL},
  };
  int m, k;
  if (!s) return 0;
  for (m = 0; m < 12; m++)
    for (k = 0; M[m][k]; k++)
      if (strcmp(s, M[m][k]) == 0) return m + 1;
  return 0;
}

/* ── time of day ──────────────────────────────────────────────────────── */

/* "8", "8:30", "8.30", "08:00", "8am", "6:30pm"; returns false if not a
 * time token. *mer: 0 none, 1 am, 2 pm. */
static bool clock_token(const char *s, int *h, int *m, int *mer) {
  char buf[TOKLEN];
  size_t n, i = 0;
  int hh = 0, mm = 0, nd = 0;
  snprintf(buf, sizeof buf, "%s", s);
  n = strlen(buf);
  *mer = 0;
  if (n > 2 && (strcmp(buf + n - 2, "am") == 0 || strcmp(buf + n - 2, "pm") == 0)) {
    *mer = buf[n - 2] == 'a' ? 1 : 2;
    buf[n - 2] = '\0';
  } else if (n > 3 && (strcmp(buf + n - 3, "a.m") == 0 || strcmp(buf + n - 3, "p.m") == 0)) {
    *mer = buf[n - 3] == 'a' ? 1 : 2;
    buf[n - 3] = '\0';
  } else if (n > 1 && buf[n - 1] == 'h' && buf[n - 2] >= '0' && buf[n - 2] <= '9') {
    buf[n - 1] = '\0';
  }
  while (buf[i] >= '0' && buf[i] <= '9') {
    if (++nd > 2) return false;
    hh = hh * 10 + (buf[i++] - '0');
  }
  if (nd == 0) return false;
  if (buf[i] == ':' || buf[i] == '.') {
    i++;
    if (!(buf[i] >= '0' && buf[i] <= '9' && buf[i + 1] >= '0' && buf[i + 1] <= '9')) return false;
    mm = (buf[i] - '0') * 10 + (buf[i + 1] - '0');
    i += 2;
  }
  if (buf[i]) return false;
  *h = hh;
  *m = mm;
  return true;
}

static bool time_of_day(np *p, int *out_h, int *out_m) {
  int save = p->pos, h = -1, m = 0, mer = 0;
  const char *s;
  bool lead = accept(p, "at") || accept(p, "@");
  bool fixed = false;
  s = peek(p, 0);
  if (!s) goto fail;
  if (strcmp(s, "noon") == 0 || strcmp(s, "midday") == 0) {
    h = 12;
    fixed = true;
    p->pos++;
  } else if (strcmp(s, "midnight") == 0) {
    h = 0;
    fixed = true;
    p->pos++;
  } else if (clock_token(s, &h, &m, &mer)) {
    p->pos++;
  } else if (lead && num(p, &h, true)) {
    if (h > 12) goto fail;
  } else {
    goto fail;
  }
  if (!fixed && mer == 0) {
    if (seq(p, "am") || seq(p, "a.m") || seq(p, "in the morning"))
      mer = 1;
    else if (seq(p, "pm") || seq(p, "p.m") || seq(p, "in the afternoon") ||
             seq(p, "in the evening") || seq(p, "at night"))
      mer = (h >= 1 && h <= 5 && strcmp(p->t[p->pos - 1], "night") == 0) ? 1 : 2;
  }
  if (mer) {
    if (h < 1 || h > 12) {
      snprintf(p->hint, sizeof p->hint, "invalid time: %d%s", h, mer == 1 ? " am" : " pm");
      goto fail;
    }
    if (mer == 1 && h == 12) h = 0;
    if (mer == 2 && h < 12) h += 12;
  }
  if (h < 0 || h > 23 || m < 0 || m > 59) {
    snprintf(p->hint, sizeof p->hint, "invalid time: %d:%02d", h, m);
    goto fail;
  }
  *out_h = h;
  *out_m = m;
  return true;
fail:
  p->pos = save;
  return false;
}

/* ── recurring days ───────────────────────────────────────────────────── */

typedef struct {
  int kind; /* 1 every day, 2 days of week, 3 day of month */
  uint8_t dow;
  int dom;
} dayspec;

static bool day_list(np *p, uint8_t *mask, bool *any_plural) {
  int save = p->pos, d;
  bool plural;
  *mask = 0;
  *any_plural = false;
  for (;;) {
    d = weekday_word(peek(p, 0), &plural);
    if (d < 0) break;
    p->pos++;
    *mask |= (uint8_t)(1u << d);
    if (plural) *any_plural = true;
    /* separators: "and", "&", "/", "and on" */
    {
      int s2 = p->pos;
      if (accept(p, "and") || accept(p, "&") || accept(p, "/")) {
        (void)accept(p, "on");
        if (weekday_word(peek(p, 0), &plural) < 0) {
          p->pos = s2;
          break;
        }
      }
    }
  }
  if (!*mask) {
    p->pos = save;
    return false;
  }
  return true;
}

static bool parse_dayspec(np *p, dayspec *ds) {
  static const char *const WEEKDAYS[] = {
      "on weekdays", "every weekday", "each weekday", "weekdays", "from monday to friday",
      "monday to friday", "monday through friday", "mon-fri", NULL};
  static const char *const WEEKENDS[] = {"on weekends", "every weekend", "at weekends", "weekends",
                                         "on the weekend", NULL};
  static const char *const EVERYDAY[] = {"every day", "each day", "daily", "everyday", NULL};
  int save = p->pos, d;
  uint8_t mask;
  bool plural;
  memset(ds, 0, sizeof *ds);
  if (one_of(p, WEEKDAYS)) {
    ds->kind = 2;
    ds->dow = 0x3E;
    return true;
  }
  if (one_of(p, WEEKENDS)) {
    ds->kind = 2;
    ds->dow = 0x41;
    return true;
  }
  if (one_of(p, EVERYDAY)) {
    ds->kind = 1;
    return true;
  }
  /* monthly */
  if (seq(p, "every month on the") || seq(p, "every month on") || seq(p, "monthly on the") ||
      seq(p, "monthly on")) {
    if (ordinal(p, &d)) {
      ds->kind = 3;
      ds->dom = d;
      return true;
    }
    p->pos = save;
  }
  if (accept(p, "on") || accept(p, "the")) (void)accept(p, "the");
  if (ordinal(p, &d) &&
      (seq(p, "of every month") || seq(p, "of each month") || seq(p, "of the month"))) {
    ds->kind = 3;
    ds->dom = d;
    return true;
  }
  p->pos = save;
  /* days of the week: an explicit "every" or "each", or a plural */
  if (accept(p, "every") || accept(p, "each")) {
    if (day_list(p, &mask, &plural)) {
      ds->kind = 2;
      ds->dow = mask;
      return true;
    }
    p->pos = save;
    return false;
  }
  (void)accept(p, "on");
  if (day_list(p, &mask, &plural) && plural) {
    ds->kind = 2;
    ds->dow = mask;
    return true;
  }
  p->pos = save;
  return false;
}

/* ── one-shot dates ───────────────────────────────────────────────────── */

typedef struct {
  int64_t days;    /* local date, days since epoch                 */
  bool roll_year;  /* "3 november": move to next year if it passed */
  int weekday;     /* >= 0: next such weekday, resolved with time  */
  bool strictly_next;
} datespec;

static bool parse_date(np *p, datespec *ds) {
  int save = p->pos, d = 0, m = 0, y = 0, wd;
  const char *s;
  astime__wall today;
  bool plural;
  astime__to_wall(p->now, p->tz, &today);
  memset(ds, 0, sizeof *ds);
  ds->weekday = -1;
  if (accept(p, "today")) {
    ds->days = today.days;
    return true;
  }
  if (accept(p, "tomorrow")) {
    ds->days = today.days + 1;
    return true;
  }
  if (seq(p, "the day after tomorrow") || seq(p, "day after tomorrow")) {
    ds->days = today.days + 2;
    return true;
  }
  /* weekday one-shots: "monday", "monday next", "next monday", "on monday" */
  if (accept(p, "next")) {
    wd = weekday_word(peek(p, 0), &plural);
    if (wd >= 0 && !plural) {
      p->pos++;
      ds->weekday = wd;
      ds->strictly_next = true;
      return true;
    }
    p->pos = save;
    return false;
  }
  (void)(accept(p, "on") || accept(p, "this"));
  wd = weekday_word(peek(p, 0), &plural);
  if (wd >= 0 && !plural) {
    p->pos++;
    if (accept(p, "next")) ds->strictly_next = true;
    ds->weekday = wd;
    return true;
  }
  p->pos = save;
  (void)(accept(p, "on") || accept(p, "the"));
  (void)accept(p, "the");
  s = peek(p, 0);
  if (s && strlen(s) == 10 && s[4] == '-' && s[7] == '-') { /* 2026-11-03 */
    char yb[5] = {s[0], s[1], s[2], s[3], 0}, mb[3] = {s[5], s[6], 0}, db[3] = {s[8], s[9], 0};
    if (digits(yb, 4, &y) && digits(mb, 2, &m) && digits(db, 2, &d)) {
      p->pos++;
      goto have_date;
    }
  }
  if (ordinal(p, &d)) { /* 3 november [2027], 3rd of november */
    (void)accept(p, "of");
    m = month_word(peek(p, 0));
    if (m) {
      p->pos++;
      if (peek(p, 0) && digits(peek(p, 0), 4, &y) && y >= 2000) p->pos++;
      else y = 0;
      goto have_date;
    }
    p->pos = save;
    return false;
  }
  m = month_word(peek(p, 0));
  if (m) { /* november 3[rd] [2027] */
    p->pos++;
    if (ordinal(p, &d)) {
      if (peek(p, 0) && digits(peek(p, 0), 4, &y) && y >= 2000) p->pos++;
      else y = 0;
      goto have_date;
    }
  }
  p->pos = save;
  return false;
have_date:
  if (y == 0) {
    y = (int)today.year;
    ds->roll_year = true;
  }
  if (m < 1 || m > 12 || d < 1 || d > astime__days_in_month(y, m)) {
    snprintf(p->hint, sizeof p->hint, "invalid date: %d-%02d-%02d", y, m, d);
    p->pos = save;
    return false;
  }
  ds->days = astime__days_from_civil(y, m, d);
  return true;
}

/* ── results ──────────────────────────────────────────────────────────── */

static astime_err make_at(np *p, const datespec *ds, int h, int m, astime_trigger *out) {
  int64_t day = ds->days, t = 0;
  int tries;
  astime__wall today;
  astime__to_wall(p->now, p->tz, &today);
  if (ds->weekday >= 0) {
    day = today.days + (ds->strictly_next ? 1 : 0);
    while (astime__weekday(day) != ds->weekday) day++;
  }
  for (tries = 0; tries < 2; tries++) {
    int64_t local = day * 86400 + (int64_t)h * 3600 + (int64_t)m * 60;
    if (astime__from_wall(local, p->tz, &t, NULL) == 0) {
      hint(p, "that time does not exist on that day (daylight saving change)");
      return ASTIME_ERR_INVALID;
    }
    if (t > p->now) break;
    if (ds->weekday >= 0 && !ds->strictly_next) {
      day += 7;
      continue;
    }
    if (ds->roll_year) { /* same day next year */
      int64_t y;
      int mo, d;
      astime__civil_from_days(day, &y, &mo, &d);
      if (d > astime__days_in_month(y + 1, mo)) break;
      day = astime__days_from_civil(y + 1, mo, d);
      continue;
    }
    break;
  }
  if (t <= p->now) {
    hint(p, "that time is already in the past");
    return ASTIME_ERR_INVALID;
  }
  memset(out, 0, sizeof *out);
  out->kind = ASTIME_TRIGGER_AT;
  out->tz = p->tz;
  out->at = t;
  out->at_offset_s = (int32_t)astime__offset(t, p->tz);
  return ASTIME_OK;
}

static astime_err make_cron(np *p, const dayspec *ds, int h, int m, astime_trigger *out) {
  char expr[64], dows[32];
  size_t n = 0;
  int d;
  dows[0] = '\0';
  if (ds->kind == 2) {
    for (d = 0; d < 7; d++)
      if ((ds->dow >> d) & 1u) n += (size_t)snprintf(dows + n, sizeof dows - n, "%s%d", n ? "," : "", d);
    if (ds->dow == 0x3E) snprintf(dows, sizeof dows, "1-5");
  }
  if (ds->kind == 1)
    snprintf(expr, sizeof expr, "%d %d * * *", m, h);
  else if (ds->kind == 2)
    snprintf(expr, sizeof expr, "%d %d * * %s", m, h, dows);
  else
    snprintf(expr, sizeof expr, "%d %d %d * *", m, h, ds->dom);
  return astime_trigger_set_cron(out, expr, p->tz, NULL) == ASTIME_OK ? ASTIME_OK : ASTIME_ERR_INVALID;
}

static bool interval(np *p, int64_t *secs) {
  int save = p->pos;
  if (accept(p, "hourly")) {
    *secs = 3600;
    return true;
  }
  if (accept(p, "every") || accept(p, "each")) {
    int64_t u;
    if (amount(p, secs, true)) return true;
    u = unit_of(peek(p, 0), false); /* "every hour", "every minute" */
    if (u) {
      p->pos++;
      *secs = u;
      return true;
    }
  }
  p->pos = save;
  return false;
}

static astime_err parse_tokens(np *p, astime_trigger *out) {
  int64_t secs;
  int h, m;
  dayspec ds;
  datespec dt;

  p->pos = 0;
  if (interval(p, &secs)) {
    if (at_end(p)) {
      if (secs < ASTIME_MIN_INTERVAL || secs > (int64_t)366 * 86400) {
        hint(p, "the interval must be between 1 minute and 366 days");
        return ASTIME_ERR_INVALID;
      }
      memset(out, 0, sizeof *out);
      out->kind = ASTIME_TRIGGER_EVERY;
      out->tz = p->tz;
      out->every_s = secs;
      return ASTIME_OK;
    }
  }

  p->pos = 0;
  if (accept(p, "in") && amount(p, &secs, true) && at_end(p)) {
    memset(out, 0, sizeof *out);
    out->kind = ASTIME_TRIGGER_AT;
    out->tz = p->tz;
    out->at = p->now + secs;
    out->at_offset_s = (int32_t)astime__offset(out->at, p->tz);
    return ASTIME_OK;
  }

  /* dayspec time | time dayspec */
  p->pos = 0;
  if (parse_dayspec(p, &ds)) {
    if (time_of_day(p, &h, &m) && at_end(p)) return make_cron(p, &ds, h, m, out);
    if (at_end(p)) hint(p, "the time is missing: for example \"every day at 8am\"");
  }
  p->pos = 0;
  if (time_of_day(p, &h, &m) && parse_dayspec(p, &ds) && at_end(p)) return make_cron(p, &ds, h, m, out);

  /* date time | time date */
  p->pos = 0;
  if (parse_date(p, &dt)) {
    if (time_of_day(p, &h, &m) && at_end(p)) return make_at(p, &dt, h, m, out);
    if (at_end(p)) hint(p, "the time is missing: for example \"tomorrow at 3pm\"");
  }
  p->pos = 0;
  if (time_of_day(p, &h, &m) && parse_date(p, &dt) && at_end(p)) return make_at(p, &dt, h, m, out);
  return ASTIME_ERR_INVALID;
}

astime_err astime_schedule_parse(const char *text, int64_t now, astime_tz tz,
                                 astime_trigger *out, char err[ASTIME_ERR_SIZE]) {
  np *p;
  const char *s;
  size_t n;
  astime_err e;
  char cerr[ASTIME_ERR_SIZE];
  if (!text || !out) {
    astime__seterr(err, "missing schedule");
    return ASTIME_ERR_INVALID;
  }
  astime__tz_refresh();
  s = text;
  while (*s == ' ' || *s == '\t' || *s == '\n') s++;
  n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '.')) n--;
  if (n == 0) {
    astime__seterr(err, "empty schedule");
    return ASTIME_ERR_INVALID;
  }
  {
    char buf[ASTIME_CRON_TEXT_MAX + 8];
    const char *c = s;
    size_t cn = n;
    int64_t at;
    int32_t off;
    if (cn > 5 && tolower((unsigned char)c[0]) == 'c' && tolower((unsigned char)c[1]) == 'r' &&
        tolower((unsigned char)c[2]) == 'o' && tolower((unsigned char)c[3]) == 'n' &&
        (c[4] == ':' || c[4] == ' ')) {
      c += 5;
      cn -= 5;
      while (cn > 0 && *c == ' ') {
        c++;
        cn--;
      }
    }
    if (cn < sizeof buf) {
      memcpy(buf, c, cn);
      buf[cn] = '\0';
      if (astime_trigger_set_cron(out, buf, tz, cerr) == ASTIME_OK) return ASTIME_OK;
      if (buf[0] == '@' || c != s) {
        astime__seterr(err, "%s", cerr);
        return ASTIME_ERR_INVALID;
      }
      if (astime__parse_at(buf, &at, &off)) { /* a full RFC 3339 time */
        if (at <= now) {
          astime__seterr(err, "\"%s\" is already in the past", buf);
          return ASTIME_ERR_INVALID;
        }
        memset(out, 0, sizeof *out);
        out->kind = ASTIME_TRIGGER_AT;
        out->tz = tz;
        out->at = at;
        out->at_offset_s = off;
        return ASTIME_OK;
      }
    }
  }
  p = (np *)calloc(1, sizeof *p);
  if (!p) return ASTIME_ERR_NOMEM;
  p->now = now;
  p->tz = tz;
  {
    char *copy = core_strndup(s, n);
    bool ok = copy && tokenize(p, copy);
    free(copy);
    e = ok && p->n > 0 ? parse_tokens(p, out) : ASTIME_ERR_INVALID;
  }
  if (e != ASTIME_OK) {
    if (p->hint[0])
      astime__seterr(err, "\"%.*s\": %s", (int)(n > 80 ? 80 : n), s, p->hint);
    else
      astime__seterr(err, "cannot read \"%.*s\" as a schedule. Examples: \"every day at 8am\", "
                          "\"weekdays at 9\", \"every monday at 6pm\", \"every 15 minutes\", "
                          "\"tomorrow at 3pm\", \"on 2026-11-03 at 09:00\", or a cron expression "
                          "such as \"0 8 * * 1-5\".",
                     (int)(n > 80 ? 80 : n), s);
    if (e != ASTIME_ERR_NOMEM) e = ASTIME_ERR_INVALID;
  }
  free(p);
  return e;
}

/* ── routines from chat ───────────────────────────────────────────────── */

astime_err astime_routine_from_chat(const char *text, int64_t now, astime_tz tz,
                                    astime_routine *out, char err[ASTIME_ERR_SIZE]) {
  const char *bar;
  char *sched = NULL, *goal = NULL, name[ASTIME_NAME_MAX];
  astime_routine r;
  astime_err e;
  if (!out) return ASTIME_ERR_INVALID;
  astime_routine_init(out);
  bar = text ? strchr(text, '|') : NULL;
  if (!bar) {
    astime__seterr(err, "use the form \"<when> | <what to do>\", for example \"every day at 8am | "
                        "prepare the brief\"");
    return ASTIME_ERR_INVALID;
  }
  sched = core_strndup(text, (size_t)(bar - text));
  goal = core_strdup(bar + 1);
  if (!sched || !goal) {
    free(sched);
    free(goal);
    return ASTIME_ERR_NOMEM;
  }
  core_str_trim(sched);
  core_str_trim(goal);
  if (!*sched || !*goal) {
    astime__seterr(err, "both the \"when\" and the \"what to do\" are needed, separated by \"|\"");
    free(sched);
    free(goal);
    return ASTIME_ERR_INVALID;
  }
  astime_routine_init(&r);
  e = astime_schedule_parse(sched, now, tz, &r.trigger, err);
  if (e == ASTIME_OK) {
    astime_slug_from_text(goal, name);
    r.name = core_strdup(name);
    r.goal = goal;
    goal = NULL;
    r.enabled = false;
    r.mode = ASTIME_MODE_CHAT;
    r.profile = ASTIME_PROFILE_CHAT;
    r.providers = ASTIME_PROVIDER_LOCAL;
    r.notify = ASTIME_NOTIFY_ALWAYS;
    e = r.name ? astime_routine_validate(&r, err) : ASTIME_ERR_NOMEM;
  }
  free(sched);
  free(goal);
  if (e != ASTIME_OK) {
    astime_routine_free(&r);
    return e;
  }
  *out = r;
  return ASTIME_OK;
}
