/*
 * os.h — the platform layer as seen by astools.
 *
 * The shared shim comes from core.h. Its fallible calls are wrapped here
 * so that astools code receives astools_err. The process, path and dynamic
 * library extension below is astools-only (os_proc_posix.c, os_proc_win32.c).
 */

#ifndef ASTOOLS_OS_H
#define ASTOOLS_OS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "astools.h"
#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

astools_err astools_from_core(core_err e);

#define os_thread_start(...) astools_from_core(os_thread_start(__VA_ARGS__))
#define os_file_replace(...) astools_from_core(os_file_replace(__VA_ARGS__))
#define os_fsync(...)        astools_from_core(os_fsync(__VA_ARGS__))
#define os_sync_parent(...)  astools_from_core(os_sync_parent(__VA_ARGS__))
#define os_mkdir_p(...)      astools_from_core(os_mkdir_p(__VA_ARGS__))
#define os_read_file(...)    astools_from_core(os_read_file(__VA_ARGS__))
#define os_write_file(...)   astools_from_core(os_write_file(__VA_ARGS__))
#define os_remove_file(...)  astools_from_core(os_remove_file(__VA_ARGS__))
#define os_remove_dir(...)   astools_from_core(os_remove_dir(__VA_ARGS__))
#define os_truncate(...)     astools_from_core(os_truncate(__VA_ARGS__))
#define os_file_size(...)    astools_from_core(os_file_size(__VA_ARGS__))
#define os_rename(...)       astools_from_core(os_rename(__VA_ARGS__))
#define os_list_dir(...)     astools_from_core(os_list_dir(__VA_ARGS__))
#define os_list_dirs(...)    astools_from_core(os_list_dirs(__VA_ARGS__))
#define os_realpath(...)     astools_from_core(os_realpath(__VA_ARGS__))

/* ═════════════════════ ⁂ astools process/path extension ═════════════════════
 * Implemented by os_proc_posix.c. Everything below returns
 * ASTOOLS_ERR_UNSUPPORTED from stub backends.
 */

/* ---- paths and stat ------------------------------------------------------ */

typedef enum {
  OS_FT_MISSING = 0,
  OS_FT_FILE,
  OS_FT_DIR,
  OS_FT_SYMLINK, /* lstat view */
  OS_FT_OTHER
} os_file_type;

typedef struct {
  os_file_type type;
  uint64_t size;
  int64_t mtime_unix;
  int readonly; /* best effort */
} os_stat_info;

/* stat (follows symlinks). OS_FT_MISSING + OK when absent. */
astools_err os_stat(const char *path, os_stat_info *out);
/* lstat view: reports OS_FT_SYMLINK for links. */
astools_err os_lstat(const char *path, os_stat_info *out);

char *os_getcwd_dup(void); /* malloc'd; NULL on failure */

/* Absolute path of the current executable (install-root discovery).
 * malloc'd. */
astools_err os_exe_path(char **out);

/* Best-effort recursive delete (scratch cleanup). Never follows symlinks
 * out of the tree: symlinks are unlinked, not descended. */
astools_err os_rmtree(const char *path);

/* Duplicate of getenv(name); NULL if unset. malloc'd. */
char *os_env_dup(const char *name);

/* ---- child processes ----------------------------------------------------- */

typedef struct {
  int64_t pid;      /* pid_t / process id                    */
  intptr_t handle;  /* Win32 HANDLE; unused on POSIX         */
  int fd_in;        /* child stdin (write end); -1 = closed  */
  int fd_out;       /* child stdout (read end); -1 = closed  */
  int fd_err;       /* child stderr (read end); -1 = closed  */
  int pgroup;       /* POSIX: child leads its own group      */
  int64_t pgid;     /* retained after leader reap for tree cleanup */
} os_proc;

typedef struct {
  const char *const *argv; /* NULL-terminated; argv[0] is the executable
                            * path exactly as to be exec'd (no PATH search
                            * unless it contains no separator) */
  const char *const *envp; /* complete environment, NULL-terminated */
  const char *cwd;         /* absolute; NULL = inherit */
  /* rlimits applied in the child before exec (0 = leave unlimited) */
  int64_t limit_cpu_seconds;
  int64_t limit_mem_bytes;
  int64_t limit_nproc;
} os_spawn_opts;

/* Spawn with all three stdio pipes, child in its own process group
 * (POSIX) so the whole tree can be killed. Pipes are non-blocking on the
 * parent side. */
astools_err os_proc_spawn(const os_spawn_opts *o, os_proc *p);

/* Tasks (threads) currently charged to the caller's real UID — exactly the
 * quantity the kernel tests RLIMIT_NPROC against at fork(). The limit is
 * per-UID and system-wide, not per process tree, so an absolute cap is only
 * meaningful relative to what the account already uses.
 * ASTOOLS_ERR_UNSUPPORTED where the platform cannot report it. */
astools_err os_proc_user_tasks(int64_t *out);

/* Multiplexed wait for pipe readiness. want_write: include fd_in
 * writability. timeout_ms < 0 waits forever. *ready is a bitmask; 0 on
 * timeout. */
#define OS_READY_IN 1u  /* fd_in writable  */
#define OS_READY_OUT 2u /* fd_out readable */
#define OS_READY_ERR 4u /* fd_err readable */
astools_err os_proc_poll(os_proc *p, int want_write, int64_t timeout_ms,
                         unsigned *ready);

/* Nonblocking pipe I/O. Reads: *got = 0 with ASTOOLS_OK means EOF (the fd
 * is closed and set to -1). Writes: partial writes are normal. EAGAIN
 * surfaces as ASTOOLS_ERR_BUSY with zero bytes moved. */
astools_err os_proc_write_stdin(os_proc *p, const void *d, size_t n,
                                size_t *wrote);
#define OS_PIPE_OUT 1
#define OS_PIPE_ERR 2
astools_err os_proc_read(os_proc *p, int which, void *buf, size_t n,
                         size_t *got);
void os_proc_close_stdin(os_proc *p);

/* Kill the whole process group/job (SIGKILL; TerminateJobObject). */
void os_proc_kill(os_proc *p);
/* Graceful stop request (SIGTERM; no-op where unsupported). */
void os_proc_terminate(os_proc *p);

/* Reap. timeout_ms < 0 waits forever; ASTOOLS_ERR_BUSY if still running.
 * exit_code: >= 0 exit status, < 0 = -(terminating signal). */
astools_err os_proc_wait(os_proc *p, int64_t timeout_ms, int *exit_code);

/* Close any remaining fds/handles. Does not kill or reap. */
void os_proc_free(os_proc *p);

/* ---- dynamic libraries (kind "library") ---------------------------- */

typedef struct { void *h; } os_dylib;

astools_err os_dylib_open(const char *path, os_dylib *out);
void       *os_dylib_sym(os_dylib *d, const char *name);
void        os_dylib_close(os_dylib *d);

#ifdef __cplusplus
}
#endif

#endif /* ASTOOLS_OS_H */
