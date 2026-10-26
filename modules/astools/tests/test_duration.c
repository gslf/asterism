/*
 * test_duration.c — duration.c: ISO 8601 periods in exact milliseconds.
 */

#include "astools_test.h"

#include "astools_internal.h"

static void dur_ok(const char *in, int64_t want) {
  int64_t got = -1;
  if (!astools_duration_parse_ms(in, &got)) {
    TEST_FAILF("duration rejected '%s'", in);
    return;
  }
  ASSERT_EQ_INT(got, want * 1000);
}


TEST(duration_accepts) {
  dur_ok("PT30S", 30);
  dur_ok("PT2M", 120);      /* PT1M is MINUTES */
  dur_ok("PT1M", 60);
  dur_ok("PT1H30M", 5400);
  dur_ok("P1DT1S", 86401);
  dur_ok("PT0S", 0);
  dur_ok("P1D", 86400);
  dur_ok("P1W", 604800);
  dur_ok("PT2H3M4S", 7384);
}

TEST(duration_rejects) {
  int64_t out = 0;
  /* P1M is MONTHS => rejected (calendar-dependent) */
  ASSERT_TRUE(!astools_duration_parse_ms("P1M", &out));
  ASSERT_TRUE(!astools_duration_parse_ms("P1Y", &out));
  ASSERT_TRUE(!astools_duration_parse_ms("P1Y2M3D", &out));
  ASSERT_TRUE(!astools_duration_parse_ms(NULL, &out));
  ASSERT_TRUE(!astools_duration_parse_ms("", &out));
  ASSERT_TRUE(!astools_duration_parse_ms("P", &out));
  ASSERT_TRUE(!astools_duration_parse_ms("PT", &out));
  ASSERT_TRUE(!astools_duration_parse_ms("30S", &out));
  ASSERT_TRUE(!astools_duration_parse_ms("PT30", &out));   /* missing unit */
  ASSERT_TRUE(!astools_duration_parse_ms("PT0.0001S", &out)); /* below precision */
  ASSERT_TRUE(!astools_duration_parse_ms("PT1S2H", &out)); /* wrong order */
  ASSERT_TRUE(!astools_duration_parse_ms("PT1H1H", &out)); /* repeated unit */
  ASSERT_TRUE(!astools_duration_parse_ms("P1W1D", &out));  /* W is exclusive */
  ASSERT_TRUE(!astools_duration_parse_ms("PT-1S", &out));
  ASSERT_TRUE(!astools_duration_parse_ms("PT1S ", &out));
}

TEST(duration_milliseconds_are_exact_and_bounded) {
  int64_t out = 0;
  const struct { const char *text; int64_t value; } cases[] = {
    {"PT0.001S",1}, {"PT1.01S",1010}, {"P1DT2H3M4.005S",93784005},
    {"PT9223372036854775.807S",INT64_MAX}
  };
  for (size_t i = 0; i < sizeof cases/sizeof *cases; i++) {
    ASSERT_TRUE(astools_duration_parse_ms(cases[i].text,&out));
    ASSERT_EQ_INT(out,cases[i].value);
  }
  const char *bad[] = {"PT9223372036854775.808S","P9999999999999999999D",
    "PT1.5H","PT1e2S","PT0,1S","PT1.S","PT.1S","PT0.0001S","P1DT","PT1.2S1S"};
  for (size_t i = 0; i < sizeof bad/sizeof *bad; i++) {
    out = 23; ASSERT_TRUE(!astools_duration_parse_ms(bad[i],&out)); ASSERT_EQ_INT(out,23);
  }
}

TEST(config_periods_preserve_milliseconds_without_wrapping) {
  char root[256], path[512]; ASSERT_TRUE(test_tmpdir(root));
  snprintf(path,sizeof path,"%s/config.xcdn",root);
  const char *durations[] = {"PT0.125S","PT4294967.294S","PT4294967.295S","PT0.0001S"};
  for (size_t i = 0; i < sizeof durations/sizeof *durations; i++) {
    FILE *file = fopen(path,"wb"); ASSERT_TRUE(file != NULL);
    ASSERT_TRUE(fprintf(file,"#astools_config {invocation:{timeout:r\"%s\"}}",durations[i]) > 0);
    ASSERT_EQ_INT(fclose(file),0);
    astools_config cfg; astools_config_defaults(&cfg); char *error = NULL;
    astools_err e = astools_config_load(path,true,&cfg,&error);
    ASSERT_EQ_INT(e,i < 2 ? ASTOOLS_OK : ASTOOLS_ERR_CONFIG);
    if (i < 2) ASSERT_EQ_INT(cfg.timeout_ms,i ? ASTOOLS_PERIOD_MAX_MS : 125);
    astools_config_free(&cfg); free(error);
  }
  test_rmtree(root);
}

TEST_LIST = {
    TEST_ENTRY(duration_accepts),
    TEST_ENTRY(duration_rejects),
    TEST_ENTRY(duration_milliseconds_are_exact_and_bounded),
    TEST_ENTRY(config_periods_preserve_milliseconds_without_wrapping),
};

RUN_ALL_TESTS()
