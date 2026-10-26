/*
 * log.c — leveled rotating file sink plus a callback sink.
 *
 * The file sink is gated by the configured level; the callback receives
 * every record and filters on its own. A record is one line:
 * "<RFC3339> LEVEL SUBSYS  message". Every failure is silent.
 */

#include "core.h"

#include <stdlib.h>
#include <string.h>

static const char *level_name(int level) {
  switch (level) {
    case CORE_LOG_ERROR: return "ERROR";
    case CORE_LOG_WARN:  return "WARN";
    case CORE_LOG_INFO:  return "INFO";
    default:             return "DEBUG";
  }
}

void core_log_init(core_log *log) {
  memset(log, 0, sizeof *log);
  log->level = CORE_LOG_INFO;
  os_mutex_init(&log->mu);
}

void core_log_destroy(core_log *log) {
  if (!log) return;
  core_log_close(log);
  os_mutex_destroy(&log->mu);
}

core_err core_log_open(core_log *log, const char *path, int level,
                       int max_size_kb, int max_files, bool sync) {
  FILE *f = NULL;
  char *copy = NULL;
  uint64_t sz = 0;
  if (!log) return CORE_ERR_INVALID;
  if (path && path[0] != '\0') {
    copy = core_strdup(path);
    if (!copy) return CORE_ERR_NOMEM;
    f = os_fopen(path, "ab");
    if (!f) {
      free(copy);
      return CORE_ERR_IO;
    }
    if (os_file_size(path, &sz) != CORE_OK) sz = 0;
  }
  os_mutex_lock(&log->mu);
  if (log->fp) fclose(log->fp);
  free(log->path);
  log->fp = f;
  log->size = sz;
  log->path = copy;
  log->level = level;
  log->max_size_kb = max_size_kb;
  log->max_files = max_files;
  log->sync = sync;
  os_mutex_unlock(&log->mu);
  return CORE_OK;
}

void core_log_close(core_log *log) {
  if (!log) return;
  os_mutex_lock(&log->mu);
  if (log->fp) {
    fflush(log->fp);
    fclose(log->fp);
    log->fp = NULL;
  }
  free(log->path);
  log->path = NULL;
  log->size = 0;
  os_mutex_unlock(&log->mu);
}

void core_log_set_callback(core_log *log, core_log_fn fn, void *userdata) {
  if (!log) return;
  os_mutex_lock(&log->mu);
  log->fn = fn;
  log->ud = userdata;
  os_mutex_unlock(&log->mu);
}

/* <path> -> <path>.1 -> ... -> <path>.(max_files-1); the overflow file is
 * deleted and a fresh current file opened. log->mu held. On failure the
 * sink may end up disabled until the next core_log_open. */
static void rotate_locked(core_log *log) {
  const char *path = log->path;
  int maxf = log->max_files < 1 ? 1 : log->max_files;
  size_t plen = strlen(path);
  char *pa = malloc(plen + 32), *pb = malloc(plen + 32);
  int i;
  fflush(log->fp);
  fclose(log->fp);
  log->fp = NULL;
  if (pa && pb) {
    if (maxf == 1) {
      (void)os_remove_file(path);
    } else {
      snprintf(pa, plen + 32, "%s.%d", path, maxf - 1);
      (void)os_remove_file(pa);
      for (i = maxf - 1; i >= 2; i--) {
        snprintf(pa, plen + 32, "%s.%d", path, i - 1);
        snprintf(pb, plen + 32, "%s.%d", path, i);
        (void)os_rename(pa, pb); /* the source may be absent */
      }
      snprintf(pa, plen + 32, "%s.1", path);
      (void)os_rename(path, pa);
    }
  }
  free(pa);
  free(pb);
  log->fp = os_fopen(path, "ab");
  log->size = 0;
  if (log->fp) {
    uint64_t sz = 0;
    /* a failed rename appends to the old file: track its real size */
    if (os_file_size(path, &sz) == CORE_OK) log->size = sz;
  }
}

void core_log_vwrite(core_log *log, core_time now, int level,
                     const char *subsys, const char *fmt, va_list ap) {
  char stamp[CORE_RFC3339_SIZE];
  char msg_stack[512], line_stack[768], head[64];
  char *msg = msg_stack, *msg_heap = NULL;
  char *line = line_stack, *line_heap = NULL;
  char *q;
  int need, headlen;
  size_t msglen, linelen;
  va_list again;
  core_log_fn fn;
  void *ud;

  if (!log || !fmt) return;
  if (!subsys) subsys = "-";

  va_copy(again, ap);
  need = vsnprintf(msg_stack, sizeof msg_stack, fmt, ap);
  if (need >= 0 && (size_t)need >= sizeof msg_stack) {
    msg_heap = malloc((size_t)need + 1);
    if (msg_heap) { /* else: keep the truncated stack copy */
      vsnprintf(msg_heap, (size_t)need + 1, fmt, again);
      msg = msg_heap;
    }
  }
  va_end(again);
  if (need < 0) return;
  for (q = msg; *q; q++) /* records are single-line */
    if (*q == '\n' || *q == '\r') *q = ' ';
  msglen = strlen(msg);

  core_time_format_rfc3339(now, stamp);
  headlen = snprintf(head, sizeof head, "%s %-5s %-8s ", stamp,
                     level_name(level), subsys);
  if (headlen < 0) {
    free(msg_heap);
    return;
  }
  if ((size_t)headlen >= sizeof head) headlen = (int)sizeof head - 1;

  linelen = (size_t)headlen + msglen;
  if (linelen + 2 > sizeof line_stack) {
    line_heap = malloc(linelen + 2);
    if (line_heap) {
      line = line_heap;
    } else {
      msglen = sizeof line_stack - 2 - (size_t)headlen;
      linelen = (size_t)headlen + msglen;
    }
  }
  memcpy(line, head, (size_t)headlen);
  memcpy(line + headlen, msg, msglen);
  line[linelen] = '\0';

  os_mutex_lock(&log->mu);
  fn = log->fn;
  ud = log->ud;
  if (log->fp && level <= log->level) {
    size_t wire = linelen + 1;
    if (log->max_size_kb > 0 &&
        log->size + wire > (uint64_t)log->max_size_kb * 1024u)
      rotate_locked(log);
    if (log->fp) {
      if (fwrite(line, 1, linelen, log->fp) == linelen &&
          fputc('\n', log->fp) != EOF)
        log->size += wire;
      fflush(log->fp);
      if (log->sync) (void)os_fsync(log->fp);
    }
  }
  os_mutex_unlock(&log->mu);

  /* Outside the lock: a callback that logs again must not deadlock. */
  if (fn) fn(level, line, ud);

  free(msg_heap);
  free(line_heap);
}
