/*
 * login.c — "asterism login chatgpt" / "asterism logout chatgpt".
 *
 * Sign in with ChatGPT lets this open-source app spend the user's own
 * ChatGPT plan. The browser flow redirects to a transient listener on
 * 127.0.0.1; on a headless machine the user opens the URL on any device and
 * pastes back the address the browser ends on. Tokens go to the pool entry's
 * auth_file (owner-only) and are never printed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/select.h>
#include <unistd.h>
#endif

#include "asmodel.h"
#include "asvault.h"
#include "modes.h"
#include "serve.h"

static char *default_root(void) {
  const char *home = getenv("USERPROFILE");
  if (home == NULL || home[0] == '\0') home = getenv("HOME");
  return home && *home ? os_path_join(home, "asterism") : NULL;
}

/* Waits up to ms for a line on stdin. 1 = line read, 0 = none, -1 = EOF. */
static int read_line(char *buf, size_t n, int ms) {
#if defined(_WIN32)
  (void)ms;
  return fgets(buf, (int)n, stdin) ? 1 : -1;
#else
  fd_set in;
  struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
  FD_ZERO(&in);
  FD_SET(0, &in);
  if (select(1, &in, NULL, NULL, &tv) <= 0) return 0;
  if (!fgets(buf, (int)n, stdin)) return -1;
  core_str_trim(buf);
  return 1;
#endif
}

static void try_browser(const char *url) {
#if defined(__APPLE__)
  const char *opener = "open";
#elif defined(_WIN32)
  const char *opener = NULL;
#else
  const char *opener = getenv("DISPLAY") || getenv("WAYLAND_DISPLAY") ? "xdg-open" : NULL;
#endif
  if (!opener || getenv("SSH_CONNECTION")) return;
#if !defined(_WIN32)
  pid_t pid = fork();
  if (pid == 0) {
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);
    execlp(opener, opener, url, (char *)NULL);
    _exit(127);
  }
#else
  (void)url;
#endif
}

int asngn_login_main(int argc, char **argv) {
  const char *root = NULL, *auth = NULL, *what = argc > 1 ? argv[1] : "";
  char *own_root = NULL, *own_auth = NULL;
  bool logout = !strcmp(argv[0], "logout"), fresh = false;
  int port = 0, status = 1;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
    else if (!strcmp(argv[i], "--auth-file") && i + 1 < argc) auth = argv[++i];
    else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--new")) fresh = true;
    else what = "";
  }
  if (strcmp(what, "chatgpt")) {
    fprintf(stderr, "usage: asterism %s chatgpt [--root <dir>] [--auth-file <file>]%s\n",
            logout ? "logout" : "login", logout ? "" : " [--port <n>] [--new]");
    return 2;
  }
  if (!root) root = own_root = default_root();
  if (!auth) auth = own_auth = root ? os_path_join(root, "auth/chatgpt.json") : NULL;
  if (!auth) {
    fputs("asterism login: no --root and no HOME\n", stderr);
    return 2;
  }
  char dir[1200], err[512] = "";
  snprintf(dir, sizeof dir, "%s", auth);
  char *slash = strrchr(dir, '/');
  if (slash) {
    *slash = 0;
    (void)os_mkdir_p(dir);
  }
  if (logout) {
    int revoked = 0;
    asmodel_err e = asmodel_chatgpt_logout(auth, &revoked, err, sizeof err);
    if (e == ASMODEL_OK) printf("Signed out%s.\n", revoked ? " and revoked the token" : "");
    else fprintf(stderr, "asterism logout chatgpt: %s\n", err);
    status = e == ASMODEL_OK ? 0 : 1;
    goto out;
  }
  asmodel_chatgpt_login_params p;
  asmodel_chatgpt_login *s = NULL;
  memset(&p, 0, sizeof p);
  p.auth_file = auth;
  p.redirect_port = port;
  p.new_registration = fresh;
  if (asmodel_chatgpt_login_begin(&p, &s, err, sizeof err) != ASMODEL_OK) {
    fprintf(stderr, "asterism login chatgpt: %s\n", err);
    goto out;
  }
  printf("Continue with ChatGPT: open this address and allow plan usage.\n\n  %s\n\n",
         asmodel_chatgpt_login_url(s));
  printf("On a machine without a browser, open it on any device. After you approve,\n"
         "the browser ends on a page at %s that does not load: copy its full\n"
         "address and paste it here, then press Enter.\n",
         asmodel_chatgpt_login_redirect_uri(s));
  fflush(stdout);
  try_browser(asmodel_chatgpt_login_url(s));
  asmodel_err e = ASMODEL_ERR_TIMEOUT;
  for (int64_t start = os_monotonic_ms(); os_monotonic_ms() - start < 600000;) {
    char line[4096];
    e = asmodel_chatgpt_login_wait(s, 250, err, sizeof err);
    if (e != ASMODEL_ERR_TIMEOUT) break;
    int got = read_line(line, sizeof line, 0);
    if (got < 0) continue;
    if (got == 0 || !*line) continue;
    e = asmodel_chatgpt_login_complete(s, line, err, sizeof err);
    memset(line, 0, sizeof line);
    if (e == ASMODEL_ERR_INVALID) {
      fprintf(stderr, "%s: paste the complete address again.\n", err);
      e = ASMODEL_ERR_TIMEOUT;
      continue;
    }
    break;
  }
  asmodel_chatgpt_login_free(s);
  if (e == ASMODEL_OK) {
    asmodel_chatgpt_status st;
    memset(&st, 0, sizeof st);
    (void)asmodel_chatgpt_status_read(auth, &st);
    printf("Signed in%s%s%s%s. Using your ChatGPT plan; manage usage at %s\n",
           st.email[0] ? " as " : "", st.email, st.plan[0] ? ", plan " : "", st.plan,
           ASMODEL_CHATGPT_USAGE_URL);
    char *models = NULL;
    if (asmodel_chatgpt_models(auth, NULL, &models, err, sizeof err) == ASMODEL_OK && models) {
      printf("Models for your plan (use one as `model:` in examples/chatgpt.xcdn):\n%s\n", models);
      free(models);
    }
    status = 0;
  } else if (e == ASMODEL_ERR_UNSUPPORTED) {
    fprintf(stderr, "asterism login chatgpt: plan usage was not granted (%s). Run it again "
                    "and allow it.\n", err);
  } else {
    fprintf(stderr, "asterism login chatgpt: %s\n", err[0] ? err : asmodel_err_name(e));
  }
out:
  free(own_root);
  free(own_auth);
  return status;
}

int asngn_vault_main(int argc, char **argv) {
  const char *root = NULL;
  char *own_root = NULL, *dir;
  int status;
  /* --root may come anywhere; the rest belongs to the vault CLI. */
  char **rest = calloc((size_t)argc + 1, sizeof *rest);
  int n = 0;
  if (!rest) return 1;
  for (int i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
    else rest[n++] = argv[i];
  }
  if (!root) root = own_root = default_root();
  dir = root ? os_path_join(root, "vault") : NULL;
  status = dir ? asvault_cli_main(n, rest, dir) : 2;
  free(dir);
  free(own_root);
  free(rest);
  return status;
}
