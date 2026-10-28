/*
 * main.c — "asterism serve": the always-on agent daemon (see serve.h).
 *
 *   asterism serve [--root <dir>] [--config <file>] [--serve-config <file>]
 *                  [--workspace <dir>] [--allow-degraded] [--background]
 *   asterism serve pair [--root <dir>]
 *
 * --background is how a login entry starts it on Windows: no console, the
 * log in <root>/serve.log and the process id in <root>/serve/serve.pid.
 *
 * "pair" prints a one-time code (valid ten minutes). Send "/start <code>" to
 * the bot from the Telegram account that will own the agent.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "modes.h"
#include "serve.h"

static char *default_root(void) {
  const char *home = getenv("USERPROFILE");
  if (home == NULL || home[0] == '\0') home = getenv("HOME");
  if (home == NULL || home[0] == '\0') return NULL;
  return os_path_join(home, "asterism");
}

/* Detach from the console; the log and the pid file go under the root. */
static void background(const char *root, char *pid_path, size_t n) {
  char log[1200], pid[32];
  snprintf(log, sizeof log, "%s/serve.log", root);
#if defined(_WIN32)
  wchar_t wlog[1200];
  FreeConsole();
  if (MultiByteToWideChar(CP_UTF8, 0, log, -1, wlog, 1200) > 0) {
    (void)_wfreopen(wlog, L"a", stderr);
    (void)_wfreopen(wlog, L"a", stdout);
  }
  snprintf(pid, sizeof pid, "%lu\n", (unsigned long)GetCurrentProcessId());
#else
  (void)freopen(log, "a", stderr);
  (void)freopen(log, "a", stdout);
  snprintf(pid, sizeof pid, "%ld\n", (long)getpid());
#endif
  setvbuf(stderr, NULL, _IONBF, 0);
  snprintf(pid_path, n, "%s/serve", root);
  (void)os_mkdir_p(pid_path);
  snprintf(pid_path, n, "%s/serve/serve.pid", root);
  if (os_write_file(pid_path, pid, strlen(pid)) != CORE_OK) pid_path[0] = '\0';
}

void asngn_serve_usage(FILE *out) {
  fputs("usage: asterism serve [--root <dir>] [--config <file>] "
        "[--serve-config <file>]\n"
        "                      [--workspace <dir>] [--allow-degraded] [--background]\n"
        "       asterism serve pair [--root <dir>]\n\n"
        "The daemon reads <root>/serve.xcdn and the bot token from the\n"
        "environment variable it names (default ASTERISM_TELEGRAM_TOKEN).\n"
        "\"pair\" prints a one-time code: send /start <code> to the bot.\n",
        out);
}

int asngn_serve_main(int argc, char **argv) {
  serve_params p;
  const char *root = NULL;
  char *own_root = NULL, pid_path[1200] = "";
  bool pair = false, detach = false;
  int i = 1, status;
  memset(&p, 0, sizeof p);
  if (argc > 1 && !strcmp(argv[1], "pair")) {
    pair = true;
    i = 2;
  }
  for (; i < argc; i++) {
    const char *a = argv[i];
    const char **slot = !strcmp(a, "--root")           ? &root
                        : !strcmp(a, "--config")       ? &p.config
                        : !strcmp(a, "--serve-config") ? &p.serve_config
                        : !strcmp(a, "--workspace")    ? &p.workspace
                                                       : NULL;
    if (!strcmp(a, "--allow-degraded")) {
      p.allow_degraded = 1;
      continue;
    }
    if (!strcmp(a, "--background")) {
      detach = true;
      continue;
    }
    if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
      asngn_serve_usage(stdout);
      return 0;
    }
    if (!slot || i + 1 >= argc) {
      asngn_serve_usage(stderr);
      return 2;
    }
    *slot = argv[++i];
  }
  if (!root) root = own_root = default_root();
  if (!root) {
    fputs("asterism serve: no --root and no HOME\n", stderr);
    return 2;
  }
  if (os_mkdir_p(root) != CORE_OK) {
    fprintf(stderr, "asterism serve: cannot create %s\n", root);
    free(own_root);
    return 1;
  }
  if (pair) {
    serve_state st;
    char err[256], code[16];
    status = 1;
    if (serve_state_open(&st, root, err, sizeof err)) {
      if (serve_pairing_create(&st, code, err, sizeof err)) {
        printf("Send this to your bot within 10 minutes:\n\n  /start %s\n\n", code);
        if (st.owner_id)
          printf("Note: this agent is already paired; the new owner replaces it.\n");
        status = 0;
      }
      if (status == 0 && st.owner_id) {
        st.owner_id = 0;
        st.owner_chat = 0;
        (void)serve_state_save(&st);
      }
      serve_state_close(&st);
    }
    if (status) fprintf(stderr, "asterism serve pair: %s\n", err);
    free(own_root);
    return status;
  }
  p.root = root;
  if (detach) background(root, pid_path, sizeof pid_path);
  status = serve_run(&p);
  if (pid_path[0]) (void)os_remove_file(pid_path);
  free(own_root);
  return status;
}
