/*
 * log.c — the astools log over the core sink, plus the audit trail.
 *
 * Logging and auditing never fail the caller.
 */

#include "astools_internal.h"

#include <stdlib.h>
#include <string.h>

astools_err astools_log_open(astools_ctx *c) {
  if (!c) return ASTOOLS_ERR_INVALID;
  return astools_from_core(core_log_open(&c->log, c->cfg.log_path,
      c->cfg.log_level, (int)c->cfg.log_max_size_kb, c->cfg.log_max_files,
      c->cfg.log_sync));
}

void astools_log_close(astools_ctx *c) {
  if (c) core_log_close(&c->log);
}

void astools_log(astools_ctx *c, int level, const char *subsys,
                 const char *fmt, ...) {
  va_list ap;
  if (!c) return;
  va_start(ap, fmt);
  core_log_vwrite(&c->log, core_clock_now(&c->clock), level, subsys, fmt, ap);
  va_end(ap);
}

/* ---- audit trail --------------------------------------------------- */

/* Append s as the body of an xCDN double-quoted string, escaping quote,
 * backslash and control characters. tool/version/command/error come from
 * untrusted manifests: an unescaped value could forge audit records. */
static astools_err audit_escape(core_buf *b, const char *s) {
  astools_err e = ASTOOLS_OK;
  const unsigned char *p;
  if (!s) return ASTOOLS_OK;
  for (p = (const unsigned char *)s; *p && e == ASTOOLS_OK; p++) {
    unsigned char ch = *p;
    if (ch == '"') {
      e = astools_buf_appends(b, "\\\"");
    } else if (ch == '\\') {
      e = astools_buf_appends(b, "\\\\");
    } else if (ch == '\n') {
      e = astools_buf_appends(b, "\\n");
    } else if (ch == '\r') {
      e = astools_buf_appends(b, "\\r");
    } else if (ch == '\t') {
      e = astools_buf_appends(b, "\\t");
    } else if (ch < 0x20) {
      e = astools_buf_printf(b, "\\u%04x", (unsigned)ch);
    } else {
      e = astools_buf_appendc(b, (char)ch);
    }
  }
  return e;
}

static astools_err audit_append_str(astools_err e, core_buf *b,
                                    const char *s) {
  return e != ASTOOLS_OK ? e : astools_buf_appends(b, s);
}

/* One #invocation record appended to <config_dir>/audit.xcdn. Append-only,
 * never rotated; args are hashed, not stored. Never fails the
 * caller. */
void astools_audit_append(astools_ctx *c, const char *tool,
                          const char *version, const char *command, bool ok,
                          const char *error_code, uint64_t duration_ms,
                          const uint8_t args_sha256[32]) {
  core_buf b;
  char stamp[32];
  char *b64 = NULL;
  char *path = NULL;
  astools_err e = ASTOOLS_OK;

  if (!c || !c->cfg.audit) return;

  core_time_format_rfc3339(core_clock_now(&c->clock), stamp);
  core_buf_init(&b);

  e = audit_append_str(e, &b, "#invocation { at: t\"");
  e = audit_append_str(e, &b, stamp);
  e = audit_append_str(e, &b, "\", tool: \"");
  if (e == ASTOOLS_OK) e = audit_escape(&b, tool ? tool : "");
  e = audit_append_str(e, &b, "\", version: \"");
  if (e == ASTOOLS_OK) e = audit_escape(&b, version ? version : "");
  e = audit_append_str(e, &b, "\", command: \"");
  if (e == ASTOOLS_OK) e = audit_escape(&b, command ? command : "");
  e = audit_append_str(e, &b, ok ? "\", ok: true" : "\", ok: false");
  if (!ok && error_code) {
    e = audit_append_str(e, &b, ", error: \"");
    if (e == ASTOOLS_OK) e = audit_escape(&b, error_code);
    e = audit_append_str(e, &b, "\"");
  }
  if (e == ASTOOLS_OK)
    e = astools_buf_printf(&b, ", duration_ms: %llu",
                           (unsigned long long)duration_ms);
  if (args_sha256) {
    b64 = core_base64_encode(args_sha256, 32);
    if (b64) {
      e = audit_append_str(e, &b, ", args_sha256: b\"");
      e = audit_append_str(e, &b, b64);
      e = audit_append_str(e, &b, "\"");
    } else {
      e = ASTOOLS_ERR_NOMEM;
    }
  }
  e = audit_append_str(e, &b, " }\n");
  if (e != ASTOOLS_OK) goto cleanup;

  path = os_path_join(c->cfg.config_dir && c->cfg.config_dir[0] != '\0'
                          ? c->cfg.config_dir
                          : ".",
                      "audit.xcdn");
  if (!path) goto cleanup;

  os_mutex_lock(&c->audit_mu);
  {
    FILE *f = os_fopen(path, "ab");
    if (f) {
      size_t wrote = fwrite(b.data, 1, b.len, f);
      if (wrote == b.len && c->cfg.log_sync) (void)os_fsync(f);
      fclose(f);
    }
  }
  os_mutex_unlock(&c->audit_mu);

cleanup:
  free(b64);
  free(path);
  core_buf_free(&b);
}
