/*
 * astime_test.h — astime tests on the shared harness (core_test.h).
 *
 * Time zones are set per test with test_set_tz. "Europe/Rome" is spelled as
 * its POSIX rule so the DST tests need no tzdata on the machine.
 */

#ifndef ASTIME_TEST_H
#define ASTIME_TEST_H

#define TEST_ERR_TYPE astime_err
#define TEST_ERR_OK   ASTIME_OK
#define TEST_ERR_NAME astime_err_name
#include "core_test.h"

#include <time.h>

#include "astime.h"

#define TZ_ROME "CET-1CEST,M3.5.0,M10.5.0/3"

static inline void test_set_tz(const char *tz) {
#if defined(_WIN32)
  _putenv_s("TZ", tz);
  _tzset();
#else
  setenv("TZ", tz, 1);
  tzset();
#endif
}

/* Unix seconds of a UTC civil time. */
static inline int64_t utc(int y, int mo, int d, int h, int mi) {
  int64_t yy = y - (mo <= 2), era = (yy >= 0 ? yy : yy - 399) / 400;
  unsigned yoe = (unsigned)(yy - era * 400);
  unsigned doy = (unsigned)((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = era * 146097 + (int64_t)doe - 719468;
  return days * 86400 + (int64_t)h * 3600 + (int64_t)mi * 60;
}

/* Write text to dir/name; 1 on success. */
static inline int test_write(const char *dir, const char *name, const char *text) {
  char path[512];
  FILE *f;
  snprintf(path, sizeof path, "%s/%s", dir, name);
  f = fopen(path, "wb");
  if (!f) return 0;
  fputs(text, f);
  return fclose(f) == 0;
}

/* Fails the test (from the calling TEST) when needle is not in hay. */
#define ASSERT_CONTAINS(hay, needle)                                         \
  do {                                                                       \
    const char *h_ = (hay), *n_ = (needle);                                  \
    if (!h_ || !n_ || !strstr(h_, n_)) {                                     \
      TEST_FAILF("ASSERT_CONTAINS(%s):\n   text: %s\n   want: %s", #hay,     \
                 h_ ? h_ : "(null)", n_ ? n_ : "(null)");                    \
      return;                                                                \
    }                                                                        \
  } while (0)

#endif /* ASTIME_TEST_H */
