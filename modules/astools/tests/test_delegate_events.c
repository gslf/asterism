/* delegate tool internals: CLI event parsers and the streaming runner. */

#include "astools_test.h"

#include "delegate.h"

#include <errno.h>
#include <signal.h>

static void feed(dlg_state *s, const char *line) {
  dlg_feed_line(s, line, strlen(line));
}

TEST(claude_success_stream) {
  dlg_state s;
  char *tail;
  dlg_state_init(&s, DLG_CLAUDE);
  feed(&s, "{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"sid-1\","
           "\"model\":\"claude-opus-5\",\"tools\":[\"Read\"]}");
  feed(&s, "{\"type\":\"assistant\",\"parent_tool_use_id\":null,\"message\":"
           "{\"content\":[{\"type\":\"text\",\"text\":\"one\"},{\"type\":"
           "\"tool_use\",\"id\":\"t1\",\"name\":\"Read\",\"input\":{}}]}}");
  feed(&s, "{\"type\":\"assistant\",\"parent_tool_use_id\":\"t1\",\"message\":"
           "{\"content\":[{\"type\":\"text\",\"text\":\"subagent\"}]}}");
  feed(&s, "{\"type\":\"user\",\"message\":{\"content\":[{\"type\":"
           "\"tool_result\",\"tool_use_id\":\"t1\",\"content\":\"x\"}]}}");
  feed(&s, "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,"
           "\"result\":\"done\",\"num_turns\":2,\"total_cost_usd\":0.5,"
           "\"usage\":{\"input_tokens\":10,\"output_tokens\":5,"
           "\"cache_read_input_tokens\":3,\"cache_creation_input_tokens\":2},"
           "\"session_id\":\"sid-1\"}");
  ASSERT_EQ_INT(s.saw_result, 1);
  ASSERT_EQ_INT(s.failed, 0);
  ASSERT_EQ_STR(s.final, "done");
  ASSERT_EQ_STR(s.session_id, "sid-1");
  ASSERT_EQ_STR(s.model, "claude-opus-5");
  ASSERT_EQ_INT(s.turns, 2);
  ASSERT_EQ_INT(s.have_usage, 1);
  ASSERT_EQ_INT(s.input_tokens, 15); /* cached and cache-written included */
  ASSERT_EQ_INT(s.cached_input_tokens, 3);
  ASSERT_EQ_INT(s.output_tokens, 5);
  ASSERT_EQ_INT(s.have_cost, 1);
  ASSERT_EQ_DBL(s.cost_usd, 0.5, 1e-12);
  ASSERT_EQ_INT(s.events, 5);
  ASSERT_EQ_INT(s.malformed, 0);
  ASSERT_TRUE(s.error == NULL && s.limit_seen == 0 && s.auth_error == 0);
  tail = dlg_tail_text(&s);
  ASSERT_EQ_STR(tail, "one"); /* subagent text is not the main thread's */
  free(tail);
  dlg_state_free(&s);
}

TEST(claude_errors_and_limits) {
  dlg_state s;
  dlg_state_init(&s, DLG_CLAUDE);
  /* The CLI retries a rate limit itself: a warning, not a stop. */
  feed(&s, "{\"type\":\"system\",\"subtype\":\"api_retry\",\"attempt\":1,"
           "\"error\":\"rate_limit\",\"error_status\":429}");
  ASSERT_EQ_INT(s.limit_seen, 1);
  ASSERT_EQ_INT(s.limit_hit, 0);
  feed(&s, "{\"type\":\"result\",\"subtype\":\"error_max_turns\",\"is_error\":"
           "true,\"num_turns\":3,\"errors\":[\"Reached maximum number of "
           "turns (3)\"],\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}");
  ASSERT_EQ_INT(s.failed, 1);
  ASSERT_TRUE(s.error && strstr(s.error, "maximum number of turns"));
  ASSERT_EQ_INT(s.limit_hit, 0);
  /* A later result decides; an API 429 there is a stopping limit. */
  feed(&s, "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":true,"
           "\"api_error_status\":429,\"result\":\"API Error: request "
           "rejected\"}");
  ASSERT_EQ_INT(s.limit_hit, 1);
  ASSERT_EQ_STR(s.limit_message, "API Error: request rejected");
  /* Success after all clears the error. */
  feed(&s, "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,"
           "\"result\":\"ok\"}");
  ASSERT_EQ_INT(s.failed, 0);
  ASSERT_TRUE(s.error == NULL);
  dlg_state_free(&s);

  dlg_state_init(&s, DLG_CLAUDE);
  feed(&s, "{\"type\":\"rate_limit_event\",\"rate_limit_info\":{\"status\":"
           "\"allowed_warning\",\"utilization\":0.85}}");
  ASSERT_EQ_INT(s.limit_hit, 0);
  ASSERT_TRUE(s.limit_message && strstr(s.limit_message, "85%"));
  feed(&s, "{\"type\":\"rate_limit_event\",\"rate_limit_info\":{\"status\":"
           "\"rejected\",\"resetsAt\":1793000000000,\"errorCode\":"
           "\"credits_required\"}}");
  ASSERT_EQ_INT(s.limit_hit, 1);
  ASSERT_EQ_INT(s.limit_resets_at, 1793000000); /* milliseconds accepted */
  ASSERT_TRUE(strstr(s.limit_message, "credits_required") != NULL);
  feed(&s, "{\"type\":\"assistant\",\"parent_tool_use_id\":null,\"error\":"
           "\"authentication_failed\",\"message\":{\"content\":[{\"type\":"
           "\"text\",\"text\":\"Invalid API key\"}]}}");
  ASSERT_EQ_INT(s.auth_error, 1);
  ASSERT_EQ_STR(s.error, "Invalid API key");
  dlg_state_free(&s);
}

TEST(codex_stream) {
  dlg_state s;
  dlg_state_init(&s, DLG_CODEX);
  feed(&s, "{\"type\":\"thread.started\",\"thread_id\":\"th-1\"}");
  feed(&s, "{\"type\":\"turn.started\"}");
  feed(&s, "{\"type\":\"item.started\",\"item\":{\"id\":\"i0\",\"type\":"
           "\"command_execution\",\"command\":\"ls\",\"status\":"
           "\"in_progress\"}}");
  feed(&s, "{\"type\":\"item.completed\",\"item\":{\"id\":\"i1\",\"type\":"
           "\"agent_message\",\"text\":\"first\"}}");
  feed(&s, "{\"type\":\"turn.completed\",\"usage\":{\"input_tokens\":100,"
           "\"cached_input_tokens\":80,\"output_tokens\":7,"
           "\"reasoning_output_tokens\":3}}");
  feed(&s, "{\"type\":\"turn.started\"}");
  feed(&s, "{\"type\":\"item.completed\",\"item\":{\"id\":\"i2\",\"type\":"
           "\"error\",\"message\":\"non-fatal\"}}");
  feed(&s, "{\"type\":\"item.completed\",\"item\":{\"id\":\"i3\",\"type\":"
           "\"agent_message\",\"text\":\"second\"}}");
  feed(&s, "{\"type\":\"turn.completed\",\"usage\":{\"input_tokens\":50,"
           "\"cached_input_tokens\":40,\"output_tokens\":3}}");
  ASSERT_EQ_STR(s.session_id, "th-1");
  ASSERT_EQ_INT(s.turns, 2);
  ASSERT_EQ_INT(s.saw_result, 1);
  ASSERT_EQ_INT(s.failed, 0);
  ASSERT_EQ_STR(s.final, "second");
  ASSERT_EQ_INT(s.input_tokens, 150);
  ASSERT_EQ_INT(s.cached_input_tokens, 120);
  ASSERT_EQ_INT(s.output_tokens, 10);
  ASSERT_EQ_INT(s.limit_seen, 0);
  feed(&s, "{\"type\":\"turn.failed\",\"error\":{\"message\":\"exceeded "
           "retry limit, last status: 429 Too Many Requests\"}}");
  ASSERT_EQ_INT(s.failed, 1);
  ASSERT_EQ_INT(s.limit_hit, 1);
  ASSERT_TRUE(strstr(s.error, "429 Too Many Requests") != NULL);
  dlg_state_free(&s);

  /* codex reports each transient retry as a top-level error event. */
  dlg_state_init(&s, DLG_CODEX);
  feed(&s, "{\"type\":\"turn.started\"}");
  feed(&s, "{\"type\":\"error\",\"message\":\"Reconnecting... 1/5 "
           "(429 Too Many Requests)\"}");
  feed(&s, "{\"type\":\"item.completed\",\"item\":{\"id\":\"i\",\"type\":"
           "\"agent_message\",\"text\":\"done\"}}");
  feed(&s, "{\"type\":\"turn.completed\",\"usage\":{\"input_tokens\":1,"
           "\"cached_input_tokens\":0,\"output_tokens\":1}}");
  ASSERT_EQ_INT(s.failed, 0);
  ASSERT_EQ_INT(s.limit_seen, 1);
  ASSERT_EQ_INT(s.limit_hit, 0);
  ASSERT_EQ_STR(s.final, "done");
  dlg_state_free(&s);

  dlg_state_init(&s, DLG_CODEX);
  feed(&s, "{\"type\":\"error\",\"message\":\"Not logged in. Run codex "
           "login.\"}");
  ASSERT_EQ_INT(s.auth_error, 1);
  ASSERT_EQ_INT(s.saw_result, 0);
  dlg_state_free(&s);
}

TEST(malformed_lines_and_bounds) {
  dlg_state s;
  char *big, *tail;
  size_t i, n;
  char deep[400];
  dlg_state_init(&s, DLG_CLAUDE);
  feed(&s, "");
  feed(&s, "   \r");
  feed(&s, "Warning: not an event");
  feed(&s, "{\"type\":");
  feed(&s, "[1,2]");
  feed(&s, "{\"no_type\":true}");
  feed(&s, "{\"type\":\"result\",\"is_error\":\"yes\",\"num_turns\":-4,"
           "\"usage\":[],\"total_cost_usd\":\"free\"}");
  /* Nesting beyond the codec's depth cap is rejected, not crashed on. */
  memcpy(deep, "{\"type\":\"x\",\"a\":", 16);
  for (i = 0; i < 100; i++) {
    deep[16 + i] = '[';
    deep[116 + i] = ']';
  }
  memcpy(deep + 216, "}", 2);
  feed(&s, deep);
  ASSERT_EQ_INT(s.malformed, 5);
  ASSERT_EQ_INT(s.events, 1);
  ASSERT_EQ_INT(s.failed, 0); /* wrongly typed fields are ignored */
  ASSERT_EQ_INT(s.turns, 0);
  ASSERT_EQ_INT(s.have_usage, 0);
  ASSERT_EQ_INT(s.have_cost, 0);

  /* Only the last DLG_TAIL_CHUNKS texts survive, oldest first. */
  for (i = 0; i < 10; i++) {
    char line[160];
    snprintf(line, sizeof line,
             "{\"type\":\"assistant\",\"parent_tool_use_id\":null,"
             "\"message\":{\"content\":[{\"type\":\"text\",\"text\":"
             "\"m%zu\"}]}}",
             i);
    feed(&s, line);
  }
  tail = dlg_tail_text(&s);
  ASSERT_EQ_STR(tail, "m4\n\nm5\n\nm6\n\nm7\n\nm8\n\nm9");
  free(tail);

  /* An oversized final is cut on a UTF-8 boundary ("é" is two bytes and
   * the odd prefix puts the cap in the middle of one). */
  n = 1 + 2 * 40000;
  big = malloc(n + 128);
  ASSERT_TRUE(big != NULL);
  strcpy(big, "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,"
              "\"result\":\"a");
  {
    size_t at = strlen(big);
    for (i = 0; i < 40000; i++) {
      big[at++] = (char)0xC3;
      big[at++] = (char)0xA9;
    }
    memcpy(big + at, "\"}", 3);
  }
  feed(&s, big);
  free(big);
  ASSERT_EQ_INT(s.final_truncated, 1);
  ASSERT_EQ_INT(strlen(s.final), DLG_FINAL_CAP - 1);
  ASSERT_TRUE(core_utf8_valid(s.final, strlen(s.final)));
  dlg_state_free(&s);
}

TEST(text_helpers) {
  const char *s = "ab\xC3\xA9" "c";
  ASSERT_EQ_INT(dlg_utf8_cut(s, 5, 3), 2);
  ASSERT_EQ_INT(dlg_utf8_cut(s, 5, 4), 4);
  ASSERT_EQ_INT(dlg_utf8_cut(s, 5, 9), 5);
  ASSERT_TRUE(dlg_text_is_limit("Claude AI usage limit reached|1793000000"));
  ASSERT_TRUE(dlg_text_is_limit("You've hit your limit \xC2\xB7 resets 3pm"));
  ASSERT_TRUE(dlg_text_is_limit("5-hour limit reached"));
  ASSERT_TRUE(dlg_text_is_limit("You've hit your usage limit. Upgrade to Pro"));
  ASSERT_TRUE(!dlg_text_is_limit("Fixed the loop bound in parse_row"));
  ASSERT_TRUE(dlg_text_is_auth("Invalid API key \xC2\xB7 Please run /login"));
  ASSERT_TRUE(dlg_text_is_auth("Not logged in"));
  ASSERT_TRUE(!dlg_text_is_auth("All tests pass"));
}

/* ---- runner ---------------------------------------------------------------- */

typedef struct {
  char lines[16][64];
  size_t n;
} line_log;

static void collect(void *ud, const char *line, size_t len) {
  line_log *l = ud;
  if (l->n < 16) {
    if (len > 63) len = 63;
    memcpy(l->lines[l->n], line, len);
    l->lines[l->n][len] = '\0';
    l->n++;
  }
}

static char *const k_env[] = {"PATH=/usr/bin:/bin", NULL};

static int run_sh(const char *script, const char *input, int64_t timeout_ms,
                  int64_t grace_ms, size_t max_line, size_t stderr_cap,
                  line_log *log, dlg_run_res *res) {
  char *argv[4];
  char emsg[256];
  dlg_run_opts o;
  argv[0] = "/bin/sh";
  argv[1] = "-c";
  argv[2] = (char *)script;
  argv[3] = NULL;
  memset(&o, 0, sizeof o);
  memset(log, 0, sizeof *log);
  o.argv = argv;
  o.envp = k_env;
  o.input = input;
  o.input_n = input ? strlen(input) : 0;
  o.timeout_ms = timeout_ms;
  o.grace_ms = grace_ms;
  o.max_line = max_line;
  o.stderr_cap = stderr_cap;
  o.on_line = collect;
  o.ud = log;
  return dlg_run(&o, res, emsg, sizeof emsg);
}

TEST(runner_lines_exit_and_stderr_tail) {
  line_log log;
  dlg_run_res res;
  ASSERT_EQ_INT(run_sh("printf 'a\\nbb\\n'; printf ccc; i=0; "
                       "while [ $i -lt 50 ]; do echo err$i >&2; i=$((i+1)); "
                       "done; exit 4",
                       NULL, 10000, 1000, 1024, 16, &log, &res),
                0);
  ASSERT_EQ_INT(res.exit_code, 4);
  ASSERT_EQ_INT(res.timed_out, 0);
  ASSERT_EQ_INT(log.n, 3);
  ASSERT_EQ_STR(log.lines[0], "a");
  ASSERT_EQ_STR(log.lines[1], "bb");
  ASSERT_EQ_STR(log.lines[2], "ccc"); /* last line without newline */
  ASSERT_TRUE(res.stderr_tail_n <= 16);
  ASSERT_TRUE(res.stderr_tail_n >= 6);
  ASSERT_EQ_STR(res.stderr_tail + res.stderr_tail_n - 6, "err49\n");
  ASSERT_TRUE(res.stderr_bytes > 200);
  dlg_run_res_free(&res);
}

TEST(runner_skips_oversized_lines) {
  line_log log;
  dlg_run_res res;
  ASSERT_EQ_INT(run_sh("printf '123456789\\nok\\nabcdefgh'", NULL, 10000,
                       1000, 4, 64, &log, &res),
                0);
  ASSERT_EQ_INT(log.n, 1);
  ASSERT_EQ_STR(log.lines[0], "ok");
  ASSERT_EQ_INT(res.oversized_lines, 2);
  ASSERT_EQ_INT(res.exit_code, 0);
  dlg_run_res_free(&res);
}

TEST(runner_feeds_stdin) {
  line_log log;
  dlg_run_res res;
  ASSERT_EQ_INT(run_sh("cat", "hello\nworld", 10000, 1000, 1024, 64, &log,
                       &res),
                0);
  ASSERT_EQ_INT(log.n, 2);
  ASSERT_EQ_STR(log.lines[0], "hello");
  ASSERT_EQ_STR(log.lines[1], "world");
  dlg_run_res_free(&res);
  /* A child that never reads its stdin cannot hurt the runner. */
  ASSERT_EQ_INT(run_sh("exec 0<&-; echo closed", "ignored input", 10000, 1000,
                       1024, 64, &log, &res),
                0);
  ASSERT_EQ_STR(log.lines[0], "closed");
  dlg_run_res_free(&res);
}

TEST(runner_deadline_term_then_kill) {
  line_log log;
  dlg_run_res res;
  ASSERT_EQ_INT(run_sh("echo started; exec sleep 30", NULL, 300, 3000, 1024,
                       64, &log, &res),
                0);
  ASSERT_EQ_INT(res.timed_out, 1);
  ASSERT_EQ_INT(res.exit_code, -SIGTERM);
  ASSERT_EQ_STR(log.lines[0], "started");
  ASSERT_TRUE(res.duration_ms < 3000);
  dlg_run_res_free(&res);
  /* SIGTERM ignored: SIGKILL after the grace period. */
  ASSERT_EQ_INT(run_sh("trap '' TERM; echo started; exec sleep 30", NULL,
                       300, 300, 1024, 64, &log, &res),
                0);
  ASSERT_EQ_INT(res.timed_out, 1);
  ASSERT_EQ_INT(res.exit_code, -SIGKILL);
  ASSERT_TRUE(res.duration_ms < 5000);
  dlg_run_res_free(&res);
}

TEST(runner_exec_failure_and_pipe_holders) {
  char *argv[2] = {"/nonexistent/astools-delegate-cli", NULL};
  char emsg[256];
  dlg_run_opts o;
  dlg_run_res res;
  line_log log;
  memset(&o, 0, sizeof o);
  o.argv = argv;
  o.envp = k_env;
  o.max_line = 64;
  o.stderr_cap = 64;
  ASSERT_EQ_INT(dlg_run(&o, &res, emsg, sizeof emsg), -1);
  ASSERT_EQ_INT(res.exec_errno, ENOENT);
  ASSERT_TRUE(strstr(emsg, "cannot execute") != NULL);
  dlg_run_res_free(&res);
  /* A descendant keeping the pipes open does not hold the run hostage
   * once the CLI itself has exited. */
  ASSERT_EQ_INT(run_sh("(sleep 3) & echo done", NULL, 20000, 1000, 1024, 64,
                       &log, &res),
                0);
  ASSERT_EQ_INT(res.exit_code, 0);
  ASSERT_EQ_STR(log.lines[0], "done");
  ASSERT_TRUE(res.duration_ms < 2900);
  dlg_run_res_free(&res);
}

TEST_LIST = {
    TEST_ENTRY(claude_success_stream),
    TEST_ENTRY(claude_errors_and_limits),
    TEST_ENTRY(codex_stream),
    TEST_ENTRY(malformed_lines_and_bounds),
    TEST_ENTRY(text_helpers),
    TEST_ENTRY(runner_lines_exit_and_stderr_tail),
    TEST_ENTRY(runner_skips_oversized_lines),
    TEST_ENTRY(runner_feeds_stdin),
    TEST_ENTRY(runner_deadline_term_then_kill),
    TEST_ENTRY(runner_exec_failure_and_pipe_holders),
};

RUN_ALL_TESTS()
