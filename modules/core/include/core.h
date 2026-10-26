/*
 * core.h — platform layer and utilities shared by every module.
 *
 * Buffers, strings, hashing, UUIDs, time, the log sink and the OS shim
 * (threads, files, clocks). No dependency beyond the C library and the
 * platform API. All paths are UTF-8; the Win32 backend converts to UTF-16.
 *
 * Fallible calls return core_err. Each module maps it to its own error
 * codes in one place (see the module's os.h).
 *
 * With ASTERISM_NO_THREADS the locking primitives compile to no-ops and
 * os_thread_start returns CORE_ERR_INVALID.
 */

#ifndef CORE_H
#define CORE_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  CORE_OK = 0,
  CORE_ERR_IO,
  CORE_ERR_NOT_FOUND,
  CORE_ERR_INVALID,
  CORE_ERR_NOMEM,
  CORE_ERR_BUSY
} core_err;

const char *core_err_name(core_err e);

/* ── growable buffer ──────────────────────────────────────────────────── */

/* data stays NULL until the first append; every successful mutation leaves
 * it NUL-terminated. */
typedef struct {
  char  *data;
  size_t len;
  size_t cap;
} core_buf;

void     core_buf_init(core_buf *b);
void     core_buf_free(core_buf *b);
core_err core_buf_append(core_buf *b, const void *data, size_t len);
core_err core_buf_appends(core_buf *b, const char *s);
core_err core_buf_appendc(core_buf *b, char ch);
core_err core_buf_printf(core_buf *b, const char *fmt, ...);
core_err core_buf_vprintf(core_buf *b, const char *fmt, va_list ap);
/* Hand over data (malloc'd, "" when empty) and reset the buffer. */
char    *core_buf_detach(core_buf *b);

/* ── strings and encodings ────────────────────────────────────────────── */

char    *core_strdup(const char *s);
char    *core_strndup(const char *s, size_t n);
int      core_strcasecmp(const char *a, const char *b); /* ASCII */
bool     core_str_has_prefix(const char *s, const char *prefix);
char    *core_str_trim(char *s);       /* ASCII whitespace, in place */
bool     core_str_blank(const char *s); /* NULL or only ASCII whitespace */
/* Slug: [a-z0-9][a-z0-9-]{0,63}. */
bool     core_slug_valid(const char *s);
/* Strict UTF-8: no overlongs, surrogates or code points above U+10FFFF. */
bool     core_utf8_valid(const char *s, size_t len);
/* Code points of a NUL-terminated string; false when it is not UTF-8. */
bool     core_utf8_count(const char *s, size_t *out_chars);
uint64_t core_fnv1a64(const void *data, size_t len);
/* RFC 4648, strict: canonical padding and trailing bits. */
char    *core_base64_encode(const uint8_t *data, size_t len);
bool     core_base64_decode(const char *s, uint8_t **out, size_t *out_len);
/* UTF-8 bytes / 4, at least 1 for non-empty text, 0 for empty text. */
int      core_token_heuristic(const char *text);

/* ── SHA-256 ──────────────────────────────────────────────────────────── */

typedef struct {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t  buffer[64];
  size_t   buflen;
} core_sha256_ctx;

void     core_sha256_init(core_sha256_ctx *ctx);
void     core_sha256_update(core_sha256_ctx *ctx, const void *data, size_t len);
void     core_sha256_final(core_sha256_ctx *ctx, uint8_t out[32]);
void     core_sha256(const void *data, size_t len, uint8_t out[32]);
core_err core_sha256_file(const char *path, uint8_t out[32]);
/* Lowercase hex of the first n bytes (n <= 32); out needs 2n+1. */
void     core_sha256_hex(const uint8_t hash[32], size_t n, char *out);

/* ── UUID ─────────────────────────────────────────────────────────────── */

/* Random (v4), lowercase, 36 chars + NUL. */
void core_uuid_v4(char out[37]);
bool core_uuid_valid(const char *s);
bool core_uuid_to_bytes(const char *s, uint8_t out[16]);
void core_uuid_from_bytes(const uint8_t in[16], char out[37]);

/* ── time ─────────────────────────────────────────────────────────────── */

typedef long long core_time; /* unix seconds, UTC */

/* Injectable clock; NULL callbacks fall back to the system clocks. */
typedef struct {
  void      *ud;
  core_time (*now)(void *ud);     /* wall clock, unix seconds UTC */
  int64_t   (*mono_ms)(void *ud); /* monotonic milliseconds       */
} core_clock;

core_clock core_clock_system(void);
core_time  core_clock_now(const core_clock *clk);
int64_t    core_clock_mono_ms(const core_clock *clk);

/* "YYYY-MM-DDTHH:MM:SSZ"; years are clamped to 0000..9999. */
#define CORE_RFC3339_SIZE 21
void core_time_format_rfc3339(core_time t, char out[CORE_RFC3339_SIZE]);
bool core_time_parse_rfc3339(const char *s, core_time *out);
/* ISO 8601 duration (PnW | PnD | PTnHnMnS mixes; months, years and
 * fractions rejected) in seconds; must be positive. */
bool core_duration_parse(const char *s, int64_t *out_seconds);

/* ── OS: threads ──────────────────────────────────────────────────────── */

#if defined(ASTERISM_NO_THREADS)
typedef struct { int unused; } os_mutex;
typedef struct { int unused; } os_cond;
typedef struct { int unused; } os_rwlock;
typedef struct { int unused; } os_thread;
#elif defined(_WIN32)
/* Storage for SRWLOCK / CONDITION_VARIABLE / HANDLE without leaking
 * <windows.h>; checked with static asserts in os_win32.c. */
typedef struct { void *h; } os_mutex;  /* SRWLOCK used exclusively */
typedef struct { void *h; } os_cond;   /* CONDITION_VARIABLE       */
typedef struct { void *h; } os_rwlock; /* SRWLOCK                  */
typedef struct { void *h; } os_thread; /* HANDLE                   */
#else
#include <pthread.h>
typedef struct { pthread_mutex_t m; } os_mutex;
typedef struct { pthread_cond_t c; } os_cond;
typedef struct { pthread_rwlock_t l; } os_rwlock;
typedef struct { pthread_t t; int valid; } os_thread;
#endif

core_err os_thread_start(os_thread *t, void *(*fn)(void *), void *arg);
void     os_thread_join(os_thread *t);

void os_mutex_init(os_mutex *m);
void os_mutex_destroy(os_mutex *m);
void os_mutex_lock(os_mutex *m);
void os_mutex_unlock(os_mutex *m);
int  os_mutex_trylock(os_mutex *m); /* 1 = acquired */

void os_cond_init(os_cond *c);
void os_cond_destroy(os_cond *c);
/* Returns 1 if signaled, 0 on timeout. */
int  os_cond_timedwait(os_cond *c, os_mutex *m, int64_t timeout_ms);
void os_cond_wait(os_cond *c, os_mutex *m);
void os_cond_signal(os_cond *c);
void os_cond_broadcast(os_cond *c);

void os_rwlock_init(os_rwlock *l);
void os_rwlock_destroy(os_rwlock *l);
void os_rwlock_rdlock(os_rwlock *l);
void os_rwlock_rdunlock(os_rwlock *l);
void os_rwlock_wrlock(os_rwlock *l);
void os_rwlock_wrunlock(os_rwlock *l);

/* ── OS: files ────────────────────────────────────────────────────────── */

/* Atomically replace dst with src; src is consumed. Parent directories are
 * synced where the platform supports it. */
core_err os_file_replace(const char *src, const char *dst);
/* Flush stdio and sync the open file, including read streams. */
core_err os_fsync(FILE *f);
/* Sync the directory holding path (POSIX); a no-op on Windows. */
core_err os_sync_parent(const char *path);
/* Exclusive single-writer lock file; fclose releases it. Keep it linked. */
FILE    *os_store_lock(const char *path);
/* Create path and missing parents; an existing directory is fine. */
core_err os_mkdir_p(const char *path);
int      os_file_exists(const char *path); /* 1 = yes, any type */
/* Whole file, NUL-terminated, free() it; out_len may be NULL. */
core_err os_read_file(const char *path, char **out, size_t *out_len);
/* Write a whole buffer (truncate). Pair with os_file_replace for atomicity. */
core_err os_write_file(const char *path, const void *data, size_t len);
core_err os_remove_file(const char *path);
core_err os_remove_dir(const char *path); /* empty directory only */
core_err os_truncate(const char *path, uint64_t size);
core_err os_file_size(const char *path, uint64_t *out); /* NOT_FOUND if missing */
/* Rename, replacing the target if present. */
core_err os_rename(const char *src, const char *dst);
/* Entry names, malloc'd array of malloc'd strings, unsorted; free with
 * os_free_names. A missing directory yields 0 entries. os_list_dir lists
 * regular files and symbolic links, never followed (POSIX) or skipped
 * reparse points (Windows); os_list_dirs lists real subdirectories. */
core_err os_list_dir(const char *path, char ***out_names, size_t *out_n);
core_err os_list_dirs(const char *path, char ***out_names, size_t *out_n);
void     os_free_names(char **names, size_t n);
FILE    *os_fopen(const char *path, const char *mode);
/* Canonical absolute path of an existing entry, malloc'd. */
core_err os_realpath(const char *path, char **out);

/* ── OS: misc ─────────────────────────────────────────────────────────── */

int64_t os_now_unix(void);      /* wall clock, unix seconds UTC */
int64_t os_monotonic_ms(void);  /* monotonic milliseconds */
void    os_sleep_ms(int ms);    /* real sleep, also without threads */
void    os_random_bytes(void *buf, size_t n); /* CSPRNG, best effort fallback */
int     os_hardware_threads(void); /* >= 1 */
/* Join with '/' (accepted by the Windows APIs used here); malloc'd. */
char   *os_path_join(const char *a, const char *b);
int     os_path_is_abs(const char *path);

/* ── log sink ─────────────────────────────────────────────────────────── */

enum { CORE_LOG_ERROR = 0, CORE_LOG_WARN, CORE_LOG_INFO, CORE_LOG_DEBUG };

/* Receives every record at any level as one formatted line
 * ("<RFC3339> LEVEL SUBSYS  message", no newline). It may re-enter the log. */
typedef void (*core_log_fn)(int level, const char *line, void *userdata);

/* A rotating file sink gated by level plus a callback sink. Every failure
 * is silent: logging never fails its caller. */
typedef struct {
  os_mutex    mu;
  FILE       *fp;
  uint64_t    size;
  char       *path;
  int         level;
  int         max_size_kb; /* 0 = never rotate */
  int         max_files;
  bool        sync;
  core_log_fn fn;
  void       *ud;
} core_log;

void     core_log_init(core_log *log);
void     core_log_destroy(core_log *log);
/* Open the file sink; path NULL or "" disables it. */
core_err core_log_open(core_log *log, const char *path, int level,
                       int max_size_kb, int max_files, bool sync);
void     core_log_close(core_log *log);
void     core_log_set_callback(core_log *log, core_log_fn fn, void *userdata);
void     core_log_vwrite(core_log *log, core_time now, int level,
                         const char *subsys, const char *fmt, va_list ap);

#ifdef __cplusplus
}
#endif

#endif /* CORE_H */
