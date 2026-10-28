/*
 * setup_service.c — keep "asterism serve" running for the user, without
 * administrator rights:
 *
 *   Linux    a systemd user unit, enabled and started; lingering keeps it
 *            up after logout when loginctl allows it
 *   macOS    a LaunchAgent, bootstrapped into the GUI domain
 *   Windows  an HKCU "Run" entry for "serve --background", started now;
 *            the daemon writes <root>/serve/serve.pid so setup can stop it
 *
 * Each one can instead be started only by hand ("asterism start"), for
 * machines where the agent should not hold memory all the time.
 *
 * Paths are quoted for the format that reads them; the bot token is never
 * part of the service (it lives in the vault).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "core.h"
#include "setup.h"

#define SERVICE_LABEL "ai.asterism.serve"

/* ── processes ───────────────────────────────────────────────────────── */

#if defined(_WIN32)

static wchar_t *wide(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
  if (w && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
    free(w);
    w = NULL;
  }
  return w;
}

/* One argument quoted as CommandLineToArgvW reads it back. */
static void quote_arg(core_buf *b, const char *a) {
  size_t slashes = 0;
  if (*a && !strpbrk(a, " \t\"")) {
    core_buf_appends(b, a);
    return;
  }
  core_buf_appends(b, "\"");
  for (; *a; a++) {
    if (*a == '\\') {
      slashes++;
      continue;
    }
    for (size_t k = 0; k < slashes * (*a == '"' ? 2 : 1); k++) core_buf_appends(b, "\\");
    slashes = 0;
    if (*a == '"') core_buf_appends(b, "\\");
    core_buf_append(b, a, 1);
  }
  for (size_t k = 0; k < slashes * 2; k++) core_buf_appends(b, "\\");
  core_buf_appends(b, "\"");
}

static int spawn(const char *const *argv, bool quiet, bool wait) {
  core_buf cmd;
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  wchar_t *w;
  DWORD code = (DWORD)-1;
  core_buf_init(&cmd);
  for (size_t i = 0; argv[i]; i++) {
    if (i) core_buf_appends(&cmd, " ");
    quote_arg(&cmd, argv[i]);
  }
  w = core_buf_appends(&cmd, "") == CORE_OK ? wide(cmd.data) : NULL;
  core_buf_free(&cmd);
  if (!w) return -1;
  memset(&si, 0, sizeof si);
  si.cb = sizeof si;
  if (!CreateProcessW(NULL, w, NULL, NULL, FALSE,
                      quiet || !wait ? CREATE_NO_WINDOW | (wait ? 0 : DETACHED_PROCESS) : 0,
                      NULL, NULL, &si, &pi)) {
    free(w);
    return -1;
  }
  free(w);
  if (wait) {
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
  } else {
    code = 0;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return (int)code;
}

int setup_run(const char *const *argv, bool quiet) { return spawn(argv, quiet, true); }

#else

int setup_run(const char *const *argv, bool quiet) {
  posix_spawn_file_actions_t fa;
  pid_t pid;
  int status = 0, rc;
  posix_spawn_file_actions_init(&fa);
  if (quiet) {
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  }
  rc = posix_spawnp(&pid, argv[0], &fa, NULL, (char *const *)argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  if (rc != 0) return -1;
  while (waitpid(pid, &status, 0) < 0)
    if (errno != EINTR) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

#endif

bool setup_in_path(const char *program) {
  char full[1200];
  return setup_which(program, full, sizeof full);
}

bool setup_which(const char *program, char *out, size_t n) {
  const char *path = getenv("PATH");
#if defined(_WIN32)
  static const char *const ext[] = {".exe", ".cmd", ".bat", ""};
  const char sep = ';';
#else
  static const char *const ext[] = {""};
  const char sep = ':';
#endif
  while (path && *path) {
    const char *end = strchr(path, sep);
    size_t len = end ? (size_t)(end - path) : strlen(path);
    for (size_t e = 0; len && e < sizeof ext / sizeof ext[0]; e++) {
      char full[1200];
      snprintf(full, sizeof full, "%.*s/%s%s", (int)len, path, program, ext[e]);
#if defined(_WIN32)
      bool found = os_file_exists(full) && !os_is_dir(full);
#else
      bool found = access(full, X_OK) == 0 && !os_is_dir(full);
#endif
      if (found) {
        snprintf(out, n, "%s", full);
        return true;
      }
    }
    path = end ? end + 1 : NULL;
  }
  return false;
}

bool setup_self_path(char *out, size_t n, const char *argv0) {
  char *real = NULL;
#if defined(__linux__)
  ssize_t len = readlink("/proc/self/exe", out, n - 1);
  if (len > 0) {
    out[len] = '\0';
    return true;
  }
#elif defined(__APPLE__)
  char buf[1024];
  uint32_t size = sizeof buf;
  if (_NSGetExecutablePath(buf, &size) == 0 && os_realpath(buf, &real) == CORE_OK) {
    snprintf(out, n, "%s", real);
    free(real);
    return true;
  }
#elif defined(_WIN32)
  wchar_t w[1024];
  DWORD len = GetModuleFileNameW(NULL, w, 1024);
  if (len > 0 && len < 1024 &&
      WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL) > 0)
    return true;
#endif
  if (argv0 && os_realpath(argv0, &real) == CORE_OK) {
    snprintf(out, n, "%s", real);
    free(real);
    return true;
  }
  return false;
}

#if !defined(_WIN32)
/* The installed unit or plist runs this root (a test root never touches
 * the user's real agent). */
static bool service_for(const char *where, const char *root) {
  char *text = NULL;
  bool ours = os_read_file(where, &text, NULL) == CORE_OK && strstr(text, root) != NULL;
  free(text);
  return ours;
}

static const char *home(void) {
  const char *h = getenv("HOME");
  return h && *h ? h : NULL;
}
#endif

/* ── Linux: systemd --user ───────────────────────────────────────────── */
#if !defined(_WIN32) && !defined(__APPLE__)

static void unit_path(char *out, size_t n) {
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) snprintf(out, n, "%s/systemd/user/asterism.service", xdg);
  else snprintf(out, n, "%s/.config/systemd/user/asterism.service", home() ? home() : ".");
}

/* A value for systemd: specifiers escaped, optionally in C-style quotes. */
static void unit_value(core_buf *b, const char *s, bool quoted) {
  if (quoted) core_buf_appends(b, "\"");
  for (; *s; s++) {
    if (*s == '%') core_buf_appends(b, "%%");
    else if (quoted && (*s == '"' || *s == '\\')) {
      core_buf_appends(b, "\\");
      core_buf_append(b, s, 1);
    } else core_buf_append(b, s, 1);
  }
  if (quoted) core_buf_appends(b, "\"");
}

static int systemctl(const char *verb, bool quiet) {
  const char *argv[] = {"systemctl", "--user", verb, "asterism", NULL};
  if (!strcmp(verb, "daemon-reload")) argv[3] = NULL;
  return setup_run(argv, quiet);
}

setup_service_kind setup_service_kind_here(void) {
  const char *argv[] = {"systemctl", "--user", "show-environment", NULL};
  return setup_in_path("systemctl") && setup_run(argv, true) == 0 ? SETUP_SERVICE_SYSTEMD
                                                                  : SETUP_SERVICE_NONE;
}

const char *setup_service_where(void) {
  static char path[1200];
  unit_path(path, sizeof path);
  return path;
}

bool setup_service_installed(void) { return os_file_exists(setup_service_where()) == 1; }

bool setup_service_ready(const char *root) { return service_for(setup_service_where(), root); }

bool setup_service_autostart(const char *root) {
  const char *argv[] = {"systemctl", "--user", "is-enabled", "--quiet", "asterism", NULL};
  return setup_service_ready(root) && setup_run(argv, true) == 0;
}

bool setup_service_active(const char *root) {
  const char *argv[] = {"systemctl", "--user", "is-active", "--quiet", "asterism", NULL};
  return setup_service_ready(root) && setup_run(argv, true) == 0;
}

bool setup_service_start(const char *exe, const char *root) {
  (void)exe;
  return setup_service_ready(root) && systemctl("start", false) == 0;
}

bool setup_service_stop(const char *root) {
  return !setup_service_ready(root) || systemctl("stop", true) == 0;
}

/* Lingering keeps user services running without a login session. */
static void linger(bool interactive) {
  char path[300];
  const char *user = getenv("USER");
  const char *argv[] = {"loginctl", "enable-linger", NULL};
  if (!user || !*user) return;
  snprintf(path, sizeof path, "/var/lib/systemd/linger/%s", user);
  if (os_file_exists(path)) return;
  if (interactive && setup_in_path("loginctl") && setup_run(argv, false) == 0) {
    printf("  lingering on: the agent keeps running after you log out\n");
    return;
  }
  printf("  to keep it running after you log out: loginctl enable-linger %s\n", user);
}

bool setup_service_install(const char *exe, const char *root, bool autostart,
                           bool interactive) {
  char path[1200], dir[1200];
  core_buf b;
  bool ok, was = setup_service_active(root);
  unit_path(path, sizeof path);
  snprintf(dir, sizeof dir, "%s", path);
  *strrchr(dir, '/') = '\0';
  core_buf_init(&b);
  core_buf_appends(&b, "# Written by asterism setup.\n"
                       "[Unit]\n"
                       "Description=asterism personal agent\n"
                       "Wants=network-online.target\n"
                       "After=network-online.target\n\n"
                       "[Service]\n"
                       "EnvironmentFile=-");
  unit_value(&b, root, false);
  core_buf_appends(&b, "/serve.env\nWorkingDirectory=");
  unit_value(&b, root, false);
  core_buf_appends(&b, "\nExecStart=");
  unit_value(&b, exe, true);
  core_buf_appends(&b, " serve --root ");
  unit_value(&b, root, true);
  core_buf_appends(&b, "\nRestart=on-failure\n"
                       "RestartSec=10\n"
                       "NoNewPrivileges=true\n\n"
                       "[Install]\n"
                       "WantedBy=default.target\n");
  ok = os_mkdir_p(dir) == CORE_OK && os_write_file(path, b.data, b.len) == CORE_OK;
  core_buf_free(&b);
  if (!ok) {
    printf("  cannot write %s\n", path);
    return false;
  }
  if (systemctl("daemon-reload", false) != 0) {
    printf("  %s written, but systemctl daemon-reload failed\n", path);
    return false;
  }
  if (!autostart) {
    (void)systemctl("disable", true);
    return !was || systemctl("restart", false) == 0;
  }
  if (systemctl("enable", true) != 0 || systemctl("restart", false) != 0) {
    printf("  %s written, but systemctl failed: systemctl --user enable --now asterism\n", path);
    return false;
  }
  linger(interactive);
  return true;
}

bool setup_service_remove(const char *root) {
  if (!setup_service_installed()) return true;
  if (!service_for(setup_service_where(), root)) {
    printf("  the service runs another engine root; left alone\n");
    return true;
  }
  (void)systemctl("stop", true);
  (void)systemctl("disable", true);
  if (os_remove_file(setup_service_where()) != CORE_OK) return false;
  (void)systemctl("daemon-reload", true);
  return true;
}

#endif

/* ── macOS: launchd ──────────────────────────────────────────────────── */
#if defined(__APPLE__)

static void domain(char *out, size_t n, bool with_label) {
  snprintf(out, n, "gui/%u%s", (unsigned)getuid(), with_label ? "/" SERVICE_LABEL : "");
}

static void xml_text(core_buf *b, const char *s) {
  for (; *s; s++) {
    if (*s == '&') core_buf_appends(b, "&amp;");
    else if (*s == '<') core_buf_appends(b, "&lt;");
    else if (*s == '>') core_buf_appends(b, "&gt;");
    else core_buf_append(b, s, 1);
  }
}

setup_service_kind setup_service_kind_here(void) { return SETUP_SERVICE_LAUNCHD; }

const char *setup_service_where(void) {
  static char path[1200];
  snprintf(path, sizeof path, "%s/Library/LaunchAgents/" SERVICE_LABEL ".plist",
           home() ? home() : ".");
  return path;
}

bool setup_service_installed(void) { return os_file_exists(setup_service_where()) == 1; }

bool setup_service_ready(const char *root) { return service_for(setup_service_where(), root); }

bool setup_service_autostart(const char *root) {
  char *text = NULL;
  bool on = setup_service_ready(root) &&
            os_read_file(setup_service_where(), &text, NULL) == CORE_OK &&
            strstr(text, "<key>RunAtLoad</key><true/>") != NULL;
  free(text);
  return on;
}

/* Loaded and running: launchctl print shows a pid. */
bool setup_service_active(const char *root) {
  char target[64], line[256];
  bool running = false;
  FILE *f;
  if (!setup_service_ready(root)) return false;
  domain(target, sizeof target, true);
  snprintf(line, sizeof line, "launchctl print %s 2>/dev/null", target);
  if ((f = popen(line, "r")) == NULL) return false;
  while (fgets(line, sizeof line, f))
    if (strstr(line, "pid = ")) running = true;
  pclose(f);
  return running;
}

bool setup_service_start(const char *exe, const char *root) {
  char target[64];
  const char *load[] = {"launchctl", "bootstrap", target, setup_service_where(), NULL};
  const char *kick[] = {"launchctl", "kickstart", target, NULL};
  (void)exe;
  if (!setup_service_ready(root)) return false;
  domain(target, sizeof target, false);
  (void)setup_run(load, true); /* already loaded is fine */
  domain(target, sizeof target, true);
  return setup_run(kick, false) == 0;
}

bool setup_service_stop(const char *root) {
  char target[64];
  const char *argv[] = {"launchctl", "bootout", target, NULL};
  if (!service_for(setup_service_where(), root)) return true;
  domain(target, sizeof target, true);
  return setup_run(argv, true) == 0;
}

bool setup_service_install(const char *exe, const char *root, bool autostart,
                           bool interactive) {
  char dir[1200], target[64];
  const char *path = setup_service_where();
  const char *argv[] = {"launchctl", "bootstrap", target, path, NULL};
  core_buf b;
  bool ok, was = setup_service_active(root);
  (void)interactive;
  snprintf(dir, sizeof dir, "%s/Library/LaunchAgents", home() ? home() : ".");
  core_buf_init(&b);
  core_buf_appends(&b,
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
      "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      "<plist version=\"1.0\"><dict>\n"
      "  <key>Label</key><string>" SERVICE_LABEL "</string>\n"
      "  <key>ProgramArguments</key><array>\n    <string>");
  xml_text(&b, exe);
  core_buf_appends(&b, "</string><string>serve</string><string>--root</string><string>");
  xml_text(&b, root);
  core_buf_appends(&b, "</string>\n  </array>\n"
                       "  <key>WorkingDirectory</key><string>");
  xml_text(&b, root);
  core_buf_appends(&b, "</string>\n  <key>RunAtLoad</key>");
  core_buf_appends(&b, autostart ? "<true/>\n" : "<false/>\n");
  core_buf_appends(&b, "  <key>KeepAlive</key><dict><key>SuccessfulExit</key><false/></dict>\n"
                       "  <key>StandardErrorPath</key><string>");
  xml_text(&b, root);
  core_buf_appends(&b, "/serve.log</string>\n</dict></plist>\n");
  ok = os_mkdir_p(dir) == CORE_OK && os_write_file(path, b.data, b.len) == CORE_OK;
  core_buf_free(&b);
  if (!ok) {
    printf("  cannot write %s\n", path);
    return false;
  }
  domain(target, sizeof target, true);
  {
    const char *out[] = {"launchctl", "bootout", target, NULL};
    (void)setup_run(out, true);
  }
  domain(target, sizeof target, false);
  if (setup_run(argv, false) != 0) {
    printf("  %s written, but launchctl bootstrap failed\n", path);
    return false;
  }
  return autostart || !was || setup_service_start(exe, root);
}

bool setup_service_remove(const char *root) {
  if (!setup_service_installed()) return true;
  if (!service_for(setup_service_where(), root)) {
    printf("  the service runs another engine root; left alone\n");
    return true;
  }
  (void)setup_service_stop(root);
  return os_remove_file(setup_service_where()) == CORE_OK;
}

#endif

/* ── Windows: HKCU Run ───────────────────────────────────────────────── */
#if defined(_WIN32)

static const wchar_t RUN_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

setup_service_kind setup_service_kind_here(void) { return SETUP_SERVICE_RUNKEY; }

const char *setup_service_where(void) {
  return "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\asterism";
}

bool setup_service_installed(void) {
  HKEY key;
  bool found = false;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
    found = RegQueryValueExW(key, L"asterism", NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
    RegCloseKey(key);
  }
  return found;
}

/* The Run entry exists and runs this root. */
static bool run_entry_ours(const char *root) {
  HKEY key;
  wchar_t value[2048], *wroot = wide(root);
  DWORD size = (DWORD)(sizeof value - sizeof value[0]);
  bool ours = false;
  memset(value, 0, sizeof value);
  if (wroot && RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
    ours = RegQueryValueExW(key, L"asterism", NULL, NULL, (BYTE *)value, &size) == ERROR_SUCCESS &&
           wcsstr(value, wroot) != NULL;
    RegCloseKey(key);
  }
  free(wroot);
  return ours;
}

static void run_entry_delete(void) {
  HKEY key;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
    (void)RegDeleteValueW(key, L"asterism");
    RegCloseKey(key);
  }
}

/* Starting by hand needs nothing installed. */
bool setup_service_ready(const char *root) {
  (void)root;
  return true;
}

bool setup_service_autostart(const char *root) { return run_entry_ours(root); }

/* The daemon started with --background, when its pid file names a live
 * asterism process. */
static HANDLE daemon_process(const char *root, DWORD access) {
  char path[1200], *text = NULL;
  wchar_t image[1024];
  DWORD n = 1024, pid;
  HANDLE h;
  snprintf(path, sizeof path, "%s/serve/serve.pid", root);
  if (os_read_file(path, &text, NULL) != CORE_OK) return NULL;
  pid = (DWORD)strtoul(text, NULL, 10);
  free(text);
  if (!pid) return NULL;
  h = OpenProcess(access | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return NULL;
  if (!QueryFullProcessImageNameW(h, 0, image, &n) || !wcsstr(_wcslwr(image), L"asterism")) {
    CloseHandle(h);
    return NULL;
  }
  return h;
}

bool setup_service_active(const char *root) {
  HANDLE h = daemon_process(root, SYNCHRONIZE);
  bool live = h && WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
  if (h) CloseHandle(h);
  return live;
}

bool setup_service_stop(const char *root) {
  HANDLE h = daemon_process(root, PROCESS_TERMINATE | SYNCHRONIZE);
  bool ok;
  if (!h) return true;
  ok = TerminateProcess(h, 0) && WaitForSingleObject(h, 10000) == WAIT_OBJECT_0;
  CloseHandle(h);
  return ok;
}

bool setup_service_start(const char *exe, const char *root) {
  const char *argv[] = {exe, "serve", "--background", "--root", root, NULL};
  return setup_service_active(root) || spawn(argv, true, false) == 0;
}

bool setup_service_install(const char *exe, const char *root, bool autostart,
                           bool interactive) {
  const char *argv[] = {exe, "serve", "--background", "--root", root, NULL};
  core_buf cmd;
  wchar_t *w;
  HKEY key;
  LONG rc = ERROR_OUTOFMEMORY;
  bool was = setup_service_active(root);
  (void)interactive;
  if (!autostart) {
    if (run_entry_ours(root)) run_entry_delete();
    if (!was) return true;
    (void)setup_service_stop(root);
    return setup_service_start(exe, root);
  }
  core_buf_init(&cmd);
  for (size_t i = 0; argv[i]; i++) {
    if (i) core_buf_appends(&cmd, " ");
    quote_arg(&cmd, argv[i]);
  }
  w = core_buf_appends(&cmd, "") == CORE_OK ? wide(cmd.data) : NULL;
  core_buf_free(&cmd);
  if (w && (rc = RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL,
                                 &key, NULL)) == ERROR_SUCCESS) {
    rc = RegSetValueExW(key, L"asterism", 0, REG_SZ, (const BYTE *)w,
                        (DWORD)((wcslen(w) + 1) * sizeof *w));
    RegCloseKey(key);
  }
  free(w);
  if (rc != ERROR_SUCCESS) {
    printf("  cannot write the Run entry (error %ld)\n", (long)rc);
    return false;
  }
  (void)setup_service_stop(root);
  if (spawn(argv, true, false) != 0) {
    printf("  the Run entry is written, but the agent did not start\n");
    return false;
  }
  return true;
}

bool setup_service_remove(const char *root) {
  (void)setup_service_stop(root);
  if (run_entry_ours(root)) run_entry_delete();
  else if (setup_service_installed())
    printf("  the login entry runs another engine root; left alone\n");
  return true;
}

#endif
