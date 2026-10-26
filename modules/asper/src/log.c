/*
 * log.c — the asper log over the core sink. Never fails the caller.
 */

#include "asper_internal.h"

asper_err asper_log_open(asper_ctx *c) {
  if (!c) return ASPER_ERR_INVALID;
  return asper_from_core(core_log_open(&c->log, c->cfg.log_path,
      c->cfg.log_level, c->cfg.log_max_size_kb, c->cfg.log_max_files,
      c->cfg.log_sync));
}

void asper_log_close(asper_ctx *c) {
  if (c) core_log_close(&c->log);
}

void asper_log(asper_ctx *c, int level, const char *subsys, const char *fmt,
               ...) {
  va_list ap;
  if (!c) return;
  va_start(ap, fmt);
  core_log_vwrite(&c->log, core_clock_now(&c->clock), level, subsys, fmt, ap);
  va_end(ap);
}
