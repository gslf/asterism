/*
 * tool_delegate.c — delegate.claude / claude-edit / codex / codex-edit:
 * run the user's own, unmodified agent CLI headless in the workspace.
 *
 * Hard rules, enforced here and not only documented:
 *   - the CLI authenticates itself with the login the user made; this tool
 *     never opens, reads or copies ~/.claude/.credentials.json,
 *     ~/.claude.json or ~/.codex/auth.json, and refuses to start a child
 *     whose argv (prompt aside) or environment names one of them;
 *   - never `claude --bare` (it ignores the subscription login), never a
 *     permission-bypass flag, never an API-key or user-agent variable: the
 *     child environment is built from an allowlist;
 *   - a usage or rate limit is reported (result.limit), never retried.
 *
 * The model chooses only the command, the task text and two bounded knobs
 * (model name, max turns). Executable, flags, permission mode and tool set
 * are source constants, like the project tool's closed argv.
 *
 * Flags (official docs, Oct 2026):
 *   claude -p <task> --output-format stream-json --verbose
 *          --permission-mode plan|acceptEdits --tools <closed list>
 *          --disallowedTools mcp__* --strict-mcp-config
 *          --setting-sources user --append-system-prompt <brief>
 *          [--max-turns N] [--model M]
 *   codex exec --json --color never --sandbox read-only|workspace-write
 *          --skip-git-repo-check --cd <workspace>
 *          [-c sandbox_workspace_write.network_access=false]  (edit only)
 *          [--model M] -                                      (task on stdin)
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#endif

#include "delegate.h"
#include "sdk.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
extern char **environ;
#endif

#ifdef _WIN32

int astd_tool_delegate(astd_req *r) {
  astd_fail(r, "delegate/unsupported",
            "delegates run on Linux and macOS only");
  return 0;
}

#else /* POSIX */

#define DLG_DEFAULT_BUDGET_MS (30LL * 60 * 1000) /* no request deadline   */
#define DLG_MIN_RUN_MS 5000
#define DLG_GRACE_MS 5000
#define DLG_GIT_TIMEOUT_MS 15000
#define DLG_GIT_OUT_CAP (1u << 20)
#define DLG_MAX_STATUS 2000      /* git status entries per snapshot      */
#define DLG_MAX_HASHED 500       /* files hashed per snapshot            */
#define DLG_HASH_FILE_CAP (16 << 20)
#define DLG_MAX_CHANGED 500      /* changed_files entries reported       */
#define DLG_CHANGED_BYTES 131072 /* path bytes reported                   */
#define DLG_TASK_MAX 32768

typedef struct {
  const char *command;
  dlg_agent agent;
  const char *program; /* executable name on the tool PATH */
  const char *label;   /* human name in messages            */
  const char *mode;    /* reported mode                      */
  int edit;
} dlg_spec;

static const dlg_spec k_specs[] = {
    {"claude", DLG_CLAUDE, "claude", "Claude Code", "plan", 0},
    {"claude-edit", DLG_CLAUDE, "claude", "Claude Code", "edit", 1},
    {"codex", DLG_CODEX, "codex", "Codex", "read-only", 0},
    {"codex-edit", DLG_CODEX, "codex", "Codex", "edit", 1},
};

/* Optional variables the operator may grant (manifest permissions.env);
 * copied only when present in this process, i.e. granted. */
static const char *const k_passthrough[] = {
    "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME",
    "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS", "CLAUDE_CONFIG_DIR",
    "CODEX_HOME", "HTTPS_PROXY", "HTTP_PROXY", "NO_PROXY", "ALL_PROXY",
    "https_proxy", "http_proxy", "no_proxy", "all_proxy", "SSL_CERT_FILE",
    "SSL_CERT_DIR", "NODE_EXTRA_CA_CERTS", "LC_ALL", "LC_CTYPE",
    "LC_MESSAGES", "TZ"};

static const char k_brief[] =
    "You are running non-interactively as a delegate of the asterism agent. "
    "Nobody can answer questions or approve permission requests during this "
    "run: do not ask, decide, and say in your final message what you would "
    "need. Work only inside the current working directory. End with a "
    "concise report: what you found or did, the files you changed, and "
    "anything left undone.";
static const char k_brief_plan[] =
    " This run is read-only: do not try to modify files; describe the "
    "changes instead.";

/* ---- string vector ----------------------------------------------------- */

typedef struct {
  char **v;
  size_t n, cap;
} strv;

static int strv_push_owned(strv *s, char *item) {
  if (!item) return -1;
  if (s->n + 2 > s->cap) {
    size_t nc = s->cap ? s->cap * 2 : 32;
    char **p = realloc(s->v, nc * sizeof *p);
    if (!p) {
      free(item);
      return -1;
    }
    s->v = p;
    s->cap = nc;
  }
  s->v[s->n++] = item;
  s->v[s->n] = NULL;
  return 0;
}

static int strv_push(strv *s, const char *item) {
  return strv_push_owned(s, core_strdup(item));
}

static void strv_free(strv *s) {
  size_t i;
  for (i = 0; i < s->n; i++) free(s->v[i]);
  free(s->v);
  memset(s, 0, sizeof *s);
}

static char *kv(const char *name, const char *value) {
  size_t a = strlen(name), b = strlen(value);
  char *s = malloc(a + b + 2);
  if (!s) return NULL;
  memcpy(s, name, a);
  s[a] = '=';
  memcpy(s + a + 1, value, b + 1);
  return s;
}

static int env_has(const strv *env, const char *name) {
  size_t i, n = strlen(name);
  for (i = 0; i < env->n; i++)
    if (strncmp(env->v[i], name, n) == 0 && env->v[i][n] == '=') return 1;
  return 0;
}

static int env_set(strv *env, const char *name, const char *value) {
  if (!value || env_has(env, name)) return 0;
  return strv_push_owned(env, kv(name, value));
}

/* ---- hard rules ---------------------------------------------------------- */

/* Credential files of the two CLIs: never named to a child we start. */
static int names_credentials(const char *s) {
  return s && (strstr(s, ".credentials.json") != NULL ||
               strstr(s, ".claude.json") != NULL ||
               strstr(s, "auth.json") != NULL);
}

/* Flags that would bypass the subscription login or the permission model. */
static int forbidden_flag(const char *a) {
  static const char *const bad[] = {
      "--bare", "--dangerously-skip-permissions",
      "--allow-dangerously-skip-permissions", "bypassPermissions",
      "--dangerously-bypass-approvals-and-sandbox", "danger-full-access",
      "--yolo", "--full-auto"};
  size_t i;
  for (i = 0; i < sizeof bad / sizeof bad[0]; i++)
    if (strcmp(a, bad[i]) == 0) return 1;
  return 0;
}

/* Keys and tokens: a delegate runs on the user's plan, never on a key. */
static int forbidden_env(const char *entry) {
  static const char *const prefixes[] = {
      "ANTHROPIC_", "OPENAI_", "CLAUDE_CODE_", "CODEX_API_KEY", "AZURE_"};
  size_t i;
  for (i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++)
    if (strncmp(entry, prefixes[i], strlen(prefixes[i])) == 0) return 1;
  return 0;
}

/* prompt_index: argv slot holding the model's task (exempt from the
 * credential-name check: the task is text for the CLI, not a path we
 * open), or -1. */
static const char *rules_violation(const strv *argv, long prompt_index,
                                   const strv *env) {
  size_t i;
  for (i = 1; i < argv->n; i++) {
    if ((long)i == prompt_index) continue;
    if (forbidden_flag(argv->v[i])) return "a permission-bypass or --bare flag";
    if (names_credentials(argv->v[i])) return "a CLI credential file path";
  }
  for (i = 0; i < env->n; i++) {
    if (forbidden_env(env->v[i])) return "an API key or CLI auth variable";
    if (names_credentials(env->v[i])) return "a CLI credential file path";
  }
  return NULL;
}

/* ---- inputs ---------------------------------------------------------------- */

/* Model names: aliases and full ids of both vendors ("sonnet",
 * "claude-sonnet-5", "opus[1m]", "gpt-5.1-codex"). Never a flag. */
static int model_valid(const char *m) {
  size_t i;
  if (!m || m[0] == '\0' || m[0] == '-' || strlen(m) > 128) return 0;
  for (i = 0; m[i] != '\0'; i++) {
    char c = m[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' ||
          c == ':' || c == '/' || c == '@' || c == '[' || c == ']'))
      return 0;
  }
  return 1;
}

/* The account's home directory from the password database. The runtime
 * sets HOME to the private scratch directory; the CLIs keep their own
 * login and settings under the real one. */
static int account_home(char **home, char **user, char **shell) {
  struct passwd pw, *res = NULL;
  long hint = sysconf(_SC_GETPW_R_SIZE_MAX);
  size_t sz = hint > 0 ? (size_t)hint : 16384;
  char *buf = NULL;
  int rc;
  *home = *user = *shell = NULL;
  for (;;) {
    char *nb = realloc(buf, sz);
    if (!nb) {
      free(buf);
      return -1;
    }
    buf = nb;
    rc = getpwuid_r(getuid(), &pw, buf, sz, &res);
    if (rc == ERANGE && sz < (1u << 20)) {
      sz *= 2;
      continue;
    }
    break;
  }
  if (rc != 0 || !res || !pw.pw_dir || pw.pw_dir[0] != '/') {
    free(buf);
    return -1;
  }
  *home = core_strdup(pw.pw_dir);
  *user = core_strdup(pw.pw_name ? pw.pw_name : "");
  *shell = core_strdup(pw.pw_shell && pw.pw_shell[0] ? pw.pw_shell : "/bin/sh");
  free(buf);
  if (!*home || !*user || !*shell) {
    free(*home);
    free(*user);
    free(*shell);
    *home = *user = *shell = NULL;
    return -1;
  }
  return 0;
}

/* First executable regular file named prog on the tool's PATH (the
 * runtime's fixed roots plus sandbox.executable_paths). Relative PATH
 * entries are skipped: they would resolve against the workspace. */
static char *find_program(const char *prog) {
  const char *path = getenv("PATH");
  const char *p = path;
  if (!path) return NULL;
  for (;;) {
    const char *q = strchr(p, ':');
    size_t dl = q ? (size_t)(q - p) : strlen(p);
    if (dl > 0 && p[0] == '/') {
      size_t pl = strlen(prog);
      char *cand = malloc(dl + pl + 2);
      struct stat st;
      if (!cand) return NULL;
      memcpy(cand, p, dl);
      cand[dl] = '/';
      memcpy(cand + dl + 1, prog, pl + 1);
      if (stat(cand, &st) == 0 && S_ISREG(st.st_mode) &&
          access(cand, X_OK) == 0)
        return cand;
      free(cand);
    }
    if (!q) break;
    p = q + 1;
  }
  return NULL;
}

/* ---- git evidence ------------------------------------------------------ */

typedef struct {
  char *path; /* workspace-relative */
  char xy[3];
  int hashed;
  uint8_t sha[32];
} git_entry;

typedef struct {
  git_entry *v;
  size_t n, cap;
  int truncated;
} git_snap;

static void snap_free(git_snap *s) {
  size_t i;
  for (i = 0; i < s->n; i++) free(s->v[i].path);
  free(s->v);
  memset(s, 0, sizeof *s);
}

static int entry_cmp(const void *a, const void *b) {
  return strcmp(((const git_entry *)a)->path, ((const git_entry *)b)->path);
}

/* Run git in the workspace with this tool's own scrubbed environment.
 * 0 only when git ran and exited 0. */
static int git_run(const char *ws, const char *const *args, astd_run_res *rr) {
  char *argv[12];
  char emsg[256];
  size_t i;
  argv[0] = "git";
  argv[1] = "--no-optional-locks";
  for (i = 0; args[i] && i < 9; i++) argv[i + 2] = (char *)args[i];
  argv[i + 2] = NULL;
  memset(rr, 0, sizeof *rr);
  if (astd_run_capture(argv, environ, ws, NULL, 0, DLG_GIT_OUT_CAP, 4096,
                       DLG_GIT_TIMEOUT_MS, rr, emsg, sizeof emsg) != 0)
    return -1;
  if (rr->exit_code != 0 || rr->timed_out) {
    free(rr->out);
    free(rr->err);
    memset(rr, 0, sizeof *rr);
    return -1;
  }
  return 0;
}

static void hash_entry(const char *ws, git_entry *e, size_t *hashed) {
  char *full;
  size_t a, b;
  struct stat st;
  if (*hashed >= DLG_MAX_HASHED) return;
  a = strlen(ws);
  b = strlen(e->path);
  full = malloc(a + b + 2);
  if (!full) return;
  memcpy(full, ws, a);
  full[a] = '/';
  memcpy(full + a + 1, e->path, b + 1);
  if (lstat(full, &st) == 0 && S_ISREG(st.st_mode) &&
      st.st_size <= DLG_HASH_FILE_CAP &&
      core_sha256_file(full, e->sha) == CORE_OK) {
    e->hashed = 1;
    (*hashed)++;
  }
  free(full);
}


/* Snapshot of `git status` for the workspace subtree, with content hashes
 * of dirty files so a second edit of an already-modified file still
 * shows. -1 when git is missing or the workspace is not in a repository. */
static int git_snapshot(const char *ws, const char *skip, git_snap *out) {
  static const char *const prefix_args[] = {"rev-parse", "--show-prefix",
                                            NULL};
  const char *status_args[9];
  char *exclude = NULL;
  astd_run_res rr;
  char *prefix;
  size_t plen, pos, i, hashed = 0, skip_len = skip ? strlen(skip) : 0;
  memset(out, 0, sizeof *out);
  status_args[0] = "status";
  status_args[1] = "--porcelain=v1";
  status_args[2] = "-z";
  status_args[3] = "--untracked-files=all";
  status_args[4] = "--";
  status_args[5] = ".";
  status_args[6] = NULL;
  if (skip_len > 0) {
    /* The runtime's scratch tree (the CLI's TMPDIR) is not evidence. */
    static const char magic[] = ":(exclude,literal)";
    exclude = malloc(sizeof magic + skip_len);
    if (!exclude) return -1;
    memcpy(exclude, magic, sizeof magic - 1);
    memcpy(exclude + sizeof magic - 1, skip, skip_len + 1);
    status_args[6] = exclude;
    status_args[7] = NULL;
  }
  if (git_run(ws, prefix_args, &rr) != 0) {
    free(exclude);
    return -1;
  }
  prefix = rr.out;
  free(rr.err);
  plen = strlen(prefix);
  while (plen > 0 && (prefix[plen - 1] == '\n' || prefix[plen - 1] == '\r'))
    prefix[--plen] = '\0';
  if (git_run(ws, status_args, &rr) != 0) {
    free(exclude);
    free(prefix);
    return -1;
  }
  free(exclude);
  out->truncated = rr.out_trunc;
  pos = 0;
  while (pos < rr.out_n) {
    const char *ent = rr.out + pos;
    size_t len = 0, pl;
    const char *path;
    git_entry *e;
    while (pos + len < rr.out_n && ent[len] != '\0') len++;
    if (pos + len >= rr.out_n) break; /* entry cut by the capture cap */
    pos += len + 1;
    if (len < 4 || ent[2] != ' ') continue;
    if (ent[0] == 'R' || ent[0] == 'C' || ent[1] == 'R' || ent[1] == 'C') {
      /* "XY to\0from\0": skip the source path. */
      while (pos < rr.out_n && rr.out[pos] != '\0') pos++;
      pos++;
    }
    path = ent + 3;
    pl = len - 3;
    if (plen > 0 && pl > plen && memcmp(path, prefix, plen) == 0) {
      path += plen;
      pl -= plen;
    }
    if (skip_len > 0 && pl >= skip_len && memcmp(path, skip, skip_len) == 0 &&
        (pl == skip_len || path[skip_len] == '/'))
      continue;
    if (out->n >= DLG_MAX_STATUS) {
      out->truncated = 1;
      break;
    }
    if (out->n == out->cap) {
      size_t nc = out->cap ? out->cap * 2 : 64;
      git_entry *nv = realloc(out->v, nc * sizeof *nv);
      if (!nv) {
        out->truncated = 1;
        break;
      }
      out->v = nv;
      out->cap = nc;
    }
    e = &out->v[out->n];
    memset(e, 0, sizeof *e);
    e->path = core_strndup(path, pl);
    if (!e->path) {
      out->truncated = 1;
      break;
    }
    e->xy[0] = ent[0];
    e->xy[1] = ent[1];
    out->n++;
  }
  free(prefix);
  free(rr.out);
  free(rr.err);
  for (i = 0; i < out->n; i++) hash_entry(ws, &out->v[i], &hashed);
  if (out->n > 1) qsort(out->v, out->n, sizeof *out->v, entry_cmp);
  return 0;
}

static int push_change(xcdn_value_t *arr, const char *path, const char *xy,
                       size_t *bytes, int *truncated) {
  xcdn_value_t *o;
  char status[3];
  size_t k = 0, i;
  if (xcdn_array_len(arr) >= DLG_MAX_CHANGED ||
      *bytes + strlen(path) > DLG_CHANGED_BYTES) {
    *truncated = 1;
    return 0;
  }
  for (i = 0; xy[i] != '\0' && i < 2; i++)
    if (xy[i] != ' ') status[k++] = xy[i];
  status[k] = '\0';
  o = xcdn_value_object();
  if (!o || astd_set_str(o, "path", path) != 0 ||
      astd_set_str(o, "status", k ? status : "clean") != 0) {
    xcdn_value_free(o);
    return -1;
  }
  *bytes += strlen(path);
  return astd_arr_push_val(arr, o);
}

/* Entries whose status or content differ between the two snapshots;
 * entries that were dirty before and clean after report "clean". */
static xcdn_value_t *changed_files(const git_snap *b, const git_snap *a,
                                   int *truncated) {
  xcdn_value_t *arr = xcdn_value_array();
  size_t i = 0, j = 0, bytes = 0;
  *truncated = b->truncated || a->truncated;
  if (!arr) return NULL;
  while (i < b->n || j < a->n) {
    int cmp, rc = 0;
    if (i < b->n && j < a->n)
      cmp = strcmp(b->v[i].path, a->v[j].path);
    else
      cmp = i < b->n ? -1 : 1;
    if (cmp < 0) {
      rc = push_change(arr, b->v[i].path, "  ", &bytes, truncated);
      i++;
    } else if (cmp > 0) {
      rc = push_change(arr, a->v[j].path, a->v[j].xy, &bytes, truncated);
      j++;
    } else {
      const git_entry *x = &b->v[i], *y = &a->v[j];
      if (memcmp(x->xy, y->xy, 2) != 0 ||
          (x->hashed && y->hashed && memcmp(x->sha, y->sha, 32) != 0))
        rc = push_change(arr, y->path, y->xy, &bytes, truncated);
      i++;
      j++;
    }
    if (rc != 0) {
      xcdn_value_free(arr);
      return NULL;
    }
  }
  return arr;
}

/* ---- result ---------------------------------------------------------------- */

/* Valid UTF-8 copy of raw bytes (stderr): invalid bytes become '?', other
 * control characters than newline and tab are dropped. */
static char *utf8_clean(const char *s, size_t n) {
  char *out = malloc(n + 1);
  size_t i = 0, k = 0;
  if (!out) return NULL;
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
                 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (len == 1) {
      if (c >= 0x20 || c == '\n' || c == '\t') out[k++] = (char)c;
      i++;
    } else if (len > 1 && i + len <= n && core_utf8_valid(s + i, len)) {
      memcpy(out + k, s + i, len);
      k += len;
      i += len;
    } else {
      out[k++] = '?';
      i++;
    }
  }
  out[k] = '\0';
  return out;
}

static int stderr_mentions_flags(const char *tail) {
  return tail && (strstr(tail, "unknown option") != NULL ||
                  strstr(tail, "unexpected argument") != NULL ||
                  strstr(tail, "unrecognized") != NULL);
}

static void on_line(void *ud, const char *line, size_t len) {
  dlg_feed_line((dlg_state *)ud, line, len);
}

#define DLG_FAIL(code, ...)                       \
  do {                                            \
    snprintf(fmsg, sizeof fmsg, __VA_ARGS__);     \
    fcode = (code);                               \
    goto out;                                     \
  } while (0)

static void run_delegate(astd_req *r, const dlg_spec *sp) {
  const char *task = astd_arg_str(r, "task", NULL);
  const char *model = astd_arg_str(r, "model", NULL);
  const xcdn_value_t *turns_arg = astd_arg(r, "max_turns");
  int64_t max_turns = astd_arg_int(r, "max_turns", 0);
  const char *ws = r->workspace;
  const char *fcode = NULL, *why, *fin;
  char fmsg[1024], emsg[512], num[32];
  char *home = NULL, *user = NULL, *shell = NULL, *cli = NULL;
  char *prompt = NULL, *tail = NULL, *errs = NULL, *skip = NULL;
  strv argv, env;
  git_snap before, after;
  int have_git = 0, ran = 0, ok, fd, final_cut = 0;
  long prompt_index = -1;
  int64_t budget = DLG_DEFAULT_BUDGET_MS;
  size_t fin_len, final_cap = DLG_FINAL_CAP;
  dlg_state st;
  dlg_run_opts ro;
  dlg_run_res rr;
  xcdn_value_t *res = NULL;
  core_buf eb;

  memset(&argv, 0, sizeof argv);
  memset(&env, 0, sizeof env);
  memset(&before, 0, sizeof before);
  memset(&after, 0, sizeof after);
  memset(&ro, 0, sizeof ro);
  memset(&rr, 0, sizeof rr);
  core_buf_init(&eb);
  dlg_state_init(&st, sp->agent);

  /* 1. Inputs: the runtime validated types and bounds; recheck what the
   * argv depends on. */
  if (!task || task[0] == '\0' || strlen(task) > DLG_TASK_MAX)
    DLG_FAIL("delegate/bad-input", "task must be 1 to %d bytes of text",
             DLG_TASK_MAX);
  if (model && !model_valid(model))
    DLG_FAIL("delegate/bad-input",
             "model must be a model name or alias (letters, digits and . _ "
             "- : / @ [ ], not starting with '-')");
  if (turns_arg && (sp->agent != DLG_CLAUDE || max_turns < 1 ||
                    max_turns > 200))
    DLG_FAIL("delegate/bad-input", "max_turns must be 1 to 200");
  if (!ws || ws[0] != '/')
    DLG_FAIL("delegate/failed", "the request carries no absolute workspace");

  /* 2. The account's home, where the CLI keeps its own login. Under the
   * strict jail it is outside the tool's filesystem grants. */
  if (account_home(&home, &user, &shell) != 0)
    DLG_FAIL("delegate/failed",
             "cannot determine the account's home directory from the "
             "password database");
  fd = open(home, O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    int e = errno;
    if (e == EACCES || e == EPERM)
      DLG_FAIL("delegate/sandbox",
               "cannot open the account's home directory %.256s (%s): the "
               "strict sandbox confines this tool to its filesystem grants, "
               "and %s keeps its login there. Run delegates at sandbox "
               "level \"basic\".",
               home, strerror(e), sp->label);
    DLG_FAIL("delegate/failed", "home directory %.256s is not usable: %s",
             home, strerror(e));
  }
  close(fd);

  /* 3. The CLI, only from the operator-approved PATH. */
  cli = find_program(sp->program);
  if (!cli) {
    const char *path = getenv("PATH");
    if (sp->agent == DLG_CLAUDE)
      DLG_FAIL("delegate/not-installed",
               "Claude Code (`claude`) is not on the tool PATH (%.300s). "
               "Install Claude Code, run `claude` once in a terminal to log "
               "in with your Claude subscription, then add the directory "
               "that contains `claude` to sandbox.executable_paths. asterism "
               "never performs the login or touches its credentials.",
               path ? path : "");
    DLG_FAIL("delegate/not-installed",
             "OpenAI Codex (`codex`) is not on the tool PATH (%.300s). "
             "Install the Codex CLI, run `codex login` once in a terminal "
             "with your ChatGPT account, then add the directory that "
             "contains `codex` to sandbox.executable_paths. asterism never "
             "performs the login or touches its credentials.",
             path ? path : "");
  }

  /* 4. Closed argv. */
  if (strv_push(&argv, cli) != 0) goto oom;
  if (sp->agent == DLG_CLAUDE) {
    size_t tl = strlen(task);
    /* A leading '-' would parse as an option; a space keeps it text. */
    prompt = malloc(tl + 2);
    if (!prompt) goto oom;
    if (task[0] == '-') {
      prompt[0] = ' ';
      memcpy(prompt + 1, task, tl + 1);
    } else {
      memcpy(prompt, task, tl + 1);
    }
    if (strv_push(&argv, "-p") != 0) goto oom;
    prompt_index = (long)argv.n;
    if (strv_push(&argv, prompt) != 0 ||
        strv_push(&argv, "--output-format") != 0 ||
        strv_push(&argv, "stream-json") != 0 ||
        strv_push(&argv, "--verbose") != 0 ||
        strv_push(&argv, "--permission-mode") != 0 ||
        strv_push(&argv, sp->edit ? "acceptEdits" : "plan") != 0 ||
        strv_push(&argv, "--tools") != 0 ||
        strv_push(&argv, sp->edit ? "Read,Edit,Write,Glob,Grep"
                                  : "Read,Glob,Grep") != 0 ||
        strv_push(&argv, "--disallowedTools") != 0 ||
        strv_push(&argv, "mcp__*") != 0 ||
        strv_push(&argv, "--strict-mcp-config") != 0 ||
        strv_push(&argv, "--setting-sources") != 0 ||
        strv_push(&argv, "user") != 0 ||
        strv_push(&argv, "--append-system-prompt") != 0)
      goto oom;
    core_buf_init(&eb);
    if (core_buf_appends(&eb, k_brief) != CORE_OK ||
        (!sp->edit && core_buf_appends(&eb, k_brief_plan) != CORE_OK) ||
        strv_push(&argv, eb.data) != 0)
      goto oom;
    core_buf_free(&eb);
    if (turns_arg) {
      snprintf(num, sizeof num, "%lld", (long long)max_turns);
      if (strv_push(&argv, "--max-turns") != 0 || strv_push(&argv, num) != 0)
        goto oom;
    }
  } else {
    if (strv_push(&argv, "exec") != 0 || strv_push(&argv, "--json") != 0 ||
        strv_push(&argv, "--color") != 0 || strv_push(&argv, "never") != 0 ||
        strv_push(&argv, "--sandbox") != 0 ||
        strv_push(&argv, sp->edit ? "workspace-write" : "read-only") != 0 ||
        strv_push(&argv, "--skip-git-repo-check") != 0 ||
        strv_push(&argv, "--cd") != 0 || strv_push(&argv, ws) != 0)
      goto oom;
    if (sp->edit && (strv_push(&argv, "-c") != 0 ||
                     strv_push(&argv,
                               "sandbox_workspace_write.network_access=false") !=
                         0))
      goto oom;
  }
  if (model && (strv_push(&argv, "--model") != 0 ||
                strv_push(&argv, model) != 0))
    goto oom;
  if (sp->agent == DLG_CODEX && strv_push(&argv, "-") != 0) goto oom;

  /* 5. Environment from an allowlist: nothing else of this process leaks. */
  {
    const char *path = getenv("PATH"), *tmp = getenv("TMPDIR");
    size_t i;
    if (env_set(&env, "PATH", path ? path : "/usr/bin:/bin") != 0 ||
        env_set(&env, "HOME", home) != 0 ||
        env_set(&env, "TMPDIR", tmp ? tmp : (r->scratch ? r->scratch : "/tmp")) != 0 ||
        env_set(&env, "USER", getenv("USER") ? getenv("USER") : user) != 0 ||
        env_set(&env, "LOGNAME", getenv("LOGNAME") ? getenv("LOGNAME") : user) != 0 ||
        env_set(&env, "SHELL", shell) != 0 ||
        env_set(&env, "LANG", getenv("LANG") ? getenv("LANG") : "C.UTF-8") != 0 ||
        env_set(&env, "TERM", "dumb") != 0 ||
        env_set(&env, "NO_COLOR", "1") != 0)
      goto oom;
    for (i = 0; i < sizeof k_passthrough / sizeof k_passthrough[0]; i++)
      if (env_set(&env, k_passthrough[i], getenv(k_passthrough[i])) != 0)
        goto oom;
  }
  why = rules_violation(&argv, prompt_index, &env);
  if (why)
    DLG_FAIL("delegate/failed", "refusing to start %s with %s", sp->label,
             why);

  /* 6. Evidence before, run, evidence after. Scratch directories of this
   * and concurrent invocations live under one base, often inside the
   * workspace; that base is excluded from the evidence. */
  {
    size_t wl = strlen(ws);
    if (r->scratch && strncmp(r->scratch, ws, wl) == 0 &&
        r->scratch[wl] == '/' && r->scratch[wl + 1] != '\0') {
      const char *rel = r->scratch + wl + 1;
      const char *slash = strrchr(rel, '/');
      skip = core_strndup(rel, slash ? (size_t)(slash - rel) : strlen(rel));
      if (!skip) goto oom;
    }
  }
  have_git = git_snapshot(ws, skip, &before) == 0;
  if (r->deadline_unix > 0) {
    int64_t left = ((int64_t)r->deadline_unix - (int64_t)time(NULL)) * 1000;
    /* Room after the CLI stops: SIGTERM grace, two git calls, the reply. */
    int64_t margin = left > 180000 ? 45000 : left / 4;
    budget = left - margin;
  }
  if (budget < DLG_MIN_RUN_MS)
    DLG_FAIL("delegate/deadline",
             "only %lld ms are left before the invocation deadline; a "
             "delegate needs minutes (the command timeout is 30 minutes)",
             (long long)(budget > 0 ? budget : 0));
  ro.argv = argv.v;
  ro.envp = env.v;
  ro.cwd = ws;
  ro.input = sp->agent == DLG_CODEX ? task : NULL;
  ro.input_n = sp->agent == DLG_CODEX ? strlen(task) : 0;
  ro.timeout_ms = budget;
  ro.grace_ms = DLG_GRACE_MS;
  ro.max_line = DLG_LINE_CAP;
  ro.stderr_cap = DLG_STDERR_TAIL;
  ro.on_line = on_line;
  ro.ud = &st;
  if (dlg_run(&ro, &rr, emsg, sizeof emsg) != 0) {
    if (rr.exec_errno == EACCES || rr.exec_errno == EPERM)
      DLG_FAIL("delegate/execution-error",
               "%s: the CLI may not be executed here (at sandbox level "
               "strict only system directories are executable; run "
               "delegates at level basic)",
               emsg);
    DLG_FAIL("delegate/execution-error", "%s", emsg);
  }
  ran = 1;
  if (have_git && git_snapshot(ws, skip, &after) != 0) have_git = 0;

  /* 7. Result. */
  ok = rr.exit_code == 0 && !rr.timed_out && st.saw_result && !st.failed;
  if (r->max_output_bytes > 0 && (size_t)(r->max_output_bytes / 4) < final_cap)
    final_cap = (size_t)(r->max_output_bytes / 4);
  if (st.final && st.final[0] != '\0') {
    fin = st.final;
    final_cut = st.final_truncated;
  } else {
    tail = dlg_tail_text(&st);
    fin = tail ? tail : "";
  }
  fin_len = dlg_utf8_cut(fin, strlen(fin), final_cap);
  if (fin_len < strlen(fin)) final_cut = 1;

  res = xcdn_value_object();
  if (!res) goto oom;
  {
    char *fin_copy = core_strndup(fin, fin_len);
    int rc = !fin_copy;
    rc = rc || astd_set_bool(res, "ok", ok) != 0;
    rc = rc || astd_set_str(res, "agent",
                            sp->agent == DLG_CLAUDE ? "claude-code" : "codex") != 0;
    rc = rc || astd_set_str(res, "mode", sp->mode) != 0;
    rc = rc || astd_set_str(res, "final", fin_copy) != 0;
    free(fin_copy);
    rc = rc || astd_set_bool(res, "final_truncated", final_cut) != 0;
    rc = rc || astd_set_int(res, "turns", st.turns) != 0;
    rc = rc || astd_set_int(res, "duration_ms",
                            rr.duration_ms > 0 ? rr.duration_ms : 0) != 0;
    if (!rc && st.have_usage) {
      xcdn_value_t *u = xcdn_value_object();
      rc = !u || astd_set_int(u, "input_tokens", st.input_tokens) != 0 ||
           astd_set_int(u, "output_tokens", st.output_tokens) != 0 ||
           astd_set_int(u, "cached_input_tokens", st.cached_input_tokens) != 0;
      if (rc)
        xcdn_value_free(u);
      else
        rc = astd_set_val(res, "usage", u) != 0;
    }
    if (!rc && st.have_cost) {
      xcdn_value_t *c = xcdn_value_float(st.cost_usd);
      rc = !c || astd_set_val(res, "cost_usd", c) != 0;
    }
    if (!rc && st.session_id && st.session_id[0] != '\0')
      rc = astd_set_str(res, sp->agent == DLG_CLAUDE ? "session_id"
                                                     : "thread_id",
                        st.session_id) != 0;
    if (!rc && st.model && st.model[0] != '\0')
      rc = astd_set_str(res, "model", st.model) != 0;
    if (!rc && st.limit_seen) {
      xcdn_value_t *l = xcdn_value_object();
      /* A limit stopped the run only if the run failed; an error text that
       * names the limit counts even when the CLI died before saying so in
       * a structured event. */
      int hit = !ok && (st.limit_hit || dlg_text_is_limit(st.error));
      rc = !l || astd_set_bool(l, "hit", hit) != 0 ||
           astd_set_str(l, "message", st.limit_message ? st.limit_message
                                                       : "usage limit") != 0;
      if (!rc && st.limit_resets_at > 0) {
        char stamp[CORE_RFC3339_SIZE];
        xcdn_value_t *t;
        core_time_format_rfc3339((core_time)st.limit_resets_at, stamp);
        t = xcdn_value_datetime(stamp);
        rc = !t || astd_set_val(l, "resets_at", t) != 0;
      }
      if (rc)
        xcdn_value_free(l);
      else
        rc = astd_set_val(res, "limit", l) != 0;
    }
    if (!rc && have_git) {
      int trunc = 0;
      xcdn_value_t *ch = changed_files(&before, &after, &trunc);
      rc = !ch || astd_set_val(res, "changed_files", ch) != 0 ||
           astd_set_bool(res, "changed_files_truncated", trunc) != 0;
    }
    rc = rc || astd_set_int(res, "exit_code", rr.exit_code) != 0;
    rc = rc || astd_set_bool(res, "timed_out", rr.timed_out) != 0;
    if (!rc && !ok) {
      core_buf_init(&eb);
      if (rr.timed_out)
        (void)core_buf_printf(&eb,
                              "%s was stopped at the invocation deadline "
                              "after %lld s",
                              sp->label, (long long)(rr.duration_ms / 1000));
      if (st.error && st.error[0] != '\0') {
        if (eb.len > 0) (void)core_buf_appends(&eb, ": ");
        (void)core_buf_appends(&eb, st.error);
      } else if (!rr.timed_out) {
        if (rr.exit_code != 0)
          (void)core_buf_printf(&eb, "%s exited with status %d", sp->label,
                                rr.exit_code);
        else
          (void)core_buf_printf(&eb, "%s ended without a final result",
                                sp->label);
      }
      if (st.limit_hit || (st.limit_seen && dlg_text_is_limit(st.error)))
        (void)core_buf_appends(&eb,
                               " — the plan's usage limit is reached; do not "
                               "retry before it resets");
      if (st.auth_error)
        (void)core_buf_appends(
            &eb, sp->agent == DLG_CLAUDE
                     ? " — log in by running `claude` once in a terminal as "
                       "this user"
                     : " — log in with `codex login` in a terminal as this "
                       "user");
      if (rr.exit_code != 0 && st.events == 0 &&
          stderr_mentions_flags(rr.stderr_tail))
        (void)core_buf_appends(&eb,
                               " — the installed CLI does not accept the "
                               "flags asterism uses; update it");
      rc = astd_set_str(res, "error", eb.data ? eb.data : "failed") != 0;
      core_buf_free(&eb);
      if (!rc && rr.stderr_tail_n > 0) {
        errs = utf8_clean(rr.stderr_tail, rr.stderr_tail_n);
        rc = !errs || astd_set_str(res, "stderr_tail", errs) != 0;
      }
    }
    rc = rc || astd_set_int(res, "events", (int64_t)st.events) != 0;
    rc = rc || astd_set_int(res, "malformed_lines", (int64_t)st.malformed) != 0;
    rc = rc || astd_set_int(res, "oversized_lines",
                            (int64_t)rr.oversized_lines) != 0;
    if (rc) goto oom;
  }
  goto out;

oom:
  fcode = "delegate/failed";
  snprintf(fmsg, sizeof fmsg, "out of memory%s",
           ran ? " after the CLI ran (its changes, if any, are in place)" : "");
out:
  if (fcode) {
    xcdn_value_free(res);
    astd_fail(r, fcode, "%s", fmsg);
  } else {
    astd_ok(r, res);
  }
  core_buf_free(&eb);
  dlg_run_res_free(&rr);
  dlg_state_free(&st);
  snap_free(&before);
  snap_free(&after);
  strv_free(&argv);
  strv_free(&env);
  free(home);
  free(user);
  free(shell);
  free(cli);
  free(prompt);
  free(tail);
  free(errs);
  free(skip);
}

int astd_tool_delegate(astd_req *r) {
  size_t i;
  for (i = 0; i < sizeof k_specs / sizeof k_specs[0]; i++) {
    if (strcmp(r->command, k_specs[i].command) == 0) {
      run_delegate(r, &k_specs[i]);
      return 0;
    }
  }
  astd_fail(r, "astools/protocol", "unknown delegate command '%s'",
            r->command);
  return 0;
}

#endif /* POSIX */
