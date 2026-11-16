/* Line anchors: the tag rule (std/anchor.c compiled in directly) and the
 * packaged fs.read / grep.search / code.read-range / edit.lines commands. */

#include "astools_test.h"

#include "astools.h"
#include "sdk.h"
#include "xcdn.h"

#ifndef ASTOOLS_STD_PACKAGES
#define ASTOOLS_STD_PACKAGES "packages"
#endif

typedef struct {
  char ws[256];
  char file[512];
  astools_ctx *c;
} fixture;

static int write_bytes(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  if (fputs(text, f) < 0) {
    fclose(f);
    return 0;
  }
  return fclose(f) == 0;
}

static int fixture_open(fixture *f, const char *content) {
  static const char *roots[2] = {ASTOOLS_STD_PACKAGES, NULL};
  astools_open_params op;
  memset(f, 0, sizeof *f);
  if (!test_tmpdir(f->ws)) return 0;
  snprintf(f->file, sizeof f->file, "%s/f.txt", f->ws);
  if (!write_bytes(f->file, content)) return 0;
  memset(&op, 0, sizeof op);
  op.registry_paths = roots;
  op.workspace_root = f->ws;
  return astools_open(&op, &f->c) == ASTOOLS_OK;
}

static void fixture_close(fixture *f) {
  astools_close(f->c);
  test_rmtree(f->ws);
}

/* "LINE:TAG" of a line with this text. */
static const char *anchor(int line, const char *text) {
  static char buf[8][32];
  static int at;
  char tag[4];
  at = (at + 1) % 8;
  astd_line_tag(text, strlen(text), tag);
  snprintf(buf[at], sizeof buf[at], "%d:%s", line, tag);
  return buf[at];
}

/* Invoke and copy one string field of the result (empty when absent). */
static int result_field(astools_ctx *c, const char *tool, const char *cmd,
                        const char *args, const char *field, char *out,
                        size_t out_sz) {
  astools_result r;
  xcdn_document_t *doc;
  const xcdn_node_t *node;
  int ok;
  memset(&r, 0, sizeof r);
  out[0] = '\0';
  if (astools_invoke(c, tool, cmd, args, 0, &r) != ASTOOLS_OK) return -1;
  ok = r.ok;
  if (ok && r.result_xcdn && (doc = xcdn_parse(r.result_xcdn, NULL)) != NULL) {
    node = doc->values_len == 1
               ? xcdn_object_get(doc->values[0]->value, field) : NULL;
    if (node && node->value->type == XCDN_VAL_STRING)
      snprintf(out, out_sz, "%s", node->value->data.string);
    xcdn_document_free(doc);
  }
  astools_result_free(&r);
  return ok;
}

/* Run edit.lines; returns ok, copies the error code and message. */
static int edit_lines(fixture *f, const char *ops, char *code, size_t code_sz,
                      char *msg, size_t msg_sz) {
  char args[2048];
  astools_result r;
  int ok;
  memset(&r, 0, sizeof r);
  snprintf(args, sizeof args, "{path: \"f.txt\", ops: [%s]}", ops);
  if (astools_invoke(f->c, "edit", "lines", args, 0, &r) != ASTOOLS_OK)
    return -1;
  ok = r.ok;
  if (code) snprintf(code, code_sz, "%s", r.error_code ? r.error_code : "");
  if (msg) snprintf(msg, msg_sz, "%s", r.error_message ? r.error_message : "");
  astools_result_free(&r);
  return ok;
}

static void expect_file(const fixture *f, const char *want) {
  char *got = test_read_file(f->file, NULL);
  ASSERT_TRUE(got != NULL);
  ASSERT_EQ_STR(got, want);
  free(got);
}

TEST(tag_rule_is_stable) {
  char a[4], b[4];
  int64_t line = 0;
  char *text;
  char want[64];
  size_t i;
  /* fixed value: the rule must not change between builds or platforms */
  astd_line_tag("int x = 1;", 10, a);
  ASSERT_EQ_STR(a, "i18");
  ASSERT_EQ_INT((int)strlen(a), 3);
  for (i = 0; i < 3; i++)
    ASSERT_TRUE((a[i] >= '0' && a[i] <= '9') || (a[i] >= 'a' && a[i] <= 'z'));
  /* trailing spaces, tabs, CR and the newline do not count */
  astd_line_tag("int x = 1; \t\r\n", 14, b);
  ASSERT_EQ_STR(a, b);
  /* leading whitespace and content do */
  astd_line_tag("  int x = 1;", 12, b);
  ASSERT_TRUE(strcmp(a, b) != 0);
  astd_line_tag("int x = 2;", 10, b);
  ASSERT_TRUE(strcmp(a, b) != 0);

  ASSERT_EQ_INT(astd_anchor_parse("42:ab3", &line, a), 0);
  ASSERT_EQ_INT((int)line, 42);
  ASSERT_EQ_STR(a, "ab3");
  ASSERT_EQ_INT(astd_anchor_parse("7:AB3|pasted text", &line, a), 0);
  ASSERT_EQ_INT((int)line, 7);
  ASSERT_EQ_STR(a, "ab3");
  ASSERT_TRUE(astd_anchor_parse("42", &line, a) != 0);
  ASSERT_TRUE(astd_anchor_parse("0:ab3", &line, a) != 0);
  ASSERT_TRUE(astd_anchor_parse("42:ab", &line, a) != 0);
  ASSERT_TRUE(astd_anchor_parse("42:ab3x", &line, a) != 0);
  ASSERT_TRUE(astd_anchor_parse("x:ab3", &line, a) != 0);

  /* anchored text keeps line endings and a missing final newline */
  text = astd_anchor_text("a\r\nb", 4, 9, NULL);
  ASSERT_TRUE(text != NULL);
  snprintf(want, sizeof want, "%s|a\r\n", anchor(9, "a"));
  snprintf(want + strlen(want), sizeof want - strlen(want), "%s|b",
           anchor(10, "b"));
  ASSERT_EQ_STR(text, want);
  free(text);
}

TEST(reads_carry_anchors) {
  fixture f;
  char got[512], want[512], args[256];
  astools_result r;
  ASSERT_TRUE(fixture_open(&f, "alpha\nbeta\ngamma\n"));

  /* default output is unchanged */
  ASSERT_EQ_INT(result_field(f.c, "fs", "read", "{path: \"f.txt\"}", "content",
                             got, sizeof got), 1);
  ASSERT_EQ_STR(got, "alpha\nbeta\ngamma\n");

  ASSERT_EQ_INT(result_field(f.c, "fs", "read",
                             "{path: \"f.txt\", anchors: true}", "content",
                             got, sizeof got), 1);
  snprintf(want, sizeof want, "%s|alpha\n%s|beta\n%s|gamma\n",
           anchor(1, "alpha"), anchor(2, "beta"), anchor(3, "gamma"));
  ASSERT_EQ_STR(got, want);

  /* paging returns whole lines, keeps numbering, and says where to go on */
  memset(&r, 0, sizeof r);
  ASSERT_OK(astools_invoke(f.c, "fs", "read",
                           "{path: \"f.txt\", anchors: true, max_bytes: 8}",
                           0, &r));
  ASSERT_EQ_INT(r.ok, 1);
  ASSERT_TRUE(strstr(r.result_xcdn, "next_offset") != NULL);
  ASSERT_TRUE(strstr(r.result_xcdn, "beta") == NULL);
  astools_result_free(&r);
  ASSERT_EQ_INT(result_field(f.c, "fs", "read",
                             "{path: \"f.txt\", anchors: true, offset: 6}",
                             "content", got, sizeof got), 1);
  snprintf(want, sizeof want, "%s|beta\n%s|gamma\n", anchor(2, "beta"),
           anchor(3, "gamma"));
  ASSERT_EQ_STR(got, want);
  /* an offset inside a line starts at the next line */
  ASSERT_EQ_INT(result_field(f.c, "fs", "read",
                             "{path: \"f.txt\", anchors: true, offset: 8}",
                             "content", got, sizeof got), 1);
  snprintf(want, sizeof want, "%s|gamma\n", anchor(3, "gamma"));
  ASSERT_EQ_STR(got, want);

  memset(&r, 0, sizeof r);
  ASSERT_OK(astools_invoke(f.c, "fs", "read",
                           "{path: \"f.txt\", anchors: true, "
                           "encoding: \"base64\"}", 0, &r));
  ASSERT_EQ_INT(r.ok, 0);
  astools_result_free(&r);

  ASSERT_EQ_INT(result_field(f.c, "code", "read-range",
                             "{path: \"f.txt\", start_line: 2, end_line: 2, "
                             "anchors: true}", "content", got, sizeof got), 1);
  snprintf(want, sizeof want, "%s|beta\n", anchor(2, "beta"));
  ASSERT_EQ_STR(got, want);

  memset(&r, 0, sizeof r);
  ASSERT_OK(astools_invoke(f.c, "grep", "search",
                           "{pattern: \"gam\", path: \".\", anchors: true}",
                           0, &r));
  ASSERT_EQ_INT(r.ok, 1);
  ASSERT_TRUE(strstr(r.result_xcdn, anchor(3, "gamma")) != NULL);
  astools_result_free(&r);
  memset(&r, 0, sizeof r);
  ASSERT_OK(astools_invoke(f.c, "grep", "search",
                           "{pattern: \"gam\", path: \".\"}", 0, &r));
  ASSERT_EQ_INT(r.ok, 1);
  ASSERT_TRUE(strstr(r.result_xcdn, "anchor") == NULL);
  astools_result_free(&r);

  /* the anchors a read returns are the ones edit.lines accepts */
  snprintf(args, sizeof args, "{start: \"%s\", action: \"delete\"}",
           anchor(2, "beta"));
  ASSERT_EQ_INT(edit_lines(&f, args, NULL, 0, NULL, 0), 1);
  expect_file(&f, "alpha\ngamma\n");
  fixture_close(&f);
}

TEST(each_action) {
  fixture f;
  char ops[512];
  ASSERT_TRUE(fixture_open(&f, "one\ntwo\nthree\nfour\n"));

  snprintf(ops, sizeof ops, "{start: \"%s\", action: \"replace\", "
           "text: \"TWO\"}", anchor(2, "two"));
  ASSERT_EQ_INT(edit_lines(&f, ops, NULL, 0, NULL, 0), 1);
  expect_file(&f, "one\nTWO\nthree\nfour\n");

  /* a range shrinks or grows; a trailing newline in text is ignored */
  snprintf(ops, sizeof ops, "{start: \"%s\", end: \"%s\", action: \"replace\", "
           "text: \"a\\nb\\nc\\n\"}", anchor(2, "TWO"), anchor(3, "three"));
  ASSERT_EQ_INT(edit_lines(&f, ops, NULL, 0, NULL, 0), 1);
  expect_file(&f, "one\na\nb\nc\nfour\n");

  snprintf(ops, sizeof ops, "{start: \"%s\", action: \"insert_before\", "
           "text: \"zero\"}", anchor(1, "one"));
  ASSERT_EQ_INT(edit_lines(&f, ops, NULL, 0, NULL, 0), 1);
  expect_file(&f, "zero\none\na\nb\nc\nfour\n");

  /* empty text is one empty line */
  snprintf(ops, sizeof ops, "{start: \"%s\", action: \"insert_after\", "
           "text: \"\"}", anchor(6, "four"));
  ASSERT_EQ_INT(edit_lines(&f, ops, NULL, 0, NULL, 0), 1);
  expect_file(&f, "zero\none\na\nb\nc\nfour\n\n");

  snprintf(ops, sizeof ops, "{start: \"%s\", end: \"%s\", action: \"delete\"}",
           anchor(3, "a"), anchor(5, "c"));
  ASSERT_EQ_INT(edit_lines(&f, ops, NULL, 0, NULL, 0), 1);
  expect_file(&f, "zero\none\nfour\n\n");
  fixture_close(&f);
}

TEST(many_ops_use_the_original_line_numbers) {
  fixture f;
  char ops[1024], sha[80], args[1100];
  ASSERT_TRUE(fixture_open(&f, "l1\nl2\nl3\nl4\nl5\nl6\n"));
  /* listed out of order on purpose; every op names lines as first read */
  snprintf(ops, sizeof ops,
           "{start: \"%s\", action: \"insert_after\", text: \"end\"},"
           "{start: \"%s\", end: \"%s\", action: \"replace\", text: \"x\\ny\\nz\"},"
           "{start: \"%s\", action: \"delete\"},"
           "{start: \"%s\", action: \"insert_before\", text: \"top\"},"
           "{start: \"%s\", action: \"insert_after\", text: \"mid\"}",
           anchor(6, "l6"), anchor(2, "l2"), anchor(3, "l3"), anchor(5, "l5"),
           anchor(1, "l1"), anchor(3, "l3"));
  snprintf(args, sizeof args, "{path: \"f.txt\", ops: [%s]}", ops);
  ASSERT_EQ_INT(result_field(f.c, "edit", "lines", args, "sha256", sha,
                             sizeof sha), 1);
  ASSERT_EQ_INT((int)strlen(sha), 64);
  expect_file(&f, "top\nl1\nx\ny\nz\nmid\nl4\nl6\nend\n");
  fixture_close(&f);
}

TEST(stale_anchor_changes_nothing) {
  fixture f;
  char ops[512], code[64], msg[512], now[64];
  const char *orig = "one\ntwo\nthree\n";
  ASSERT_TRUE(fixture_open(&f, orig));

  /* the first op is fine, the second names a line that has changed */
  snprintf(ops, sizeof ops,
           "{start: \"%s\", action: \"delete\"},"
           "{start: \"%s\", action: \"replace\", text: \"x\"}",
           anchor(1, "one"), anchor(2, "old two"));
  ASSERT_EQ_INT(edit_lines(&f, ops, code, sizeof code, msg, sizeof msg), 0);
  ASSERT_EQ_STR(code, "edit/stale-anchor");
  ASSERT_TRUE(strstr(msg, anchor(2, "old two")) != NULL);
  snprintf(now, sizeof now, "%s|two", anchor(2, "two"));
  ASSERT_TRUE(strstr(msg, now) != NULL); /* shows the current line */
  expect_file(&f, orig);

  /* right content, wrong line number: also stale, with a hint */
  snprintf(ops, sizeof ops, "{start: \"%s\", action: \"delete\"}",
           anchor(1, "three"));
  ASSERT_EQ_INT(edit_lines(&f, ops, code, sizeof code, msg, sizeof msg), 0);
  ASSERT_EQ_STR(code, "edit/stale-anchor");
  ASSERT_TRUE(strstr(msg, "line 3") != NULL);
  expect_file(&f, orig);

  /* past the end of the file */
  snprintf(ops, sizeof ops, "{start: \"%s\", action: \"delete\"}",
           anchor(9, "three"));
  ASSERT_EQ_INT(edit_lines(&f, ops, code, sizeof code, msg, sizeof msg), 0);
  ASSERT_EQ_STR(code, "edit/stale-anchor");
  expect_file(&f, orig);

  /* a stale end anchor */
  snprintf(ops, sizeof ops, "{start: \"%s\", end: \"%s\", action: \"delete\"}",
           anchor(1, "one"), anchor(2, "nope"));
  ASSERT_EQ_INT(edit_lines(&f, ops, code, sizeof code, msg, sizeof msg), 0);
  ASSERT_EQ_STR(code, "edit/stale-anchor");
  expect_file(&f, orig);

  ASSERT_EQ_INT(edit_lines(&f, "{start: \"two\", action: \"delete\"}", code,
                           sizeof code, msg, sizeof msg), 0);
  ASSERT_EQ_STR(code, "edit/bad-anchor");
  snprintf(ops, sizeof ops, "{start: \"%s\", end: \"%s\", action: \"delete\"}",
           anchor(3, "three"), anchor(1, "one"));
  ASSERT_EQ_INT(edit_lines(&f, ops, code, sizeof code, msg, sizeof msg), 0);
  ASSERT_EQ_STR(code, "edit/bad-anchor");
  expect_file(&f, orig);
  fixture_close(&f);
}

TEST(overlap_is_rejected) {
  fixture f;
  char ops[512], code[64];
  const char *orig = "one\ntwo\nthree\nfour\n";
  ASSERT_TRUE(fixture_open(&f, orig));

  snprintf(ops, sizeof ops,
           "{start: \"%s\", end: \"%s\", action: \"replace\", text: \"x\"},"
           "{start: \"%s\", action: \"delete\"}",
           anchor(1, "one"), anchor(3, "three"), anchor(3, "three"));
  ASSERT_EQ_INT(edit_lines(&f, ops, code, sizeof code, NULL, 0), 0);
  ASSERT_EQ_STR(code, "edit/overlap");
  expect_file(&f, orig);

  /* an insert inside a replaced range */
  snprintf(ops, sizeof ops,
           "{start: \"%s\", end: \"%s\", action: \"delete\"},"
           "{start: \"%s\", action: \"insert_after\", text: \"x\"}",
           anchor(1, "one"), anchor(3, "three"), anchor(2, "two"));
  ASSERT_EQ_INT(edit_lines(&f, ops, code, sizeof code, NULL, 0), 0);
  ASSERT_EQ_STR(code, "edit/overlap");

  /* two inserts into the same gap */
  snprintf(ops, sizeof ops,
           "{start: \"%s\", action: \"insert_after\", text: \"x\"},"
           "{start: \"%s\", action: \"insert_before\", text: \"y\"}",
           anchor(2, "two"), anchor(3, "three"));
  ASSERT_EQ_INT(edit_lines(&f, ops, code, sizeof code, NULL, 0), 0);
  ASSERT_EQ_STR(code, "edit/overlap");
  expect_file(&f, orig);

  /* touching ranges and an insert at a range edge are fine */
  snprintf(ops, sizeof ops,
           "{start: \"%s\", action: \"replace\", text: \"ONE\"},"
           "{start: \"%s\", action: \"replace\", text: \"TWO\"},"
           "{start: \"%s\", action: \"insert_after\", text: \"2.5\"}",
           anchor(1, "one"), anchor(2, "two"), anchor(2, "two"));
  ASSERT_EQ_INT(edit_lines(&f, ops, code, sizeof code, NULL, 0), 1);
  expect_file(&f, "ONE\nTWO\n2.5\nthree\nfour\n");
  fixture_close(&f);
}

TEST(crlf_and_no_final_newline) {
  fixture f;
  char ops[512], got[256], want[256];

  /* CRLF: same tags as LF, endings kept, new text gets CRLF too */
  ASSERT_TRUE(fixture_open(&f, "one\r\ntwo\r\nthree\r\n"));
  ASSERT_EQ_INT(result_field(f.c, "fs", "read",
                             "{path: \"f.txt\", anchors: true}", "content",
                             got, sizeof got), 1);
  snprintf(want, sizeof want, "%s|one\r\n%s|two\r\n%s|three\r\n",
           anchor(1, "one"), anchor(2, "two"), anchor(3, "three"));
  ASSERT_EQ_STR(got, want);
  snprintf(ops, sizeof ops, "{start: \"%s\", action: \"replace\", "
           "text: \"2a\\n2b\"}", anchor(2, "two"));
  ASSERT_EQ_INT(edit_lines(&f, ops, NULL, 0, NULL, 0), 1);
  expect_file(&f, "one\r\n2a\r\n2b\r\nthree\r\n");
  fixture_close(&f);

  /* no newline at end of file: stays that way */
  ASSERT_TRUE(fixture_open(&f, "one\ntwo"));
  ASSERT_EQ_INT(result_field(f.c, "fs", "read",
                             "{path: \"f.txt\", anchors: true}", "content",
                             got, sizeof got), 1);
  snprintf(want, sizeof want, "%s|one\n%s|two", anchor(1, "one"),
           anchor(2, "two"));
  ASSERT_EQ_STR(got, want);
  snprintf(ops, sizeof ops, "{start: \"%s\", action: \"replace\", "
           "text: \"TWO\"}", anchor(2, "two"));
  ASSERT_EQ_INT(edit_lines(&f, ops, NULL, 0, NULL, 0), 1);
  expect_file(&f, "one\nTWO");
  snprintf(ops, sizeof ops, "{start: \"%s\", action: \"insert_after\", "
           "text: \"three\"}", anchor(2, "TWO"));
  ASSERT_EQ_INT(edit_lines(&f, ops, NULL, 0, NULL, 0), 1);
  expect_file(&f, "one\nTWO\nthree");
  snprintf(ops, sizeof ops, "{start: \"%s\", end: \"%s\", action: \"delete\"}",
           anchor(1, "one"), anchor(3, "three"));
  ASSERT_EQ_INT(edit_lines(&f, ops, NULL, 0, NULL, 0), 1);
  expect_file(&f, "");
  fixture_close(&f);
}

/* edit.lines is a write like the other edit commands: a read-only
 * workspace refuses it before the tool runs. */
TEST(read_only_workspace_denies_edit_lines) {
  char ws[256], file[512], config_path[512], config[2048], args[256];
  astools_open_params op;
  astools_ctx *c = NULL;
  astools_result r;
  char *got;
  size_t i;
  ASSERT_TRUE(test_tmpdir(ws));
  for (i = 0; ws[i]; i++)
    if (ws[i] == '\\') ws[i] = '/';
  snprintf(file, sizeof file, "%s/f.txt", ws);
  ASSERT_TRUE(write_bytes(file, "one\n"));
  snprintf(config_path, sizeof config_path, "%s/ro.xcdn", ws);
  snprintf(config, sizeof config,
           "#astools_config {"
           " registry:{paths:[{path:\"%s\",trust:\"standard\"}],"
           "watch:\"off\",pinning:\"off\"},"
           " workspace:{root:\"%s\"},"
           " grants:{workspace_access:\"read\"} }",
           ASTOOLS_STD_PACKAGES, ws);
  ASSERT_TRUE(write_bytes(config_path, config));
  memset(&op, 0, sizeof op);
  op.config_path = config_path;
  ASSERT_OK(astools_open(&op, &c));
  snprintf(args, sizeof args, "{path: \"f.txt\", ops: [{start: \"%s\", "
           "action: \"delete\"}]}", anchor(1, "one"));
  memset(&r, 0, sizeof r);
  ASSERT_ERR(astools_invoke(c, "edit", "lines", args, 0, &r),
             ASTOOLS_ERR_DENIED);
  astools_result_free(&r);
  memset(&r, 0, sizeof r);
  ASSERT_ERR(astools_invoke(c, "edit", "replace",
                            "{path: \"f.txt\", find: \"one\", "
                            "replace_with: \"x\"}", 0, &r),
             ASTOOLS_ERR_DENIED);
  astools_result_free(&r);
  got = test_read_file(file, NULL);
  ASSERT_TRUE(got != NULL);
  ASSERT_EQ_STR(got, "one\n");
  free(got);
  astools_close(c);
  test_rmtree(ws);
}

TEST_LIST = {
  TEST_ENTRY(tag_rule_is_stable),
  TEST_ENTRY(reads_carry_anchors),
  TEST_ENTRY(each_action),
  TEST_ENTRY(many_ops_use_the_original_line_numbers),
  TEST_ENTRY(stale_anchor_changes_nothing),
  TEST_ENTRY(overlap_is_rejected),
  TEST_ENTRY(crlf_and_no_final_newline),
  TEST_ENTRY(read_only_workspace_denies_edit_lines),
};

RUN_ALL_TESTS()
