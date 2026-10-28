/*
 * astime_internal.h — shared helpers of libastime (not installed).
 *
 * Calendar arithmetic is proleptic Gregorian on days since 1970-01-01.
 * "Local seconds" are a wall-clock reading expressed as if it were UTC
 * (days * 86400 + seconds of day); converting them back to instants is
 * where DST gaps (no instant) and repeats (two instants) show up.
 */

#ifndef ASTIME_INTERNAL_H
#define ASTIME_INTERNAL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "astime.h"
#include "core.h"

astime_err astime_from_core(core_err e);

/* ── calendar ─────────────────────────────────────────────────────────── */

int64_t astime__floor_div(int64_t a, int64_t b);
int64_t astime__days_from_civil(int64_t y, int m, int d);
void    astime__civil_from_days(int64_t z, int64_t *y, int *m, int *d);
int     astime__days_in_month(int64_t y, int m);
int     astime__weekday(int64_t days); /* 0 = Sunday */

/* ── zones ────────────────────────────────────────────────────────────── */

void    astime__tz_refresh(void); /* re-read TZ (tzset) */
int64_t astime__offset(int64_t t, astime_tz tz);

typedef struct {
  int64_t days; /* local date, days since epoch */
  int64_t year;
  int month, day, hour, min, sec, wday;
} astime__wall;

void astime__to_wall(int64_t t, astime_tz tz, astime__wall *w);
/* Instants showing local seconds L: returns 0 (gap), 1 or 2 (repeat);
 * first <= second. Either out may be NULL. */
int  astime__from_wall(int64_t local, astime_tz tz, int64_t *first, int64_t *second);
/* Earliest instant showing L or, inside a gap, the instant the gap ends. */
int64_t astime__wall_instant(int64_t local, astime_tz tz);
/* "YYYY-MM-DD" of t in tz. */
void astime__day_text(int64_t t, astime_tz tz, char out[11]);

/* ── cron internals ───────────────────────────────────────────────────── */

/* At least one calendar date matches (ignores leap-year spacing). */
bool astime__cron_satisfiable(const astime_cron *c);

/* ── text helpers ─────────────────────────────────────────────────────── */

void astime__seterr(char *err, const char *fmt, ...);
astime_err astime__strings_push(astime_strings *l, char *owned);
astime_err astime__strings_pushf(astime_strings *l, const char *fmt, ...);
/* Copy at most cap-1 bytes, never splitting a UTF-8 sequence; newlines and
 * other controls become spaces. */
void astime__copy_short(char *dst, size_t cap, const char *src);
/* "PT30M", "P1D", "P1DT2H" ... (positive seconds). */
void astime__format_duration(int64_t s, char out[40]);
/* "2026-11-02T09:00:00+01:00" (offset 0 gives "Z"). */
void astime__format_at(int64_t t, int32_t offset_s, char out[40]);
/* RFC 3339 with offset; out_offset receives seconds east. */
bool astime__parse_at(const char *s, int64_t *out, int32_t *out_offset);
/* "HH:MM-HH:MM", end may be 24:00. */
bool astime__parse_quiet(const char *s, int *start, int *end);
/* Signatures stored in the state to notice trigger/watch changes. */
void astime__trigger_sig(const astime_routine *r, char out[ASTIME_SIG_MAX]);
void astime__watch_sig(const astime_routine *r, char out[ASTIME_SIG_MAX]);

/* Line (1-based) of key path "a.b" in xCDN text, 0 when not found. */
size_t astime__locate_line(const char *text, const char *path);

/* ── scheduling internals ─────────────────────────────────────────────── */

typedef struct {
  bool due;
  astime_due_reason reason;
  int64_t slot;      /* schedule slot served, or watch poll time */
  bool slot_pending; /* the trigger has an unconsumed slot       */
  int64_t sched_slot;
  bool watch_due;
} astime__eval;

/* Armed instant to use: the state's, or `fallback` when unarmed or the
 * trigger changed since. */
int64_t astime__armed(const astime_routine *r, const astime_state *st, int64_t fallback);
void astime__evaluate(const astime_routine *r, const astime_state *st, int64_t t,
                      int64_t armed, int64_t grace, astime__eval *out);
astime_err astime__next_due(const astime_routine *r, const astime_state *st,
                            int64_t now, int64_t armed, int64_t grace, int64_t *out);
/* Minimum gap in seconds between consecutive fires after now (trigger or
 * watch), 0 when unknown/one-shot. */
int64_t astime__min_gap(const astime_routine *r, int64_t now);

#endif /* ASTIME_INTERNAL_H */
