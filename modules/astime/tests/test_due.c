/*
 * test_due.c — the scheduler: on-time slots, arming, missed-run catch-up
 * without backlog, intervals, one-shots that disable themselves, quiet
 * hours, daily caps, watches, deferrals, running exclusion, trigger
 * changes, next_due and concurrent callers.
 */

#include "astime_test.h"

typedef struct {
  char dir[256];
  astime_store *s;
} fixture;

/* One routine file named `name` with `body` inside #routine { ... }. */
static int fx_open(fixture *f, const char *name, const char *body) {
  char file[96], text[2048];
  if (!test_tmpdir(f->dir)) return 0;
  snprintf(file, sizeof file, "%s.xcdn", name);
  snprintf(text, sizeof text, "#routine { name: \"%s\", goal: \"do it\", %s }\n", name, body);
  if (!test_write(f->dir, file, text)) return 0;
  return astime_store_open(f->dir, &f->s) == ASTIME_OK;
}

static void fx_close(fixture *f) {
  astime_store_close(f->s);
  test_rmtree(f->dir);
}

/* Number of due routines at t; the first item copied to *first. */
static size_t due_at(fixture *f, int64_t t, astime_due_item *first) {
  astime_due_list l;
  size_t n;
  if (astime_due(f->s, t, &l) != ASTIME_OK) return 999;
  n = l.n;
  if (n && first) *first = l.items[0];
  astime_due_list_free(&l);
  return n;
}

static int run(fixture *f, const char *name, int64_t t, astime_status st, const char *hash) {
  return astime_mark_started(f->s, name, t) == ASTIME_OK &&
         astime_mark_finished(f->s, name, t + 30, st, NULL, hash) == ASTIME_OK;
}

static int64_t next_due(fixture *f, const char *name, int64_t now) {
  astime_routine r;
  astime_state st;
  int64_t out = -1;
  if (astime_store_get(f->s, name, &r, &st) != ASTIME_OK) return -2;
  if (astime_next_due(&r, &st, now, &out) != ASTIME_OK) out = 0;
  astime_routine_free(&r);
  return out;
}

#define DAY 86400

TEST(cron_on_time_and_not_twice) {
  fixture f;
  astime_due_item it;
  int64_t d1 = utc(2026, 10, 26, 0, 0);
  ASSERT_TRUE(fx_open(&f, "brief", "enabled: true, trigger: { cron: \"0 8 * * *\", tz: \"utc\" }"));
  ASSERT_EQ_INT(due_at(&f, d1 + 7 * 3600, NULL), 0); /* arms at 07:00 */
  ASSERT_EQ_INT(due_at(&f, d1 + 8 * 3600 + 20, &it), 1);
  ASSERT_EQ_STR(it.name, "brief");
  ASSERT_EQ_INT(it.reason, ASTIME_DUE_SCHEDULE);
  ASSERT_EQ_INT(it.due_at, d1 + 8 * 3600);
  ASSERT_EQ_INT(it.deliver_after, 0);
  ASSERT_EQ_INT(it.watch, ASTIME_WATCH_NONE);
  /* still due until the run starts (the daemon may poll twice) */
  ASSERT_EQ_INT(due_at(&f, d1 + 8 * 3600 + 25, NULL), 1);
  ASSERT_TRUE(run(&f, "brief", d1 + 8 * 3600 + 30, ASTIME_STATUS_OK, NULL));
  ASSERT_EQ_INT(due_at(&f, d1 + 8 * 3600 + 90, NULL), 0);
  ASSERT_EQ_INT(due_at(&f, d1 + 12 * 3600, NULL), 0);
  ASSERT_EQ_INT(next_due(&f, "brief", d1 + 12 * 3600), d1 + DAY + 8 * 3600);
  ASSERT_EQ_INT(due_at(&f, d1 + DAY + 8 * 3600, &it), 1);
  ASSERT_EQ_INT(it.due_at, d1 + DAY + 8 * 3600);
  fx_close(&f);
}

TEST(slots_before_arming_never_fire) {
  fixture f;
  int64_t d1 = utc(2026, 10, 26, 0, 0);
  ASSERT_TRUE(fx_open(&f, "brief", "enabled: true, trigger: { cron: \"0 8 * * *\", tz: \"utc\" }"));
  /* first seen at 10:00: the 08:00 slot predates the routine */
  ASSERT_EQ_INT(due_at(&f, d1 + 10 * 3600, NULL), 0);
  ASSERT_EQ_INT(next_due(&f, "brief", d1 + 10 * 3600), d1 + DAY + 8 * 3600);
  /* arming survives a restart */
  astime_store_close(f.s);
  ASSERT_OK(astime_store_open(f.dir, &f.s));
  ASSERT_EQ_INT(due_at(&f, d1 + 11 * 3600, NULL), 0);
  ASSERT_EQ_INT(due_at(&f, d1 + DAY + 8 * 3600, NULL), 1);
  fx_close(&f);
}

TEST(missed_run_catch_up_once_and_no_backlog) {
  fixture f;
  astime_due_item it;
  int64_t d1 = utc(2026, 10, 26, 0, 0);
  ASSERT_TRUE(fx_open(&f, "brief", "enabled: true, trigger: { cron: \"0 8 * * *\", tz: \"utc\" }"));
  ASSERT_EQ_INT(due_at(&f, d1 + 7 * 3600, NULL), 0);
  ASSERT_TRUE(run(&f, "brief", d1 + 8 * 3600, ASTIME_STATUS_OK, NULL));
  /* machine off until day 2, 15:00: 7 h late, inside the 12 h grace */
  astime_store_close(f.s);
  ASSERT_OK(astime_store_open(f.dir, &f.s));
  ASSERT_EQ_INT(due_at(&f, d1 + DAY + 15 * 3600, &it), 1);
  ASSERT_EQ_INT(it.reason, ASTIME_DUE_CATCH_UP);
  ASSERT_EQ_INT(it.due_at, d1 + DAY + 8 * 3600);
  ASSERT_TRUE(run(&f, "brief", d1 + DAY + 15 * 3600, ASTIME_STATUS_OK, NULL));
  ASSERT_EQ_INT(due_at(&f, d1 + DAY + 15 * 3600 + 60, NULL), 0);
  /* off again until day 5, 21:00: three slots missed, the last 13 h ago */
  ASSERT_EQ_INT(due_at(&f, d1 + 4 * DAY + 21 * 3600, NULL), 0);
  ASSERT_EQ_INT(next_due(&f, "brief", d1 + 4 * DAY + 21 * 3600), d1 + 5 * DAY + 8 * 3600);
  ASSERT_EQ_INT(due_at(&f, d1 + 5 * DAY + 8 * 3600, &it), 1);
  ASSERT_EQ_INT(it.reason, ASTIME_DUE_SCHEDULE);
  /* a shorter grace drops even the recent slot */
  astime_store_set_catch_up(f.s, 3600);
  ASSERT_TRUE(run(&f, "brief", d1 + 5 * DAY + 8 * 3600, ASTIME_STATUS_OK, NULL));
  ASSERT_EQ_INT(due_at(&f, d1 + 6 * DAY + 10 * 3600, NULL), 0);
  ASSERT_EQ_INT(due_at(&f, d1 + 6 * DAY + 8 * 3600 + 240, NULL), 1); /* on time still runs */
  fx_close(&f);
}

TEST(every_interval_grid) {
  fixture f;
  astime_due_item it;
  int64_t t0 = utc(2026, 10, 26, 10, 0);
  ASSERT_TRUE(fx_open(&f, "poll", "enabled: true, trigger: { every: \"PT30M\" }"));
  ASSERT_EQ_INT(due_at(&f, t0, &it), 1); /* first run when armed */
  ASSERT_EQ_INT(it.due_at, t0);
  ASSERT_TRUE(run(&f, "poll", t0 + 5, ASTIME_STATUS_OK, NULL));
  ASSERT_EQ_INT(due_at(&f, t0 + 29 * 60, NULL), 0);
  ASSERT_EQ_INT(next_due(&f, "poll", t0 + 60), t0 + 1800);
  ASSERT_EQ_INT(due_at(&f, t0 + 1800, &it), 1);
  ASSERT_EQ_INT(it.due_at, t0 + 1800);
  ASSERT_TRUE(run(&f, "poll", t0 + 1800 + 40, ASTIME_STATUS_OK, NULL));
  /* three hours away: one catch-up for the latest grid point only */
  ASSERT_EQ_INT(due_at(&f, t0 + 3 * 3600 + 40 * 60, &it), 1);
  ASSERT_EQ_INT(it.reason, ASTIME_DUE_CATCH_UP);
  ASSERT_EQ_INT(it.due_at, t0 + 3 * 3600 + 30 * 60);
  ASSERT_TRUE(run(&f, "poll", t0 + 3 * 3600 + 40 * 60, ASTIME_STATUS_OK, NULL));
  ASSERT_EQ_INT(due_at(&f, t0 + 3 * 3600 + 50 * 60, NULL), 0);
  ASSERT_EQ_INT(next_due(&f, "poll", t0 + 3 * 3600 + 50 * 60), t0 + 4 * 3600); /* phase kept */
  fx_close(&f);
}

TEST(one_shot_runs_once_then_disables) {
  fixture f;
  astime_due_item it;
  astime_routine r;
  char path[512];
  char *disk;
  int64_t at = utc(2026, 10, 27, 9, 0);
  ASSERT_TRUE(fx_open(&f, "remind", "enabled: true, trigger: { at: \"2026-10-27T10:00:00+01:00\" }"));
  ASSERT_EQ_INT(due_at(&f, at - 3600, NULL), 0);
  ASSERT_EQ_INT(next_due(&f, "remind", at - 3600), at);
  ASSERT_EQ_INT(due_at(&f, at + 10, &it), 1);
  ASSERT_EQ_INT(it.due_at, at);
  ASSERT_TRUE(run(&f, "remind", at + 10, ASTIME_STATUS_OK, NULL));
  ASSERT_OK(astime_store_get(f.s, "remind", &r, NULL));
  ASSERT_TRUE(!r.enabled);
  astime_routine_free(&r);
  snprintf(path, sizeof path, "%s/remind.xcdn", f.dir);
  disk = test_read_file(path, NULL);
  ASSERT_CONTAINS(disk, "enabled: false,");
  ASSERT_CONTAINS(disk, "at: \"2026-10-27T10:00:00+01:00\"");
  free(disk);
  ASSERT_EQ_INT(due_at(&f, at + 120, NULL), 0);
  ASSERT_EQ_INT(next_due(&f, "remind", at + 120), 0);
  /* re-enabled, the consumed slot does not fire again */
  ASSERT_OK(astime_store_set_enabled(f.s, "remind", true));
  ASSERT_EQ_INT(due_at(&f, at + 180, NULL), 0);
  fx_close(&f);
}

TEST(one_shot_failure_stays_enabled) {
  fixture f;
  astime_routine r;
  int64_t at = utc(2026, 10, 27, 9, 0);
  ASSERT_TRUE(fx_open(&f, "remind", "enabled: true, trigger: { at: \"2026-10-27T09:00:00Z\" }"));
  ASSERT_EQ_INT(due_at(&f, at - 60, NULL), 0);
  ASSERT_EQ_INT(due_at(&f, at, NULL), 1);
  ASSERT_TRUE(run(&f, "remind", at, ASTIME_STATUS_FAILED, NULL));
  ASSERT_OK(astime_store_get(f.s, "remind", &r, NULL));
  ASSERT_TRUE(r.enabled);
  astime_routine_free(&r);
  ASSERT_EQ_INT(due_at(&f, at + 300, NULL), 0); /* the slot was consumed */
  fx_close(&f);
}

TEST(quiet_hours_defer_delivery) {
  fixture f;
  astime_due_item it;
  astime_routine r;
  int64_t d1 = utc(2026, 10, 26, 0, 0), end = 0;
  ASSERT_TRUE(fx_open(&f, "early", "enabled: true, trigger: { cron: \"30 6,12,23 * * *\", tz: \"utc\" }, "
                                    "quiet_hours: \"22:00-07:00\""));
  ASSERT_EQ_INT(due_at(&f, d1, NULL), 0);
  ASSERT_EQ_INT(due_at(&f, d1 + 6 * 3600 + 30 * 60, &it), 1);
  ASSERT_EQ_INT(it.deliver_after, d1 + 7 * 3600);
  ASSERT_TRUE(run(&f, "early", d1 + 6 * 3600 + 30 * 60, ASTIME_STATUS_OK, NULL));
  ASSERT_EQ_INT(due_at(&f, d1 + 12 * 3600 + 30 * 60, &it), 1);
  ASSERT_EQ_INT(it.deliver_after, 0);
  ASSERT_TRUE(run(&f, "early", d1 + 12 * 3600 + 30 * 60, ASTIME_STATUS_OK, NULL));
  ASSERT_EQ_INT(due_at(&f, d1 + 23 * 3600 + 30 * 60, &it), 1);
  ASSERT_EQ_INT(it.deliver_after, d1 + DAY + 7 * 3600); /* across midnight */
  ASSERT_OK(astime_store_get(f.s, "early", &r, NULL));
  ASSERT_TRUE(astime_in_quiet_hours(&r, d1 + 22 * 3600, &end));
  ASSERT_EQ_INT(end, d1 + DAY + 7 * 3600);
  ASSERT_TRUE(!astime_in_quiet_hours(&r, d1 + 7 * 3600, NULL));
  ASSERT_TRUE(astime_in_quiet_hours(&r, d1 + 7 * 3600 - 1, NULL));
  /* nothing to deliver, nothing to defer */
  r.notify = ASTIME_NOTIFY_NEVER;
  ASSERT_OK(astime_store_put(f.s, &r, true, NULL));
  ASSERT_TRUE(run(&f, "early", d1 + 23 * 3600 + 30 * 60, ASTIME_STATUS_OK, NULL));
  ASSERT_EQ_INT(due_at(&f, d1 + DAY + 6 * 3600 + 30 * 60, &it), 1);
  ASSERT_EQ_INT(it.deliver_after, 0);
  /* an empty window silences nothing */
  r.quiet_start_min = r.quiet_end_min = 600;
  ASSERT_TRUE(!astime_in_quiet_hours(&r, d1 + 10 * 3600, NULL));
  astime_routine_free(&r);
  fx_close(&f);
}

#if !defined(_WIN32)
TEST(quiet_hours_follow_local_time) {
  fixture f;
  astime_due_item it;
  int64_t d1 = utc(2026, 10, 26, 0, 0); /* CET, UTC+1 */
  test_set_tz(TZ_ROME);
  ASSERT_TRUE(fx_open(&f, "early", "enabled: true, trigger: { cron: \"30 6 * * *\" }, "
                                    "quiet_hours: \"22:00-07:00\""));
  ASSERT_EQ_INT(due_at(&f, d1, NULL), 0);
  ASSERT_EQ_INT(due_at(&f, d1 + 5 * 3600 + 30 * 60, &it), 1); /* 06:30 local */
  ASSERT_EQ_INT(it.due_at, d1 + 5 * 3600 + 30 * 60);
  ASSERT_EQ_INT(it.deliver_after, d1 + 6 * 3600); /* 07:00 local */
  fx_close(&f);
}
#endif

TEST(max_runs_per_day) {
  fixture f;
  int64_t t0 = utc(2026, 10, 26, 22, 0);
  int i;
  ASSERT_TRUE(fx_open(&f, "busy", "enabled: true, trigger: { every: \"PT10M\", tz: \"utc\" }, "
                                   "max_runs_per_day: 3"));
  for (i = 0; i < 3; i++) {
    ASSERT_EQ_INT(due_at(&f, t0 + i * 600, NULL), 1);
    ASSERT_TRUE(run(&f, "busy", t0 + i * 600, ASTIME_STATUS_OK, NULL));
  }
  ASSERT_EQ_INT(due_at(&f, t0 + 3 * 600, NULL), 0);
  ASSERT_EQ_INT(due_at(&f, t0 + 7000, NULL), 0);
  /* the cap lifts at midnight (UTC here): the latest grid point runs */
  ASSERT_EQ_INT(next_due(&f, "busy", t0 + 3 * 600), utc(2026, 10, 27, 0, 0));
  ASSERT_EQ_INT(due_at(&f, utc(2026, 10, 27, 0, 0), NULL), 1);
  /* failures count too, skips do not */
  ASSERT_TRUE(run(&f, "busy", utc(2026, 10, 27, 0, 0), ASTIME_STATUS_FAILED, NULL));
  ASSERT_TRUE(run(&f, "busy", utc(2026, 10, 27, 0, 10), ASTIME_STATUS_SKIPPED, NULL));
  ASSERT_TRUE(run(&f, "busy", utc(2026, 10, 27, 0, 20), ASTIME_STATUS_OK, NULL));
  ASSERT_EQ_INT(due_at(&f, utc(2026, 10, 27, 0, 30), NULL), 1);
  ASSERT_TRUE(run(&f, "busy", utc(2026, 10, 27, 0, 30), ASTIME_STATUS_OK, NULL));
  ASSERT_EQ_INT(due_at(&f, utc(2026, 10, 27, 0, 40), NULL), 0);
  fx_close(&f);
}

TEST(watch_polls_on_interval) {
  fixture f;
  astime_due_item it;
  astime_state st;
  int64_t t0 = utc(2026, 10, 26, 9, 0);
  const char *h1 = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";
  ASSERT_TRUE(fx_open(&f, "page", "enabled: true, watch: { url: \"https://example.com/page\", "
                                   "every: \"PT1H\" }"));
  ASSERT_EQ_INT(due_at(&f, t0, &it), 1);
  ASSERT_EQ_INT(it.reason, ASTIME_DUE_WATCH);
  ASSERT_EQ_INT(it.watch, ASTIME_WATCH_URL);
  ASSERT_EQ_STR(it.last_watch_hash, ""); /* first check: baseline */
  ASSERT_TRUE(run(&f, "page", t0, ASTIME_STATUS_SKIPPED, h1));
  ASSERT_OK(astime_store_get(f.s, "page", NULL, &st));
  ASSERT_EQ_STR(st.last_watch_hash, h1);
  ASSERT_EQ_INT(st.last_watch_at, t0 + 30);
  ASSERT_EQ_INT(due_at(&f, t0 + 1800, NULL), 0);
  ASSERT_EQ_INT(next_due(&f, "page", t0 + 1800), t0 + 30 + 3600);
  ASSERT_EQ_INT(due_at(&f, t0 + 3630, &it), 1);
  ASSERT_EQ_STR(it.last_watch_hash, h1);
  /* a failed fetch (no hash) still waits a full interval */
  ASSERT_OK(astime_mark_started(f.s, "page", t0 + 3630));
  ASSERT_OK(astime_mark_finished(f.s, "page", t0 + 3640, ASTIME_STATUS_FAILED, "fetch failed", NULL));
  ASSERT_EQ_INT(due_at(&f, t0 + 3700, NULL), 0);
  ASSERT_EQ_INT(due_at(&f, t0 + 3630 + 3600, NULL), 1);
  fx_close(&f);
}

TEST(watch_with_trigger_and_file_hash) {
  fixture f;
  astime_due_item it;
  astime_routine r;
  char path[512], h[ASTIME_HASH_MAX], h2[ASTIME_HASH_MAX], text[1024], err[ASTIME_ERR_SIZE];
  int64_t d1 = utc(2026, 10, 26, 0, 0);
  ASSERT_TRUE(test_tmpdir(f.dir));
  snprintf(path, sizeof path, "%s/watched.txt", f.dir);
  ASSERT_TRUE(test_write(f.dir, "watched.txt", "abc"));
  ASSERT_OK(astime_hash_file(path, h));
  ASSERT_EQ_STR(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  astime_hash_bytes("abc", 3, h2);
  ASSERT_EQ_STR(h2, h);
  snprintf(text, sizeof text,
           "#routine { name: \"mix\", enabled: true, goal: \"g\", trigger: { cron: \"0 8 * * *\", "
           "tz: \"utc\" }, watch: { file: \"%s\", every: \"PT10M\" } }", path);
  ASSERT_TRUE(test_write(f.dir, "mix.xcdn", text));
  ASSERT_OK(astime_store_open(f.dir, &f.s));
  /* first poll at arming records the baseline */
  ASSERT_EQ_INT(due_at(&f, d1 + 7 * 3600, &it), 1);
  ASSERT_EQ_INT(it.reason, ASTIME_DUE_WATCH);
  ASSERT_EQ_INT(it.watch, ASTIME_WATCH_FILE);
  ASSERT_EQ_STR(it.last_watch_hash, "");
  ASSERT_TRUE(run(&f, "mix", d1 + 7 * 3600, ASTIME_STATUS_SKIPPED, h));
  ASSERT_EQ_INT(due_at(&f, d1 + 7 * 3600 + 300, NULL), 0);
  ASSERT_EQ_INT(due_at(&f, d1 + 7 * 3600 + 630, &it), 1);
  ASSERT_EQ_STR(it.last_watch_hash, h); /* unchanged: the daemon skips */
  ASSERT_TRUE(run(&f, "mix", d1 + 7 * 3600 + 630, ASTIME_STATUS_SKIPPED, h));
  /* the schedule fires unconditionally and also serves the poll */
  ASSERT_EQ_INT(due_at(&f, d1 + 8 * 3600, &it), 1);
  ASSERT_EQ_INT(it.reason, ASTIME_DUE_SCHEDULE);
  ASSERT_EQ_INT(it.due_at, d1 + 8 * 3600);
  ASSERT_TRUE(run(&f, "mix", d1 + 8 * 3600, ASTIME_STATUS_OK, h));
  ASSERT_EQ_INT(due_at(&f, d1 + 8 * 3600 + 300, NULL), 0);
  /* the file changes: the next poll sees a different hash */
  ASSERT_TRUE(test_write(f.dir, "watched.txt", "abcd"));
  ASSERT_OK(astime_hash_file(path, h2));
  ASSERT_EQ_INT(due_at(&f, d1 + 8 * 3600 + 630, &it), 1);
  ASSERT_EQ_INT(it.reason, ASTIME_DUE_WATCH);
  ASSERT_TRUE(strcmp(it.last_watch_hash, h2) != 0);
  ASSERT_TRUE(run(&f, "mix", d1 + 8 * 3600 + 630, ASTIME_STATUS_OK, h2));
  /* a new target resets the baseline */
  ASSERT_OK(astime_store_get(f.s, "mix", &r, NULL));
  free(r.watch_target);
  snprintf(text, sizeof text, "%s/other.txt", f.dir);
  r.watch_target = core_strdup(text);
  ASSERT_OK(astime_store_put(f.s, &r, true, err));
  astime_routine_free(&r);
  ASSERT_EQ_INT(due_at(&f, d1 + 8 * 3600 + 700, &it), 1);
  ASSERT_EQ_STR(it.last_watch_hash, "");
  fx_close(&f);
}

TEST(deferred_run_is_retried) {
  fixture f;
  astime_due_item it;
  astime_state st;
  int64_t d1 = utc(2026, 10, 26, 0, 0), slot = d1 + 8 * 3600;
  ASSERT_TRUE(fx_open(&f, "brief", "enabled: true, trigger: { cron: \"0 8 * * *\", tz: \"utc\" }"));
  ASSERT_EQ_INT(due_at(&f, d1, NULL), 0);
  ASSERT_EQ_INT(due_at(&f, slot, NULL), 1);
  ASSERT_OK(astime_mark_started(f.s, "brief", slot));
  ASSERT_EQ_INT(due_at(&f, slot + 10, NULL), 0); /* running */
  ASSERT_OK(astime_mark_finished(f.s, "brief", slot + 20, ASTIME_STATUS_DEFERRED, "quota window closed",
                                 NULL));
  ASSERT_OK(astime_store_get(f.s, "brief", NULL, &st));
  ASSERT_EQ_INT(st.defer_until, slot + 20 + ASTIME_DEFER_RETRY);
  ASSERT_EQ_INT(st.last_status, ASTIME_STATUS_DEFERRED);
  ASSERT_EQ_INT(st.runs_today, 0);
  ASSERT_EQ_INT(due_at(&f, slot + 300, NULL), 0);
  ASSERT_EQ_INT(next_due(&f, "brief", slot + 300), slot + 20 + ASTIME_DEFER_RETRY);
  ASSERT_EQ_INT(due_at(&f, slot + 20 + ASTIME_DEFER_RETRY, &it), 1);
  ASSERT_EQ_INT(it.due_at, slot); /* same slot, late */
  ASSERT_EQ_INT(it.reason, ASTIME_DUE_CATCH_UP);
  ASSERT_TRUE(run(&f, "brief", slot + 20 + ASTIME_DEFER_RETRY, ASTIME_STATUS_OK, NULL));
  ASSERT_EQ_INT(due_at(&f, slot + 3600, NULL), 0);
  fx_close(&f);
}

TEST(disabled_and_rearmed_routines) {
  fixture f;
  astime_routine r;
  astime_state st;
  char err[ASTIME_ERR_SIZE];
  int64_t d1 = utc(2026, 10, 26, 0, 0);
  ASSERT_TRUE(fx_open(&f, "brief", "enabled: false, trigger: { cron: \"0 8 * * *\", tz: \"utc\" }"));
  ASSERT_EQ_INT(due_at(&f, d1 + 7 * 3600, NULL), 0);
  ASSERT_EQ_INT(due_at(&f, d1 + 8 * 3600, NULL), 0);
  ASSERT_EQ_INT(next_due(&f, "brief", d1 + 8 * 3600), 0);
  /* enabled at 09:00: the 08:00 slot is not caught up */
  ASSERT_OK(astime_store_set_enabled(f.s, "brief", true));
  ASSERT_EQ_INT(due_at(&f, d1 + 9 * 3600, NULL), 0);
  ASSERT_OK(astime_store_get(f.s, "brief", &r, &st));
  ASSERT_EQ_INT(st.armed_at, d1 + 9 * 3600);
  /* changing the trigger re-arms: the new 08:30 slot today is not owed */
  ASSERT_OK(astime_trigger_set_cron(&r.trigger, "30 8 * * *", ASTIME_TZ_UTC, err));
  ASSERT_OK(astime_store_put(f.s, &r, true, err));
  ASSERT_EQ_INT(due_at(&f, d1 + 10 * 3600, NULL), 0);
  ASSERT_OK(astime_store_get(f.s, "brief", NULL, &st));
  ASSERT_EQ_INT(st.armed_at, d1 + 10 * 3600);
  ASSERT_EQ_INT(due_at(&f, d1 + DAY + 8 * 3600 + 30 * 60, NULL), 1);
  /* disabling disarms */
  ASSERT_OK(astime_store_set_enabled(f.s, "brief", false));
  ASSERT_EQ_INT(due_at(&f, d1 + DAY + 9 * 3600, NULL), 0);
  ASSERT_OK(astime_store_get(f.s, "brief", NULL, &st));
  ASSERT_EQ_INT(st.armed_at, 0);
  astime_routine_free(&r);
  fx_close(&f);
}

TEST(next_wakeup_over_the_store) {
  fixture f;
  char err[ASTIME_ERR_SIZE];
  astime_routine r;
  int64_t d1 = utc(2026, 10, 26, 0, 0), t = 0;
  ASSERT_TRUE(fx_open(&f, "brief", "enabled: true, trigger: { cron: \"0 8 * * *\", tz: \"utc\" }"));
  ASSERT_EQ_INT(due_at(&f, d1, NULL), 0);
  ASSERT_OK(astime_store_next_wakeup(f.s, d1, &t));
  ASSERT_EQ_INT(t, d1 + 8 * 3600);
  ASSERT_OK(astime_routine_parse("#routine { name: \"poll\", enabled: true, trigger: { every: \"PT2H\" }, "
                                 "goal: \"g\" }", NULL, NULL, &r, err));
  ASSERT_OK(astime_store_put(f.s, &r, false, err));
  astime_routine_free(&r);
  ASSERT_EQ_INT(due_at(&f, d1 + 3600, NULL), 1); /* poll arms and is due */
  ASSERT_OK(astime_mark_started(f.s, "poll", d1 + 3600));
  ASSERT_OK(astime_mark_finished(f.s, "poll", d1 + 3660, ASTIME_STATUS_OK, NULL, NULL));
  ASSERT_OK(astime_store_next_wakeup(f.s, d1 + 3700, &t));
  ASSERT_EQ_INT(t, d1 + 3 * 3600);
  fx_close(&f);
}

TEST(pure_next_due_and_fires) {
  astime_routine r;
  astime_state st;
  char err[ASTIME_ERR_SIZE];
  int64_t now = utc(2026, 10, 26, 9, 0), fires[5], out = 0;
  size_t got = 0;
  ASSERT_OK(astime_routine_parse("#routine { name: \"r\", enabled: true, trigger: { cron: \"0 */6 * * *\", "
                                 "tz: \"utc\" }, goal: \"g\" }", NULL, NULL, &r, err));
  ASSERT_OK(astime_next_fires(&r, NULL, now, fires, 5, &got));
  ASSERT_EQ_INT(got, 5);
  ASSERT_EQ_INT(fires[0], utc(2026, 10, 26, 12, 0));
  ASSERT_EQ_INT(fires[4], utc(2026, 10, 27, 12, 0));
  /* fresh state: armed now, the 06:00 slot is older than arming */
  ASSERT_OK(astime_next_due(&r, NULL, now, &out));
  ASSERT_EQ_INT(out, utc(2026, 10, 26, 12, 0));
  /* armed earlier and never ran: the 06:00 slot is owed now */
  astime_state_init(&st);
  st.armed_at = utc(2026, 10, 25, 0, 0);
  ASSERT_OK(astime_next_due(&r, &st, now, &out));
  ASSERT_EQ_INT(out, now);
  r.enabled = false;
  ASSERT_ERR(astime_next_due(&r, &st, now, &out), ASTIME_ERR_NOT_FOUND);
  astime_routine_free(&r);
  /* every: fires continue from the last slot */
  ASSERT_OK(astime_routine_parse("#routine { name: \"r\", enabled: true, trigger: { every: \"PT45M\" }, "
                                 "goal: \"g\" }", NULL, NULL, &r, err));
  st.last_due_at = utc(2026, 10, 26, 8, 0);
  ASSERT_OK(astime_next_fires(&r, &st, now, fires, 3, &got));
  ASSERT_EQ_INT(got, 3);
  ASSERT_EQ_INT(fires[0], utc(2026, 10, 26, 9, 30));
  ASSERT_EQ_INT(fires[1], utc(2026, 10, 26, 10, 15));
  ASSERT_OK(astime_next_fires(&r, NULL, now, fires, 2, &got));
  ASSERT_EQ_INT(fires[0], now);
  ASSERT_EQ_INT(fires[1], now + 2700);
  astime_routine_free(&r);
}

#if !defined(ASTERISM_NO_THREADS)
typedef struct {
  fixture *f;
  int64_t base;
  int failures;
} worker_arg;

static void *worker(void *p) {
  worker_arg *a = (worker_arg *)p;
  int i;
  for (i = 0; i < 200; i++) {
    astime_due_list l;
    int64_t t = a->base + (int64_t)i * 60;
    size_t k;
    if (astime_due(a->f->s, t, &l) != ASTIME_OK) {
      a->failures++;
      continue;
    }
    for (k = 0; k < l.n; k++) {
      astime_err e = astime_mark_started(a->f->s, l.items[k].name, t);
      if (e == ASTIME_OK)
        e = astime_mark_finished(a->f->s, l.items[k].name, t, ASTIME_STATUS_OK, NULL, NULL);
      if (e != ASTIME_OK && e != ASTIME_ERR_BUSY) a->failures++;
    }
    astime_due_list_free(&l);
  }
  return NULL;
}

TEST(concurrent_callers) {
  fixture f;
  os_thread th[4];
  worker_arg args[4];
  astime_state st;
  astime_err e;
  int i;
  ASSERT_TRUE(fx_open(&f, "tick", "enabled: true, trigger: { every: \"PT1M\" }"));
  for (i = 0; i < 4; i++) {
    args[i].f = &f;
    args[i].base = utc(2026, 10, 26, 0, 0);
    args[i].failures = 0;
    ASSERT_TRUE(os_thread_start(&th[i], worker, &args[i]) == CORE_OK);
  }
  for (i = 0; i < 4; i++) os_thread_join(&th[i]);
  e = astime_store_get(f.s, "tick", NULL, &st);
  fx_close(&f);
  ASSERT_OK(e);
  for (i = 0; i < 4; i++) ASSERT_EQ_INT(args[i].failures, 0);
  ASSERT_EQ_INT(st.running_since, 0);
  /* Threads poll with interleaved clocks, so a thread may start a routine
   * another one just finished: every start is a run, at most one each. */
  ASSERT_TRUE(st.runs_today >= 1 && st.runs_today <= 4 * 200);
}
#endif

TEST_LIST = {
    TEST_ENTRY(cron_on_time_and_not_twice),
    TEST_ENTRY(slots_before_arming_never_fire),
    TEST_ENTRY(missed_run_catch_up_once_and_no_backlog),
    TEST_ENTRY(every_interval_grid),
    TEST_ENTRY(one_shot_runs_once_then_disables),
    TEST_ENTRY(one_shot_failure_stays_enabled),
    TEST_ENTRY(quiet_hours_defer_delivery),
#if !defined(_WIN32)
    TEST_ENTRY(quiet_hours_follow_local_time),
#endif
    TEST_ENTRY(max_runs_per_day),
    TEST_ENTRY(watch_polls_on_interval),
    TEST_ENTRY(watch_with_trigger_and_file_hash),
    TEST_ENTRY(deferred_run_is_retried),
    TEST_ENTRY(disabled_and_rearmed_routines),
    TEST_ENTRY(next_wakeup_over_the_store),
    TEST_ENTRY(pure_next_due_and_fires),
#if !defined(ASTERISM_NO_THREADS)
    TEST_ENTRY(concurrent_callers),
#endif
};

RUN_ALL_TESTS()
