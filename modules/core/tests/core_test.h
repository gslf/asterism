/*
 * core_test.h — the test harness shared by every module (no framework).
 *
 * Usage:
 *   TEST(my_case) { ASSERT_EQ_INT(1 + 1, 2); }
 *   TEST_LIST = { TEST_ENTRY(my_case), };
 *   RUN_ALL_TESTS()
 *
 * Every assertion prints file:line on failure and aborts the current test
 * (early return) while the runner continues with the remaining tests.
 * The process exit code is the number of failed tests.
 *
 * Include this header FIRST in every test file: it defines the POSIX
 * feature-test macros needed by mkdtemp before any system header lands.
 *
 * ASSERT_OK / ASSERT_ERR check the error type named by TEST_ERR_TYPE,
 * TEST_ERR_OK and TEST_ERR_NAME; a module harness defines them before
 * including this file. The default is core_err.
 */

#ifndef CORE_TEST_H
#define CORE_TEST_H

#if !defined(_WIN32)
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"

#ifndef TEST_ERR_TYPE
#define TEST_ERR_TYPE core_err
#define TEST_ERR_OK   CORE_OK
#define TEST_ERR_NAME core_err_name
#endif

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef void (*test_fn)(void);
typedef struct {
  const char *name;
  test_fn fn;
} test_case;

static int test_case_failed = 0;

#define TEST(name) static void name(void)
#define TEST_LIST static const test_case test_cases[]
#define TEST_ENTRY(name) { #name, name }

#define TEST_FAILF(...)                                    \
  do {                                                      \
    fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__);  \
    fprintf(stderr, __VA_ARGS__);                           \
    fputc('\n', stderr);                                    \
    test_case_failed = 1;                             \
  } while (0)

#define ASSERT_TRUE(cond)                                   \
  do {                                                      \
    if (!(cond)) {                                          \
      TEST_FAILF("ASSERT_TRUE(%s)", #cond);                \
      return;                                               \
    }                                                       \
  } while (0)

#define ASSERT_EQ_INT(actual, expected)                                     \
  do {                                                                      \
    long long a_ = (long long)(actual), e_ = (long long)(expected);         \
    if (a_ != e_) {                                                         \
      TEST_FAILF("ASSERT_EQ_INT(%s, %s): %lld != %lld", #actual,           \
                  #expected, a_, e_);                                       \
      return;                                                               \
    }                                                                       \
  } while (0)

#define ASSERT_EQ_STR(actual, expected)                                     \
  do {                                                                      \
    const char *a_ = (actual), *e_ = (expected);                            \
    if (a_ == NULL || e_ == NULL || strcmp(a_, e_) != 0) {                  \
      TEST_FAILF("ASSERT_EQ_STR(%s):\n   actual: %s\n expected: %s",       \
                  #actual, a_ ? a_ : "(null)", e_ ? e_ : "(null)");         \
      return;                                                               \
    }                                                                       \
  } while (0)

#define ASSERT_EQ_DBL(actual, expected, eps)                                \
  do {                                                                      \
    double a_ = (actual), e_ = (expected);                                  \
    if (!(fabs(a_ - e_) <= (double)(eps))) {                                \
      TEST_FAILF("ASSERT_EQ_DBL(%s, %s): %.17g != %.17g (eps %g)",         \
                  #actual, #expected, a_, e_, (double)(eps));               \
      return;                                                               \
    }                                                                       \
  } while (0)

#define ASSERT_OK(expr)                                                     \
  do {                                                                      \
    TEST_ERR_TYPE e_ = (expr);                                              \
    if (e_ != TEST_ERR_OK) {                                                \
      TEST_FAILF("ASSERT_OK(%s): %s", #expr, TEST_ERR_NAME(e_));            \
      return;                                                               \
    }                                                                       \
  } while (0)

#define ASSERT_ERR(expr, want)                                              \
  do {                                                                      \
    TEST_ERR_TYPE e_ = (expr);                                              \
    if (e_ != (want)) {                                                     \
      TEST_FAILF("ASSERT_ERR(%s): got %s, want %s", #expr,                  \
                 TEST_ERR_NAME(e_), TEST_ERR_NAME(want));                   \
      return;                                                               \
    }                                                                       \
  } while (0)

/* Fresh private temp directory; 1 on success. out must hold 256 bytes. */
static int test_tmpdir(char out[256]) {
#if defined(_WIN32)
  char base[MAX_PATH];
  DWORD n = GetTempPathA(MAX_PATH, base);
  unsigned tries;
  if (n == 0 || n >= MAX_PATH) return 0;
  for (tries = 0; tries < 100; tries++) {
    snprintf(out, 256, "%sasterism_test_%08x_%u", base,
             (unsigned)GetTickCount() ^ ((unsigned)rand() << 12), tries);
    if (_mkdir(out) == 0) {
      for (size_t i = 0; out[i]; i++) if (out[i] == '\\') out[i] = '/';
      return 1;
    }
  }
  return 0;
#else
  snprintf(out, 256, "%s", "/tmp/asterism_test_XXXXXX");
  if (mkdtemp(out) == NULL) return 0;
  char *resolved = NULL;
  if (os_realpath(out, &resolved) != CORE_OK || strlen(resolved) >= 256) {
    free(resolved); rmdir(out); return 0;
  }
  strcpy(out, resolved);
  free(resolved);
  return 1;
#endif
}

/* Best-effort recursive delete of a test tree. */
static void test_rmtree(const char *path) {
#if defined(_WIN32)
  char pattern[512], sub[512];
  WIN32_FIND_DATAA fd;
  HANDLE h;
  snprintf(pattern, sizeof pattern, "%s\\*", path);
  h = FindFirstFileA(pattern, &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
        continue;
      snprintf(sub, sizeof sub, "%s\\%s", path, fd.cFileName);
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        test_rmtree(sub);
      else
        DeleteFileA(sub);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
  }
  RemoveDirectoryA(path);
#else
  DIR *d = opendir(path);
  struct dirent *ent;
  struct stat st;
  char sub[1024];
  if (!d) {
    remove(path);
    return;
  }
  while ((ent = readdir(d)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;
    snprintf(sub, sizeof sub, "%s/%s", path, ent->d_name);
    if (lstat(sub, &st) == 0 && S_ISDIR(st.st_mode))
      test_rmtree(sub);
    else
      remove(sub);
  }
  closedir(d);
  rmdir(path);
#endif
}

/* Whole file into a malloc'd NUL-terminated buffer; NULL if unreadable. */
static char *test_read_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  long sz;
  char *buf;
  size_t got;
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 ||
      fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }
  buf = (char *)malloc((size_t)sz + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  got = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  buf[got] = '\0';
  if (out_len) *out_len = got;
  return buf;
}

#define RUN_ALL_TESTS()                                                     \
  int main(void) {                                                          \
    size_t i_, n_ = sizeof(test_cases) / sizeof(test_cases[0]); \
    int failed_ = 0;                                                        \
    (void)test_tmpdir;                                                \
    (void)test_rmtree;                                                \
    (void)test_read_file;                                             \
    for (i_ = 0; i_ < n_; i_++) {                                           \
      test_case_failed = 0;                                           \
      fprintf(stderr, "[ RUN  ] %s\n", test_cases[i_].name);          \
      test_cases[i_].fn();                                            \
      if (test_case_failed) {                                         \
        failed_++;                                                          \
        fprintf(stderr, "[ FAIL ] %s\n", test_cases[i_].name);        \
      } else {                                                              \
        fprintf(stderr, "[  OK  ] %s\n", test_cases[i_].name);        \
      }                                                                     \
    }                                                                       \
    fprintf(stderr, "%d of %d tests passed\n", (int)n_ - failed_, (int)n_); \
    return failed_;                                                         \
  }

#endif /* CORE_TEST_H */
