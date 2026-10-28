/*
 * test_cron.c — cron parsing and fire times: month ends, leap years, the
 * day-of-month / day-of-week rule, steps, names, macros, DST in
 * Europe/Rome, and invalid expressions.
 */

#include "astime_test.h"

static astime_err next_utc(const char *expr, int64_t after, int64_t *out) {
  astime_cron c;
  astime_err e = astime_cron_parse(expr, &c, NULL);
  if (e != ASTIME_OK) return e;
  return astime_cron_next(&c, after, ASTIME_TZ_UTC, out);
}

#define EXPECT_NEXT(expr, after, want)                \
  do {                                                \
    int64_t got_ = 0;                                 \
    ASSERT_OK(next_utc((expr), (after), &got_));      \
    ASSERT_EQ_INT(got_, (want));                      \
  } while (0)

TEST(parse_fields_and_masks) {
  astime_cron c;
  ASSERT_OK(astime_cron_parse("0 8 * * 1-5", &c, NULL));
  ASSERT_EQ_INT(c.minute, 1);
  ASSERT_EQ_INT(c.hour, 1u << 8);
  ASSERT_EQ_INT(c.dow, 0x3E);
  ASSERT_EQ_INT(c.month, 0x1FFE);
  ASSERT_EQ_INT(c.flags, ASTIME_CRON_DOM_STAR);
  ASSERT_OK(astime_cron_parse("  */15  9-17/2 1,15 jan,JUL Mon-Fri  ", &c, NULL));
  ASSERT_EQ_INT(c.minute, (UINT64_C(1) << 0) | (UINT64_C(1) << 15) | (UINT64_C(1) << 30) | (UINT64_C(1) << 45));
  ASSERT_EQ_INT(c.hour, (1u << 9) | (1u << 11) | (1u << 13) | (1u << 15) | (1u << 17));
  ASSERT_EQ_INT(c.dom, (1u << 1) | (1u << 15));
  ASSERT_EQ_INT(c.month, (1u << 1) | (1u << 7));
  ASSERT_EQ_INT(c.dow, 0x3E);
  ASSERT_EQ_INT(c.flags, 0);
  /* a/n means a-max/n */
  ASSERT_OK(astime_cron_parse("5/20 * * * *", &c, NULL));
  ASSERT_EQ_INT(c.minute, (UINT64_C(1) << 5) | (UINT64_C(1) << 25) | (UINT64_C(1) << 45));
}

TEST(sunday_is_zero_or_seven) {
  astime_cron a, b, s, r;
  ASSERT_OK(astime_cron_parse("0 0 * * 0", &a, NULL));
  ASSERT_OK(astime_cron_parse("0 0 * * 7", &b, NULL));
  ASSERT_OK(astime_cron_parse("0 0 * * sun", &s, NULL));
  ASSERT_EQ_INT(a.dow, 1);
  ASSERT_EQ_INT(b.dow, 1);
  ASSERT_EQ_INT(s.dow, 1);
  ASSERT_OK(astime_cron_parse("0 0 * * fri-sun", &r, NULL));
  ASSERT_EQ_INT(r.dow, (1u << 5) | (1u << 6) | 1u);
  ASSERT_OK(astime_cron_parse("0 0 * * 5-7", &r, NULL));
  ASSERT_EQ_INT(r.dow, (1u << 5) | (1u << 6) | 1u);
  ASSERT_OK(astime_cron_parse("0 0 * * */2", &r, NULL));
  ASSERT_EQ_INT(r.dow, 1u | (1u << 2) | (1u << 4) | (1u << 6));
}

TEST(macros) {
  astime_cron m, e;
  static const char *const pairs[][2] = {
      {"@hourly", "0 * * * *"}, {"@daily", "0 0 * * *"},   {"@midnight", "0 0 * * *"},
      {"@weekly", "0 0 * * 0"}, {"@monthly", "0 0 1 * *"}, {"@yearly", "0 0 1 1 *"},
      {"@ANNUALLY", "0 0 1 1 *"},
  };
  size_t i;
  char err[ASTIME_ERR_SIZE];
  for (i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
    ASSERT_OK(astime_cron_parse(pairs[i][0], &m, NULL));
    ASSERT_OK(astime_cron_parse(pairs[i][1], &e, NULL));
    ASSERT_TRUE(memcmp(&m, &e, sizeof m) == 0);
  }
  ASSERT_ERR(astime_cron_parse("@reboot", &m, err), ASTIME_ERR_INVALID);
  ASSERT_CONTAINS(err, "@reboot is not supported");
  ASSERT_ERR(astime_cron_parse("@often", &m, err), ASTIME_ERR_INVALID);
  ASSERT_CONTAINS(err, "unknown macro");
}

TEST(invalid_expressions) {
  static const char *const bad[][2] = {
      {"", "empty expression"},
      {"* * * *", "expected 5 fields"},
      {"* * * * * *", "got 6"},
      {"60 * * * *", "minute: value 60 out of range 0-59"},
      {"* 24 * * *", "hour: value 24 out of range 0-23"},
      {"* * 0 * *", "day-of-month: value 0 out of range 1-31"},
      {"* * 32 * *", "day-of-month: value 32"},
      {"* * * 13 *", "month: value 13 out of range 1-12"},
      {"* * * * 8", "day-of-week: value 8 out of range 0-7"},
      {"5-1 * * * *", "minute: range 5-1 is reversed"},
      {"*/0 * * * *", "minute: step 0 out of range"},
      {"*/ * * * *", "minute: missing step"},
      {"1,,2 * * * *", "minute: empty list item"},
      {"1, * * * *", "minute: empty list item"},
      {"abc * * * *", "minute: names are not allowed here"},
      {"0 0 * foo *", "month: unknown name \"foo\""},
      {"0 0 * * mon-", "day-of-week: missing value"},
      {"0 0 * * monday", "day-of-week: unknown name \"monday\""},
      {"1x * * * *", "minute: unexpected character 'x'"},
      {"0 0 L * *", "day-of-month: names are not allowed"},
  };
  size_t i;
  for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    astime_cron c;
    char err[ASTIME_ERR_SIZE];
    err[0] = '\0';
    ASSERT_ERR(astime_cron_parse(bad[i][0], &c, err), ASTIME_ERR_INVALID);
    ASSERT_CONTAINS(err, bad[i][1]);
  }
}

TEST(next_is_strictly_after) {
  int64_t t = utc(2026, 10, 26, 8, 0);
  EXPECT_NEXT("0 8 * * *", t, utc(2026, 10, 27, 8, 0));
  EXPECT_NEXT("0 8 * * *", t - 1, t);
  EXPECT_NEXT("* * * * *", t, t + 60);
  EXPECT_NEXT("* * * * *", t + 59, t + 60);
  EXPECT_NEXT("*/15 * * * *", utc(2026, 10, 26, 10, 7), utc(2026, 10, 26, 10, 15));
  EXPECT_NEXT("5/20 * * * *", utc(2026, 10, 26, 10, 46), utc(2026, 10, 26, 11, 5));
  EXPECT_NEXT("0 9-17/2 * * *", utc(2026, 10, 26, 9, 0), utc(2026, 10, 26, 11, 0));
  EXPECT_NEXT("0 9-17/2 * * *", utc(2026, 10, 26, 17, 0), utc(2026, 10, 27, 9, 0));
}

TEST(month_ends_and_year_end) {
  EXPECT_NEXT("0 0 31 * *", utc(2026, 4, 15, 0, 0), utc(2026, 5, 31, 0, 0));
  EXPECT_NEXT("0 0 31 * *", utc(2026, 5, 31, 0, 0), utc(2026, 7, 31, 0, 0));
  EXPECT_NEXT("30 23 30 * *", utc(2026, 2, 1, 0, 0), utc(2026, 3, 30, 23, 30));
  EXPECT_NEXT("0 0 1 * *", utc(2026, 12, 15, 0, 0), utc(2027, 1, 1, 0, 0));
  EXPECT_NEXT("59 23 31 12 *", utc(2026, 12, 31, 23, 59), utc(2027, 12, 31, 23, 59));
  EXPECT_NEXT("@yearly", utc(2026, 6, 1, 0, 0), utc(2027, 1, 1, 0, 0));
  EXPECT_NEXT("0 12 * feb,aug *", utc(2026, 2, 28, 12, 0), utc(2026, 8, 1, 12, 0));
}

TEST(leap_years) {
  EXPECT_NEXT("0 0 29 2 *", utc(2026, 3, 1, 0, 0), utc(2028, 2, 29, 0, 0));
  EXPECT_NEXT("0 0 29 2 *", utc(2028, 2, 29, 0, 0), utc(2032, 2, 29, 0, 0));
  /* 2100 is not a leap year: 2096 -> 2104 is the longest gap */
  EXPECT_NEXT("0 0 29 2 *", utc(2096, 3, 1, 0, 0), utc(2104, 2, 29, 0, 0));
  EXPECT_NEXT("0 0 28 2 *", utc(2028, 2, 28, 0, 0), utc(2029, 2, 28, 0, 0));
  EXPECT_NEXT("0 0 * 2 *", utc(2028, 2, 28, 0, 0), utc(2028, 2, 29, 0, 0));
}

TEST(impossible_is_not_found) {
  astime_cron c;
  int64_t out = 0;
  ASSERT_OK(astime_cron_parse("0 0 30 2 *", &c, NULL));
  ASSERT_ERR(astime_cron_next(&c, utc(2026, 1, 1, 0, 0), ASTIME_TZ_UTC, &out), ASTIME_ERR_NOT_FOUND);
  ASSERT_ERR(astime_cron_prev(&c, utc(2026, 1, 1, 0, 0), ASTIME_TZ_UTC, &out), ASTIME_ERR_NOT_FOUND);
  ASSERT_OK(astime_cron_parse("0 0 31 4,6,9,11 *", &c, NULL));
  ASSERT_ERR(astime_cron_next(&c, utc(2026, 1, 1, 0, 0), ASTIME_TZ_UTC, &out), ASTIME_ERR_NOT_FOUND);
}

TEST(dom_dow_or_rule) {
  /* Both restricted: the 13th OR any Friday. 2026-11-01 is a Sunday. */
  int64_t t = utc(2026, 11, 1, 0, 0);
  EXPECT_NEXT("0 12 13 * 5", t, utc(2026, 11, 6, 12, 0));
  EXPECT_NEXT("0 12 13 * 5", utc(2026, 11, 6, 12, 0), utc(2026, 11, 13, 12, 0));
  EXPECT_NEXT("0 12 13 * 5", utc(2026, 11, 13, 12, 0), utc(2026, 11, 20, 12, 0));
  /* 13 Dec 2026 is a Sunday: matches through the day-of-month. */
  EXPECT_NEXT("0 12 13 * 5", utc(2026, 12, 12, 0, 0), utc(2026, 12, 13, 12, 0));
  /* A starred field turns the rule into AND (Vixie): odd days that are Fridays. */
  EXPECT_NEXT("0 12 */2 * 5", t, utc(2026, 11, 13, 12, 0));
  EXPECT_NEXT("0 12 */2 * 5", utc(2026, 11, 13, 12, 0), utc(2026, 11, 27, 12, 0));
  /* Only the day-of-week restricted. */
  EXPECT_NEXT("0 9 * * mon", t, utc(2026, 11, 2, 9, 0));
  /* Only the day-of-month restricted. */
  EXPECT_NEXT("0 9 15 * *", t, utc(2026, 11, 15, 9, 0));
}

TEST(prev_is_at_or_before) {
  astime_cron c;
  int64_t out = 0;
  ASSERT_OK(astime_cron_parse("0 8 * * *", &c, NULL));
  ASSERT_OK(astime_cron_prev(&c, utc(2026, 10, 26, 8, 0), ASTIME_TZ_UTC, &out));
  ASSERT_EQ_INT(out, utc(2026, 10, 26, 8, 0));
  ASSERT_OK(astime_cron_prev(&c, utc(2026, 10, 26, 7, 59), ASTIME_TZ_UTC, &out));
  ASSERT_EQ_INT(out, utc(2026, 10, 25, 8, 0));
  ASSERT_OK(astime_cron_parse("0 0 29 2 *", &c, NULL));
  ASSERT_OK(astime_cron_prev(&c, utc(2027, 1, 1, 0, 0), ASTIME_TZ_UTC, &out));
  ASSERT_EQ_INT(out, utc(2024, 2, 29, 0, 0));
}

#if !defined(_WIN32)

static astime_err next_local(const char *expr, int64_t after, int64_t *out) {
  astime_cron c;
  astime_err e = astime_cron_parse(expr, &c, NULL);
  if (e != ASTIME_OK) return e;
  return astime_cron_next(&c, after, ASTIME_TZ_LOCAL, out);
}

/* 2026-03-29 02:00 CET jumps to 03:00 CEST (01:00 UTC). */
TEST(dst_gap_is_skipped) {
  int64_t out = 0;
  test_set_tz(TZ_ROME);
  /* 02:30 does not exist on the 29th: next is the 30th at 02:30 CEST. */
  ASSERT_OK(next_local("30 2 * * *", utc(2026, 3, 28, 11, 0), &out));
  ASSERT_EQ_INT(out, utc(2026, 3, 30, 0, 30));
  /* Hourly: 01:00 CET, then 03:00 CEST (02:00 is skipped). */
  ASSERT_OK(next_local("0 * * * *", utc(2026, 3, 28, 23, 30), &out));
  ASSERT_EQ_INT(out, utc(2026, 3, 29, 0, 0));
  ASSERT_OK(next_local("0 * * * *", utc(2026, 3, 29, 0, 0), &out));
  ASSERT_EQ_INT(out, utc(2026, 3, 29, 1, 0));
  /* Wall times around the gap are unaffected. */
  ASSERT_OK(next_local("59 1 * * *", utc(2026, 3, 28, 12, 0), &out));
  ASSERT_EQ_INT(out, utc(2026, 3, 29, 0, 59));
  ASSERT_OK(next_local("0 3 * * *", utc(2026, 3, 28, 12, 0), &out));
  ASSERT_EQ_INT(out, utc(2026, 3, 29, 1, 0));
  ASSERT_EQ_INT(astime_utc_offset(utc(2026, 3, 29, 1, 0), ASTIME_TZ_LOCAL), 7200);
  ASSERT_EQ_INT(astime_utc_offset(utc(2026, 3, 29, 0, 59), ASTIME_TZ_LOCAL), 3600);
}

/* 2026-10-25 03:00 CEST falls back to 02:00 CET (01:00 UTC). */
TEST(dst_repeat_fires_once) {
  int64_t out = 0;
  astime_cron c;
  test_set_tz(TZ_ROME);
  /* 02:30 happens twice: only the first (CEST, 00:30 UTC) fires. */
  ASSERT_OK(next_local("30 2 * * *", utc(2026, 10, 24, 12, 0), &out));
  ASSERT_EQ_INT(out, utc(2026, 10, 25, 0, 30));
  ASSERT_OK(next_local("30 2 * * *", out, &out));
  ASSERT_EQ_INT(out, utc(2026, 10, 26, 1, 30)); /* next day, CET */
  /* Asked from inside the repeated hour, the second 02:30 is not offered. */
  ASSERT_OK(next_local("30 2 * * *", utc(2026, 10, 25, 1, 10), &out));
  ASSERT_EQ_INT(out, utc(2026, 10, 26, 1, 30));
  /* Hourly: 02:00 CEST (00:00 UTC), then 03:00 CET (02:00 UTC). */
  ASSERT_OK(next_local("0 * * * *", utc(2026, 10, 24, 23, 30), &out));
  ASSERT_EQ_INT(out, utc(2026, 10, 25, 0, 0));
  ASSERT_OK(next_local("0 * * * *", out, &out));
  ASSERT_EQ_INT(out, utc(2026, 10, 25, 2, 0));
  /* prev from the second pass returns the first occurrence. */
  ASSERT_OK(astime_cron_parse("30 2 * * *", &c, NULL));
  ASSERT_OK(astime_cron_prev(&c, utc(2026, 10, 25, 1, 45), ASTIME_TZ_LOCAL, &out));
  ASSERT_EQ_INT(out, utc(2026, 10, 25, 0, 30));
  /* Ordinary days use the plain offset. */
  ASSERT_OK(next_local("0 8 * * 1-5", utc(2026, 10, 25, 12, 0), &out));
  ASSERT_EQ_INT(out, utc(2026, 10, 26, 7, 0));
  ASSERT_OK(next_local("0 8 * * 1-5", utc(2026, 7, 1, 12, 0), &out));
  ASSERT_EQ_INT(out, utc(2026, 7, 2, 6, 0));
}

TEST(local_and_utc_formatting) {
  char buf[ASTIME_TIME_TEXT_SIZE];
  test_set_tz(TZ_ROME);
  astime_format_time(utc(2026, 10, 26, 7, 0), ASTIME_TZ_LOCAL, buf);
  ASSERT_EQ_STR(buf, "2026-10-26 08:00");
  astime_format_time(utc(2026, 10, 26, 7, 0), ASTIME_TZ_UTC, buf);
  ASSERT_EQ_STR(buf, "2026-10-26 07:00 UTC");
  astime_format_time(utc(2026, 7, 1, 22, 30), ASTIME_TZ_LOCAL, buf);
  ASSERT_EQ_STR(buf, "2026-07-02 00:30");
}

#endif

TEST(trigger_set_cron_keeps_text) {
  astime_trigger t;
  char err[ASTIME_ERR_SIZE];
  ASSERT_OK(astime_trigger_set_cron(&t, "  0 8 * * MON-FRI ", ASTIME_TZ_UTC, err));
  ASSERT_EQ_INT(t.kind, ASTIME_TRIGGER_CRON);
  ASSERT_EQ_STR(t.cron_text, "0 8 * * MON-FRI");
  ASSERT_EQ_INT(t.cron.dow, 0x3E);
  ASSERT_ERR(astime_trigger_set_cron(&t, "0 8 * *", ASTIME_TZ_UTC, err), ASTIME_ERR_INVALID);
  ASSERT_CONTAINS(err, "expected 5 fields");
}

TEST_LIST = {
    TEST_ENTRY(parse_fields_and_masks),
    TEST_ENTRY(sunday_is_zero_or_seven),
    TEST_ENTRY(macros),
    TEST_ENTRY(invalid_expressions),
    TEST_ENTRY(next_is_strictly_after),
    TEST_ENTRY(month_ends_and_year_end),
    TEST_ENTRY(leap_years),
    TEST_ENTRY(impossible_is_not_found),
    TEST_ENTRY(dom_dow_or_rule),
    TEST_ENTRY(prev_is_at_or_before),
#if !defined(_WIN32)
    TEST_ENTRY(dst_gap_is_skipped),
    TEST_ENTRY(dst_repeat_fires_once),
    TEST_ENTRY(local_and_utc_formatting),
#endif
    TEST_ENTRY(trigger_set_cron_keeps_text),
};

RUN_ALL_TESTS()
