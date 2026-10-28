/*
 * schedule.c — when a routine is due. Pure functions of (routine, state, t).
 *
 * A trigger produces slots: cron fire times, a grid every_s apart that
 * continues from the last consumed slot (or starts when the routine is
 * armed), or the one-shot instant. A slot is pending at t when it is the
 * latest slot <= t, newer than the last consumed one, not before the
 * routine was armed, and at most `grace` old: missed slots collapse into
 * one catch-up run and a long outage drops them. A watch is polled every
 * watch_every_s from the last poll (or from arming).
 */

#include <string.h>

#include "astime_internal.h"

#define ON_TIME 300 /* a slot served within 5 minutes is "on time" */

int64_t astime__armed(const astime_routine *r, const astime_state *st, int64_t fallback) {
  char sig[ASTIME_SIG_MAX];
  if (!st || !st->armed_at) return fallback;
  if (st->trigger_sig[0]) {
    astime__trigger_sig(r, sig);
    if (strcmp(sig, st->trigger_sig) != 0) return fallback;
  }
  return st->armed_at;
}

static int64_t last_watch(const astime_routine *r, const astime_state *st) {
  char sig[ASTIME_SIG_MAX];
  if (!st) return 0;
  if (st->watch_sig[0]) {
    astime__watch_sig(r, sig);
    if (strcmp(sig, st->watch_sig) != 0) return 0;
  }
  return st->last_watch_at;
}

static bool sched_pending(const astime_routine *r, const astime_state *st, int64_t t,
                          int64_t armed, int64_t grace, int64_t *slot) {
  const astime_trigger *tr = &r->trigger;
  int64_t last = st ? st->last_due_at : 0, p = 0;
  switch (tr->kind) {
  case ASTIME_TRIGGER_CRON:
    if (astime_cron_prev(&tr->cron, t, tr->tz, &p) != ASTIME_OK) return false;
    break;
  case ASTIME_TRIGGER_EVERY: {
    int64_t base = last > 0 ? last : armed;
    if (tr->every_s <= 0 || t < base) return false;
    p = base + astime__floor_div(t - base, tr->every_s) * tr->every_s;
    break;
  }
  case ASTIME_TRIGGER_AT:
    p = tr->at;
    if (p > t) return false;
    break;
  case ASTIME_TRIGGER_NONE:
  default:
    return false;
  }
  if (p <= last || p < armed) return false;
  if (t - p > (grace > ON_TIME ? grace : ON_TIME)) return false;
  *slot = p;
  return true;
}

static bool next_slot_after(const astime_routine *r, const astime_state *st, int64_t t,
                            int64_t armed, int64_t *out) {
  const astime_trigger *tr = &r->trigger;
  int64_t last = st ? st->last_due_at : 0;
  switch (tr->kind) {
  case ASTIME_TRIGGER_CRON:
    return astime_cron_next(&tr->cron, t, tr->tz, out) == ASTIME_OK;
  case ASTIME_TRIGGER_EVERY: {
    int64_t base = last > 0 ? last : armed, n;
    if (tr->every_s <= 0) return false;
    if (t < base)
      n = last > 0 ? base + tr->every_s : base;
    else
      n = base + (astime__floor_div(t - base, tr->every_s) + 1) * tr->every_s;
    while (n <= t) n += tr->every_s;
    *out = n;
    return true;
  }
  case ASTIME_TRIGGER_AT:
    if (tr->at > t && tr->at > last) {
      *out = tr->at;
      return true;
    }
    return false;
  case ASTIME_TRIGGER_NONE:
  default:
    return false;
  }
}

static bool capped(const astime_routine *r, const astime_state *st, int64_t t) {
  char day[11];
  if (!r->max_runs_per_day || !st || st->runs_today < r->max_runs_per_day) return false;
  astime__day_text(t, r->trigger.tz, day);
  return strcmp(day, st->runs_day) == 0;
}

static int64_t next_midnight(int64_t t, astime_tz tz) {
  astime__wall w;
  astime__to_wall(t, tz, &w);
  return astime__wall_instant((w.days + 1) * 86400, tz);
}

void astime__evaluate(const astime_routine *r, const astime_state *st, int64_t t,
                      int64_t armed, int64_t grace, astime__eval *out) {
  int64_t slot = 0;
  memset(out, 0, sizeof *out);
  if (sched_pending(r, st, t, armed, grace, &slot)) {
    out->slot_pending = true;
    out->sched_slot = slot;
  }
  if (r->watch_kind != ASTIME_WATCH_NONE) {
    int64_t lw = last_watch(r, st);
    int64_t w = lw > 0 ? lw + r->watch_every_s : armed;
    if (t >= w) {
      out->watch_due = true;
      if (!out->slot_pending) slot = w;
    }
  }
  if (!r->enabled) return;
  if (st && st->defer_until > t) return;
  if (capped(r, st, t)) return;
  if (out->slot_pending) {
    out->due = true;
    out->reason = t - out->sched_slot <= ON_TIME ? ASTIME_DUE_SCHEDULE : ASTIME_DUE_CATCH_UP;
    out->slot = out->sched_slot;
  } else if (out->watch_due) {
    out->due = true;
    out->reason = ASTIME_DUE_WATCH;
    out->slot = slot;
  }
}

astime_err astime__next_due(const astime_routine *r, const astime_state *st, int64_t now,
                            int64_t armed, int64_t grace, int64_t *out) {
  int64_t t = now;
  int iter;
  if (!r || !out) return ASTIME_ERR_INVALID;
  if (!r->enabled) return ASTIME_ERR_NOT_FOUND;
  astime__tz_refresh();
  for (iter = 0; iter < 64; iter++) {
    astime__eval ev;
    int64_t cand = INT64_MAX, n;
    if (st && st->defer_until > t) {
      t = st->defer_until;
      continue;
    }
    if (capped(r, st, t)) {
      t = next_midnight(t, r->trigger.tz);
      continue;
    }
    astime__evaluate(r, st, t, armed, grace, &ev);
    if (ev.due) {
      *out = t;
      return ASTIME_OK;
    }
    if (next_slot_after(r, st, t, armed, &n) && n > t) cand = n;
    if (r->watch_kind != ASTIME_WATCH_NONE) {
      int64_t lw = last_watch(r, st);
      int64_t w = lw > 0 ? lw + r->watch_every_s : armed;
      if (w > t && w < cand) cand = w;
    }
    if (cand == INT64_MAX) return ASTIME_ERR_NOT_FOUND;
    t = cand;
  }
  return ASTIME_ERR_NOT_FOUND;
}

astime_err astime_next_due(const astime_routine *r, const astime_state *st, int64_t now,
                           int64_t *out) {
  if (!r || !out) return ASTIME_ERR_INVALID;
  return astime__next_due(r, st, now, astime__armed(r, st, now), ASTIME_CATCH_UP_DEFAULT, out);
}

astime_err astime_next_fires(const astime_routine *r, const astime_state *st, int64_t now,
                             int64_t *out, size_t want, size_t *got) {
  const astime_trigger *tr;
  size_t n = 0;
  if (!r || (!out && want) || !got) return ASTIME_ERR_INVALID;
  tr = &r->trigger;
  *got = 0;
  astime__tz_refresh();
  if (tr->kind == ASTIME_TRIGGER_CRON) {
    int64_t t = now;
    while (n < want && astime_cron_next(&tr->cron, t, tr->tz, &t) == ASTIME_OK) out[n++] = t;
  } else if (tr->kind == ASTIME_TRIGGER_EVERY && tr->every_s > 0) {
    int64_t last = st ? st->last_due_at : 0, t;
    if (last > 0) {
      t = last + tr->every_s;
      if (t <= now) t = last + (astime__floor_div(now - last, tr->every_s) + 1) * tr->every_s;
    } else {
      t = now; /* the first run happens when the routine is armed */
    }
    for (; n < want; t += tr->every_s) out[n++] = t;
  } else if (tr->kind == ASTIME_TRIGGER_AT) {
    if (want > 0 && tr->at > now && (!st || tr->at > st->last_due_at)) out[n++] = tr->at;
  }
  *got = n;
  return ASTIME_OK;
}

bool astime_in_quiet_hours(const astime_routine *r, int64_t t, int64_t *out_end) {
  astime__wall w;
  int m, s, e;
  int64_t end_day;
  bool in;
  if (!r || !r->quiet_set || r->quiet_start_min == r->quiet_end_min) return false;
  astime__tz_refresh();
  astime__to_wall(t, r->trigger.tz, &w);
  m = w.hour * 60 + w.min;
  s = r->quiet_start_min;
  e = r->quiet_end_min;
  end_day = w.days;
  if (s < e) {
    in = m >= s && m < e;
  } else {
    in = m >= s || m < e;
    if (in && m >= s) end_day++;
  }
  if (!in) return false;
  if (out_end) *out_end = astime__wall_instant(end_day * 86400 + (int64_t)e * 60, r->trigger.tz);
  return true;
}

int64_t astime__min_gap(const astime_routine *r, int64_t now) {
  int64_t gap = 0;
  const astime_trigger *tr = &r->trigger;
  if (tr->kind == ASTIME_TRIGGER_EVERY) {
    gap = tr->every_s;
  } else if (tr->kind == ASTIME_TRIGGER_CRON) {
    int64_t prev, t;
    int i;
    astime__tz_refresh();
    if (astime_cron_next(&tr->cron, now, tr->tz, &prev) == ASTIME_OK) {
      for (i = 0; i < 400; i++) {
        if (astime_cron_next(&tr->cron, prev, tr->tz, &t) != ASTIME_OK) break;
        if (gap == 0 || t - prev < gap) gap = t - prev;
        if (t - now > 8 * 86400) break;
        prev = t;
      }
    }
  }
  if (r->watch_kind != ASTIME_WATCH_NONE && r->watch_every_s > 0 &&
      (gap == 0 || r->watch_every_s < gap))
    gap = r->watch_every_s;
  return gap;
}
