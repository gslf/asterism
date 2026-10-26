/*
 * cli.c — "asterism tools": manifest schema + lint, catalog and grammar
 * rendering, lockfile approval and one-off invocation, for tool authors.
 *
 * Exit codes: check returns the number of lint errors (2 on read/parse
 * failure); the registry commands return 0/1; usage errors return 2.
 * Counts are clamped to 255 so they survive the exit-status truncation.
 */

#include "astools_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* ---- shared helpers ------------------------------------------------------ */

static void chk_usage(FILE *to) {
  fputs(
      "usage: asterism tools check <manifest-or-package-dir>\n"
      "       asterism tools catalog --root <dir> [--workspace <w>]\n"
      "                      [--config <file>]\n"
      "                      [--level index|summary|full] [--budget N]\n"
      "       asterism tools grammar --root <dir> [--workspace <w>]\n"
      "                      [--config <file>]\n"
      "       asterism tools approve <ref> --root <dir> [--config <file>]\n"
      "                      [--workspace <w>]\n"
      "       asterism tools invoke <tool>.<command> <args-xcdn> --root <dir>\n"
      "                      [--workspace <w>] [--config <file>]\n",
      to);
}

/* Clamp a finding count into the representable exit-status range. */
static int chk_exit_count(int n) {
  if (n < 0) return 0;
  return n > 255 ? 255 : n;
}

/* Print text ensuring a trailing newline. */
static void chk_puts(const char *text) {
  size_t n;
  if (!text) return;
  n = strlen(text);
  if (n == 0) return;
  fputs(text, stdout);
  if (text[n - 1] != '\n') fputc('\n', stdout);
}

static void chk_report(astools_ctx *c, astools_err e, const char *what) {
  const char *msg = c ? astools_last_error(c) : NULL;
  fprintf(stderr, "error: %s failed: %s\n", what,
          (msg && msg[0] != '\0') ? msg : astools_err_name(e));
}

typedef struct {
  const char *root;
  const char *workspace;
  const char *config;
  const char *level;
  const char *budget;
} chk_opts;

static int chk_parse_opts(int argc, char **argv, chk_opts *o, int allow_level,
                          int allow_config) {
  int i;
  memset(o, 0, sizeof *o);
  for (i = 0; i < argc; i++) {
    const char *a = argv[i];
    const char **slot = NULL;
    if (strcmp(a, "--root") == 0)
      slot = &o->root;
    else if (strcmp(a, "--workspace") == 0)
      slot = &o->workspace;
    else if (allow_config && strcmp(a, "--config") == 0)
      slot = &o->config;
    else if (allow_level && strcmp(a, "--level") == 0)
      slot = &o->level;
    else if (allow_level && strcmp(a, "--budget") == 0)
      slot = &o->budget;
    if (!slot) {
      fprintf(stderr, "error: unknown option '%s'\n", a);
      return -1;
    }
    if (i + 1 >= argc) {
      fprintf(stderr, "error: option '%s' needs a value\n", a);
      return -1;
    }
    *slot = argv[++i];
  }
  if (!o->root) {
    fprintf(stderr, "error: --root is required\n");
    return -1;
  }
  return 0;
}

static int chk_open(const chk_opts *o, astools_ctx **out) {
  astools_open_params p;
  const char *paths[2];
  astools_err e;
  memset(&p, 0, sizeof p);
  paths[0] = o->root;
  paths[1] = NULL;
  p.registry_paths = paths;
  p.config_path = o->config;
  p.workspace_root = o->workspace;
  *out = NULL;
  e = astools_open(&p, out);
  if (e != ASTOOLS_OK) {
    chk_report(*out, e, "open");
    if (*out) astools_close(*out);
    *out = NULL;
    return -1;
  }
  return 0;
}

/* ---- manifest check ------------------------------------------------------ */

static int cmd_manifest(const char *arg) {
  os_stat_info st;
  char *mpath;
  char *text = NULL;
  size_t len = 0;
  char *err_msg = NULL;
  astools_manifest *m;
  core_buf b;
  int errs;

  if (os_stat(arg, &st) == ASTOOLS_OK && st.type == OS_FT_DIR)
    mpath = os_path_join(arg, "manifest.xcdn");
  else
    mpath = core_strdup(arg);
  if (!mpath) {
    fprintf(stderr, "error: out of memory\n");
    return 2;
  }
  if (os_read_file(mpath, &text, &len) != ASTOOLS_OK) {
    printf("error: cannot read %s\n", mpath);
    free(mpath);
    return 2;
  }
  m = astools_manifest_parse(text, len, NULL, &err_msg);
  free(text);
  if (!m) {
    printf("error: %s\n", err_msg ? err_msg : "manifest rejected");
    free(err_msg);
    free(mpath);
    return 2;
  }
  free(mpath);

  core_buf_init(&b);
  errs = astools_manifest_lint(m, &b);
  if (b.data && b.len > 0) {
    fputs(b.data, stdout);
    if (b.data[b.len - 1] != '\n') fputc('\n', stdout);
  }
  if (errs == 0)
    printf("ok: %s %s, %zu commands\n", m->id, m->version, m->commands_len);
  core_buf_free(&b);
  astools_manifest_free(m);
  return chk_exit_count(errs);
}

/* ---- catalog / grammar / approve ---------------------------------------- */

static int cmd_catalog(int argc, char **argv) {
  chk_opts o;
  astools_ctx *c;
  astools_catalog_level lvl = ASTOOLS_CATALOG_SUMMARY;
  size_t budget = 0;
  char *text = NULL;
  astools_err e;

  if (chk_parse_opts(argc, argv, &o, 1, 1) != 0) return 2;
  if (o.level) {
    if (strcmp(o.level, "index") == 0)
      lvl = ASTOOLS_CATALOG_INDEX;
    else if (strcmp(o.level, "summary") == 0)
      lvl = ASTOOLS_CATALOG_SUMMARY;
    else if (strcmp(o.level, "full") == 0)
      lvl = ASTOOLS_CATALOG_FULL;
    else {
      fprintf(stderr, "error: --level must be index, summary or full\n");
      return 2;
    }
  }
  if (o.budget) {
    char *end = NULL;
    long v = strtol(o.budget, &end, 10);
    if (end == o.budget || *end != '\0' || v < 0) {
      fprintf(stderr, "error: --budget must be a non-negative integer\n");
      return 2;
    }
    budget = (size_t)v;
  }
  if (chk_open(&o, &c) != 0) return 1;
  e = astools_catalog(c, lvl, budget, &text);
  if (e != ASTOOLS_OK) {
    chk_report(c, e, "catalog");
    astools_close(c);
    return 1;
  }
  chk_puts(text);
  astools_free(text);
  astools_close(c);
  return 0;
}

static int cmd_grammar(int argc, char **argv) {
  chk_opts o;
  astools_ctx *c;
  char *text = NULL;
  astools_err e;

  if (chk_parse_opts(argc, argv, &o, 0, 1) != 0) return 2;
  if (chk_open(&o, &c) != 0) return 1;
  e = astools_grammar_export(c, &text);
  if (e != ASTOOLS_OK) {
    chk_report(c, e, "grammar");
    astools_close(c);
    return 1;
  }
  chk_puts(text);
  astools_free(text);
  astools_close(c);
  return 0;
}

static int cmd_approve(int argc, char **argv) {
  chk_opts o;
  astools_ctx *c;
  const char *ref;
  astools_err e;

  if (argc < 1 || argv[0][0] == '-') {
    fprintf(stderr, "error: approve needs a tool ref\n");
    return 2;
  }
  ref = argv[0];
  if (chk_parse_opts(argc - 1, argv + 1, &o, 0, 1) != 0) return 2;
  if (chk_open(&o, &c) != 0) return 1;
  e = astools_tool_approve(c, ref);
  if (e != ASTOOLS_OK) {
    chk_report(c, e, "approve");
    astools_close(c);
    return 1;
  }
  printf("approved: %s\n", ref);
  astools_close(c);
  return 0;
}

/* ---- invoke ------------------------------------------------------------- */

/* Run one command of an installed package under the configured policy.
 * stdout: the result value; stderr: the tool error. Exit 0 ok, 1 tool
 * failure, 2 usage or engine failure. */
static int cmd_invoke(int argc, char **argv) {
  chk_opts o;
  astools_ctx *c;
  astools_result r;
  const char *dot;
  char *tool;
  astools_err e;
  int rc;

  if (argc < 2 || argv[0][0] == '-' || argv[1][0] == '-') {
    fprintf(stderr, "error: invoke needs <tool>.<command> and <args-xcdn>\n");
    return 2;
  }
  dot = strchr(argv[0], '.');
  if (!dot || dot == argv[0] || dot[1] == '\0') {
    fprintf(stderr, "error: invoke target must be <tool>.<command>\n");
    return 2;
  }
  if (chk_parse_opts(argc - 2, argv + 2, &o, 0, 1) != 0) return 2;
  tool = core_strndup(argv[0], (size_t)(dot - argv[0]));
  if (!tool) {
    fprintf(stderr, "error: out of memory\n");
    return 2;
  }
  if (chk_open(&o, &c) != 0) {
    free(tool);
    return 2;
  }
  memset(&r, 0, sizeof r);
  e = astools_invoke(c, tool, dot + 1, argv[1], 0, &r);
  free(tool);
  if (e != ASTOOLS_OK) {
    fprintf(stderr, "error: %s\n", r.error_message ? r.error_message : astools_err_name(e));
    rc = 2;
  } else {
    chk_puts(r.result_xcdn);
    if (!r.ok)
      fprintf(stderr, "%s: %s\n", r.error_code ? r.error_code : "error",
              r.error_message ? r.error_message : "");
    rc = r.ok ? 0 : 1;
  }
  astools_result_free(&r);
  astools_close(c);
  return rc;
}

/* ---- entry --------------------------------------------------------------- */

/* argv[0] is the mode name ("tools"); argv[1] the command. */
int astools_cli_main(int argc, char **argv) {
  const char *cmd;
  if (argc < 2) {
    chk_usage(stderr);
    return 2;
  }
  cmd = argv[1];
  if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
    chk_usage(stdout);
    return 0;
  }
  if (strcmp(cmd, "check") == 0) {
    if (argc != 3 || argv[2][0] == '-') {
      chk_usage(stderr);
      return 2;
    }
    return cmd_manifest(argv[2]);
  }
  if (strcmp(cmd, "catalog") == 0) return cmd_catalog(argc - 2, argv + 2);
  if (strcmp(cmd, "grammar") == 0) return cmd_grammar(argc - 2, argv + 2);
  if (strcmp(cmd, "approve") == 0) return cmd_approve(argc - 2, argv + 2);
  if (strcmp(cmd, "invoke") == 0) return cmd_invoke(argc - 2, argv + 2);
  fprintf(stderr, "error: unknown command '%s'\n", cmd);
  chk_usage(stderr);
  return 2;
}
