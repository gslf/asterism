/*
 * test_log.c — level gate, record format, callback fan-out, rotation.
 */

#include "core_test.h"

#include <string.h>

typedef struct {
  int n;
  int saw_debug;
  char last[512];
} capture;

static void capture_cb(int level, const char *line, void *ud) {
  capture *c = (capture *)ud;
  c->n++;
  if (level == CORE_LOG_DEBUG) c->saw_debug = 1;
  snprintf(c->last, sizeof c->last, "%s", line);
}

static void write_line(core_log *log, int level, const char *subsys,
                       const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  core_log_vwrite(log, 1786701600LL, level, subsys, fmt, ap);
  va_end(ap);
}

TEST(gate_format_and_callback) {
  char dir[256], path[300];
  core_log log;
  capture cap;
  char *text;
  memset(&cap, 0, sizeof cap);
  ASSERT_TRUE(test_tmpdir(dir));
  snprintf(path, sizeof path, "%s/engine.log", dir);
  core_log_init(&log);
  core_log_set_callback(&log, capture_cb, &cap);
  ASSERT_OK(core_log_open(&log, path, CORE_LOG_INFO, 0, 1, false));
  write_line(&log, CORE_LOG_INFO, "session", "hello info %d", 7);
  write_line(&log, CORE_LOG_DEBUG, "cache", "debug hidden");
  write_line(&log, CORE_LOG_INFO, "route", "line one\nline two");
  core_log_destroy(&log);

  /* The callback sees every record; records are single lines. */
  ASSERT_EQ_INT(cap.n, 3);
  ASSERT_TRUE(cap.saw_debug);
  ASSERT_TRUE(strstr(cap.last, "line one line two") != NULL);

  text = test_read_file(path, NULL);
  ASSERT_TRUE(text != NULL);
  ASSERT_TRUE(strstr(text, "debug hidden") == NULL);
  ASSERT_TRUE(strncmp(text, "2026-08-14T10:00:00Z INFO  session  hello info 7\n",
                      49) == 0);
  ASSERT_TRUE(strstr(text, "line one line two") != NULL);
  free(text);
  test_rmtree(dir);
}

TEST(rotation_keeps_max_files) {
  char dir[256], path[300], rot1[310], rot2[310];
  core_log log;
  int i;
  ASSERT_TRUE(test_tmpdir(dir));
  snprintf(path, sizeof path, "%s/engine.log", dir);
  snprintf(rot1, sizeof rot1, "%s.1", path);
  snprintf(rot2, sizeof rot2, "%s.2", path);
  core_log_init(&log);
  ASSERT_OK(core_log_open(&log, path, CORE_LOG_INFO, 1, 2, false));
  for (i = 0; i < 100; i++)
    write_line(&log, CORE_LOG_INFO, "test", "record %03d padding padding", i);
  core_log_destroy(&log);
  ASSERT_TRUE(os_file_exists(path));
  ASSERT_TRUE(os_file_exists(rot1));
  ASSERT_TRUE(!os_file_exists(rot2));
  test_rmtree(dir);
}

TEST(no_path_disables_the_file_sink) {
  core_log log;
  capture cap;
  memset(&cap, 0, sizeof cap);
  core_log_init(&log);
  core_log_set_callback(&log, capture_cb, &cap);
  ASSERT_OK(core_log_open(&log, NULL, CORE_LOG_DEBUG, 0, 1, false));
  write_line(&log, CORE_LOG_WARN, "x", "only the callback");
  ASSERT_EQ_INT(cap.n, 1);
  ASSERT_TRUE(log.fp == NULL);
  core_log_destroy(&log);
}

TEST_LIST = {
    TEST_ENTRY(gate_format_and_callback),
    TEST_ENTRY(rotation_keeps_max_files),
    TEST_ENTRY(no_path_disables_the_file_sink),
};

RUN_ALL_TESTS()
