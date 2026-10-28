/*
 * test_natural.c — schedules from everyday phrasings, raw cron,
 * helpful errors, routines from chat and slugs.
 *
 * "Now" is Monday 2026-10-26 10:00 in Rome (09:00 UTC, CET).
 */

#include "astime_test.h"

#define NOW utc(2026, 10, 26, 9, 0)

typedef struct {
  const char *text;
  astime_trigger_kind kind;
  const char *cron;  /* CRON: canonical text */
  int64_t every;     /* EVERY: seconds       */
  int64_t at;        /* AT: instant          */
} nat_case;

static void run_cases(const nat_case *cases, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    astime_trigger t;
    char err[ASTIME_ERR_SIZE];
    astime_err e;
    err[0] = '\0';
    e = astime_schedule_parse(cases[i].text, NOW, ASTIME_TZ_LOCAL, &t, err);
    if (e != ASTIME_OK) {
      TEST_FAILF("\"%s\": %s (%s)", cases[i].text, astime_err_name(e), err);
      return;
    }
    if (t.kind != cases[i].kind) {
      TEST_FAILF("\"%s\": kind %d, want %d", cases[i].text, (int)t.kind, (int)cases[i].kind);
      return;
    }
    if (t.tz != ASTIME_TZ_LOCAL) {
      TEST_FAILF("\"%s\": zone not local", cases[i].text);
      return;
    }
    if (t.kind == ASTIME_TRIGGER_CRON && strcmp(t.cron_text, cases[i].cron) != 0) {
      TEST_FAILF("\"%s\": cron \"%s\", want \"%s\"", cases[i].text, t.cron_text, cases[i].cron);
      return;
    }
    if (t.kind == ASTIME_TRIGGER_EVERY && t.every_s != cases[i].every) {
      TEST_FAILF("\"%s\": every %lld, want %lld", cases[i].text, (long long)t.every_s,
                 (long long)cases[i].every);
      return;
    }
    if (t.kind == ASTIME_TRIGGER_AT) {
      if (t.at != cases[i].at) {
        TEST_FAILF("\"%s\": at %lld, want %lld", cases[i].text, (long long)t.at, (long long)cases[i].at);
        return;
      }
      if (t.at_offset_s != astime_utc_offset(t.at, ASTIME_TZ_LOCAL)) {
        TEST_FAILF("\"%s\": offset %d", cases[i].text, (int)t.at_offset_s);
        return;
      }
    }
  }
}

#define C ASTIME_TRIGGER_CRON
#define E ASTIME_TRIGGER_EVERY
#define A ASTIME_TRIGGER_AT

TEST(recurring_phrasings) {
  const nat_case cases[] = {
      {"every day at 8am", C, "0 8 * * *", 0, 0},
      {"weekdays at 9", C, "0 9 * * 1-5", 0, 0},
      {"every monday at 6pm", C, "0 18 * * 1", 0, 0},
      {"Every day at 8 AM", C, "0 8 * * *", 0, 0},
      {"Each day at 7:30", C, "30 7 * * *", 0, 0},
      {"every day at 8.15", C, "15 8 * * *", 0, 0},
      {"daily at 6:30pm", C, "30 18 * * *", 0, 0},
      {"every day at 12am", C, "0 0 * * *", 0, 0},
      {"every day at noon", C, "0 12 * * *", 0, 0},
      {"every day at midday", C, "0 12 * * *", 0, 0},
      {"at midnight every day", C, "0 0 * * *", 0, 0},
      {"at 7 every day", C, "0 7 * * *", 0, 0},
      {"at 9 on weekdays", C, "0 9 * * 1-5", 0, 0},
      {"every weekday at 9:15", C, "15 9 * * 1-5", 0, 0},
      {"monday through friday at 8:30", C, "30 8 * * 1-5", 0, 0},
      {"on weekends at 10am", C, "0 10 * * 0,6", 0, 0},
      {"every monday and thursday at 10", C, "0 10 * * 1,4", 0, 0},
      {"every monday, wednesday and friday at 7", C, "0 7 * * 1,3,5", 0, 0},
      {"mondays and fridays at 7 in the morning", C, "0 7 * * 1,5", 0, 0},
      {"saturdays and sundays at 10", C, "0 10 * * 0,6", 0, 0},
      {"on mondays at 9", C, "0 9 * * 1", 0, 0},
      {"every sat at 11pm", C, "0 23 * * 6", 0, 0},
      {"every saturday at 11 in the evening", C, "0 23 * * 6", 0, 0},
      {"every day at 3 in the afternoon", C, "0 15 * * *", 0, 0},
      {"every month on the 1st at 9am", C, "0 9 1 * *", 0, 0},
      {"on the 15th of every month at 8", C, "0 8 15 * *", 0, 0},
      {"on the first of every month at 9", C, "0 9 1 * *", 0, 0},
  };
  test_set_tz(TZ_ROME);
  run_cases(cases, sizeof cases / sizeof cases[0]);
}

TEST(interval_phrasings) {
  const nat_case cases[] = {
      {"every 15 minutes", E, NULL, 900, 0},
      {"every 30 minutes", E, NULL, 1800, 0},
      {"every hour", E, NULL, 3600, 0},
      {"hourly", E, NULL, 3600, 0},
      {"every 2 hours", E, NULL, 7200, 0},
      {"every two hours", E, NULL, 7200, 0},
      {"every half hour", E, NULL, 1800, 0},
      {"every quarter of an hour", E, NULL, 900, 0},
      {"every 30min", E, NULL, 1800, 0},
      {"every 2h", E, NULL, 7200, 0},
      {"every 2 days", E, NULL, 172800, 0},
  };
  test_set_tz(TZ_ROME);
  run_cases(cases, sizeof cases / sizeof cases[0]);
}

TEST(one_shot_phrasings) {
  const nat_case cases[] = {
      {"tomorrow at 3pm", A, NULL, 0, utc(2026, 10, 27, 14, 0)},
      {"tomorrow at 15", A, NULL, 0, utc(2026, 10, 27, 14, 0)},
      {"on 2026-11-03 at 09:00", A, NULL, 0, utc(2026, 11, 3, 8, 0)},
      {"today at 6pm", A, NULL, 0, utc(2026, 10, 26, 17, 0)},
      {"at 3pm tomorrow", A, NULL, 0, utc(2026, 10, 27, 14, 0)},
      {"the day after tomorrow at 8", A, NULL, 0, utc(2026, 10, 28, 7, 0)},
      {"on november 3rd at 9am", A, NULL, 0, utc(2026, 11, 3, 8, 0)},
      {"3 november at 9:00", A, NULL, 0, utc(2026, 11, 3, 8, 0)},
      {"on 3 november 2027 at 9:30", A, NULL, 0, utc(2027, 11, 3, 8, 30)},
      {"on january 1st at 9", A, NULL, 0, utc(2027, 1, 1, 8, 0)},   /* rolls over */
      {"on october 26 at 9", A, NULL, 0, utc(2027, 10, 26, 7, 0)},  /* passed today: CEST next year */
      {"on monday at 9", A, NULL, 0, utc(2026, 11, 2, 8, 0)},       /* 09:00 today has passed */
      {"monday at 11", A, NULL, 0, utc(2026, 10, 26, 10, 0)},       /* later today */
      {"next friday at noon", A, NULL, 0, utc(2026, 10, 30, 11, 0)},
      {"thursday next at 2pm", A, NULL, 0, utc(2026, 10, 29, 13, 0)},
      {"in 30 minutes", A, NULL, 0, NOW + 1800},
      {"in an hour", A, NULL, 0, NOW + 3600},
      {"in 2 hours", A, NULL, 0, NOW + 7200},
      {"in half an hour", A, NULL, 0, NOW + 1800},
  };
  test_set_tz(TZ_ROME);
  run_cases(cases, sizeof cases / sizeof cases[0]);
}

TEST(raw_cron_and_times) {
  const nat_case cases[] = {
      {"0 8 * * 1-5", C, "0 8 * * 1-5", 0, 0},
      {"@daily", C, "@daily", 0, 0},
      {"cron: */5 * * * *", C, "*/5 * * * *", 0, 0},
      {"30 7 * * mon,thu", C, "30 7 * * mon,thu", 0, 0},
      {"2026-11-02T09:00:00+01:00", A, NULL, 0, utc(2026, 11, 2, 8, 0)},
  };
  test_set_tz(TZ_ROME);
  run_cases(cases, sizeof cases / sizeof cases[0]);
}

TEST(utc_zone) {
  astime_trigger t;
  char err[ASTIME_ERR_SIZE];
  test_set_tz(TZ_ROME);
  ASSERT_OK(astime_schedule_parse("tomorrow at 3pm", NOW, ASTIME_TZ_UTC, &t, err));
  ASSERT_EQ_INT(t.at, utc(2026, 10, 27, 15, 0));
  ASSERT_EQ_INT(t.at_offset_s, 0);
  ASSERT_EQ_INT(t.tz, ASTIME_TZ_UTC);
  ASSERT_OK(astime_schedule_parse("every day at 8", NOW, ASTIME_TZ_UTC, &t, err));
  ASSERT_EQ_INT(t.tz, ASTIME_TZ_UTC);
}

TEST(errors_are_helpful) {
  static const struct {
    const char *text, *msg;
  } cases[] = {
      {"whenever", "cannot read \"whenever\" as a schedule. Examples: \"every day at 8am\""},
      {"every monday", "\"every monday\": the time is missing: for example \"every day at 8am\""},
      {"tomorrow", "\"tomorrow\": the time is missing: for example \"tomorrow at 3pm\""},
      {"every day at 25", "invalid time: 25:00"},
      {"every day at 13pm", "invalid time: 13 pm"},
      {"every day at 8:75", "invalid time: 8:75"},
      {"today at 8", "that time is already in the past"},
      {"on 2025-01-01 at 9", "that time is already in the past"},
      {"on november 31 at 9", "invalid date: 2026-11-31"},
      {"every 0 minutes", "the interval must be between 1 minute and 366 days"},
      {"every 30 seconds", "cannot read"},
      {"on march 28 2027 at 2:30", "that time does not exist on that day (daylight saving change)"},
      {"@reboot", "@reboot is not supported"},
      {"cron: 0 8 * *", "expected 5 fields"},
      {"", "empty schedule"},
      {"2020-01-01T00:00:00Z", "is already in the past"},
  };
  size_t i;
  test_set_tz(TZ_ROME);
  for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    astime_trigger t;
    char err[ASTIME_ERR_SIZE];
    err[0] = '\0';
    ASSERT_ERR(astime_schedule_parse(cases[i].text, NOW, ASTIME_TZ_LOCAL, &t, err),
               ASTIME_ERR_INVALID);
    ASSERT_CONTAINS(err, cases[i].msg);
  }
}

TEST(routine_from_chat) {
  astime_routine r;
  char err[ASTIME_ERR_SIZE], *text = NULL;
  test_set_tz(TZ_ROME);
  ASSERT_OK(astime_routine_from_chat("every day at 8 | prepare the brief", NOW, ASTIME_TZ_LOCAL,
                                     &r, err));
  ASSERT_EQ_STR(r.name, "prepare-the-brief");
  ASSERT_EQ_STR(r.goal, "prepare the brief");
  ASSERT_TRUE(!r.enabled);
  ASSERT_EQ_INT(r.trigger.kind, ASTIME_TRIGGER_CRON);
  ASSERT_EQ_STR(r.trigger.cron_text, "0 8 * * *");
  ASSERT_EQ_INT(r.mode, ASTIME_MODE_CHAT);
  ASSERT_EQ_INT(r.profile, ASTIME_PROFILE_CHAT);
  ASSERT_EQ_INT(r.providers, ASTIME_PROVIDER_LOCAL);
  ASSERT_EQ_INT(r.notify, ASTIME_NOTIFY_ALWAYS);
  ASSERT_TRUE(!r.tools_set);
  ASSERT_OK(astime_routine_serialize(&r, &text));
  ASSERT_EQ_STR(text, "#routine {\n"
                      "  name: \"prepare-the-brief\",\n"
                      "  enabled: false,\n"
                      "  trigger: { cron: \"0 8 * * *\", tz: \"local\" },\n"
                      "  goal: \"prepare the brief\",\n"
                      "  mode: \"chat\",\n"
                      "  profile: \"chat\",\n"
                      "  providers: [\"local\"],\n"
                      "  notify: \"always\",\n"
                      "}\n");
  astime_free(text);
  astime_routine_free(&r);
  ASSERT_OK(astime_routine_from_chat("  tomorrow at 3pm |  Renew the car insurance, compare 3 offers!  ",
                                     NOW, ASTIME_TZ_LOCAL, &r, err));
  ASSERT_EQ_STR(r.name, "renew-the-car-insurance-compare-3-offers");
  ASSERT_EQ_STR(r.goal, "Renew the car insurance, compare 3 offers!");
  ASSERT_EQ_INT(r.trigger.kind, ASTIME_TRIGGER_AT);
  ASSERT_EQ_INT(r.trigger.at, utc(2026, 10, 27, 14, 0));
  ASSERT_OK(astime_routine_serialize(&r, &text));
  ASSERT_CONTAINS(text, "trigger: { at: \"2026-10-27T15:00:00+01:00\", tz: \"local\" },\n");
  astime_free(text);
  astime_routine_free(&r);
  /* goals with pipes keep everything after the first one */
  ASSERT_OK(astime_routine_from_chat("every 2 hours | grep ERROR | count", NOW, ASTIME_TZ_LOCAL, &r, err));
  ASSERT_EQ_STR(r.goal, "grep ERROR | count");
  astime_routine_free(&r);
  ASSERT_ERR(astime_routine_from_chat("every day at 8 prepare the brief", NOW, ASTIME_TZ_LOCAL, &r, err),
             ASTIME_ERR_INVALID);
  ASSERT_CONTAINS(err, "use the form \"<when> | <what to do>\"");
  ASSERT_ERR(astime_routine_from_chat("every day at 8 |   ", NOW, ASTIME_TZ_LOCAL, &r, err),
             ASTIME_ERR_INVALID);
  ASSERT_CONTAINS(err, "both the \"when\" and the \"what to do\" are needed");
  ASSERT_ERR(astime_routine_from_chat("sooner or later | something", NOW, ASTIME_TZ_LOCAL, &r, err),
             ASTIME_ERR_INVALID);
  ASSERT_CONTAINS(err, "cannot read \"sooner or later\"");
}

TEST(slugs) {
  char out[ASTIME_NAME_MAX];
  astime_slug_from_text("Élan: the café's crème brûlée? Déjà vu!", out);
  ASSERT_EQ_STR(out, "elan-the-cafe-s-creme-brulee-deja-vu");
  astime_slug_from_text("  Prepare the morning brief: calendar, news, status of the repositories  ", out);
  ASSERT_EQ_STR(out, "prepare-the-morning-brief-calendar-news");
  ASSERT_TRUE(strlen(out) <= 40);
  astime_slug_from_text("!!!", out);
  ASSERT_EQ_STR(out, "routine");
  astime_slug_from_text("Größe ☃ naïve", out);
  ASSERT_EQ_STR(out, "grosse-naive");
  astime_slug_from_text("abcdefghijabcdefghijabcdefghijabcdefghijabcdefghij", out);
  ASSERT_EQ_STR(out, "abcdefghijabcdefghijabcdefghijabcdefghij");
  ASSERT_TRUE(core_slug_valid(out));
}

TEST_LIST = {
#if !defined(_WIN32)
    TEST_ENTRY(recurring_phrasings),
    TEST_ENTRY(interval_phrasings),
    TEST_ENTRY(one_shot_phrasings),
    TEST_ENTRY(raw_cron_and_times),
    TEST_ENTRY(utc_zone),
    TEST_ENTRY(errors_are_helpful),
    TEST_ENTRY(routine_from_chat),
#endif
    TEST_ENTRY(slugs),
};

RUN_ALL_TESTS()
