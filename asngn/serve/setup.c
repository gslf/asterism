/*
 * setup.c — "asterism setup": from an installed binary to a paired agent
 * that keeps running. Every step looks at what is already there, so running
 * it again repairs or resumes instead of starting over.
 *
 *   asterism setup [--root <dir>] [--tier base|standard|plus] [--yes]
 *                  [--token-stdin] [--no-models] [--no-telegram]
 *                  [--manual | --autostart] [--no-service] [--remove-service]
 *   asterism start | stop | status [--root <dir>]
 *
 * The questions come first (tier, bot token, pairing, an optional ChatGPT
 * sign-in), then what takes long and needs nobody: the model downloads and
 * the service. The model manifest and the tier configurations are embedded
 * at build time from scripts/models.manifest.tsv and examples/<tier>.xcdn.
 * The bot token goes to the vault, released only to "serve"; it is never
 * written to a configuration file or printed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define isatty _isatty
#define popen _popen
#define pclose _pclose
#else
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif

#include "asmodel.h"
#include "asvault.h"
#include "modes.h"
#include "serve.h"
#include "setup.h"
#include "setup_data.h"

#define TOKEN_SECRET "telegram-token"

typedef struct {
  const char *root;
  const char *tier;          /* --tier                                   */
  const char *manifest_path; /* hidden: a manifest file instead          */
  const char *api_base;      /* hidden: Telegram API for tests           */
  int pair_timeout_s;        /* hidden: 0 = wait until paired            */
  bool yes, token_stdin, no_models, no_telegram, no_service, remove_service;
  bool manual, autostart;    /* --manual / --autostart; neither: ask     */
  bool interactive;
  const char *argv0;
} options;

/* What each step found or did, for the summary. */
typedef struct {
  const char *tier;
  bool config_ok, tools_ok, models_ok, token_ok, paired, service_ok;
  bool service_skipped, models_skipped, telegram_skipped, autostart;
  char bot[65];
  char service_note[160];
} outcome;

/* ── terminal ────────────────────────────────────────────────────────── */

static void step(const char *title) { printf("\n%s\n", title); }

static bool read_line(char *buf, size_t n) {
  if (!fgets(buf, (int)n, stdin)) return false;
  core_str_trim(buf);
  return true;
}

static bool ask_yes(const options *o, const char *question, bool def) {
  char line[16];
  if (!o->interactive) return def;
  printf("  %s %s ", question, def ? "[Y/n]" : "[y/N]");
  fflush(stdout);
  if (!read_line(line, sizeof line) || !line[0]) return def;
  return line[0] == 'y' || line[0] == 'Y';
}

/* A line typed without echo (the bot token). */
static char *read_secret(bool from_stdin, const char *prompt) {
  char buf[512];
  char *ok;
  if (!from_stdin) {
    fputs(prompt, stdout);
    fflush(stdout);
  }
#if defined(_WIN32)
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode = 0;
  bool tty = !from_stdin && GetConsoleMode(in, &mode);
  if (tty) SetConsoleMode(in, mode & ~(DWORD)ENABLE_ECHO_INPUT);
#else
  struct termios old, quiet;
  bool tty = !from_stdin && isatty(0) && tcgetattr(0, &old) == 0;
  if (tty) {
    quiet = old;
    quiet.c_lflag &= ~(tcflag_t)ECHO;
    tcsetattr(0, TCSANOW, &quiet);
  }
#endif
  ok = fgets(buf, sizeof buf, stdin);
#if defined(_WIN32)
  if (tty) SetConsoleMode(in, mode);
#else
  if (tty) tcsetattr(0, TCSANOW, &old);
#endif
  if (tty) fputc('\n', stdout);
  if (!ok) return NULL;
  core_str_trim(buf);
  char *s = core_strdup(buf);
  memset(buf, 0, sizeof buf);
  return s;
}

static void wipe_free(char *s) {
  if (s) memset(s, 0, strlen(s));
  free(s);
}

static char *default_root(void) {
  const char *home = getenv("USERPROFILE");
  if (home == NULL || home[0] == '\0') home = getenv("HOME");
  return home && *home ? os_path_join(home, "asterism") : NULL;
}

static char *at_root(const options *o, const char *name) { return os_path_join(o->root, name); }

/* ── hardware and tier ───────────────────────────────────────────────── */

typedef struct {
  uint64_t ram_mb, vram_mb;
  bool apple;
  int gpu_build; /* asmodel_llama_gpu(): 1 GPU offload, 0 CPU only, -1 no llama */
} hardware;

static uint64_t command_number(const char *cmd) {
  char line[128] = "";
  FILE *f = popen(cmd, "r");
  if (!f) return 0;
  if (!fgets(line, sizeof line, f)) line[0] = 0;
  pclose(f);
  return strtoull(line, NULL, 10);
}

static hardware detect(void) {
  hardware h;
  memset(&h, 0, sizeof h);
#if defined(__APPLE__)
  h.apple = true;
  h.ram_mb = command_number("sysctl -n hw.memsize 2>/dev/null") / (1024 * 1024);
  h.vram_mb = h.ram_mb; /* unified memory */
#elif defined(_WIN32)
  MEMORYSTATUSEX ms;
  ms.dwLength = sizeof ms;
  if (GlobalMemoryStatusEx(&ms)) h.ram_mb = ms.ullTotalPhys / (1024 * 1024);
  if (setup_in_path("nvidia-smi"))
    h.vram_mb = command_number(
        "nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>NUL");
#else
  char *text = NULL;
  if (os_read_file("/proc/meminfo", &text, NULL) == CORE_OK) {
    const char *p = strstr(text, "MemTotal:");
    if (p) h.ram_mb = strtoull(p + 9, NULL, 10) / 1024;
    free(text);
  }
  /* the largest NVIDIA GPU, when the driver tools are installed */
  if (setup_in_path("nvidia-smi"))
    h.vram_mb = command_number(
        "nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null "
        "| sort -n | tail -1");
#endif
  h.gpu_build = asmodel_llama_gpu();
  return h;
}

static const char *recommend(const hardware *h) {
  if (h->gpu_build == 0) return "base"; /* CPU-only build: the small tier */
  if (h->apple) return h->ram_mb >= 24000 ? "plus" : h->ram_mb >= 15000 ? "standard" : "base";
  if (h->vram_mb >= 14000) return "plus";
  if (h->vram_mb >= 7000) return "standard";
  return "base";
}

static bool tier_known(const char *t) {
  return t && (!strcmp(t, "base") || !strcmp(t, "standard") || !strcmp(t, "plus"));
}

static const char *tier_config(const char *t) {
  return !strcmp(t, "plus")       ? (const char *)setup_tier_plus
         : !strcmp(t, "standard") ? (const char *)setup_tier_standard
                                  : (const char *)setup_tier_base;
}

/* The tier to use: --tier, the one config.xcdn already runs, or a choice. */
static const char *choose_tier(const options *o, const hardware *hw, const char *manifest,
                               bool *config_exists, const char **current_out) {
  char *path = at_root(o, "config.xcdn"), *text = NULL, line[32];
  const char *rec = recommend(hw), *current = NULL;
  *current_out = NULL;
  *config_exists = path && os_file_exists(path);
  if (*config_exists && os_read_file(path, &text, NULL) == CORE_OK) {
    current = *current_out = setup_manifest_tier_of(manifest, text);
    free(text);
  }
  free(path);
  printf("  memory %.1f GB", (double)hw->ram_mb / 1024.0);
  if (hw->apple) printf(", unified with the GPU");
  else if (hw->vram_mb) printf(", GPU %.1f GB", (double)hw->vram_mb / 1024.0);
  printf("\n");
  if (hw->gpu_build == 0 && (hw->vram_mb >= 7000 || (hw->apple && hw->ram_mb >= 15000)))
    printf("  this build runs models on the CPU only, so tier base; a GPU build:\n"
           "  ./install.sh --from-source\n");
  if (o->tier) return o->tier;
  if (*config_exists) {
    if (current) printf("  config.xcdn runs tier %s\n", current);
    else printf("  config.xcdn uses its own models (pass --tier to fetch a tier)\n");
    return current;
  }
  printf("  tiers: base (8 GB RAM, no GPU), standard (8 GB VRAM), plus (16 GB VRAM)\n");
  if (!o->interactive) {
    printf("  tier %s\n", rec);
    return rec;
  }
  for (;;) {
    printf("  Tier [%s]: ", rec);
    fflush(stdout);
    if (!read_line(line, sizeof line) || !line[0]) return rec;
    if (tier_known(line)) {
      static char chosen[32];
      snprintf(chosen, sizeof chosen, "%s", line);
      return chosen;
    }
    printf("  base, standard or plus\n");
  }
}

/* ── config and tools ────────────────────────────────────────────────── */

static bool write_config(const options *o, const char *tier) {
  char *path = at_root(o, "config.xcdn");
  const char *text = tier_config(tier);
  bool ok = path && os_write_file(path, text, strlen(text)) == CORE_OK;
  printf(ok ? "  config.xcdn: written for tier %s\n" : "  cannot write config.xcdn\n", tier);
  free(path);
  return ok;
}

/* Copies a directory tree, keeping the execute bits of tool binaries. */
static bool copy_tree(const char *from, const char *to) {
  char **names = NULL;
  size_t n = 0;
  bool ok = os_mkdir_p(to) == CORE_OK && os_list_dir(from, &names, &n) == CORE_OK;
  for (size_t i = 0; ok && i < n; i++) {
    char *src = os_path_join(from, names[i]), *dst = os_path_join(to, names[i]), *data = NULL;
    size_t len = 0;
    ok = src && dst && os_read_file(src, &data, &len) == CORE_OK &&
         os_write_file(dst, data, len) == CORE_OK;
#if !defined(_WIN32)
    struct stat sb;
    if (ok && stat(src, &sb) == 0) (void)chmod(dst, sb.st_mode & 0777);
#endif
    free(data);
    free(src);
    free(dst);
  }
  os_free_names(names, n);
  names = NULL;
  n = 0;
  if (ok && os_list_dirs(from, &names, &n) == CORE_OK) {
    for (size_t i = 0; ok && i < n; i++) {
      char *src = os_path_join(from, names[i]), *dst = os_path_join(to, names[i]);
      ok = src && dst && copy_tree(src, dst);
      free(src);
      free(dst);
    }
    os_free_names(names, n);
  }
  return ok;
}

/* Tool packages next to the executable: share/asterism/tools when
 * installed, build/packages in a build tree. */
static bool install_tools(const options *o) {
  char exe[1024], dir[1200], *dst = at_root(o, "tools");
  const char *const rel[] = {"../share/asterism/tools", "../packages"};
  char **names = NULL;
  size_t n = 0, copied = 0;
  bool ok = true, found = false;
  if (!dst || !setup_self_path(exe, sizeof exe, o->argv0)) {
    free(dst);
    return false;
  }
  for (char *s = exe; *s; s++)
    if (*s == '\\') *s = '/';
  if (strrchr(exe, '/')) *strrchr(exe, '/') = '\0';
  for (size_t r = 0; r < 2 && !found; r++) {
    snprintf(dir, sizeof dir, "%s/%s", exe, rel[r]);
    found = os_is_dir(dir) == 1;
  }
  if (!found) {
    printf("  no tool packages next to %s; the agent runs without tools\n", exe);
    free(dst);
    return false;
  }
  if (os_list_dirs(dir, &names, &n) == CORE_OK) {
    for (size_t i = 0; i < n; i++) {
      char *src = os_path_join(dir, names[i]), *to = os_path_join(dst, names[i]);
      if (src && to && copy_tree(src, to)) copied++;
      else ok = false;
      free(src);
      free(to);
    }
    os_free_names(names, n);
  }
  printf(ok ? "  tools: %zu packages in %s\n" : "  tools: %zu packages copied, some failed\n",
         copied, dst);
  free(dst);
  return ok && copied > 0;
}

#define GRANTS_MARK "// Written by asterism setup"

static void xcdn_string(core_buf *b, const char *s) {
  core_buf_appends(b, "\"");
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') core_buf_appends(b, "\\");
    core_buf_append(b, s, 1);
  }
  core_buf_appends(b, "\"");
}

/* The folders holding Codex and Claude Code: where PATH finds them and,
 * through a link, where the program really is. */
typedef struct {
  char path[1100];
} folder;

static size_t cli_dirs(folder *dirs, size_t max) {
  static const char *const clis[] = {"claude", "codex"};
  size_t n = 0;
  for (size_t c = 0; c < 2; c++) {
    char full[1100], *real = NULL;
    const char *cand[2];
    if (!setup_which(clis[c], full, sizeof full)) continue;
    cand[0] = full;
    cand[1] = os_realpath(full, &real) == CORE_OK ? real : NULL;
    for (size_t k = 0; k < 2 && cand[k]; k++) {
      char dir[1100], *slash;
      bool seen = false;
      snprintf(dir, sizeof dir, "%s", cand[k]);
      for (char *s = dir; *s; s++)
        if (*s == '\\') *s = '/';
      if ((slash = strrchr(dir, '/')) == NULL) continue;
      *slash = '\0';
      if (n >= max) break;
      for (size_t i = 0; i < n && !seen; i++)
        seen = strncmp(dirs[i].path, dir, sizeof dirs[i].path) == 0;
      if (!seen) snprintf(dirs[n++].path, sizeof dirs[0].path, "%s", dir);
    }
    free(real);
  }
  return n;
}

/* astools.xcdn says what the tools may do: work in the session's or the
 * project's folder, run builds, tests and git, and start Codex or Claude
 * Code. Written once; later runs only add the folders of those programs when
 * they were installed since, so the user's own edits stay. */
static bool tool_grants(const options *o) {
  char *path = at_root(o, "astools.xcdn"), *text = NULL;
  folder dirs[8];
  size_t nd = cli_dirs(dirs, 8), added = 0;
  core_buf b;
  bool ok = true;
  if (!path) return false;
  core_buf_init(&b);
  if (!os_file_exists(path)) {
    core_buf_appends(&b, GRANTS_MARK "; see docs/TOOLS.md. Edit freely.\n"
                         "#astools_config {\n"
                         "  registry: { paths: [ { path: \"tools\", trust: \"standard\" } ],\n"
                         "              watch: \"off\", pinning: \"off\" },\n"
                         "  workspace: { root: \".\" },\n"
                         "  // folders the tools may start programs from (Codex, Claude Code)\n"
                         "  sandbox: { executable_paths: [");
    for (size_t i = 0; i < nd; i++) {
      core_buf_appends(&b, i ? ", " : " ");
      xcdn_string(&b, dirs[i].path);
    }
    core_buf_appends(&b, " ] },\n"
                         "  grants: {\n"
                         "    workspace_access: \"read-write\",\n"
                         "    tools: [\n"
                         "      { tool: \"proc\", proc: true },\n"
                         "      { tool: \"git\", proc: true },\n"
                         "      { tool: \"project\", proc: true },\n"
                         "      { tool: \"delegate\", proc: true, net: true, env: [\"LANG\"] },\n"
                         "    ],\n"
                         "  },\n"
                         "}\n");
    ok = os_write_file(path, b.data, b.len) == CORE_OK;
    printf(ok ? "  astools.xcdn: tools may change files in the work folder and run builds\n"
              : "  cannot write astools.xcdn\n");
    added = nd;
  } else if (os_read_file(path, &text, NULL) == CORE_OK && !strncmp(text, GRANTS_MARK, 27)) {
    const char *list = strstr(text, "executable_paths: [");
    if (list) {
      size_t at = (size_t)(list - text) + 19;
      core_buf_append(&b, text, at);
      for (size_t i = 0; i < nd; i++) {
        core_buf q;
        core_buf_init(&q);
        xcdn_string(&q, dirs[i].path);
        if (core_buf_appends(&q, "") == CORE_OK && !strstr(text, q.data)) {
          core_buf_appends(&b, " ");
          core_buf_appends(&b, q.data);
          core_buf_appends(&b, ",");
          added++;
        }
        core_buf_free(&q);
      }
      core_buf_appends(&b, text + at);
      if (added) ok = os_write_file(path, b.data, b.len) == CORE_OK;
    }
  }
  for (size_t i = 0; ok && i < added; i++)
    printf("  the agent may start programs from %s (Codex, Claude Code)\n", dirs[i].path);
  core_buf_free(&b);
  free(text);
  free(path);
  return ok;
}

/* ── Telegram: token ─────────────────────────────────────────────────── */

static bool token_shape(const char *t) {
  const char *colon = t ? strchr(t, ':') : NULL;
  if (!colon || colon == t || !colon[1]) return false;
  for (const char *p = t; p < colon; p++)
    if (*p < '0' || *p > '9') return false;
  for (const char *p = colon + 1; *p; p++)
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
          *p == '_' || *p == '-'))
      return false;
  return true;
}

static aslink_err open_bot(const char *token, const char *api_base, aslink_ctx **bot,
                           aslink_bot_info *info, aslink_status *st) {
  aslink_telegram_params p;
  aslink_err e;
  memset(&p, 0, sizeof p);
  memset(st, 0, sizeof *st);
  p.token = token;
  p.api_base = api_base && *api_base ? api_base : NULL;
  if ((e = aslink_telegram_open(&p, bot)) != ASLINK_OK) return e;
  if ((e = aslink_get_me(*bot, info, st)) != ASLINK_OK) {
    aslink_close(*bot);
    *bot = NULL;
  }
  return e;
}

static asvault *open_vault(const options *o) {
  char *dir = at_root(o, "vault");
  asvault *v = NULL;
  if (!dir || asvault_open(dir, &v) != ASVAULT_OK) v = NULL;
  free(dir);
  return v;
}

/* The value of NAME=value in an env file (the old serve.env). */
static char *env_file_value(const char *path, const char *name) {
  char *text = NULL, *line, *next, *value = NULL;
  size_t n = strlen(name);
  if (os_read_file(path, &text, NULL) != CORE_OK) return NULL;
  for (line = text; line && *line && !value; line = next) {
    next = strchr(line, '\n');
    if (next) *next++ = '\0';
    core_str_trim(line);
    if (!strncmp(line, name, n) && line[n] == '=') value = core_strdup(line + n + 1);
  }
  memset(text, 0, strlen(text));
  free(text);
  if (value) core_str_trim(value);
  return value;
}

/* serve.xcdn names the vault secret. A file setup wrote before the vault
 * named an environment variable: that one key is replaced. */
static bool config_token_secret(const options *o) {
  char *path = at_root(o, "serve.xcdn"), *text = NULL;
  core_buf b;
  bool ok = false;
  core_buf_init(&b);
  if (!path) return false;
  if (!os_file_exists(path)) {
    core_buf_appends(&b, "// Written by asterism setup; see examples/serve.xcdn for every key.\n"
                         "#serve {\n  telegram: { token_secret: \"" TOKEN_SECRET "\", ");
    if (o->api_base) core_buf_printf(&b, "api_base: \"%s\", ", o->api_base);
    core_buf_appends(&b, "topics: true },\n"
                         "  routines: { dir: \"routines\" },\n"
                         "  stars: [ { name: \"default\", mode: \"chat\", profile: \"chat\" } ],\n"
                         "}\n");
    ok = os_write_file(path, b.data, b.len) == CORE_OK;
  } else if (os_read_file(path, &text, NULL) == CORE_OK) {
    const char *tg = strstr(text, "telegram:");
    const char *key = tg ? strstr(tg, "token_env:") : NULL;
    const char *end = tg ? strchr(tg, '}') : NULL;
    if (key && end && key > end) key = NULL; /* a star's own bot, not the main one */
    const char *open = key ? strchr(key, '"') : NULL;
    const char *close = open ? strchr(open + 1, '"') : NULL;
    if (strstr(text, "token_secret:")) ok = true;
    else if (close) {
      core_buf_append(&b, text, (size_t)(key - text));
      core_buf_appends(&b, "token_secret: \"" TOKEN_SECRET "\"");
      core_buf_appends(&b, close + 1);
      ok = os_write_file(path, b.data, b.len) == CORE_OK;
    }
  }
  if (!ok)
    printf("  add token_secret: \"" TOKEN_SECRET "\" to telegram in %s\n", path);
  core_buf_free(&b);
  free(text);
  free(path);
  return ok;
}

/* Finds the token (vault, environment, old serve.env) or asks for it, checks
 * it with getMe and keeps it in the vault. The bot stays open for pairing. */
static aslink_ctx *telegram_token(const options *o, outcome *out, char *api_base, size_t api_n) {
  serve_config cfg;
  char err[256], env_path[1200];
  char *token = NULL;
  bool from_vault = false, from_env_file = false;
  asvault *v = open_vault(o);
  aslink_ctx *bot = NULL;
  aslink_bot_info info;
  aslink_status st;
  aslink_err e = ASLINK_ERR_INVALID;

  serve_config_defaults(&cfg);
  if (!serve_config_load(&cfg, o->root, NULL, err, sizeof err)) {
    printf("  serve.xcdn: %s\n", err);
    asvault_close(v);
    return NULL;
  }
  snprintf(api_base, api_n, "%s", o->api_base ? o->api_base : cfg.api_base);
  snprintf(env_path, sizeof env_path, "%s/serve.env", o->root);
  if (v && asvault_get(v, cfg.token_secret[0] ? cfg.token_secret : TOKEN_SECRET, "serve",
                       &token) == ASVAULT_OK) {
    from_vault = true;
  } else if (getenv(cfg.token_env) && *getenv(cfg.token_env)) {
    token = core_strdup(getenv(cfg.token_env));
  } else if ((token = env_file_value(env_path, cfg.token_env)) != NULL) {
    from_env_file = true;
  }
  serve_config_free(&cfg);

  for (int tries = 0; tries < 3; tries++) {
    if (!token) {
      if (o->interactive)
        printf("  Create a bot: in Telegram open @BotFather, send /newbot, copy the token.\n");
      else if (!o->token_stdin) break;
      token = read_secret(o->token_stdin, "  Bot token (Enter to skip): ");
      if (!token || !*token) {
        wipe_free(token);
        token = NULL;
        break;
      }
      if (!token_shape(token)) {
        printf("  that is not a bot token (<digits>:<key>)\n");
        wipe_free(token);
        token = NULL;
        if (!o->interactive) break;
        continue;
      }
    }
    e = open_bot(token, api_base, &bot, &info, &st);
    if (e == ASLINK_OK) break;
    if (e == ASLINK_ERR_UNAUTHORIZED || e == ASLINK_ERR_NOT_FOUND) {
      printf("  Telegram rejected the token\n");
      wipe_free(token);
      token = NULL;
      from_vault = from_env_file = false;
      if (!o->interactive) break;
      continue;
    }
    printf("  cannot reach Telegram: %s %s\n", aslink_err_name(e), st.message);
    break;
  }
  if (e != ASLINK_OK || !bot) {
    if (!token) printf("  no bot token: the agent cannot be reached on Telegram yet\n");
    wipe_free(token);
    asvault_close(v);
    return NULL;
  }
  snprintf(out->bot, sizeof out->bot, "%s", info.username);
  printf("  bot: @%s\n", info.username);

  if (!from_vault) {
    const char *tools[] = {"serve"};
    asvault_policy policy = {tools, 1, NULL, 0};
    if (!v || asvault_set(v, TOKEN_SECRET, token, &policy) != ASVAULT_OK) {
      printf("  cannot store the token in the vault: %s\n", v ? asvault_last_error(v) : "");
    } else if (config_token_secret(o)) {
      printf("  token: in the vault, released only to the daemon\n");
      if (from_env_file && os_remove_file(env_path) == CORE_OK)
        printf("  serve.env removed: the vault holds the token now\n");
      out->token_ok = true;
    }
  } else {
    out->token_ok = config_token_secret(o);
    printf("  token: in the vault\n");
  }
  wipe_free(token);
  asvault_close(v);
  return bot;
}

/* ── Telegram: pairing ───────────────────────────────────────────────── */

static bool is_conflict(const aslink_status *st) {
  return st->http_status == 409 || st->telegram_code == 409;
}

/* setup claims the pairing code itself, so the daemon need not run yet. */
static bool telegram_pair(const options *o, aslink_ctx *bot, outcome *out) {
  serve_state state;
  char err[256], code[16] = "";
  int64_t offset = 0, code_at = 0, started = os_now_unix();
  bool paired = false, told = false;
  if (!serve_state_open(&state, o->root, err, sizeof err)) {
    printf("  %s\n", err);
    return false;
  }
  if (state.owner_id) {
    printf("  paired with your Telegram account\n");
    serve_state_close(&state);
    return true;
  }
  while (!paired) {
    aslink_update *u = NULL;
    size_t n = 0;
    aslink_status st;
    int64_t now = os_now_unix();
    int wait = 20;
    aslink_err e;
    if (!code_at || now - code_at > 540) { /* codes live ten minutes */
      if (!serve_pairing_create(&state, code, err, sizeof err)) {
        printf("  %s\n", err);
        break;
      }
      code_at = now;
      printf("  On your phone, open this link and tap Start:\n\n"
             "    https://t.me/%s?start=%s\n\n"
             "  or send the bot:  /start %s\n"
             "  Waiting… (Ctrl-C stops; run asterism setup again later)\n",
             out->bot, code, code);
      fflush(stdout);
    }
    if (o->pair_timeout_s) {
      if (now - started >= o->pair_timeout_s) {
        printf("  no pairing within %d s\n", o->pair_timeout_s);
        break;
      }
      if (wait > o->pair_timeout_s - (int)(now - started))
        wait = o->pair_timeout_s - (int)(now - started);
    }
    e = aslink_get_updates(bot, offset, wait, &u, &n, &st);
    if (e != ASLINK_OK) {
      if (e == ASLINK_ERR_UNAUTHORIZED) {
        printf("  Telegram rejected the token\n");
        break;
      }
      if (!told && is_conflict(&st))
        printf("  another program reads this bot's messages (an asterism serve still\n"
               "  running?): stop it, pairing goes on by itself\n");
      else if (!told)
        printf("  Telegram: %s %s; retrying\n", aslink_err_name(e), st.message);
      told = true;
      os_sleep_ms(3000);
      continue;
    }
    for (size_t i = 0; i < n; i++) {
      const aslink_update *m = &u[i];
      const char *text = m->text ? m->text : "";
      if (m->update_id >= offset) offset = m->update_id + 1;
      if (paired || m->kind != ASLINK_UPDATE_MESSAGE || strcmp(m->chat_type, "private") ||
          m->from_is_bot)
        continue;
      if (!strncmp(text, "/start ", 7) &&
          serve_pairing_claim(&state, text + 7, m->from_id, m->chat_id)) {
        paired = true;
        (void)aslink_send_message(bot, m->chat_id, 0, serve_s(S_PAIRED), NULL, NULL, NULL);
        (void)aslink_send_message(bot, m->chat_id, 0, serve_s(S_HELP), NULL, NULL, NULL);
      } else {
        (void)aslink_send_message(bot, m->chat_id, 0, serve_s(S_PAIR_FIRST), NULL, NULL, NULL);
      }
    }
    aslink_updates_free(u, n);
  }
  if (paired) {
    /* confirm what was read, so the daemon does not see it again */
    aslink_update *u = NULL;
    aslink_status st;
    size_t n = 0;
    if (aslink_get_updates(bot, offset, 0, &u, &n, &st) == ASLINK_OK) aslink_updates_free(u, n);
    printf("  paired: the agent answers only your account\n");
  }
  serve_state_close(&state);
  return paired;
}

/* ── optional subscriptions ──────────────────────────────────────────── */

static void subscriptions(const options *o) {
  bool codex = setup_in_path("codex"), claude = setup_in_path("claude");
  printf("  Codex CLI: %s · Claude Code: %s (each signs in with its own command)\n",
         codex ? "found" : "not installed", claude ? "found" : "not installed");
  if (ask_yes(o, "Sign in with your ChatGPT plan now?", false)) {
    char *argv[] = {"login", "chatgpt", "--root", (char *)o->root, NULL};
    (void)asngn_login_main(4, argv);
  } else {
    printf("  later: asterism login chatgpt\n");
  }
}

/* ── checks shared with doctor ───────────────────────────────────────── */

void asngn_setup_report(const char *root, FILE *f) {
  char path[1200], **names = NULL;
  size_t n = 0;
  serve_config cfg;
  serve_state state;
  char err[256];
  aslink_telegram_params p;
  aslink_ctx *probe = NULL;
  bool transport, token = false;

  snprintf(path, sizeof path, "%s/tools", root);
  if (os_list_dirs(path, &names, &n) != CORE_OK) n = 0;
  os_free_names(names, n);
  fprintf(f, "%s tools: %zu packages in %s\n", n ? "ok  " : "warn", n, path);

  memset(&p, 0, sizeof p);
  p.token = "1:probe";
  transport = aslink_telegram_open(&p, &probe) != ASLINK_ERR_UNSUPPORTED;
  aslink_close(probe);
  fprintf(f, "%s telegram transport: %s\n", transport ? "ok  " : "warn",
          transport ? "built in" : "this build has none (no libcurl or WinHTTP)");

  serve_config_defaults(&cfg);
  if (serve_config_load(&cfg, root, NULL, err, sizeof err)) {
    asvault *v = NULL;
    char *value = NULL;
    snprintf(path, sizeof path, "%s/vault", root);
    if (cfg.token_secret[0] && asvault_open(path, &v) == ASVAULT_OK &&
        asvault_get(v, cfg.token_secret, "serve", &value) == ASVAULT_OK)
      token = true;
    asvault_free_secret(value);
    asvault_close(v);
    if (!token && getenv(cfg.token_env) && *getenv(cfg.token_env)) token = true;
    fprintf(f, "%s telegram token: %s\n", token ? "ok  " : "warn",
            token ? (cfg.token_secret[0] ? "in the vault" : "in the environment")
                  : "missing (asterism setup)");
  } else {
    fprintf(f, "warn serve.xcdn: %s\n", err);
  }
  serve_config_free(&cfg);

  if (serve_state_open(&state, root, err, sizeof err)) {
    fprintf(f, "%s telegram pairing: %s\n", state.owner_id ? "ok  " : "warn",
            state.owner_id ? "paired" : "not paired (asterism setup)");
    serve_state_close(&state);
  }
  if (setup_service_kind_here() == SETUP_SERVICE_NONE) return;
  if (!setup_service_ready(root)) {
    fprintf(f, "warn agent: not set up to run in the background (asterism setup)\n");
    return;
  }
  fprintf(f, "ok   agent: %s, %s\n",
          setup_service_active(root) ? "running" : "stopped",
          setup_service_autostart(root) ? "starts when you log in"
                                        : "started by hand (asterism start)");
}

/* Model files of other tiers left in models/ after a switch. */
static void unused_models(const options *o, const char *manifest, const char *tier) {
  setup_model *all = NULL;
  size_t n = 0, found = 0;
  if (!setup_manifest_parse(manifest, NULL, &all, &n)) return;
  for (size_t i = 0; i < n; i++) {
    char path[1200];
    bool used = false;
    for (const char *t = all[i].tiers; *t;) { /* the row's tiers, comma-separated */
      size_t len = strcspn(t, ",");
      if (len == strlen(tier) && !strncmp(t, tier, len)) used = true;
      t += len + (t[len] == ',');
    }
    snprintf(path, sizeof path, "%s/models/%s", o->root, all[i].name);
    if (used || !os_file_exists(path)) continue;
    /* the same file may be listed for more than one tier */
    for (size_t j = 0; j < i && !used; j++) used = !strcmp(all[j].name, all[i].name);
    if (used) continue;
    if (found++) printf(", %s", all[i].name);
    else printf("  not used by tier %s: %s", tier, all[i].name);
  }
  if (found) printf("\n  (delete them from %s/models to free space)\n", o->root);
  free(all);
}

/* ── asterism start | stop | status ─────────────────────────────────── */

int asngn_service_main(int argc, char **argv) {
  const char *what = argv[0], *root = NULL;
  char *own_root = NULL, exe[1024];
  int status = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
    else {
      fprintf(stderr, "usage: asterism %s [--root <dir>]\n", what);
      return 2;
    }
  }
  if (!root) root = own_root = default_root();
  if (!root) {
    fputs("asterism: no --root and no HOME\n", stderr);
    return 2;
  }
  if (!setup_self_path(exe, sizeof exe, NULL)) snprintf(exe, sizeof exe, "asterism");
  if (setup_service_kind_here() == SETUP_SERVICE_NONE || !setup_service_ready(root)) {
    printf("The agent is not set up to run in the background here.\n"
           "Run asterism setup, or run it in this terminal: asterism serve\n");
    status = !strcmp(what, "status") ? 0 : 1;
  } else if (!strcmp(what, "start")) {
    if (setup_service_active(root)) {
      printf("asterism is already running.\n");
    } else if (setup_service_start(exe, root)) {
      printf("asterism is running. Stop it with: asterism stop\n");
    } else {
      printf("asterism did not start; see asterism status\n");
      status = 1;
    }
  } else if (!strcmp(what, "stop")) {
    if (!setup_service_active(root)) {
      printf("asterism is not running.\n");
    } else if (setup_service_stop(root)) {
      printf("asterism stopped. Start it again with: asterism start\n");
    } else {
      printf("asterism did not stop\n");
      status = 1;
    }
  } else {
    printf("asterism is %s; it %s.\n", setup_service_active(root) ? "running" : "stopped",
           setup_service_autostart(root) ? "starts by itself when you log in"
                                         : "runs only after asterism start");
#if defined(_WIN32) || defined(__APPLE__)
    printf("Log: %s/serve.log\n", root);
#else
    printf("Log: journalctl --user -u asterism -f\n");
#endif
  }
  free(own_root);
  return status;
}

/* ── main ────────────────────────────────────────────────────────────── */

static void usage(FILE *f) {
  fputs("usage: asterism setup [--root <dir>] [--tier base|standard|plus] [--yes]\n"
        "                      [--token-stdin] [--no-models] [--no-telegram]\n"
        "                      [--manual | --autostart] [--no-service] [--remove-service]\n\n"
        "Gets asterism ready on this machine: the model tier for its memory and\n"
        "GPU, verified model downloads (resumed when interrupted), config.xcdn,\n"
        "the tool packages, the Telegram bot token (kept in the vault), pairing\n"
        "with your Telegram account and a user service that keeps it running.\n"
        "Run it again at any time: it keeps what is already in place.\n\n"
        "  --yes             accept every default, ask nothing\n"
        "  --tier <name>     switch to that tier (the old config.xcdn becomes .bak)\n"
        "  --manual          the agent runs only after asterism start\n"
        "  --autostart       the agent also starts when you log in\n"
        "  --token-stdin     read the bot token from standard input\n"
        "  --remove-service  stop the agent and remove its service, nothing else\n",
        f);
}

static bool parse(int argc, char **argv, options *o) {
  memset(o, 0, sizeof *o);
  o->argv0 = argv[0];
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char **slot = !strcmp(a, "--root")       ? &o->root
                        : !strcmp(a, "--tier")     ? &o->tier
                        : !strcmp(a, "--manifest") ? &o->manifest_path
                        : !strcmp(a, "--api-base") ? &o->api_base
                                                   : NULL;
    if (slot && i + 1 < argc) *slot = argv[++i];
    else if (!strcmp(a, "--pair-timeout") && i + 1 < argc) o->pair_timeout_s = atoi(argv[++i]);
    else if (!strcmp(a, "--yes") || !strcmp(a, "-y")) o->yes = true;
    else if (!strcmp(a, "--token-stdin")) o->token_stdin = true;
    else if (!strcmp(a, "--no-models")) o->no_models = true;
    else if (!strcmp(a, "--no-telegram")) o->no_telegram = true;
    else if (!strcmp(a, "--no-service")) o->no_service = true;
    else if (!strcmp(a, "--manual")) o->manual = true;
    else if (!strcmp(a, "--autostart")) o->autostart = true;
    else if (!strcmp(a, "--remove-service")) o->remove_service = true;
    else if (!strcmp(a, "--service")) {} /* the default now */
    else return false;
  }
  if (o->tier && !tier_known(o->tier)) {
    fprintf(stderr, "asterism setup: tier %s: base, standard or plus%s\n", o->tier,
            !strcmp(o->tier, "legacy") ? " (legacy: scripts/fetch-models)" : "");
    return false;
  }
  o->interactive = !o->yes && !o->token_stdin && isatty(0);
  return true;
}

static void summary(const options *o, const outcome *r) {
  printf("\n⁂ Summary for %s\n", o->root);
  if (r->models_skipped) printf("  – models (skipped)\n");
  else printf("  %s models%s\n", r->models_ok ? "✓" : "✗",
              r->models_ok ? "" : ": run asterism setup again to resume");
  printf("  %s config.xcdn%s%s\n", r->config_ok ? "✓" : "✗", r->tier ? ", tier " : "",
         r->tier ? r->tier : "");
  printf("  %s tools\n", r->tools_ok ? "✓" : "✗");
  if (r->telegram_skipped) {
    printf("  – Telegram (skipped)\n");
  } else {
    printf("  %s Telegram bot%s%s\n", r->token_ok ? "✓" : "✗", r->bot[0] ? " @" : "", r->bot);
    printf("  %s paired with your account\n", r->paired ? "✓" : "✗");
  }
  if (r->service_skipped) printf("  – service: %s\n", r->service_note);
  else printf("  %s service: %s\n", r->service_ok ? "✓" : "✗", r->service_note);
  if (r->paired && r->service_ok && r->autostart)
    printf("\n  Done. asterism writes to you on Telegram when it is up.\n");
  else if (r->paired && r->service_ok)
    printf("\n  Done. Start the agent with: asterism start (stop: asterism stop)\n");
  printf("  Check everything any time: asterism doctor\n");
}

int asngn_setup_main(int argc, char **argv) {
  options o;
  outcome r;
  hardware hw;
  char *own_root = NULL, *manifest = NULL, exe[1024], api_base[256] = "";
  aslink_ctx *bot = NULL;
  bool config_exists = false, stopped = false;
  const char *current = NULL;

  if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
    usage(stdout);
    return 0;
  }
  if (!parse(argc, argv, &o)) {
    usage(stderr);
    return 2;
  }
  if (!o.root) o.root = own_root = default_root();
  if (!o.root || os_mkdir_p(o.root) != CORE_OK) {
    fprintf(stderr, "asterism setup: cannot use %s\n", o.root ? o.root : "(no HOME)");
    free(own_root);
    return 1;
  }
  if (o.remove_service) {
    bool ok = setup_service_remove(o.root);
    printf(ok ? "service removed; %s keeps your data\n" : "cannot remove the service\n",
           o.root);
    free(own_root);
    return ok ? 0 : 1;
  }
  if (o.manifest_path && os_read_file(o.manifest_path, &manifest, NULL) != CORE_OK) {
    fprintf(stderr, "asterism setup: cannot read %s\n", o.manifest_path);
    free(own_root);
    return 1;
  }
  memset(&r, 0, sizeof r);
  if (!setup_self_path(exe, sizeof exe, argv[0])) snprintf(exe, sizeof exe, "%s", argv[0]);
  printf("⁂ asterism %s — setting up %s\n", asngn_version(), o.root);

  step("1. This machine");
  hw = detect();
  if (hw.gpu_build < 0) {
    char *path = at_root(&o, "config.xcdn");
    printf("  this build has no embedded llama.cpp: point config.xcdn at a model\n"
           "  server (see examples/lmstudio.xcdn)\n");
    r.models_skipped = true;
    config_exists = path && os_file_exists(path);
    free(path);
  } else {
    /* the tier configurations name the embedded manifest's files */
    r.tier = choose_tier(&o, &hw, (const char *)setup_manifest, &config_exists, &current);
  }
  if (r.tier && config_exists && o.tier && (!current || strcmp(current, o.tier))) {
    char question[160];
    snprintf(question, sizeof question,
             "Switch to tier %s? (config.xcdn is kept as config.xcdn.bak)", o.tier);
    if (ask_yes(&o, question, true)) {
      char *path = at_root(&o, "config.xcdn"), *bak = at_root(&o, "config.xcdn.bak");
      config_exists = !(path && bak && os_rename(path, bak) == CORE_OK);
      if (!config_exists) printf("  old config.xcdn saved as config.xcdn.bak\n");
      free(path);
      free(bak);
    } else {
      r.tier = current;
    }
  }
  r.config_ok = config_exists || (r.tier && write_config(&o, r.tier));
  if (config_exists) printf("  config.xcdn: kept\n");
  r.tools_ok = install_tools(&o);
  r.tools_ok = tool_grants(&o) && r.tools_ok;

  step("2. Telegram");
  if (o.no_telegram) {
    r.telegram_skipped = true;
    printf("  skipped\n");
  } else if ((bot = telegram_token(&o, &r, api_base, sizeof api_base)) != NULL) {
    /* one reader of the bot at a time: the service waits for pairing */
    if (setup_service_active(o.root)) stopped = setup_service_stop(o.root);
    r.paired = telegram_pair(&o, bot, &r);
    aslink_close(bot);
  }

  if (o.interactive) {
    step("3. Subscriptions (optional)");
    subscriptions(&o);
  }

  step(o.interactive ? "4. Downloads" : "3. Downloads");
  if (o.no_models || !r.tier) {
    r.models_skipped = true;
    printf("  %s\n", o.no_models ? "skipped (--no-models)" : "no tier: nothing to fetch");
  } else {
    setup_model *m = NULL;
    size_t n = 0;
    if (!setup_manifest_parse(manifest ? manifest : (const char *)setup_manifest, r.tier, &m,
                              &n) || n == 0)
      printf("  the model manifest has no files for tier %s\n", r.tier);
    else r.models_ok = setup_models_install(o.root, m, n);
    free(m);
    unused_models(&o, manifest ? manifest : (const char *)setup_manifest, r.tier);
  }

  step(o.interactive ? "5. Starting and stopping" : "4. Starting and stopping");
  if (o.no_service) {
    r.service_skipped = true;
    snprintf(r.service_note, sizeof r.service_note, "skipped; start it with: asterism serve");
  } else if (setup_service_kind_here() == SETUP_SERVICE_NONE) {
    r.service_skipped = true;
    snprintf(r.service_note, sizeof r.service_note,
             "no systemd user session; start it with: asterism serve");
  } else if (!r.models_ok && !r.models_skipped) {
    r.service_skipped = true;
    snprintf(r.service_note, sizeof r.service_note, "waits for the models");
  } else {
    char hello[1200];
    /* the default keeps what this root already has */
    r.autostart = setup_service_ready(o.root) ? setup_service_autostart(o.root) : true;
    if (o.manual) r.autostart = false;
    else if (o.autostart) r.autostart = true;
    else r.autostart = ask_yes(&o, "Start the agent by itself when you log in? "
                                   "(no: only when you run asterism start)", r.autostart);
    snprintf(hello, sizeof hello, "%s/serve", o.root);
    (void)os_mkdir_p(hello);
    snprintf(hello, sizeof hello, "%s/serve/hello", o.root);
    if (r.paired) (void)os_write_file(hello, r.tier ? r.tier : "", r.tier ? strlen(r.tier) : 0);
    r.service_ok = setup_service_install(exe, o.root, r.autostart, o.interactive);
    snprintf(r.service_note, sizeof r.service_note, "%s",
             !r.service_ok  ? "see above"
             : r.autostart ? "starts when you log in (asterism stop / asterism start)"
                           : "start it with asterism start, stop it with asterism stop");
    if (r.service_ok && r.autostart) printf("  running; it also starts when you log in\n");
    else if (r.service_ok) printf("  ready: asterism start runs it, asterism stop ends it\n");
    else printf("  the agent could not be set up to run in the background\n");
  }

  if (r.service_skipped) printf("  %s\n", r.service_note);
  if (stopped && !r.service_ok) (void)setup_service_start(exe, o.root);
  summary(&o, &r);
  free(manifest);
  free(own_root);
  return r.config_ok && (r.models_ok || r.models_skipped) ? 0 : 1;
}
