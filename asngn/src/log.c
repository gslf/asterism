/*
 * log.c — the engine log over the core sink. Never fails the caller.
 *
 * Sessions log through their owner context, so one engine has one sink.
 */

#include "asngn_internal.h"

asngn_err asngn_log_open(asngn_ctx *c) {
  core_err e;
  if (!c) return ASNGN_ERR_INVALID;
  e = core_log_open(&c->log, c->cfg.log_path, c->cfg.log_level,
                    c->cfg.log_max_size_kb, c->cfg.log_max_files,
                    c->cfg.log_sync);
  if (e == CORE_ERR_IO)
    return asngn_seterr(c, ASNGN_ERR_IO, "log: cannot open '%s'",
                        c->cfg.log_path);
  return asngn_from_core(e);
}

void asngn_log_close(asngn_ctx *c) {
  if (c) core_log_close(&c->log);
}

void asngn_log(asngn_ctx *c, int level, const char *subsys, const char *fmt,
               ...) {
  va_list ap;
  if (c && c->owner) c = c->owner;
  if (!c) return;
  va_start(ap, fmt);
  core_log_vwrite(&c->log, core_clock_now(&c->clock), level, subsys, fmt, ap);
  va_end(ap);
}
