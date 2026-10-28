/*
 * tz.c — calendar arithmetic and the two zones astime knows: UTC and the
 * process local zone.
 *
 * Local offsets come from localtime_r (localtime_s on Windows) and are
 * derived by comparing the broken-down wall time with the instant, so no
 * tm_gmtoff or timegm is needed. Wall-to-instant conversion tries the
 * offsets in force a day before and a day after: a reading valid under
 * neither falls in a DST gap, one valid under both is repeated.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "astime_internal.h"

/* ── calendar ─────────────────────────────────────────────────────────── */

int64_t astime__floor_div(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
  return q;
}

int64_t astime__days_from_civil(int64_t y, int m, int d) {
  int64_t era;
  unsigned yoe, doy, doe;
  y -= (m <= 2);
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);
  doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

void astime__civil_from_days(int64_t z, int64_t *py, int *pm, int *pd) {
  int64_t era, y;
  unsigned doe, yoe, doy, mp, d, m;
  z += 719468;
  era = (z >= 0 ? z : z - 146096) / 146097;
  doe = (unsigned)(z - era * 146097);
  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int64_t)yoe + era * 400;
  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  *py = y + (m <= 2);
  *pm = (int)m;
  *pd = (int)d;
}

int astime__days_in_month(int64_t y, int m) {
  static const int dm[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m < 1 || m > 12) return 0;
  if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
  return dm[m - 1];
}

int astime__weekday(int64_t days) {
  int64_t w = (days + 4) % 7; /* 1970-01-01 was a Thursday */
  if (w < 0) w += 7;
  return (int)w;
}

/* ── zones ────────────────────────────────────────────────────────────── */

void astime__tz_refresh(void) {
#if defined(_WIN32)
  _tzset();
#else
  tzset();
#endif
}

static bool local_tm(int64_t t, struct tm *tm) {
  time_t tt = (time_t)t;
  if ((int64_t)tt != t) return false;
#if defined(_WIN32)
  return localtime_s(tm, &tt) == 0;
#else
  return localtime_r(&tt, tm) != NULL;
#endif
}

int64_t astime__offset(int64_t t, astime_tz tz) {
  struct tm tm;
  int64_t local;
  if (tz == ASTIME_TZ_UTC) return 0;
  if (!local_tm(t, &tm)) return 0;
  local = astime__days_from_civil((int64_t)tm.tm_year + 1900, tm.tm_mon + 1,
                                  tm.tm_mday) * 86400 +
          (int64_t)tm.tm_hour * 3600 + (int64_t)tm.tm_min * 60 +
          (tm.tm_sec > 59 ? 59 : tm.tm_sec);
  return local - t;
}

int64_t astime_utc_offset(int64_t t, astime_tz tz) {
  astime__tz_refresh();
  return astime__offset(t, tz);
}

void astime__to_wall(int64_t t, astime_tz tz, astime__wall *w) {
  int64_t local = t + astime__offset(t, tz);
  int64_t sod;
  w->days = astime__floor_div(local, 86400);
  sod = local - w->days * 86400;
  astime__civil_from_days(w->days, &w->year, &w->month, &w->day);
  w->hour = (int)(sod / 3600);
  w->min = (int)((sod / 60) % 60);
  w->sec = (int)(sod % 60);
  w->wday = astime__weekday(w->days);
}

int astime__from_wall(int64_t local, astime_tz tz, int64_t *first, int64_t *second) {
  int64_t oa, ob, t1, t2;
  bool v1, v2;
  if (tz == ASTIME_TZ_UTC) {
    if (first) *first = local;
    if (second) *second = local;
    return 1;
  }
  oa = astime__offset(local - 86400, tz);
  ob = astime__offset(local + 86400, tz);
  t1 = local - oa;
  if (oa == ob) {
    if (first) *first = t1;
    if (second) *second = t1;
    return 1;
  }
  t2 = local - ob;
  v1 = astime__offset(t1, tz) == oa;
  v2 = astime__offset(t2, tz) == ob;
  if (v1 && v2 && t1 != t2) {
    if (first) *first = t1 < t2 ? t1 : t2;
    if (second) *second = t1 < t2 ? t2 : t1;
    return 2;
  }
  if (v1 || v2) {
    int64_t t = v1 ? t1 : t2;
    if (first) *first = t;
    if (second) *second = t;
    return 1;
  }
  return 0;
}

int64_t astime__wall_instant(int64_t local, astime_tz tz) {
  int64_t t;
  if (astime__from_wall(local, tz, &t, NULL) > 0) return t;
  /* Inside a gap: the clock jumps at local - offset_before. */
  return local - astime__offset(local - 86400, tz);
}

void astime__day_text(int64_t t, astime_tz tz, char out[11]) {
  astime__wall w;
  astime__to_wall(t, tz, &w);
  unsigned y = (unsigned)(w.year < 0 ? 0 : w.year % 10000);
  out[0] = (char)('0' + y / 1000);
  out[1] = (char)('0' + (y / 100) % 10);
  out[2] = (char)('0' + (y / 10) % 10);
  out[3] = (char)('0' + y % 10);
  out[4] = '-';
  out[5] = (char)('0' + (unsigned)w.month / 10 % 10);
  out[6] = (char)('0' + (unsigned)w.month % 10);
  out[7] = '-';
  out[8] = (char)('0' + (unsigned)w.day / 10 % 10);
  out[9] = (char)('0' + (unsigned)w.day % 10);
  out[10] = '\0';
}

void astime_format_time(int64_t t, astime_tz tz, char out[ASTIME_TIME_TEXT_SIZE]) {
  astime__wall w;
  if (!out) return;
  astime__tz_refresh();
  astime__to_wall(t, tz, &w);
  snprintf(out, ASTIME_TIME_TEXT_SIZE, "%04u-%02u-%02u %02u:%02u%s",
           (unsigned)(w.year < 0 ? 0 : w.year % 10000), (unsigned)w.month % 100u,
           (unsigned)w.day % 100u, (unsigned)w.hour % 100u, (unsigned)w.min % 100u,
           tz == ASTIME_TZ_UTC ? " UTC" : "");
}

const char *astime_tz_name(astime_tz tz) {
  return tz == ASTIME_TZ_UTC ? "utc" : "local";
}

/* ── time text ────────────────────────────────────────────────────────── */

void astime__format_duration(int64_t s, char out[40]) {
  int64_t d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60, sec = s % 60;
  size_t n = 0;
  out[0] = '\0';
  n += (size_t)snprintf(out + n, 40 - n, "P");
  if (d) n += (size_t)snprintf(out + n, 40 - n, "%lldD", (long long)d);
  if (h || m || sec) {
    n += (size_t)snprintf(out + n, 40 - n, "T");
    if (h) n += (size_t)snprintf(out + n, 40 - n, "%lldH", (long long)h);
    if (m) n += (size_t)snprintf(out + n, 40 - n, "%lldM", (long long)m);
    if (sec) (void)snprintf(out + n, 40 - n, "%lldS", (long long)sec);
  } else if (!d) {
    (void)snprintf(out + n, 40 - n, "T0S");
  }
}

void astime__format_at(int64_t t, int32_t offset_s, char out[40]) {
  int64_t local = t + offset_s, days = astime__floor_div(local, 86400), y;
  int64_t sod = local - days * 86400;
  int m, d, off = offset_s < 0 ? -offset_s : offset_s;
  astime__civil_from_days(days, &y, &m, &d);
  if (offset_s == 0) {
    snprintf(out, 40, "%04d-%02d-%02dT%02d:%02d:%02dZ", (int)(y % 10000), m, d,
             (int)(sod / 3600), (int)((sod / 60) % 60), (int)(sod % 60));
  } else {
    snprintf(out, 40, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
             (int)(y % 10000), m, d, (int)(sod / 3600), (int)((sod / 60) % 60),
             (int)(sod % 60), offset_s < 0 ? '-' : '+', off / 3600,
             (off / 60) % 60);
  }
}

bool astime__parse_at(const char *s, int64_t *out, int32_t *out_offset) {
  size_t n;
  int32_t off = 0;
  core_time t;
  if (!s || !core_time_parse_rfc3339(s, &t)) return false;
  *out = (int64_t)t;
  n = strlen(s);
  if (n >= 6 && (s[n - 6] == '+' || s[n - 6] == '-') && s[n - 3] == ':') {
    int hh = (s[n - 5] - '0') * 10 + (s[n - 4] - '0');
    int mm = (s[n - 2] - '0') * 10 + (s[n - 1] - '0');
    off = (int32_t)(hh * 3600 + mm * 60);
    if (s[n - 6] == '-') off = -off;
  }
  if (out_offset) *out_offset = off;
  return true;
}

static bool two_digits(const char *s, int *out) {
  if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9') return false;
  *out = (s[0] - '0') * 10 + (s[1] - '0');
  return true;
}

/* "H:MM" or "HH:MM"; returns chars consumed, 0 on error. */
static size_t parse_hhmm(const char *s, int *out_min, bool allow_24) {
  int h, m;
  size_t n;
  if (s[0] >= '0' && s[0] <= '9' && s[1] == ':') {
    h = s[0] - '0';
    n = 1;
  } else if (two_digits(s, &h) && s[2] == ':') {
    n = 2;
  } else {
    return 0;
  }
  if (!two_digits(s + n + 1, &m)) return 0;
  if (m > 59) return 0;
  if (h > 23 && !(allow_24 && h == 24 && m == 0)) return 0;
  *out_min = h * 60 + m;
  return n + 3;
}

bool astime__parse_quiet(const char *s, int *start, int *end) {
  size_t n;
  if (!s) return false;
  while (*s == ' ') s++;
  n = parse_hhmm(s, start, false);
  if (!n) return false;
  s += n;
  while (*s == ' ') s++;
  if (*s != '-') return false;
  s++;
  while (*s == ' ') s++;
  n = parse_hhmm(s, end, true);
  if (!n) return false;
  s += n;
  while (*s == ' ') s++;
  return *s == '\0';
}
