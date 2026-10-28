/*
 * test_state.c — per-routine state: persistence, corruption recovery,
 * interrupted runs, the store lock and store bookkeeping.
 */

#include "astime_test.h"

static const char *DAILY =
    "#routine { name: \"daily\", enabled: true, trigger: { cron: \"0 8 * * *\", tz: \"utc\" }, "
    "goal: \"say hi\" }\n";

TEST(state_round_trip) {
  char dir[256], err[ASTIME_ERR_SIZE], path[512];
  astime_state st, back;
  char *text;
  ASSERT_TRUE(test_tmpdir(dir));
  astime_state_init(&st);
  ASSERT_ERR(astime_state_load(dir, "x", &back, err), ASTIME_ERR_NOT_FOUND);
  st.armed_at = utc(2026, 10, 1, 7, 0);
  snprintf(st.trigger_sig, sizeof st.trigger_sig, "cron:1:100:fffffffe:1ffe:7f:1:utc");
  snprintf(st.watch_sig, sizeof st.watch_sig, "url:0123abcd");
  st.last_due_at = utc(2026, 10, 26, 8, 0);
  st.last_run_at = utc(2026, 10, 26, 8, 0) + 12;
  st.running_since = 0;
  st.last_status = ASTIME_STATUS_FAILED;
  snprintf(st.last_error, sizeof st.last_error, "model \"local\" timed out \\ retry");
  st.runs_today = 3;
  snprintf(st.runs_day, sizeof st.runs_day, "2026-10-26");
  st.consecutive_failures = 2;
  snprintf(st.last_watch_hash, sizeof st.last_watch_hash, "%s",
           "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08");
  st.last_watch_at = utc(2026, 10, 26, 7, 30);
  st.next_due = utc(2026, 10, 27, 8, 0);
  st.defer_until = utc(2026, 10, 26, 8, 10);
  ASSERT_OK(astime_state_save(dir, "x", &st));
  ASSERT_OK(astime_state_load(dir, "x", &back, err));
  ASSERT_EQ_INT(back.armed_at, st.armed_at);
  ASSERT_EQ_STR(back.trigger_sig, st.trigger_sig);
  ASSERT_EQ_STR(back.watch_sig, st.watch_sig);
  ASSERT_EQ_INT(back.last_due_at, st.last_due_at);
  ASSERT_EQ_INT(back.last_run_at, st.last_run_at);
  ASSERT_EQ_INT(back.running_since, 0);
  ASSERT_EQ_INT(back.last_status, ASTIME_STATUS_FAILED);
  ASSERT_EQ_STR(back.last_error, st.last_error);
  ASSERT_EQ_INT(back.runs_today, 3);
  ASSERT_EQ_STR(back.runs_day, "2026-10-26");
  ASSERT_EQ_INT(back.consecutive_failures, 2);
  ASSERT_EQ_STR(back.last_watch_hash, st.last_watch_hash);
  ASSERT_EQ_INT(back.last_watch_at, st.last_watch_at);
  ASSERT_EQ_INT(back.next_due, st.next_due);
  ASSERT_EQ_INT(back.defer_until, st.defer_until);
  /* readable on disk */
  snprintf(path, sizeof path, "%s/.state/x.xcdn", dir);
  text = test_read_file(path, NULL);
  ASSERT_CONTAINS(text, "#routine_state {\n");
  ASSERT_CONTAINS(text, "  last_due_at: \"2026-10-26T08:00:00Z\",\n");
  ASSERT_CONTAINS(text, "  last_status: \"failed\",\n");
  ASSERT_CONTAINS(text, "  last_error: \"model \\\"local\\\" timed out \\\\ retry\",\n");
  free(text);
  test_rmtree(dir);
}

TEST(state_load_rejects_bad_values_and_ignores_unknown_keys) {
  char dir[256], err[ASTIME_ERR_SIZE];
  astime_state st;
  ASSERT_TRUE(test_tmpdir(dir));
  ASSERT_OK(astime_state_save(dir, "y", &(astime_state){0}));
  ASSERT_TRUE(test_write(dir, ".state/y.xcdn",
                         "#routine_state { last_status: \"ok\", future_field: [1, 2], runs_today: 1 }"));
  ASSERT_OK(astime_state_load(dir, "y", &st, err));
  ASSERT_EQ_INT(st.last_status, ASTIME_STATUS_OK);
  ASSERT_EQ_INT(st.runs_today, 1);
  ASSERT_TRUE(test_write(dir, ".state/y.xcdn", "#routine_state { last_status: \"great\" }"));
  ASSERT_ERR(astime_state_load(dir, "y", &st, err), ASTIME_ERR_PARSE);
  ASSERT_CONTAINS(err, "last_status: invalid value");
  ASSERT_TRUE(test_write(dir, ".state/y.xcdn", "#routine_state { last_due_at: \"yesterday\" }"));
  ASSERT_ERR(astime_state_load(dir, "y", &st, err), ASTIME_ERR_PARSE);
  ASSERT_TRUE(test_write(dir, ".state/y.xcdn", "#routine_state { last_watch_hash: \"XYZ\" }"));
  ASSERT_ERR(astime_state_load(dir, "y", &st, err), ASTIME_ERR_PARSE);
  ASSERT_TRUE(test_write(dir, ".state/y.xcdn", "{{{{"));
  ASSERT_ERR(astime_state_load(dir, "y", &st, err), ASTIME_ERR_PARSE);
  ASSERT_CONTAINS(err, "syntax error");
  test_rmtree(dir);
}

TEST(corrupt_state_is_reported_and_reset) {
  char dir[256], path[512];
  astime_store *s = NULL;
  astime_strings issues = {NULL, 0};
  astime_state st;
  astime_due_list due;
  ASSERT_TRUE(test_tmpdir(dir));
  ASSERT_TRUE(test_write(dir, "daily.xcdn", DAILY));
  ASSERT_OK(astime_state_save(dir, "daily", &(astime_state){0}));
  ASSERT_TRUE(test_write(dir, ".state/daily.xcdn", "#routine_state { runs_today: \"lots\" "));
  ASSERT_OK(astime_store_open(dir, &s));
  ASSERT_OK(astime_store_issues(s, &issues));
  ASSERT_EQ_INT(issues.n, 1);
  ASSERT_CONTAINS(issues.items[0], "daily.xcdn");
  ASSERT_CONTAINS(issues.items[0], "state reset (old file kept as .state/daily.xcdn.corrupt)");
  astime_strings_free(&issues);
  snprintf(path, sizeof path, "%s/.state/daily.xcdn.corrupt", dir);
  ASSERT_TRUE(os_file_exists(path));
  ASSERT_OK(astime_store_get(s, "daily", NULL, &st));
  ASSERT_EQ_INT(st.last_status, ASTIME_STATUS_NONE);
  /* the store keeps working */
  ASSERT_OK(astime_due(s, utc(2026, 10, 26, 7, 0), &due));
  ASSERT_EQ_INT(due.n, 0);
  astime_due_list_free(&due);
  ASSERT_OK(astime_due(s, utc(2026, 10, 26, 8, 0), &due));
  ASSERT_EQ_INT(due.n, 1);
  astime_due_list_free(&due);
  astime_store_close(s);
  test_rmtree(dir);
}

TEST(interrupted_run_is_closed_as_failed) {
  char dir[256];
  astime_store *s = NULL;
  astime_state st;
  ASSERT_TRUE(test_tmpdir(dir));
  ASSERT_TRUE(test_write(dir, "daily.xcdn", DAILY));
  ASSERT_OK(astime_store_open(dir, &s));
  ASSERT_OK(astime_mark_started(s, "daily", utc(2026, 10, 26, 8, 0)));
  astime_store_close(s); /* the daemon dies mid-run */
  ASSERT_OK(astime_store_open(dir, &s));
  ASSERT_OK(astime_store_get(s, "daily", NULL, &st));
  ASSERT_EQ_INT(st.running_since, 0);
  ASSERT_EQ_INT(st.last_status, ASTIME_STATUS_FAILED);
  ASSERT_CONTAINS(st.last_error, "interrupted");
  ASSERT_EQ_INT(st.consecutive_failures, 1);
  ASSERT_EQ_INT(st.last_run_at, utc(2026, 10, 26, 8, 0));
  astime_store_close(s);
  test_rmtree(dir);
}

TEST(store_is_locked_per_process) {
  char dir[256];
  astime_store *a = NULL, *b = NULL;
  ASSERT_TRUE(test_tmpdir(dir));
  ASSERT_OK(astime_store_open(dir, &a));
  ASSERT_ERR(astime_store_open(dir, &b), ASTIME_ERR_BUSY);
  astime_store_close(a);
  ASSERT_OK(astime_store_open(dir, &b));
  astime_store_close(b);
  test_rmtree(dir);
}

TEST(store_put_get_list_remove_and_names) {
  char dir[256], err[ASTIME_ERR_SIZE], name[ASTIME_NAME_MAX], path[512];
  astime_store *s = NULL;
  astime_routine r, got;
  astime_strings names = {NULL, 0}, issues = {NULL, 0};
  ASSERT_TRUE(test_tmpdir(dir));
  ASSERT_TRUE(test_write(dir, "daily.xcdn", DAILY));
  ASSERT_TRUE(test_write(dir, "bad.xcdn", "#routine { name: \"bad\" }"));
  ASSERT_OK(astime_store_open(dir, &s));
  ASSERT_OK(astime_store_issues(s, &issues));
  ASSERT_EQ_INT(issues.n, 1);
  ASSERT_CONTAINS(issues.items[0], "bad.xcdn: goal: missing (required)");
  astime_strings_free(&issues);

  ASSERT_OK(astime_routine_from_chat("every day at 8 | prepare the brief", utc(2026, 10, 26, 9, 0),
                                     ASTIME_TZ_UTC, &r, err));
  ASSERT_OK(astime_store_put(s, &r, false, err));
  ASSERT_ERR(astime_store_put(s, &r, false, err), ASTIME_ERR_EXISTS);
  ASSERT_CONTAINS(err, "already exists");
  ASSERT_OK(astime_store_unique_name(s, r.goal, name));
  ASSERT_EQ_STR(name, "prepare-the-brief-2");
  ASSERT_OK(astime_store_unique_name(s, "Daily", name));
  ASSERT_EQ_STR(name, "daily-2");
  ASSERT_OK(astime_store_unique_name(s, "fresh", name));
  ASSERT_EQ_STR(name, "fresh");
  /* an invalid file on disk still owns its name */
  ASSERT_OK(astime_store_unique_name(s, "bad", name));
  ASSERT_EQ_STR(name, "bad-2");
  {
    astime_routine b;
    ASSERT_OK(astime_routine_copy(&r, &b));
    free(b.name);
    b.name = core_strdup("bad");
    ASSERT_ERR(astime_store_put(s, &b, false, err), ASTIME_ERR_EXISTS);
    astime_routine_free(&b);
  }
  ASSERT_OK(astime_store_list(s, &names));
  ASSERT_EQ_INT(names.n, 2);
  ASSERT_EQ_STR(names.items[0], "daily");
  ASSERT_EQ_STR(names.items[1], "prepare-the-brief");
  astime_strings_free(&names);

  ASSERT_OK(astime_store_set_enabled(s, "prepare-the-brief", true));
  ASSERT_OK(astime_store_get(s, "prepare-the-brief", &got, NULL));
  ASSERT_TRUE(got.enabled);
  astime_routine_free(&got);
  snprintf(path, sizeof path, "%s/prepare-the-brief.xcdn", dir);
  {
    char *disk = test_read_file(path, NULL);
    ASSERT_CONTAINS(disk, "enabled: true,");
    free(disk);
  }
  ASSERT_ERR(astime_store_get(s, "nope", &got, NULL), ASTIME_ERR_NOT_FOUND);
  ASSERT_ERR(astime_store_set_enabled(s, "nope", true), ASTIME_ERR_NOT_FOUND);

  /* reload keeps in-memory state and picks up edits */
  ASSERT_OK(astime_mark_started(s, "daily", utc(2026, 10, 26, 8, 0)));
  ASSERT_TRUE(test_write(dir, "daily.xcdn",
                         "#routine { name: \"daily\", enabled: true, trigger: { cron: \"0 9 * * *\", "
                         "tz: \"utc\" }, goal: \"say hello\" }\n"));
  ASSERT_OK(astime_store_reload(s));
  {
    astime_state st;
    ASSERT_OK(astime_store_get(s, "daily", &got, &st));
    ASSERT_EQ_STR(got.goal, "say hello");
    ASSERT_EQ_INT(st.running_since, utc(2026, 10, 26, 8, 0));
    astime_routine_free(&got);
  }
  ASSERT_ERR(astime_store_remove(s, "daily"), ASTIME_ERR_BUSY);
  ASSERT_OK(astime_mark_finished(s, "daily", utc(2026, 10, 26, 8, 1), ASTIME_STATUS_OK, NULL, NULL));
  ASSERT_OK(astime_store_remove(s, "daily"));
  ASSERT_ERR(astime_store_remove(s, "daily"), ASTIME_ERR_NOT_FOUND);
  snprintf(path, sizeof path, "%s/daily.xcdn", dir);
  ASSERT_TRUE(!os_file_exists(path));
  snprintf(path, sizeof path, "%s/.state/daily.xcdn", dir);
  ASSERT_TRUE(!os_file_exists(path));
  ASSERT_OK(astime_store_list(s, &names));
  ASSERT_EQ_INT(names.n, 1);
  astime_strings_free(&names);
  astime_routine_free(&r);
  astime_store_close(s);
  test_rmtree(dir);
}

TEST(mark_finished_bookkeeping) {
  char dir[256];
  astime_store *s = NULL;
  astime_state st;
  int64_t t0 = utc(2026, 10, 26, 8, 0);
  ASSERT_TRUE(test_tmpdir(dir));
  ASSERT_TRUE(test_write(dir, "daily.xcdn", DAILY));
  ASSERT_OK(astime_store_open(dir, &s));
  ASSERT_ERR(astime_mark_started(s, "nope", t0), ASTIME_ERR_NOT_FOUND);
  ASSERT_ERR(astime_mark_finished(s, "daily", t0, ASTIME_STATUS_NONE, NULL, NULL), ASTIME_ERR_INVALID);
  ASSERT_ERR(astime_mark_finished(s, "daily", t0, ASTIME_STATUS_OK, NULL, "NOT-HEX"), ASTIME_ERR_INVALID);
  ASSERT_OK(astime_mark_started(s, "daily", t0));
  ASSERT_ERR(astime_mark_started(s, "daily", t0), ASTIME_ERR_BUSY);
  ASSERT_OK(astime_mark_finished(s, "daily", t0 + 30, ASTIME_STATUS_FAILED,
                                 "provider unreachable: a very long explanation that keeps going "
                                 "and going and going well past the limit of the short error field "
                                 "so that it has to be truncated somewhere around two hundred bytes "
                                 "without splitting any UTF-8 sequence like è or ☃ in the middle",
                                 NULL));
  ASSERT_OK(astime_store_get(s, "daily", NULL, &st));
  ASSERT_EQ_INT(st.last_status, ASTIME_STATUS_FAILED);
  ASSERT_EQ_INT(st.consecutive_failures, 1);
  ASSERT_EQ_INT(st.runs_today, 1);
  ASSERT_EQ_STR(st.runs_day, "2026-10-26");
  ASSERT_TRUE(strlen(st.last_error) < ASTIME_SHORT_ERROR_MAX);
  ASSERT_TRUE(core_utf8_valid(st.last_error, strlen(st.last_error)));
  ASSERT_OK(astime_mark_started(s, "daily", t0 + 60));
  ASSERT_OK(astime_mark_finished(s, "daily", t0 + 90, ASTIME_STATUS_FAILED, "again", NULL));
  ASSERT_OK(astime_mark_started(s, "daily", t0 + 120));
  ASSERT_OK(astime_mark_finished(s, "daily", t0 + 150, ASTIME_STATUS_OK, "ignored", NULL));
  ASSERT_OK(astime_store_get(s, "daily", NULL, &st));
  ASSERT_EQ_INT(st.consecutive_failures, 0);
  ASSERT_EQ_STR(st.last_error, "");
  ASSERT_EQ_INT(st.runs_today, 3);
  /* the next day resets the count; skipped runs do not count */
  ASSERT_OK(astime_mark_started(s, "daily", t0 + 86400));
  ASSERT_OK(astime_mark_finished(s, "daily", t0 + 86400, ASTIME_STATUS_SKIPPED, "nothing new", NULL));
  ASSERT_OK(astime_store_get(s, "daily", NULL, &st));
  ASSERT_EQ_INT(st.runs_today, 3);
  ASSERT_EQ_STR(st.last_error, "nothing new");
  ASSERT_OK(astime_mark_started(s, "daily", t0 + 86400 + 60));
  ASSERT_OK(astime_mark_finished(s, "daily", t0 + 86400 + 60, ASTIME_STATUS_OK, NULL, NULL));
  ASSERT_OK(astime_store_get(s, "daily", NULL, &st));
  ASSERT_EQ_INT(st.runs_today, 1);
  ASSERT_EQ_STR(st.runs_day, "2026-10-27");
  astime_store_close(s);
  /* persisted */
  {
    char err[ASTIME_ERR_SIZE];
    ASSERT_OK(astime_state_load(dir, "daily", &st, err));
    ASSERT_EQ_INT(st.runs_today, 1);
    ASSERT_EQ_INT(st.last_status, ASTIME_STATUS_OK);
  }
  test_rmtree(dir);
}

TEST_LIST = {
    TEST_ENTRY(state_round_trip),
    TEST_ENTRY(state_load_rejects_bad_values_and_ignores_unknown_keys),
    TEST_ENTRY(corrupt_state_is_reported_and_reset),
    TEST_ENTRY(interrupted_run_is_closed_as_failed),
    TEST_ENTRY(store_is_locked_per_process),
    TEST_ENTRY(store_put_get_list_remove_and_names),
    TEST_ENTRY(mark_finished_bookkeeping),
};

RUN_ALL_TESTS()
