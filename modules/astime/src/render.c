/*
 * render.c — routines in words: trigger descriptions, the dry-run preview
 * and the static lint.
 *
 * Output is plain text meant for a chat message: one fact per line, lists
 * as "  - item". Everything derives from the routine, the state and the
 * `now` passed in, so the text is reproducible in tests.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astime_internal.h"

static const char *const DAY_NAME[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                       "Thursday", "Friday", "Saturday"};
static const char *const MONTH_NAME[] = {"January", "February", "March", "April",
                                         "May", "June", "July", "August",
                                         "September", "October", "November", "December"};

/* "a", "a and b", "a, b and c". */
static void join(core_buf *b, const char *const *items, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    if (i > 0) core_buf_appends(b, i + 1 == n ? " and " : ", ");
    core_buf_appends(b, items[i]);
  }
}

static size_t bits(uint64_t mask, int lo, int hi, int *out) {
  size_t n = 0;
  int v;
  for (v = lo; v <= hi; v++)
    if ((mask >> v) & 1u) out[n++] = v;
  return n;
}

/* Step k > 1 starting at lo covering lo..hi exactly; 0 when not a step. */
static int step_of(const int *v, size_t n, int lo, int hi) {
  int k;
  size_t i;
  if (n < 2 || v[0] != lo) return 0;
  k = v[1] - v[0];
  if (k < 2) return 0;
  for (i = 1; i < n; i++)
    if (v[i] - v[i - 1] != k) return 0;
  return v[n - 1] + k > hi ? k : 0;
}

static bool contiguous(const int *v, size_t n) {
  size_t i;
  for (i = 1; i < n; i++)
    if (v[i] != v[i - 1] + 1) return false;
  return n > 0;
}

static void two_digit_list(core_buf *b, const int *v, size_t n) {
  char tmp[64][4];
  const char *items[64];
  size_t i;
  for (i = 0; i < n && i < 64; i++) {
    snprintf(tmp[i], sizeof tmp[i], "%02d", v[i]);
    items[i] = tmp[i];
  }
  join(b, items, i);
}

/* "during hours 08-18", "during hours 08, 12 and 18" */
static void hours_phrase(core_buf *b, const int *h, size_t nh) {
  core_buf_appends(b, "during hours ");
  if (nh > 2 && contiguous(h, nh))
    core_buf_printf(b, "%02d-%02d", h[0], h[nh - 1]);
  else
    two_digit_list(b, h, nh);
}

static void time_phrase(core_buf *b, const astime_cron *c) {
  int m[60], h[24];
  size_t nm = bits(c->minute, 0, 59, m), nh = bits(c->hour, 0, 23, h), i;
  bool full_h = nh == 24;
  int mstep = step_of(m, nm, 0, 59), hstep = step_of(h, nh, 0, 23);
  if (nm == 1) {
    if (full_h) {
      core_buf_printf(b, "every hour at minute %02d", m[0]);
    } else if (hstep) {
      core_buf_printf(b, "every %d hours at minute %02d", hstep, m[0]);
    } else if (nh > 6 && contiguous(h, nh)) {
      core_buf_printf(b, "every hour from %02d:%02d to %02d:%02d", h[0], m[0], h[nh - 1], m[0]);
    } else {
      char tmp[24][8];
      const char *items[24];
      for (i = 0; i < nh; i++) {
        snprintf(tmp[i], sizeof tmp[i], "%02d:%02d", h[i], m[0]);
        items[i] = tmp[i];
      }
      core_buf_appends(b, "at ");
      join(b, items, nh);
    }
    return;
  }
  if (nm == 60) {
    core_buf_appends(b, "every minute");
  } else if (mstep) {
    core_buf_printf(b, "every %d minutes", mstep);
  } else {
    core_buf_appends(b, "at minutes ");
    two_digit_list(b, m, nm);
    if (full_h) core_buf_appends(b, " past every hour");
  }
  if (!full_h) {
    core_buf_appendc(b, ' ');
    if (hstep)
      core_buf_printf(b, "every %d hours", hstep);
    else
      hours_phrase(b, h, nh);
  }
}

static void dow_phrase(core_buf *b, uint8_t dow) {
  const char *items[7];
  size_t n = 0;
  int d;
  if (dow == 0x3E) {
    core_buf_appends(b, "on weekdays");
    return;
  }
  if (dow == 0x41) {
    core_buf_appends(b, "on weekends");
    return;
  }
  for (d = 1; d <= 7; d++) /* Monday first, Sunday last */
    if ((dow >> (d % 7)) & 1u) items[n++] = DAY_NAME[d % 7];
  core_buf_appends(b, "every ");
  join(b, items, n);
}

static void dom_phrase(core_buf *b, uint32_t dom) {
  int d[31];
  size_t n = bits(dom, 1, 31, d);
  if (n == 1)
    core_buf_printf(b, "on day %d", d[0]);
  else {
    char tmp[31][4];
    const char *items[31];
    size_t i;
    for (i = 0; i < n; i++) {
      snprintf(tmp[i], sizeof tmp[i], "%d", d[i]);
      items[i] = tmp[i];
    }
    core_buf_appends(b, "on days ");
    join(b, items, n);
  }
}

static void months_list(core_buf *b, uint16_t month) {
  const char *items[12];
  size_t n = 0;
  int m;
  for (m = 1; m <= 12; m++)
    if ((month >> m) & 1u) items[n++] = MONTH_NAME[m - 1];
  join(b, items, n);
}

static void describe_cron(core_buf *b, const astime_trigger *t) {
  const astime_cron *c = &t->cron;
  bool dom_r = (c->dom & 0xFFFFFFFEu) != 0xFFFFFFFEu;
  bool dow_r = (c->dow & 0x7F) != 0x7F;
  bool months_full = (c->month & 0x1FFE) == 0x1FFE, clock;
  core_buf tp;
  core_buf_init(&tp);
  time_phrase(&tp, c);
  /* Times of day ("at 08:00") read after the days; recurrences inside the
   * day ("every 15 minutes") stand alone or follow a comma. */
  clock = core_str_has_prefix(tp.data, "at ") && tp.data[3] >= '0' && tp.data[3] <= '9';
  if (!dom_r && !dow_r) {
    core_buf_appends(b, clock ? "every day " : "");
    core_buf_appends(b, tp.data);
    if (!months_full) {
      core_buf_appends(b, ", only in ");
      months_list(b, c->month);
    }
  } else {
    if (dom_r) {
      dom_phrase(b, c->dom);
      if (months_full) {
        core_buf_appends(b, " of the month");
      } else {
        core_buf_appends(b, " of ");
        months_list(b, c->month);
      }
    }
    if (dom_r && dow_r) {
      bool both = (c->flags & (ASTIME_CRON_DOM_STAR | ASTIME_CRON_DOW_STAR)) != 0;
      core_buf_appends(b, both ? " when it is also " : " or ");
    }
    if (dow_r) dow_phrase(b, c->dow);
    if (!dom_r && !months_full) {
      core_buf_appends(b, ", only in ");
      months_list(b, c->month);
    }
    core_buf_appends(b, clock ? " " : ", ");
    core_buf_appends(b, tp.data);
  }
  core_buf_free(&tp);
  core_buf_printf(b, " (cron \"%s\", %s)", t->cron_text,
                  t->tz == ASTIME_TZ_UTC ? "UTC" : "local time");
}

/* "30 minutes", "1 hour and 30 minutes", "2 days" */
static void duration_words(core_buf *b, int64_t s) {
  static const char *const SING[] = {"day", "hour", "minute", "second"};
  static const char *const PLUR[] = {"days", "hours", "minutes", "seconds"};
  int64_t parts[4];
  char tmp[4][32];
  const char *items[4];
  size_t n = 0, i;
  parts[0] = s / 86400;
  parts[1] = (s % 86400) / 3600;
  parts[2] = (s % 3600) / 60;
  parts[3] = s % 60;
  for (i = 0; i < 4; i++) {
    if (!parts[i]) continue;
    snprintf(tmp[n], sizeof tmp[n], "%lld %s", (long long)parts[i],
             parts[i] == 1 ? SING[i] : PLUR[i]);
    items[n] = tmp[n];
    n++;
  }
  join(b, items, n);
}

static void every_words(core_buf *b, int64_t s) {
  if (s == 86400) core_buf_appends(b, "every day");
  else if (s == 3600) core_buf_appends(b, "every hour");
  else if (s == 60) core_buf_appends(b, "every minute");
  else {
    core_buf_appends(b, "every ");
    duration_words(b, s);
  }
}

static void describe(core_buf *b, const astime_trigger *t) {
  char when[ASTIME_TIME_TEXT_SIZE];
  switch (t->kind) {
  case ASTIME_TRIGGER_CRON:
    describe_cron(b, t);
    break;
  case ASTIME_TRIGGER_EVERY:
    every_words(b, t->every_s);
    break;
  case ASTIME_TRIGGER_AT:
    astime_format_time(t->at, t->tz, when);
    when[10] = '\0';
    core_buf_printf(b, "once, on %s at %.5s (%s)", when, when + 11,
                    t->tz == ASTIME_TZ_UTC ? "UTC" : "local time");
    break;
  case ASTIME_TRIGGER_NONE:
  default:
    core_buf_appends(b, "no schedule: watch only");
    break;
  }
}

astime_err astime_describe_trigger(const astime_trigger *t, char **out_text) {
  core_buf b;
  if (!t || !out_text) return ASTIME_ERR_INVALID;
  core_buf_init(&b);
  astime__tz_refresh();
  describe(&b, t);
  if (!b.data && core_buf_appends(&b, "") != CORE_OK) return ASTIME_ERR_NOMEM;
  *out_text = core_buf_detach(&b);
  return *out_text ? ASTIME_OK : ASTIME_ERR_NOMEM;
}

/* ── lint ─────────────────────────────────────────────────────────────── */

static bool runs_without_tools(const astime_routine *r) {
  return r->mode == ASTIME_MODE_CHAT || r->profile == ASTIME_PROFILE_CHAT;
}

static bool profile_mutates(astime_profile p) {
  return p == ASTIME_PROFILE_CODING_SANDBOXED || p == ASTIME_PROFILE_AUTOMATION_CI;
}

static void paid_list(core_buf *b, unsigned providers) {
  const char *items[4];
  size_t n = 0;
  unsigned bit;
  for (bit = ASTIME_PROVIDER_CHATGPT; bit <= ASTIME_PROVIDER_CLAUDE_CODE; bit <<= 1)
    if (providers & bit) items[n++] = astime_provider_name(bit);
  join(b, items, n);
}

static void quiet_text(const astime_routine *r, char out[32]) {
  snprintf(out, 32, "%02d:%02d-%02d:%02d", r->quiet_start_min / 60, r->quiet_start_min % 60,
           r->quiet_end_min / 60, r->quiet_end_min % 60);
}

static astime_err push_buf(astime_strings *out, core_buf *b) {
  if (!b->data) return ASTIME_ERR_NOMEM;
  return astime__strings_push(out, core_buf_detach(b));
}

astime_err astime_lint(const astime_routine *r, int64_t now, astime_strings *out) {
  int64_t gap;
  astime_err e = ASTIME_OK;
  core_buf b;
  if (!r || !out) return ASTIME_ERR_INVALID;
  out->items = NULL;
  out->n = 0;
  astime__tz_refresh();
  gap = astime__min_gap(r, now);
  core_buf_init(&b);

  if (!r->enabled) {
    core_buf_appends(&b, "The routine is disabled: it will not run until it is enabled.");
    if ((e = push_buf(out, &b)) != ASTIME_OK) goto done;
  }
  if (r->watch_kind == ASTIME_WATCH_URL && profile_mutates(r->profile) && !runs_without_tools(r)) {
    core_buf_printf(&b, "watch.url reads untrusted web content and profile \"%s\" allows "
                        "changes: every action will require approval.",
                    astime_profile_name(r->profile));
    if ((e = push_buf(out, &b)) != ASTIME_OK) goto done;
  }
  if ((r->providers & ASTIME_PROVIDER_PAID) && gap > 0 && gap < 3600 &&
      (r->max_runs_per_day == 0 || r->max_runs_per_day > 24)) {
    core_buf_appends(&b, "Paid providers (");
    paid_list(&b, r->providers);
    core_buf_appends(&b, ") on a frequent schedule (");
    every_words(&b, gap);
    core_buf_appends(&b, "): every run uses subscription quota; restrict to "
                         "providers: [\"local\"] or set max_runs_per_day.");
    if ((e = push_buf(out, &b)) != ASTIME_OK) goto done;
  }
  if (gap > 0 && gap < 300) {
    core_buf_appends(&b, "The interval is shorter than 5 minutes (");
    every_words(&b, gap);
    core_buf_appends(&b, "): it loads the daemon and the model for little benefit.");
    if ((e = push_buf(out, &b)) != ASTIME_OK) goto done;
  }
  if (r->quiet_set) {
    char q[32];
    quiet_text(r, q);
    if (r->quiet_start_min == r->quiet_end_min) {
      core_buf_printf(&b, "quiet_hours \"%s\" is an empty window: it silences nothing.", q);
      if ((e = push_buf(out, &b)) != ASTIME_OK) goto done;
    } else if (r->quiet_start_min == 0 && r->quiet_end_min == 1440) {
      core_buf_printf(&b, "quiet_hours \"%s\" covers all 24 hours: every delivery will be "
                          "deferred.", q);
      if ((e = push_buf(out, &b)) != ASTIME_OK) goto done;
    } else if (r->notify != ASTIME_NOTIFY_NEVER && r->trigger.kind != ASTIME_TRIGGER_NONE) {
      int64_t fires[16], end = 0;
      size_t got = 0, i;
      bool hit = false;
      (void)astime_next_fires(r, NULL, now, fires, 16, &got);
      for (i = 0; i < got && !hit; i++) hit = astime_in_quiet_hours(r, fires[i], &end);
      if (hit) {
        core_buf_printf(&b, "Some runs fall inside quiet hours (%s): deliveries will arrive "
                            "at %02d:%02d.",
                        q, (r->quiet_end_min / 60) % 24, r->quiet_end_min % 60);
        if ((e = push_buf(out, &b)) != ASTIME_OK) goto done;
      }
    }
  }
  if (r->trigger.kind == ASTIME_TRIGGER_AT && r->trigger.at <= now) {
    char when[ASTIME_TIME_TEXT_SIZE];
    astime_format_time(r->trigger.at, r->trigger.tz, when);
    core_buf_printf(&b, "The one-shot time (%s) is in the past: it will not run.", when);
    if ((e = push_buf(out, &b)) != ASTIME_OK) goto done;
  }
  if (r->tools_set && r->tools_n > 0 && runs_without_tools(r)) {
    core_buf_appends(&b, "tools is set, but the chat mode or profile runs without tools: "
                         "the list will be ignored.");
    if ((e = push_buf(out, &b)) != ASTIME_OK) goto done;
  }
done:
  core_buf_free(&b);
  if (e != ASTIME_OK) astime_strings_free(out);
  return e;
}

/* ── dry-run ──────────────────────────────────────────────────────────── */

static void fire_line(core_buf *b, int64_t t, astime_tz tz) {
  char when[ASTIME_TIME_TEXT_SIZE];
  astime__wall w;
  astime_format_time(t, tz, when);
  astime__to_wall(t, tz, &w);
  core_buf_printf(b, "  - %s (%s)\n", when, DAY_NAME[w.wday]);
}

static const char *status_words(astime_status s) {
  switch (s) {
  case ASTIME_STATUS_OK: return "succeeded";
  case ASTIME_STATUS_FAILED: return "failed";
  case ASTIME_STATUS_SKIPPED: return "skipped";
  case ASTIME_STATUS_DEFERRED: return "deferred";
  case ASTIME_STATUS_NONE: break;
  }
  return "no result yet";
}

static const char *profile_words(astime_profile p) {
  switch (p) {
  case ASTIME_PROFILE_CODING_READONLY: return "read-only tools";
  case ASTIME_PROFILE_CODING_SANDBOXED: return "sandboxed tools, changes ask for approval";
  case ASTIME_PROFILE_AUTOMATION_CI:
    return "automation: non-destructive actions run without confirmation";
  case ASTIME_PROFILE_CHAT: break;
  }
  return "conversation only, no tools";
}

astime_err astime_dry_run(const astime_routine *r, const astime_state *st, int64_t now,
                          char **out_text) {
  core_buf b;
  astime_strings warn = {NULL, 0};
  int64_t fires[3];
  size_t got = 0, i;
  unsigned bit;
  bool first;
  astime_err e;
  if (!r || !out_text || !r->name || !r->goal) return ASTIME_ERR_INVALID;
  *out_text = NULL;
  astime__tz_refresh();
  core_buf_init(&b);

  core_buf_printf(&b, "Routine \"%s\" (%s)\n", r->name, r->enabled ? "enabled" : "disabled");

  core_buf_appends(&b, "When: ");
  describe(&b, &r->trigger);
  core_buf_appendc(&b, '\n');

  if (r->trigger.kind != ASTIME_TRIGGER_NONE) {
    (void)astime_next_fires(r, st, now, fires, 3, &got);
    core_buf_appends(&b, "Next runs:");
    if (got == 0) core_buf_appends(&b, " none");
    core_buf_appendc(&b, '\n');
    for (i = 0; i < got; i++) fire_line(&b, fires[i], r->trigger.tz);
  } else if (r->watch_kind != ASTIME_WATCH_NONE) {
    int64_t t = st && st->last_watch_at ? st->last_watch_at + r->watch_every_s : now;
    if (t < now) t = now;
    core_buf_appends(&b, "Next checks:\n");
    for (i = 0; i < 3; i++) fire_line(&b, t + (int64_t)i * r->watch_every_s, r->trigger.tz);
  }

  if (r->watch_kind != ASTIME_WATCH_NONE) {
    core_buf_printf(&b, "Watches: the %s %s, checked ",
                    r->watch_kind == ASTIME_WATCH_URL ? "page" : "file", r->watch_target);
    every_words(&b, r->watch_every_s);
    core_buf_appends(&b, "; the routine runs only when the content changes\n");
  }

  core_buf_printf(&b, "Goal: %s\n", r->goal);
  core_buf_printf(&b, "Mode: %s, profile %s (%s)\n", astime_mode_name(r->mode),
                  astime_profile_name(r->profile), profile_words(r->profile));

  core_buf_appends(&b, "Tools: ");
  if (runs_without_tools(r)) {
    core_buf_appends(&b, "none (the chat mode or profile runs without tools)");
  } else if (!r->tools_set) {
    core_buf_appends(&b, "all enabled tools the profile allows");
  } else if (r->tools_n == 0) {
    core_buf_appends(&b, "none");
  } else {
    for (i = 0; i < r->tools_n; i++) core_buf_printf(&b, "%s%s", i ? ", " : "", r->tools[i]);
  }
  core_buf_appendc(&b, '\n');

  core_buf_appends(&b, "Providers: ");
  first = true;
  for (bit = ASTIME_PROVIDER_LOCAL; bit <= ASTIME_PROVIDER_CLAUDE_CODE; bit <<= 1) {
    if (!(r->providers & bit)) continue;
    core_buf_printf(&b, "%s%s", first ? "" : ", ", astime_provider_name(bit));
    first = false;
  }
  core_buf_appendc(&b, '\n');
  core_buf_appends(&b, "Paid inference: ");
  if (r->providers & ASTIME_PROVIDER_PAID) {
    core_buf_appends(&b, "yes, ");
    paid_list(&b, r->providers);
    core_buf_appends(&b, " (subscription quota)\n");
  } else {
    core_buf_appends(&b, "no\n");
  }
  core_buf_appends(&b, "Maximum cost: ");
  if (!(r->providers & ASTIME_PROVIDER_PAID))
    core_buf_appends(&b, "€0 (local model only)\n");
  else if (r->max_runs_per_day > 0)
    core_buf_printf(&b, "nothing beyond the subscription; at most %d runs a day use its quota\n",
                    r->max_runs_per_day);
  else
    core_buf_appends(&b, "nothing beyond the subscription, but quota use has no daily cap "
                         "(set max_runs_per_day)\n");

  core_buf_appends(&b, "Delivery: ");
  if (r->deliver_channel) {
    core_buf_appends(&b, r->deliver_channel);
    if (r->deliver_star) core_buf_printf(&b, ", star \"%s\"", r->deliver_star);
  } else {
    core_buf_appends(&b, "default channel");
  }
  switch (r->notify) {
  case ASTIME_NOTIFY_ON_CHANGE:
    core_buf_appends(&b, "; notify only when the result changes\n");
    break;
  case ASTIME_NOTIFY_NEVER:
    core_buf_appends(&b, "; never notify\n");
    break;
  case ASTIME_NOTIFY_ALWAYS:
  default:
    core_buf_appends(&b, "; always notify\n");
    break;
  }

  core_buf_appends(&b, "Quiet hours: ");
  if (!r->quiet_set) {
    core_buf_appends(&b, "none\n");
  } else {
    char q[32];
    quiet_text(r, q);
    if (r->quiet_start_min == r->quiet_end_min)
      core_buf_printf(&b, "%s (empty window)\n", q);
    else
      core_buf_printf(&b, "%s (deliveries in this window arrive at %02d:%02d)\n",
                      q, (r->quiet_end_min / 60) % 24, r->quiet_end_min % 60);
  }

  core_buf_appends(&b, "Limit: ");
  if (r->max_runs_per_day > 0)
    core_buf_printf(&b, "at most %d runs a day\n", r->max_runs_per_day);
  else
    core_buf_appends(&b, "no daily limit\n");

  if (st && st->last_run_at) {
    char when[ASTIME_TIME_TEXT_SIZE];
    astime_format_time(st->last_run_at, r->trigger.tz, when);
    core_buf_printf(&b, "Last run: %s, %s", when,
                    st->running_since ? "running" : status_words(st->last_status));
    if (st->last_error[0]) core_buf_printf(&b, " (%s)", st->last_error);
    core_buf_appendc(&b, '\n');
    if (st->consecutive_failures > 0)
      core_buf_printf(&b, "Consecutive failures: %d\n", st->consecutive_failures);
  }

  e = astime_lint(r, now, &warn);
  if (e == ASTIME_OK && warn.n) {
    core_buf_appends(&b, "Warnings:\n");
    for (i = 0; i < warn.n; i++) core_buf_printf(&b, "  - %s\n", warn.items[i]);
  }
  astime_strings_free(&warn);
  if (e == ASTIME_OK && !b.data) e = ASTIME_ERR_NOMEM;
  if (e != ASTIME_OK) {
    core_buf_free(&b);
    return e;
  }
  *out_text = core_buf_detach(&b);
  return *out_text ? ASTIME_OK : ASTIME_ERR_NOMEM;
}
