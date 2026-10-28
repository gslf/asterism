/*
 * test_text.c — HTML escaping, UTF-16 counting and message splitting.
 */

#include "aslink_test.h"

#define STAR "\xf0\x9f\x8c\x9f" /* U+1F31F: 4 bytes, 2 UTF-16 units */
#define EGRAVE "\xc3\xa8"       /* U+00E8: 2 bytes, 1 unit */

static size_t units_of(const char *s) {
  size_t u = 0;
  if (!aslink_utf16_units(s, strlen(s), &u)) return (size_t)-1;
  return u;
}

TEST(html_escape) {
  char *s = aslink_html_escape("a & b < c > d \"e\" 'f' " EGRAVE);
  ASSERT_EQ_STR(s, "a &amp; b &lt; c &gt; d &quot;e&quot; 'f' " EGRAVE);
  free(s);
  s = aslink_html_escape("");
  ASSERT_EQ_STR(s, "");
  free(s);
  s = aslink_html_escape("<b>&amp;</b>");
  ASSERT_EQ_STR(s, "&lt;b&gt;&amp;amp;&lt;/b&gt;");
  free(s);
  ASSERT_TRUE(aslink_html_escape(NULL) == NULL);
  ASSERT_TRUE(aslink_html_escape("bad \xff byte") == NULL);
  ASSERT_TRUE(aslink_html_escape("overlong \xc0\xaf") == NULL);
  ASSERT_TRUE(aslink_html_escape("surrogate \xed\xa0\x80") == NULL);
}

TEST(utf16_units) {
  ASSERT_EQ_INT(units_of(""), 0);
  ASSERT_EQ_INT(units_of("abc"), 3);
  ASSERT_EQ_INT(units_of("a" STAR EGRAVE), 4);
  ASSERT_EQ_INT(units_of("\xe2\x9c\xa8"), 1); /* U+2728, BMP */
  ASSERT_EQ_INT(units_of("\xff"), (size_t)-1);
  ASSERT_EQ_INT(units_of("\xf0\x9f\x8c"), (size_t)-1); /* truncated */
}

/* Split and compare against the expected parts (NULL-terminated). */
static int split_is(const char *text, size_t limit, const char *const *want) {
  char **parts = NULL;
  size_t n = 0, i;
  int ok = 1;
  if (aslink_split_text(text, limit, &parts, &n) != ASLINK_OK) return 0;
  for (i = 0; want[i]; i++) {
    if (i >= n || strcmp(parts[i], want[i]) != 0) {
      fprintf(stderr, "  part %u: got [%s] want [%s]\n", (unsigned)i,
              i < n ? parts[i] : "(none)", want[i]);
      ok = 0;
    }
  }
  if (i != n) {
    fprintf(stderr, "  got %u parts, want %u\n", (unsigned)n, (unsigned)i);
    ok = 0;
  }
  aslink_strings_free(parts, n);
  return ok;
}

TEST(split_short_and_empty) {
  static const char *const one[] = {"hello", NULL};
  static const char *const none[] = {NULL};
  ASSERT_TRUE(split_is("hello", 0, one));
  ASSERT_TRUE(split_is("", 0, none));
  ASSERT_TRUE(split_is("\n\n  \n", 0, none));
}

TEST(split_prefers_paragraphs_lines_spaces) {
  static const char *const para[] = {"para one.", "para two is", "longer", NULL};
  static const char *const line[] = {"line one here", "line two", NULL};
  /* a paragraph break in the first half loses to a line in the second */
  static const char *const late[] = {"ab\n\ncdefgh ij", "klmnop", NULL};
  static const char *const hard[] = {"abcdefghij", "klmnopqrst", "uvwxyz", NULL};
  ASSERT_TRUE(split_is("para one.\n\npara two is longer", 15, para));
  ASSERT_TRUE(split_is("line one here\nline two", 16, line));
  ASSERT_TRUE(split_is("ab\n\ncdefgh ij\nklmnop", 14, late));
  ASSERT_TRUE(split_is("abcdefghijklmnopqrstuvwxyz", 10, hard));
}

TEST(split_multibyte) {
  static const char *const grave[] = {EGRAVE EGRAVE EGRAVE EGRAVE,
                                      EGRAVE EGRAVE EGRAVE EGRAVE, EGRAVE EGRAVE, NULL};
  static const char *const stars3[] = {STAR, STAR, STAR, NULL};
  static const char *const stars4[] = {STAR STAR, STAR, NULL};
  static const char *const mixed[] = {"a", STAR, "b", NULL};
  ASSERT_TRUE(split_is(EGRAVE EGRAVE EGRAVE EGRAVE EGRAVE EGRAVE EGRAVE EGRAVE EGRAVE
                       EGRAVE, 4, grave));
  /* a surrogate pair counts 2 and is never split */
  ASSERT_TRUE(split_is(STAR STAR STAR, 3, stars3));
  ASSERT_TRUE(split_is(STAR STAR STAR, 4, stars4));
  ASSERT_TRUE(split_is("a" STAR "b", 2, mixed));
}

static char *repeat(const char *unit, size_t times) {
  size_t n = strlen(unit), i;
  char *s = (char *)malloc(n * times + 1);
  if (!s) return NULL;
  for (i = 0; i < times; i++) memcpy(s + i * n, unit, n);
  s[n * times] = 0;
  return s;
}

TEST(split_at_the_telegram_limit) {
  char **parts = NULL;
  size_t n = 0;
  char *a4096 = repeat("a", 4096), *a4097 = repeat("a", 4097);
  char *s2048 = repeat(STAR, 2048), *s2049 = repeat(STAR, 2049);
  ASSERT_TRUE(a4096 && a4097 && s2048 && s2049);
  ASSERT_OK(aslink_split_text(a4096, 0, &parts, &n));
  ASSERT_EQ_INT(n, 1);
  aslink_strings_free(parts, n);
  ASSERT_OK(aslink_split_text(a4097, 0, &parts, &n));
  ASSERT_EQ_INT(n, 2);
  ASSERT_EQ_INT(strlen(parts[0]), 4096);
  ASSERT_EQ_STR(parts[1], "a");
  aslink_strings_free(parts, n);
  ASSERT_OK(aslink_split_text(s2048, 0, &parts, &n));
  ASSERT_EQ_INT(n, 1);
  aslink_strings_free(parts, n);
  ASSERT_OK(aslink_split_text(s2049, 0, &parts, &n));
  ASSERT_EQ_INT(n, 2);
  ASSERT_EQ_INT(units_of(parts[0]), 4096);
  ASSERT_EQ_STR(parts[1], STAR);
  aslink_strings_free(parts, n);
  free(a4096);
  free(a4097);
  free(s2048);
  free(s2049);
}

/* A long text of words with no newline: every part fits, breaks fall on
 * spaces in the second half of the window, and nothing is lost. */
TEST(split_long_line) {
  char **parts = NULL;
  size_t n = 0, i, words = 0;
  char *text = repeat("parola " EGRAVE STAR " ", 3000); /* 3000 * 11 units */
  ASSERT_TRUE(text != NULL);
  ASSERT_OK(aslink_split_text(text, 0, &parts, &n));
  ASSERT_TRUE(n >= 9);
  for (i = 0; i < n; i++) {
    size_t u = units_of(parts[i]);
    const char *p;
    ASSERT_TRUE(u <= 4096);
    if (i + 1 < n) ASSERT_TRUE(u >= 2048);
    ASSERT_TRUE(parts[i][0] != ' ' && parts[i][strlen(parts[i]) - 1] != ' ');
    for (p = parts[i]; (p = strstr(p, "parola")) != NULL; p++) words++;
  }
  ASSERT_EQ_INT(words, 3000);
  aslink_strings_free(parts, n);
  free(text);
}

TEST(split_rejects_bad_input) {
  char **parts = (char **)1;
  size_t n = 5;
  ASSERT_ERR(aslink_split_text(NULL, 0, &parts, &n), ASLINK_ERR_INVALID);
  ASSERT_TRUE(parts == NULL);
  ASSERT_EQ_INT(n, 0);
  ASSERT_ERR(aslink_split_text("abc", 1, &parts, &n), ASLINK_ERR_INVALID);
  ASSERT_ERR(aslink_split_text("bad \xff", 0, &parts, &n), ASLINK_ERR_INVALID);
  ASSERT_ERR(aslink_split_text("abc", 0, NULL, &n), ASLINK_ERR_INVALID);
  aslink_strings_free(NULL, 0);
}

TEST(copy_display_truncates_on_boundaries) {
  char dst[4];
  aslink_copy_display(dst, sizeof dst, "ab" EGRAVE, 4);
  ASSERT_EQ_STR(dst, "ab"); /* "è" would need bytes 3-4 */
  aslink_copy_display(dst, sizeof dst, "abc", 3);
  ASSERT_EQ_STR(dst, "abc");
  aslink_copy_display(dst, sizeof dst, "a\xff", 2);
  ASSERT_EQ_STR(dst, "");
}

TEST(copy_display_neutralises_controls) {
  char dst[64];
  const char *spoof = "invoice\xe2\x80\xae" "fdp.exe"; /* U+202E */
  const char *ctrl = "Ann\nSYSTEM:\x1b[31m\xc2\x85x"; /* LF, ESC, NEL */
  aslink_copy_display(dst, sizeof dst, spoof, strlen(spoof));
  ASSERT_EQ_STR(dst, "invoice\xef\xbf\xbd" "fdp.exe");
  aslink_copy_display(dst, sizeof dst, ctrl, strlen(ctrl));
  ASSERT_EQ_STR(dst, "Ann SYSTEM: [31m  x");
  aslink_copy_display(dst, sizeof dst, "ok \xe2\x80\x8b", 6); /* ZWSP stays */
  ASSERT_EQ_STR(dst, "ok \xe2\x80\x8b");
}

TEST_LIST = {
  TEST_ENTRY(html_escape),
  TEST_ENTRY(utf16_units),
  TEST_ENTRY(split_short_and_empty),
  TEST_ENTRY(split_prefers_paragraphs_lines_spaces),
  TEST_ENTRY(split_multibyte),
  TEST_ENTRY(split_at_the_telegram_limit),
  TEST_ENTRY(split_long_line),
  TEST_ENTRY(split_rejects_bad_input),
  TEST_ENTRY(copy_display_truncates_on_boundaries),
  TEST_ENTRY(copy_display_neutralises_controls),
};

RUN_ALL_TESTS()
