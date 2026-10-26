/*
 * test_util.c — control stripping and sentence trimming.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_test.h"

#include <string.h>

#include "asngn_internal.h"

TEST(strip_controls) {
  char s[] = "a\x01" "b\tc\nd\x7f" "e";
  size_t n = asngn_strip_controls(s, strlen(s));
  s[n] = '\0';
  ASSERT_EQ_STR(s, "ab\tc\nde");
}

TEST(sentence_trim_cases) {
  const char *s1 = "One. Two. Three incomplete";
  const char *s2 = "No boundary at all";
  const char *s3 = "Line one\nline two partial";
  const char *s4 = "Ends clean.";
  ASSERT_EQ_INT((long long)asngn_sentence_trim(s1, strlen(s1)), 9);
  ASSERT_EQ_INT((long long)asngn_sentence_trim(s2, strlen(s2)),
                (long long)strlen(s2));
  ASSERT_EQ_INT((long long)asngn_sentence_trim(s3, strlen(s3)), 9);
  ASSERT_EQ_INT((long long)asngn_sentence_trim(s4, strlen(s4)),
                (long long)strlen(s4));
}

TEST_LIST = {
    TEST_ENTRY(strip_controls),
    TEST_ENTRY(sentence_trim_cases),
};

RUN_ALL_TESTS()
