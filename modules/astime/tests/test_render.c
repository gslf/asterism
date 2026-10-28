/*
 * test_render.c — dry-run snapshots, trigger descriptions and lint
 * warnings. Local time is Europe/Rome.
 */

#include "astime_test.h"

static const char *EXAMPLE =
    "#routine {\n"
    "  name: \"morning-brief\",\n"
    "  enabled: true,\n"
    "  trigger: { cron: \"0 8 * * 1-5\", tz: \"local\" },\n"
    "  watch: { url: \"https://example.com/page\", every: \"PT1H\" },\n"
    "  goal: \"Prepare the morning brief: calendar, news, repository status.\",\n"
    "  mode: \"automate\",\n"
    "  profile: \"automation-ci\",\n"
    "  tools: [\"fs\", \"grep\"],\n"
    "  providers: [\"local\"],\n"
    "  notify: \"always\",\n"
    "  quiet_hours: \"22:00-07:00\",\n"
    "  deliver: { channel: \"telegram\", star: \"default\" },\n"
    "  max_runs_per_day: 24,\n"
    "}\n";

static const char *DRY_EN =
    "Routine \"morning-brief\" (enabled)\n"
    "When: on weekdays at 08:00 (cron \"0 8 * * 1-5\", local time)\n"
    "Next runs:\n"
    "  - 2026-10-26 08:00 (Monday)\n"
    "  - 2026-10-27 08:00 (Tuesday)\n"
    "  - 2026-10-28 08:00 (Wednesday)\n"
    "Watches: the page https://example.com/page, checked every hour; the routine runs only when "
    "the content changes\n"
    "Goal: Prepare the morning brief: calendar, news, repository status.\n"
    "Mode: automate, profile automation-ci (automation: non-destructive actions run without "
    "confirmation)\n"
    "Tools: fs, grep\n"
    "Providers: local\n"
    "Paid inference: no\n"
    "Maximum cost: €0 (local model only)\n"
    "Delivery: telegram, star \"default\"; always notify\n"
    "Quiet hours: 22:00-07:00 (deliveries in this window arrive at 07:00)\n"
    "Limit: at most 24 runs a day\n"
    "Warnings:\n"
    "  - watch.url reads untrusted web content and profile \"automation-ci\" allows changes: "
    "every action will require approval.\n";

#if !defined(_WIN32)
TEST(dry_run_snapshot_documented_example) {
  astime_routine r;
  char *text = NULL, err[ASTIME_ERR_SIZE];
  int64_t now = utc(2026, 10, 26, 6, 0); /* 07:00 in Rome, CET */
  test_set_tz(TZ_ROME);
  ASSERT_OK(astime_routine_parse(EXAMPLE, NULL, NULL, &r, err));
  ASSERT_OK(astime_dry_run(&r, NULL, now, &text));
  ASSERT_EQ_STR(text, DRY_EN);
  astime_free(text);
  astime_routine_free(&r);
}

static const char *CHAT_EN =
    "Routine \"check-the-mail\" (disabled)\n"
    "When: every 15 minutes\n"
    "Next runs:\n"
    "  - 2026-10-26 10:15 (Monday)\n"
    "  - 2026-10-26 10:30 (Monday)\n"
    "  - 2026-10-26 10:45 (Monday)\n"
    "Goal: check the mail\n"
    "Mode: chat, profile chat (conversation only, no tools)\n"
    "Tools: none (the chat mode or profile runs without tools)\n"
    "Providers: local, chatgpt\n"
    "Paid inference: yes, chatgpt (subscription quota)\n"
    "Maximum cost: nothing beyond the subscription, but quota use has no daily cap (set "
    "max_runs_per_day)\n"
    "Delivery: default channel; always notify\n"
    "Quiet hours: 23:00-07:00 (deliveries in this window arrive at 07:00)\n"
    "Limit: no daily limit\n"
    "Last run: 2026-10-26 09:45, failed (provider unreachable)\n"
    "Consecutive failures: 2\n"
    "Warnings:\n"
    "  - The routine is disabled: it will not run until it is enabled.\n"
    "  - Paid providers (chatgpt) on a frequent schedule (every 15 minutes): every run uses "
    "subscription quota; restrict to providers: [\"local\"] or set max_runs_per_day.\n";

TEST(dry_run_snapshot_chat_routine_with_state) {
  astime_routine r;
  astime_state st;
  char *text = NULL, err[ASTIME_ERR_SIZE];
  int64_t now = utc(2026, 10, 26, 9, 0); /* 10:00 in Rome */
  test_set_tz(TZ_ROME);
  ASSERT_OK(astime_routine_from_chat("every 15 minutes | check the mail", now, ASTIME_TZ_LOCAL,
                                     &r, err));
  r.providers |= ASTIME_PROVIDER_CHATGPT;
  r.quiet_set = true;
  r.quiet_start_min = 23 * 60;
  r.quiet_end_min = 7 * 60;
  astime_state_init(&st);
  st.last_due_at = utc(2026, 10, 26, 8, 45);
  st.last_run_at = utc(2026, 10, 26, 8, 45) + 10;
  st.last_status = ASTIME_STATUS_FAILED;
  snprintf(st.last_error, sizeof st.last_error, "provider unreachable");
  st.consecutive_failures = 2;
  ASSERT_OK(astime_dry_run(&r, &st, now, &text));
  ASSERT_EQ_STR(text, CHAT_EN);
  astime_free(text);
  astime_routine_free(&r);
}

TEST(dry_run_one_shot_and_watch_only) {
  astime_routine r;
  astime_state st;
  char *text = NULL, err[ASTIME_ERR_SIZE];
  int64_t now = utc(2026, 10, 26, 9, 0);
  test_set_tz(TZ_ROME);
  ASSERT_OK(astime_routine_from_chat("on november 3 at 9 | renew the insurance", now,
                                     ASTIME_TZ_LOCAL, &r, err));
  ASSERT_OK(astime_dry_run(&r, NULL, now, &text));
  ASSERT_CONTAINS(text, "When: once, on 2026-11-03 at 09:00 (local time)\n"
                        "Next runs:\n  - 2026-11-03 09:00 (Tuesday)\n");
  astime_free(text);
  /* consumed: nothing left */
  astime_state_init(&st);
  st.last_due_at = r.trigger.at;
  ASSERT_OK(astime_dry_run(&r, &st, now, &text));
  ASSERT_CONTAINS(text, "When: once, on 2026-11-03 at 09:00 (local time)\nNext runs: none\n");
  astime_free(text);
  astime_routine_free(&r);
  ASSERT_OK(astime_routine_parse("#routine { name: \"w\", enabled: true, goal: \"g\", "
                                 "watch: { file: \"/var/log/app.log\", every: \"PT10M\" } }",
                                 NULL, NULL, &r, err));
  ASSERT_OK(astime_dry_run(&r, NULL, now, &text));
  ASSERT_CONTAINS(text, "When: no schedule: watch only\n"
                        "Next checks:\n"
                        "  - 2026-10-26 10:00 (Monday)\n"
                        "  - 2026-10-26 10:10 (Monday)\n"
                        "  - 2026-10-26 10:20 (Monday)\n"
                        "Watches: the file /var/log/app.log, checked every 10 minutes; the routine "
                        "runs only when the content changes\n");
  astime_free(text);
  astime_routine_free(&r);
}
#endif

TEST(describe_triggers) {
  static const struct {
    const char *cron, *words;
  } cases[] = {
      {"0 8 * * *", "every day at 08:00"},
      {"30 7 * * 1,4", "every Monday and Thursday at 07:30"},
      {"0 7 * * 1,3,5", "every Monday, Wednesday and Friday at 07:00"},
      {"0 10 * * 0,6", "on weekends at 10:00"},
      {"0 18 * * 0", "every Sunday at 18:00"},
      {"*/15 * * * *", "every 15 minutes"},
      {"* * * * *", "every minute"},
      {"0 * * * *", "every hour at minute 00"},
      {"15 */2 * * *", "every 2 hours at minute 15"},
      {"0 8,12,18 * * *", "every day at 08:00, 12:00 and 18:00"},
      {"0 9 1 * *", "on day 1 of the month at 09:00"},
      {"0 9 1,15 * *", "on days 1 and 15 of the month at 09:00"},
      {"@yearly", "on day 1 of January at 00:00"},
      {"0 12 13 * 5", "on day 13 of the month or every Friday at 12:00"},
      {"*/30 9-17 * * 1-5", "on weekdays, every 30 minutes during hours 09-17"},
      {"0 9 * 7,8 *", "every day at 09:00, only in July and August"},
      {"0,30 * * * *", "every 30 minutes"},
      {"5,20 * * * *", "at minutes 05 and 20 past every hour"},
  };
  size_t i;
  for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    astime_trigger t;
    char *text = NULL, want[256];
    ASSERT_OK(astime_trigger_set_cron(&t, cases[i].cron, ASTIME_TZ_LOCAL, NULL));
    ASSERT_OK(astime_describe_trigger(&t, &text));
    snprintf(want, sizeof want, "%s (cron \"%s\", local time)", cases[i].words, cases[i].cron);
    ASSERT_EQ_STR(text, want);
    astime_free(text);
    t.tz = ASTIME_TZ_UTC;
    ASSERT_OK(astime_describe_trigger(&t, &text));
    snprintf(want, sizeof want, "%s (cron \"%s\", UTC)", cases[i].words, cases[i].cron);
    ASSERT_EQ_STR(text, want);
    astime_free(text);
  }
  {
    astime_trigger t;
    char *text = NULL;
    memset(&t, 0, sizeof t);
    t.kind = ASTIME_TRIGGER_EVERY;
    t.every_s = 5400;
    ASSERT_OK(astime_describe_trigger(&t, &text));
    ASSERT_EQ_STR(text, "every 1 hour and 30 minutes");
    astime_free(text);
    t.every_s = 7200;
    ASSERT_OK(astime_describe_trigger(&t, &text));
    ASSERT_EQ_STR(text, "every 2 hours");
    astime_free(text);
    t.every_s = 86400;
    ASSERT_OK(astime_describe_trigger(&t, &text));
    ASSERT_EQ_STR(text, "every day");
    astime_free(text);
    t.kind = ASTIME_TRIGGER_AT;
    t.tz = ASTIME_TZ_UTC;
    t.at = utc(2026, 11, 3, 8, 0);
    ASSERT_OK(astime_describe_trigger(&t, &text));
    ASSERT_EQ_STR(text, "once, on 2026-11-03 at 08:00 (UTC)");
    astime_free(text);
  }
}

static astime_err lint_text(const char *body, int64_t now, astime_strings *out) {
  char text[1024], err[ASTIME_ERR_SIZE];
  astime_routine r;
  astime_err e;
  snprintf(text, sizeof text, "#routine { name: \"r\", goal: \"g\", %s }", body);
  e = astime_routine_parse(text, NULL, NULL, &r, err);
  if (e != ASTIME_OK) {
    fprintf(stderr, "  parse: %s\n", err);
    return e;
  }
  e = astime_lint(&r, now, out);
  astime_routine_free(&r);
  return e;
}

TEST(lint_warnings) {
  astime_strings w = {NULL, 0};
  int64_t now = utc(2026, 10, 26, 9, 0);
  /* a clean routine has no warnings */
  ASSERT_OK(lint_text("enabled: true, trigger: { cron: \"0 8 * * *\" }", now, &w));
  ASSERT_EQ_INT(w.n, 0);
  astime_strings_free(&w);
  ASSERT_OK(lint_text("trigger: { cron: \"0 8 * * *\" }", now, &w));
  ASSERT_EQ_INT(w.n, 1);
  ASSERT_EQ_STR(w.items[0], "The routine is disabled: it will not run until it is enabled.");
  astime_strings_free(&w);
  /* untrusted web content with a mutating profile */
  ASSERT_OK(lint_text("enabled: true, mode: \"coding\", watch: { url: \"https://x.it\", every: \"PT1H\" }",
                      now, &w));
  ASSERT_EQ_INT(w.n, 1);
  ASSERT_EQ_STR(w.items[0], "watch.url reads untrusted web content and profile \"coding-sandboxed\" "
                            "allows changes: every action will require approval.");
  astime_strings_free(&w);
  ASSERT_OK(lint_text("enabled: true, mode: \"coding\", profile: \"coding-readonly\", "
                      "watch: { url: \"https://x.it\", every: \"PT1H\" }", now, &w));
  ASSERT_EQ_INT(w.n, 0);
  astime_strings_free(&w);
  /* paid providers on a frequent schedule, unless capped */
  ASSERT_OK(lint_text("enabled: true, trigger: { cron: \"*/10 * * * *\" }, providers: [\"local\", \"codex\", "
                      "\"claude-code\"]", now, &w));
  ASSERT_EQ_INT(w.n, 1);
  ASSERT_EQ_STR(w.items[0], "Paid providers (codex and claude-code) on a frequent schedule (every 10 "
                            "minutes): every run uses subscription quota; restrict to providers: "
                            "[\"local\"] or set max_runs_per_day.");
  astime_strings_free(&w);
  ASSERT_OK(lint_text("enabled: true, trigger: { every: \"PT10M\" }, providers: [\"chatgpt\"], "
                      "max_runs_per_day: 6", now, &w));
  ASSERT_EQ_INT(w.n, 0);
  astime_strings_free(&w);
  /* shorter than five minutes, from any source */
  ASSERT_OK(lint_text("enabled: true, trigger: { every: \"PT2M\" }", now, &w));
  ASSERT_EQ_INT(w.n, 1);
  ASSERT_EQ_STR(w.items[0], "The interval is shorter than 5 minutes (every 2 minutes): it loads the "
                            "daemon and the model for little benefit.");
  astime_strings_free(&w);
  ASSERT_OK(lint_text("enabled: true, trigger: { cron: \"* 9 * * *\" }", now, &w));
  ASSERT_EQ_INT(w.n, 1);
  ASSERT_CONTAINS(w.items[0], "The interval is shorter than 5 minutes (every minute)");
  astime_strings_free(&w);
  ASSERT_OK(lint_text("enabled: true, trigger: { cron: \"@daily\" }, watch: { file: \"/a\", every: \"PT1M\" }",
                      now, &w));
  ASSERT_EQ_INT(w.n, 1);
  ASSERT_CONTAINS(w.items[0], "shorter than 5 minutes (every minute)");
  astime_strings_free(&w);
  /* quiet hours that do nothing or everything */
  ASSERT_OK(lint_text("enabled: true, trigger: { cron: \"0 12 * * *\" }, quiet_hours: \"10:00-10:00\"",
                      now, &w));
  ASSERT_EQ_INT(w.n, 1);
  ASSERT_EQ_STR(w.items[0], "quiet_hours \"10:00-10:00\" is an empty window: it silences nothing.");
  astime_strings_free(&w);
  ASSERT_OK(lint_text("enabled: true, trigger: { cron: \"0 12 * * *\" }, quiet_hours: \"00:00-24:00\"",
                      now, &w));
  ASSERT_EQ_INT(w.n, 1);
  ASSERT_EQ_STR(w.items[0], "quiet_hours \"00:00-24:00\" covers all 24 hours: every delivery will be deferred.");
  astime_strings_free(&w);
  /* runs that land inside quiet hours */
  ASSERT_OK(lint_text("enabled: true, trigger: { cron: \"30 6 * * *\", tz: \"utc\" }, "
                      "quiet_hours: \"22:00-07:00\"", now, &w));
  ASSERT_EQ_INT(w.n, 1);
  ASSERT_EQ_STR(w.items[0], "Some runs fall inside quiet hours (22:00-07:00): deliveries will arrive at 07:00.");
  astime_strings_free(&w);
  ASSERT_OK(lint_text("enabled: true, notify: \"never\", trigger: { cron: \"30 6 * * *\", tz: \"utc\" }, "
                      "quiet_hours: \"22:00-07:00\"", now, &w));
  ASSERT_EQ_INT(w.n, 0);
  astime_strings_free(&w);
  /* a one-shot in the past */
  ASSERT_OK(lint_text("enabled: true, trigger: { at: \"2026-10-01T09:00:00Z\", tz: \"utc\" }", now, &w));
  ASSERT_EQ_INT(w.n, 1);
  ASSERT_EQ_STR(w.items[0], "The one-shot time (2026-10-01 09:00 UTC) is in the past: it will not run.");
  astime_strings_free(&w);
  /* tools that a chat routine cannot use */
  ASSERT_OK(lint_text("enabled: true, trigger: { cron: \"@daily\" }, tools: [\"fs\"]", now, &w));
  ASSERT_EQ_INT(w.n, 1);
  ASSERT_CONTAINS(w.items[0], "tools is set, but the chat mode or profile runs without tools");
  astime_strings_free(&w);
  /* several at once, in order */
  ASSERT_OK(lint_text("trigger: { every: \"PT1M\" }, providers: [\"chatgpt\"], tools: [\"fs\"]",
                      now, &w));
  ASSERT_EQ_INT(w.n, 4);
  ASSERT_CONTAINS(w.items[0], "disabled");
  ASSERT_CONTAINS(w.items[1], "Paid providers (chatgpt)");
  ASSERT_CONTAINS(w.items[2], "shorter than 5 minutes");
  ASSERT_CONTAINS(w.items[3], "tools is set");
  astime_strings_free(&w);
}

TEST_LIST = {
#if !defined(_WIN32)
    TEST_ENTRY(dry_run_snapshot_documented_example),
    TEST_ENTRY(dry_run_snapshot_chat_routine_with_state),
    TEST_ENTRY(dry_run_one_shot_and_watch_only),
#endif
    TEST_ENTRY(describe_triggers),
    TEST_ENTRY(lint_warnings),
};

RUN_ALL_TESTS()
