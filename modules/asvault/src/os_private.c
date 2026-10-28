/*
 * os_private.c — owner-only files and no-echo terminal input (os.h).
 *
 * POSIX: files are created with O_CREAT | O_EXCL | O_NOFOLLOW and mode 0600
 * (fchmod again, whatever the umask), written with write(2) and synced; the
 * vault directory is chmod 0700. Key material is read with read(2), so no
 * stdio buffer keeps a copy. Echo is turned off with termios; SIGINT,
 * SIGTERM, SIGHUP and SIGQUIT restore the terminal before the default
 * action runs.
 *
 * Windows: files rely on the inherited profile ACL; reads are unbuffered
 * stdio and echo is turned off with SetConsoleMode.
 */

#include "asvault_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif

/* ---- files -------------------------------------------------------------- */

asvault_err os_private_dir(const char *path) {
  asvault_err e = os_mkdir_p(path);
  if (e != ASVAULT_OK) return e;
#if !defined(_WIN32)
  if (chmod(path, 0700) != 0) return ASVAULT_ERR_IO;
#endif
  return ASVAULT_OK;
}

#if !defined(_WIN32)

static asvault_err write_all(int fd, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  while (len > 0) {
    ssize_t n = write(fd, p, len);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return ASVAULT_ERR_IO;
    p += n;
    len -= (size_t)n;
  }
  return ASVAULT_OK;
}

static asvault_err create_excl(const char *path, const void *data,
                               size_t len) {
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                0600);
  asvault_err e;
  if (fd < 0) return ASVAULT_ERR_IO;
  e = fchmod(fd, 0600) == 0 ? ASVAULT_OK : ASVAULT_ERR_IO;
  if (e == ASVAULT_OK) e = write_all(fd, data, len);
  if (e == ASVAULT_OK && fsync(fd) != 0) e = ASVAULT_ERR_IO;
  if (close(fd) != 0 && e == ASVAULT_OK) e = ASVAULT_ERR_IO;
  if (e != ASVAULT_OK) (void)unlink(path);
  return e;
}

static int remove_stale(const char *path) {
  return unlink(path) == 0 || errno == ENOENT;
}

#else /* _WIN32 */

static asvault_err create_excl(const char *path, const void *data,
                               size_t len) {
  FILE *f = os_fopen(path, "wbx");
  asvault_err e = ASVAULT_OK;
  if (!f) return ASVAULT_ERR_IO;
  setvbuf(f, NULL, _IONBF, 0);
  if (len > 0 && fwrite(data, 1, len, f) != len) e = ASVAULT_ERR_IO;
  if (e == ASVAULT_OK && os_fsync(f) != CORE_OK) e = ASVAULT_ERR_IO;
  if (fclose(f) != 0 && e == ASVAULT_OK) e = ASVAULT_ERR_IO;
  if (e != ASVAULT_OK) (void)os_remove_file(path);
  return e;
}

static int remove_stale(const char *path) {
  asvault_err e = os_remove_file(path);
  return e == ASVAULT_OK || e == ASVAULT_ERR_NOT_FOUND;
}

#endif

asvault_err os_private_create(const char *path, const void *data, size_t len) {
  asvault_err e;
  if (!path || (!data && len)) return ASVAULT_ERR_INVALID;
  e = create_excl(path, data, len);
  if (e == ASVAULT_OK) e = os_sync_parent(path);
  return e;
}

asvault_err os_private_replace(const char *path, const void *data, size_t len) {
  size_t plen;
  char *tmp;
  asvault_err e;

  if (!path || (!data && len)) return ASVAULT_ERR_INVALID;
  plen = strlen(path);
  tmp = (char *)malloc(plen + sizeof ".tmp");
  if (!tmp) return ASVAULT_ERR_NOMEM;
  memcpy(tmp, path, plen);
  memcpy(tmp + plen, ".tmp", sizeof ".tmp");
  /* the writer lock is held: a leftover temp file is from a crash */
  e = remove_stale(tmp) ? create_excl(tmp, data, len) : ASVAULT_ERR_IO;
  if (e == ASVAULT_OK) {
    e = os_file_replace(tmp, path);
    if (e != ASVAULT_OK) (void)remove_stale(tmp);
  }
  free(tmp);
  return e;
}

asvault_err os_private_read(const char *path, void *buf, size_t cap,
                            size_t *out_len) {
  uint8_t *p = (uint8_t *)buf;
  size_t got = 0;
  uint8_t extra;
#if !defined(_WIN32)
  struct stat st;
  int fd;

  *out_len = 0;
  fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return errno == ENOENT ? ASVAULT_ERR_NOT_FOUND : ASVAULT_ERR_IO;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    return ASVAULT_ERR_INVALID;
  }
  while (got < cap) {
    ssize_t n = read(fd, p + got, cap - got);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      close(fd);
      return ASVAULT_ERR_IO;
    }
    if (n == 0) break;
    got += (size_t)n;
  }
  if (got == cap) {
    ssize_t n;
    do {
      n = read(fd, &extra, 1);
    } while (n < 0 && errno == EINTR);
    if (n != 0) {
      close(fd);
      return n > 0 ? ASVAULT_ERR_INVALID : ASVAULT_ERR_IO;
    }
  }
  close(fd);
#else
  FILE *f;
  *out_len = 0;
  if (!os_file_exists(path)) return ASVAULT_ERR_NOT_FOUND;
  f = os_fopen(path, "rb");
  if (!f) return ASVAULT_ERR_IO;
  setvbuf(f, NULL, _IONBF, 0);
  got = fread(p, 1, cap, f);
  if (got == cap && fread(&extra, 1, 1, f) == 1) {
    fclose(f);
    return ASVAULT_ERR_INVALID;
  }
  if (ferror(f)) {
    fclose(f);
    return ASVAULT_ERR_IO;
  }
  fclose(f);
#endif
  *out_len = got;
  return ASVAULT_OK;
}

int os_private_mode(const char *path, unsigned *out_mode) {
#if !defined(_WIN32)
  struct stat st;
  if (stat(path, &st) != 0) return 0;
  *out_mode = (unsigned)(st.st_mode & 07777);
  return 1;
#else
  (void)path;
  (void)out_mode;
  return 0;
#endif
}

/* ---- terminal ----------------------------------------------------------- */

#if !defined(_WIN32)

static struct termios g_saved_tty;
static volatile sig_atomic_t g_tty_saved = 0;
static const int g_signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};
static struct sigaction g_old_actions[sizeof g_signals / sizeof g_signals[0]];

static void tty_restore_signal(int sig) {
  if (g_tty_saved) (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_tty);
  /* SA_RESETHAND restored the default action; it runs on return */
  (void)raise(sig);
}

static int tty_echo_off(void) {
  struct termios t;
  struct sigaction sa;
  size_t i;
  if (tcgetattr(STDIN_FILENO, &g_saved_tty) != 0) return 0;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = tty_restore_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESETHAND;
  g_tty_saved = 1;
  for (i = 0; i < sizeof g_signals / sizeof g_signals[0]; i++)
    (void)sigaction(g_signals[i], &sa, &g_old_actions[i]);
  t = g_saved_tty;
  t.c_lflag &= (tcflag_t)~(ECHO | ECHONL);
  t.c_lflag |= ICANON;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) != 0) {
    for (i = 0; i < sizeof g_signals / sizeof g_signals[0]; i++)
      (void)sigaction(g_signals[i], &g_old_actions[i], NULL);
    g_tty_saved = 0;
    return 0;
  }
  return 1;
}

static void tty_echo_restore(void) {
  size_t i;
  (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_tty);
  g_tty_saved = 0;
  for (i = 0; i < sizeof g_signals / sizeof g_signals[0]; i++)
    (void)sigaction(g_signals[i], &g_old_actions[i], NULL);
  asvault_wipe(&g_saved_tty, sizeof g_saved_tty);
}

int os_stdin_is_tty(void) { return isatty(STDIN_FILENO); }

static long read_stdin(void *buf, size_t n) {
  ssize_t got;
  do {
    got = read(STDIN_FILENO, buf, n);
  } while (got < 0 && errno == EINTR);
  return (long)got;
}

#else /* _WIN32 */

static DWORD g_old_console_mode;

static int tty_echo_off(void) {
  HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
  if (h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &g_old_console_mode))
    return 0;
  return SetConsoleMode(h, g_old_console_mode & ~(DWORD)ENABLE_ECHO_INPUT)
             ? 1
             : 0;
}

static void tty_echo_restore(void) {
  HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
  if (h != INVALID_HANDLE_VALUE) SetConsoleMode(h, g_old_console_mode);
}

int os_stdin_is_tty(void) { return _isatty(_fileno(stdin)); }

static long read_stdin(void *buf, size_t n) {
  return (long)_read(_fileno(stdin), buf, (unsigned)(n > 65536 ? 65536 : n));
}

#endif

asvault_err os_read_stdin(char *buf, size_t cap, size_t *out_len,
                          const char *prompt) {
  size_t got = 0;
  int echo_off = 0, line = prompt != NULL;
  asvault_err e = ASVAULT_OK;

  *out_len = 0;
  if (!buf || cap < 2) return ASVAULT_ERR_INVALID;
  if (prompt) {
    /* echo goes off before the prompt shows, so nothing typed after it
     * is echoed (or flushed away) */
    if (os_stdin_is_tty()) echo_off = tty_echo_off();
    fputs(prompt, stderr);
    fflush(stderr);
  }
  for (;;) {
    long n;
    if (got == cap - 1) { /* one more byte means too long */
      char extra;
      n = read_stdin(&extra, 1);
      if (n > 0 && !(line && extra == '\n')) e = ASVAULT_ERR_INVALID;
      if (n < 0) e = ASVAULT_ERR_IO;
      asvault_wipe(&extra, 1);
      break;
    }
    /* a line is read byte by byte so nothing past it is consumed */
    n = read_stdin(buf + got, line ? 1 : cap - 1 - got);
    if (n < 0) {
      e = ASVAULT_ERR_IO;
      break;
    }
    if (n == 0) break;
    got += (size_t)n;
    if (line && buf[got - 1] == '\n') break;
  }
  if (echo_off) tty_echo_restore();
  if (prompt) {
    fputc('\n', stderr);
    fflush(stderr);
  }
  buf[got] = '\0';
  *out_len = got;
  return e;
}
