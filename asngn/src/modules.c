/*
 * modules.c — in-process integration with ⁂ asper and ⁂ astools.
 *
 * ⁂ asngn owns the module lifecycle: both modules are opened during
 * asngn_open against directories under the engine root and closed at
 * asngn_close; their log callbacks are funneled into ⁂ asngn's logging
 * under the subsystem tags "asper" and "astools". Both integrations are
 * optional and degrade: a module that fails to open leaves the
 * engine running with the corresponding zones and steps absent.
 *
 * Policy: ⁂ asngn narrows, never widens, module policy. Sandbox
 * level and grants come from the ⁂ astools configuration; ⁂ asngn only ever
 * layers tighter per-invocation deadlines and grant narrowing on top of
 * what the module already permits.
 *
 * Ownership: every string that crosses the module boundary is copied at
 * the boundary. Module allocations are released here with asper_free /
 * asper_records_free / astools_free; callers of this module only ever
 * free() what they receive — the allocators never mix.
 *
 * MIT License — per aspera ad astra.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asngn_internal.h"

#include "asper.h"
#include "astools.h"
#include "xcdn.h"

/* ═══════════════════════ helpers ═══════════════════════ */

/* Join a configured module path onto the engine root: absolute paths
 * (leading '/', or a Windows drive prefix) pass through, "." is the
 * engine root itself, everything else is root-relative. Returns a
 * malloc'd string; NULL on OOM (or when path is NULL). */
static char *mod_join(const asngn_ctx *c, const char *path) {
  if (!path) return NULL;
  if (os_path_is_abs(path))
    return core_strdup(path);
  if (strcmp(path, ".") == 0) return core_strdup(c->root);
  return os_path_join(c->root, path);
}

/* Module log levels share ⁂ asngn's numeric convention (0..3); clamp
 * defensively anyway. */
static int mod_level(int level) {
  if (level < ASNGN_LOG_ERROR) return ASNGN_LOG_ERROR;
  if (level > ASNGN_LOG_DEBUG) return ASNGN_LOG_DEBUG;
  return level;
}

/* The modules hand their loggers the already-formatted line, timestamp
 * and level included; re-logging that verbatim double-prefixes the file
 * log and leaves the TUI trace showing nothing but dates. Skip their
 * "<ISO timestamp> LEVEL " lead-in and keep the message proper. */
static const char *mod_strip_prefix(const char *msg) {
  const char *p = msg;
  size_t i;
  static const char *const LEVELS[] = {"ERROR", "WARN", "INFO", "DEBUG",
                                       NULL};
  /* timestamp token: starts "dddd-" and runs to the first space */
  for (i = 0; i < 4; i++)
    if (p[i] < '0' || p[i] > '9') return msg;
  if (p[4] != '-') return msg;
  while (*p != '\0' && *p != ' ') p++;
  while (*p == ' ') p++;
  for (i = 0; LEVELS[i] != NULL; i++) {
    size_t n = strlen(LEVELS[i]);
    if (strncmp(p, LEVELS[i], n) == 0 &&
        (p[n] == ' ' || p[n] == '\0')) {
      p += n;
      while (*p == ' ') p++;
      return p;
    }
  }
  return msg;
}

/* A record whose body is empty after stripping carries no signal; keep
 * it out of the WARN/ERROR band so it never reaches the trace pane. */
static void mod_log(asngn_ctx *c, int level, const char *subsys,
                    const char *msg) {
  const char *m = msg != NULL ? mod_strip_prefix(msg) : "";
  const char *q;
  int lv = mod_level(level);
  for (q = m; *q == ' '; q++) {}
  if (*q == '\0' && lv <= ASNGN_LOG_WARN) lv = ASNGN_LOG_INFO;
  asngn_log(c, lv, subsys, "%s", m);
}

static void mod_asper_log(int level, const char *msg, void *ud) {
  mod_log((asngn_ctx *)ud, level, "asper", msg);
}

static void mod_astools_log(int level, const char *msg, void *ud) {
  mod_log((asngn_ctx *)ud, level, "astools", msg);
}

/* Longest prefix of s[0..len) not exceeding max bytes that does not split
 * a UTF-8 sequence. */
static size_t mod_utf8_clip(const char *s, size_t len, size_t max) {
  if (len <= max) return len;
  while (max > 0 && ((unsigned char)s[max] & 0xC0) == 0x80) max--;
  return max;
}

/* ═══════════════════════ open / close ═══════════════════════ */

static asngn_err mod_open_asper(asngn_ctx *c) {
  char *root, *conf = NULL;
  asngn_err e;

  root = mod_join(c, c->cfg.asper_root);
  if (!root)
    return asngn_seterr(c, ASNGN_ERR_NOMEM, "modules: out of memory");
  if (c->cfg.asper_config) {
    conf = mod_join(c, c->cfg.asper_config);
    if (!conf) {
      free(root);
      return asngn_seterr(c, ASNGN_ERR_NOMEM, "modules: out of memory");
    }
  }

  e = os_mkdir_p(root);
  if (e != ASNGN_OK) {
    asngn_log(c, ASNGN_LOG_ERROR, "asper",
              "cannot create memory root %s; memory disabled (degraded)",
              root);
  } else {
    asper_open_params p;
    asper_model_binding models;
    asper_err ae;
    memset(&p, 0, sizeof(p));
    p.memory_root = root;
    p.config_path = conf;
    p.models = &models;
    memset(&models, 0, sizeof models);
    models.manager = c->shared_models;
    {
      int curator_slot = asngn_models_slot_for_role(c, ASNGN_ROLE_COMPRESSOR);
      int embed_slot = asngn_models_slot_for_role(c, ASNGN_ROLE_EMBEDDER);
      if (curator_slot >= 0)
        models.curator_model_id = c->models[curator_slot].cfg.id;
      if (embed_slot >= 0) {
        models.embedding_model_id = c->models[embed_slot].cfg.id;
        models.embedding_dim = c->models[embed_slot].cfg.dim;
      }
    }
    ae = asper_open(&p, &c->asper);
    if (ae != ASPER_OK) {
      /* No context was created, so there is no asper_last_error to read:
       * the stable code name is all we have. */
      c->asper = NULL;
      asngn_log(c, ASNGN_LOG_ERROR, "asper",
                "asper_open failed: %s; memory disabled (degraded)",
                asper_err_name(ae));
    } else {
      c->asper_ok = true;
      asper_set_logger(c->asper, mod_asper_log, c);
    }
  }

  free(root);
  free(conf);
  return ASNGN_OK;
}

/* Tool scratch for any workspace other than a session's private one (a
 * project, a fixed folder): under cache/scratch/, so a user's folder never
 * gains .astools/tmp. NULL keeps the configured default; *oom on allocation
 * failure. */
static char *mod_scratch_for(asngn_ctx *c, const char *workspace, bool *oom) {
  char *own = asngn_realpath(c->sessions_dir), *dir = NULL, *out = NULL;
  uint8_t hash[32];
  char hex[17];
  size_t n;
  *oom = false;
  if (own != NULL) {
    n = strlen(own);
    if (strncmp(workspace, own, n) == 0 &&
        (workspace[n] == '\0' || workspace[n] == '/' || workspace[n] == '\\')) {
      free(own);
      return NULL;
    }
    free(own);
  }
  core_sha256(workspace, strlen(workspace), hash);
  core_sha256_hex(hash, 8, hex);
  dir = os_path_join(c->cache_dir ? c->cache_dir : c->root, "scratch");
  if (dir != NULL) out = os_path_join(dir, hex);
  *oom = out == NULL;
  free(dir);
  return out;
}

static asngn_err mod_open_astools_at(asngn_ctx *c, const char *workspace) {
  char *root, *work, *conf = NULL, *scratch;
  bool oom;
  asngn_err e;

  root = mod_join(c, c->cfg.astools_root);
  work = core_strdup(workspace);
  if (c->cfg.astools_config) conf = mod_join(c, c->cfg.astools_config);
  scratch = mod_scratch_for(c, workspace, &oom);
  if (!root || !work || (c->cfg.astools_config && !conf) || oom) {
    free(root);
    free(work);
    free(conf);
    free(scratch);
    return asngn_seterr(c, ASNGN_ERR_NOMEM, "modules: out of memory");
  }

  e = os_mkdir_p(root);
  if (e == ASNGN_OK) e = os_mkdir_p(work);
  if (e != ASNGN_OK) {
    asngn_log(c, ASNGN_LOG_ERROR, "astools",
              "cannot create registry/workspace under %s; tools disabled "
              "(degraded)",
              c->root);
  } else {
    const char *regs[2];
    astools_open_params p;
    astools_err ae;
    regs[0] = root;
    regs[1] = NULL;
    memset(&p, 0, sizeof(p));
    /* an explicit ⁂ astools config owns the registry roots (its entries
     * may carry full trust); the default root applies only without one,
     * so the same directory is never scanned at two trust levels */
    p.registry_paths = conf != NULL ? NULL : regs;
    p.config_path = conf;
    p.workspace_root = work;
    p.scratch_root = scratch;
    ae = astools_open(&p, &c->astools);
    if (ae != ASTOOLS_OK) {
      c->astools = NULL;
      asngn_log(c, ASNGN_LOG_ERROR, "astools",
                "astools_open failed: %s; tools disabled (degraded)",
                astools_err_name(ae));
    } else {
      char *active = core_strdup(work);
      if (active == NULL) {
        astools_close(c->astools);
        c->astools = NULL;
        free(root);
        free(work);
        free(conf);
        free(scratch);
        return asngn_seterr(c, ASNGN_ERR_NOMEM,
                            "modules: out of memory");
      }
      free(c->astools_workspace_active);
      c->astools_workspace_active = active;
      c->astools_ok = true;
      astools_set_logger(c->astools, mod_astools_log, c);
    }
  }

  free(root);
  free(work);
  free(conf);
  free(scratch);
  return ASNGN_OK;
}

asngn_err asngn_modules_open(asngn_ctx *c) {
  asngn_err e;

  if (!c) return ASNGN_ERR_INVALID;
  c->asper_ok = false;
  c->astools_ok = false;

  if (c->cfg.asper_enable) {
    e = mod_open_asper(c);
    if (e != ASNGN_OK) return e;
  }
  if (c->cfg.astools_enable) {
    e = mod_open_astools_at(c, c->workspace.canonical_root);
    if (e != ASNGN_OK) return e;
  }
  return ASNGN_OK;
}

asngn_err asngn_modules_readiness(asngn_ctx *c) {
  asper_readiness mr;
  astools_readiness tr;
  const char *why = NULL;
  if (c == NULL) return ASNGN_ERR_INVALID;
  memset(&mr, 0, sizeof mr);
  memset(&tr, 0, sizeof tr);
  if (c->cfg.asper_enable && (!c->asper_ok || c->asper == NULL))
    why = "asper is unavailable";
  else if (c->cfg.asper_enable &&
           (asper_get_readiness(c->asper, &mr) != ASPER_OK ||
            !mr.store_ok || !mr.embedder_ok || !mr.curator_ok))
    why = "asper store, embedder, or curator is not ready";
  else if (c->cfg.astools_enable && (!c->astools_ok || c->astools == NULL))
    why = "astools is unavailable";
  else if (c->cfg.astools_enable &&
           (astools_get_readiness(c->astools, &tr) != ASTOOLS_OK ||
            !tr.workspace_bound))
    why = "astools has no canonical workspace";
  else if (c->cfg.astools_enable && (tr.workspace_access & 2) == 0)
    why = "astools lacks a read-write workspace grant";
  else if (c->cfg.astools_enable && tr.sandbox_level < 1)
    why = "astools sandbox is disabled";
  else if (c->cfg.astools_enable && tr.tools_enabled == 0)
    why = "astools has no enabled tools";
  if (why == NULL) return ASNGN_OK;
  if (c->allow_degraded) {
    asngn_log(c, ASNGN_LOG_WARN, "session",
              "coding readiness degraded: %s (--allow-degraded)", why);
    return ASNGN_OK;
  }
  return asngn_seterr(c, ASNGN_ERR_MODULE,
                      "coding readiness failed: %s; pass "
                      "--allow-degraded to opt in", why);
}

void asngn_modules_close(asngn_ctx *c) {
  if (!c) return;
  if (c->astools) {
    astools_close(c->astools);
    c->astools = NULL;
  }
  if (c->asper) {
    asper_close(c->asper);
    c->asper = NULL;
  }
  c->asper_ok = false;
  c->astools_ok = false;
  free(c->astools_workspace_active);
  c->astools_workspace_active = NULL;

}

asngn_err asngn_modules_workspace_sync(asngn_ctx *c, const char *root) {
  asngn_err e;
  if (c == NULL || root == NULL || root[0] == '\0')
    return ASNGN_ERR_INVALID;
  if (!c->cfg.astools_enable) return ASNGN_OK;

  /* The agent worker serializes turns across sessions.  Rebinding here
   * gives every turn an ⁂ astools context whose auto-grant and relative-path
   * base are exactly that session's private workspace. */
  if (c->astools_workspace_active != NULL &&
      strcmp(c->astools_workspace_active, root) == 0)
    return ASNGN_OK;
  if (c->astools != NULL) {
    astools_close(c->astools);
    c->astools = NULL;
  }
  c->astools_ok = false;
  free(c->astools_workspace_active);
  c->astools_workspace_active = NULL;
  e = mod_open_astools_at(c, root);
  if (e != ASNGN_OK) return e;
  if (c->cfg.profile == ASNGN_PROFILE_CODING)
    return asngn_modules_readiness(c);
  return ASNGN_OK;
}

asngn_err asngn_modules_annotations(asngn_ctx *c, const char *ref,
                                     const char *cmd, asngn_tool_note *out) {
  char *text = NULL;
  xcdn_error_t xe;
  xcdn_document_t *doc;
  const xcdn_node_t *root_node = NULL;
  const xcdn_value_t *obj, *cmds;
  const char *ver;
  asngn_tool_note note;
  astools_err ae;
  asngn_err e;
  size_t i, n;
  bool found = false;

  if (!c || !ref || !cmd || !out) return ASNGN_ERR_INVALID;
  memset(out, 0, sizeof(*out));
  if (!c->astools_ok)
    return asngn_seterr(c, ASNGN_ERR_UNSUPPORTED, "astools disabled");

  ae = astools_tool_manifest(c->astools, ref, &text);
  if (ae != ASTOOLS_OK)
    return asngn_seterr(c, ASNGN_ERR_MODULE, "astools: %s: %s",
                        astools_err_name(ae),
                        astools_last_error(c->astools));

  doc = xcdn_parse_str(text, strlen(text), &xe);
  astools_free(text);
  if (!doc)
    return asngn_seterr(c, ASNGN_ERR_MODULE,
                        "astools: manifest for %s does not parse: %s "
                        "(line %zu)",
                        ref, xe.message, xe.span.line);

  /* The manifest is the first #astools_tool-tagged value, falling back to
   * the first object value. */
  for (i = 0; i < doc->values_len; i++) {
    if (xcdn_node_has_tag(doc->values[i], "astools_tool")) {
      root_node = doc->values[i];
      break;
    }
  }
  for (i = 0; !root_node && i < doc->values_len; i++) {
    if (doc->values[i]->value &&
        doc->values[i]->value->type == XCDN_VAL_OBJECT)
      root_node = doc->values[i];
  }
  obj = root_node ? root_node->value : NULL;
  if (!obj || obj->type != XCDN_VAL_OBJECT) {
    xcdn_document_free(doc);
    return asngn_seterr(c, ASNGN_ERR_MODULE,
                        "astools: manifest for %s has no tool object", ref);
  }

  memset(&note, 0, sizeof(note));
  snprintf(note.ref, sizeof(note.ref), "%s", ref);
  snprintf(note.cmd, sizeof(note.cmd), "%s", cmd);
  ver = asngn_xstr(asngn_xfield(obj, "version"));
  if (ver) snprintf(note.version, sizeof(note.version), "%s", ver);
  /* Package-level grants requested by the manifest: any network request
   * (true, or a non-empty host list) makes the tool a network egress. */
  {
    const xcdn_value_t *perm = asngn_xfield(obj, "permissions");
    const xcdn_value_t *net = perm && perm->type == XCDN_VAL_OBJECT
                                  ? asngn_xfield(perm, "net") : NULL;
    bool b = false;
    if (net && asngn_xbool(net, &b)) note.network = b;
    else if (net && net->type == XCDN_VAL_ARRAY) note.network = xcdn_array_len(net) > 0;
    else if (net && net->type == XCDN_VAL_OBJECT) note.network = true;
  }
  {
    const char *id = asngn_xstr(asngn_xfield(obj, "id"));
    note.delegate = id && !strcmp(id, "delegate");
  }

  cmds = asngn_xfield(obj, "commands");
  if (!cmds || cmds->type != XCDN_VAL_ARRAY) {
    xcdn_document_free(doc);
    return asngn_seterr(c, ASNGN_ERR_MODULE,
                        "astools: manifest for %s has no commands array",
                        ref);
  }

  n = xcdn_array_len(cmds);
  for (i = 0; i < n && !found; i++) {
    const xcdn_node_t *cn = xcdn_array_get(cmds, i);
    const xcdn_value_t *cv = cn ? cn->value : NULL;
    const xcdn_value_t *ann;
    const char *name;
    bool b;

    if (!cv || cv->type != XCDN_VAL_OBJECT) continue;
    name = asngn_xstr(asngn_xfield(cv, "name"));
    if (!name || strcmp(name, cmd) != 0) continue;
    found = true;

    /* An absent annotations object means all annotations false. */
    ann = asngn_xfield(cv, "annotations");
    if (ann && ann->type == XCDN_VAL_OBJECT) {
      if (asngn_xbool(asngn_xfield(ann, "read_only"), &b))
        note.read_only = b;
      if (asngn_xbool(asngn_xfield(ann, "destructive"), &b))
        note.destructive = b;
      if (asngn_xbool(asngn_xfield(ann, "idempotent"), &b))
        note.idempotent = b;
      if (asngn_xbool(asngn_xfield(ann, "long_running"), &b))
        note.long_running = b;
    }
  }
  xcdn_document_free(doc);

  if (!found) {
    e = asngn_seterr(c, ASNGN_ERR_NOT_FOUND,
                     "astools: tool %s has no command %s", ref, cmd);
    return e;
  }

  *out = note;
  return ASNGN_OK;
}

/* ═══════════════════════ memory zone (⁂ asper) ═══════════════════════ */

static asngn_err mod_asper_err(asngn_ctx *c, asper_err e,
                               const char *operation) {
  asngn_err mapped;
  switch (e) {
    case ASPER_OK: return ASNGN_OK;
    case ASPER_ERR_NOT_FOUND: mapped = ASNGN_ERR_NOT_FOUND; break;
    case ASPER_ERR_NOMEM: mapped = ASNGN_ERR_NOMEM; break;
    case ASPER_ERR_TIMEOUT: mapped = ASNGN_ERR_TIMEOUT; break;
    case ASPER_ERR_CANCELLED: mapped = ASNGN_ERR_CANCELLED; break;
    case ASPER_ERR_LIMIT: mapped = ASNGN_ERR_LIMIT; break;
    case ASPER_ERR_BUSY: mapped = ASNGN_ERR_BUSY; break;
    case ASPER_ERR_INVALID: mapped = ASNGN_ERR_INVALID; break;
    default: mapped = ASNGN_ERR_MODULE; break;
  }
  return asngn_seterr(c, mapped, "asper %s: %s: %s", operation,
                      asper_err_name(e),
                      c->asper ? asper_last_error(c->asper) : "unavailable");
}

asngn_err asngn_modules_event_append(asngn_ctx *c, const char *scope,
                                      asngn_memory_kind kind,
                                      const char *text,
                                      const char *object_ref, bool pinned,
                                      char out_id[37]) {
  return asngn_modules_event_append_from(c, scope, kind, text, object_ref,
                                         pinned, ASNGN_ORIGIN_HOST, out_id);
}

asngn_err asngn_modules_event_append_from(asngn_ctx *c, const char *scope,
                                           asngn_memory_kind kind,
                                           const char *text,
                                           const char *object_ref, bool pinned,
                                           asngn_origin origin,
                                           char out_id[37]) {
  asper_event_input in;
  asper_err ae;
  if (!c || !scope || !text) return ASNGN_ERR_INVALID;
  if (!c->asper_ok || !c->asper) return ASNGN_ERR_UNSUPPORTED;
  memset(&in, 0, sizeof in);
  in.scope = scope;
  in.kind = (asper_event_kind)kind;
  in.text = text;
  in.object_ref = object_ref;
  in.pinned = pinned ? 1 : 0;
  /* asngn_origin mirrors asper_origin value for value. */
  in.origin = (asper_origin)origin;
  ae = asper_event_append(c->asper, &in, out_id);
  return mod_asper_err(c, ae, "event_append");
}

void asngn_modules_events_free(asngn_memory_event *events, size_t n) {
  if (!events) return;
  for (size_t i = 0; i < n; i++) free(events[i].text);
  free(events);
}

asngn_err asngn_modules_event_list(asngn_ctx *c, const char *scope,
                                    asngn_memory_event **out, size_t *out_n) {
  asper_event *src = NULL;
  asngn_memory_event *dst = NULL;
  size_t n = 0;
  asper_err ae;
  if (!c || !scope || !out || !out_n) return ASNGN_ERR_INVALID;
  *out = NULL;
  *out_n = 0;
  if (!c->asper_ok || !c->asper) return ASNGN_ERR_UNSUPPORTED;
  ae = asper_event_list(c->asper, scope, &src, &n);
  if (ae != ASPER_OK) return mod_asper_err(c, ae, "event_list");
  if (n) {
    dst = (asngn_memory_event *)calloc(n, sizeof *dst);
    if (!dst) {
      asper_events_free(src, n);
      return ASNGN_ERR_NOMEM;
    }
  }
  for (size_t i = 0; i < n; i++) {
    memcpy(dst[i].id, src[i].id, sizeof dst[i].id);
    dst[i].sequence = src[i].sequence;
    dst[i].at = (core_time)src[i].at;
    dst[i].kind = (asngn_memory_kind)src[i].kind;
    dst[i].text = core_strdup(src[i].text ? src[i].text : "");
    snprintf(dst[i].object_ref, sizeof dst[i].object_ref, "%s",
             src[i].object_ref);
    dst[i].pinned = src[i].pinned != 0;
    if (!dst[i].text) {
      asper_events_free(src, n);
      asngn_modules_events_free(dst, n);
      return ASNGN_ERR_NOMEM;
    }
  }
  asper_events_free(src, n);
  *out = dst;
  *out_n = n;
  return ASNGN_OK;
}

asngn_err asngn_modules_event_pin(asngn_ctx *c, const char *scope,
                                   const char *event_id, bool pinned) {
  asper_err ae;
  if (!c || !scope || !event_id) return ASNGN_ERR_INVALID;
  if (!c->asper_ok || !c->asper) return ASNGN_ERR_UNSUPPORTED;
  ae = asper_event_set_pinned(c->asper, scope, event_id, pinned ? 1 : 0);
  return mod_asper_err(c, ae, "event_pin");
}

asngn_err asngn_modules_object_put(asngn_ctx *c, const void *data,
                                    size_t len, char out_ref[72]) {
  asper_err ae;
  if (!c || (!data && len) || !out_ref) return ASNGN_ERR_INVALID;
  if (!c->asper_ok || !c->asper) return ASNGN_ERR_UNSUPPORTED;
  ae = asper_object_put(c->asper, data, len, out_ref);
  return mod_asper_err(c, ae, "object_put");
}

asngn_err asngn_modules_object_read(asngn_ctx *c, const char *ref,
                                     size_t offset, size_t max_bytes,
                                     void **out, size_t *out_len) {
  void *src = NULL, *copy = NULL;
  size_t n = 0;
  asper_err ae;
  if (!c || !ref || !out || !out_len) return ASNGN_ERR_INVALID;
  *out = NULL;
  *out_len = 0;
  if (!c->asper_ok || !c->asper) return ASNGN_ERR_UNSUPPORTED;
  ae = asper_object_read(c->asper, ref, offset, max_bytes, &src, &n);
  if (ae != ASPER_OK) return mod_asper_err(c, ae, "object_read");
  copy = malloc(n + 1);
  if (!copy) {
    asper_free(src);
    return ASNGN_ERR_NOMEM;
  }
  if (n) memcpy(copy, src, n);
  ((unsigned char *)copy)[n] = 0;
  asper_free(src);
  *out = copy;
  *out_len = n;
  return ASNGN_OK;
}

asngn_err asngn_modules_checkpoint(asngn_ctx *c, const char *scope,
                                    const char *text, char out_id[37]) {
  asper_err ae;
  if (!c || !scope || !text) return ASNGN_ERR_INVALID;
  if (!c->asper_ok || !c->asper) return ASNGN_ERR_UNSUPPORTED;
  ae = asper_checkpoint_commit(c->asper, scope, text, out_id);
  return mod_asper_err(c, ae, "checkpoint");
}

asngn_err asngn_modules_compact(asngn_ctx *c) {
  asper_err ae;
  if (!c) return ASNGN_ERR_INVALID;
  if (!c->asper_ok || !c->asper) return ASNGN_ERR_UNSUPPORTED;
  ae = asper_flush(c->asper, 1);
  return mod_asper_err(c, ae, "flush");
}

typedef struct {
  asngn_ctx *c;
  int slot;
} mod_token_counter;

static int mod_count_tokens(const char *text, void *ud) {
  mod_token_counter *counter = (mod_token_counter *)ud;
  return asngn_models_count_tokens(counter->c, counter->slot, text);
}

asngn_err asngn_modules_context(asngn_ctx *c, asngn_session *session,
                                 const char *base_prompt, const char *query,
                                 size_t history_tokens,
                                 size_t checkpoint_tokens, int count_slot,
                                 char **out_system, char **out_context,
                                 size_t *out_system_tokens,
                                 size_t *out_context_tokens) {
  asper_context_request req;
  asper_context_pack pack;
  mod_token_counter counter;
  asper_err ae;
  if (!c || !session || !base_prompt || !query || !out_system ||
      !out_context) return ASNGN_ERR_INVALID;
  *out_system = NULL;
  *out_context = NULL;
  if (!c->asper_ok || !c->asper) return ASNGN_ERR_UNSUPPORTED;
  /* Serialize snapshot observation across lanes so a slower earlier scan cannot
   * overwrite a newer version. Memory keys distinguish separate worktrees. */
  asngn_ctx *coordinator = c->owner ? c->owner : c;
  asngn_workspace_info workspace = session->workspace;
  uint8_t root_hash[32]; char resource[75] = "workspace:";
  core_sha256(workspace.canonical_root,strlen(workspace.canonical_root),root_hash);
  core_sha256_hex(root_hash,32,resource+10);
  os_mutex_lock(&coordinator->mod_mu);
  asngn_err snapshot = asngn_workspace_snapshot(&workspace,NULL);
  ae = asper_memory_observe_dependency(c->asper,resource,snapshot == ASNGN_OK ? workspace.fingerprint : NULL);
  os_mutex_unlock(&coordinator->mod_mu);
  if (ae != ASPER_OK) return mod_asper_err(c,ae,"memory_dependency");
  memset(&req, 0, sizeof req);
  memset(&pack, 0, sizeof pack);
  counter.c = c;
  counter.slot = count_slot;
  req.scope = session->slug;
  req.base_system_prompt = base_prompt;
  req.query = query;
  req.history_tokens = history_tokens;
  req.checkpoint_tokens = checkpoint_tokens;
  req.count_tokens = mod_count_tokens;
  req.count_userdata = &counter;
  ae = c->owner ? asper_context_materialize_project(c->asper,&req,c->lane_project,&pack)
                : asper_context_materialize(c->asper, &req, &pack);
  if (ae != ASPER_OK) return mod_asper_err(c, ae, "context_materialize");
  *out_system = core_strdup(pack.system_prompt ? pack.system_prompt : "");
  *out_context = core_strdup(pack.context_text ? pack.context_text : "");
  if (out_system_tokens) *out_system_tokens = pack.system_tokens;
  if (out_context_tokens) *out_context_tokens = pack.context_tokens;
  asper_context_pack_free(&pack);
  if (!*out_system || !*out_context) {
    free(*out_system);
    free(*out_context);
    *out_system = NULL;
    *out_context = NULL;
    return ASNGN_ERR_NOMEM;
  }
  return ASNGN_OK;
}

/* ═══════════════════════ recall ═══════════════════════ */

#define SIB_RECALL_CITES     3
#define SIB_CITE_MAX_BYTES 2048

asngn_err asngn_modules_recall(asngn_ctx *c, const char *question,
                                char **out_block) {
  char *answer = NULL;
  asper_record **cited = NULL;
  size_t cited_n = 0;
  asper_err ae;
  core_buf b;
  bool ok;
  size_t i;

  if (!c || !question || !out_block) return ASNGN_ERR_INVALID;
  *out_block = NULL;

  if (!c->asper_ok) {
    *out_block = core_strdup("memory: unavailable");
    goto out_fixed;
  }

  ae = c->owner ? asper_recall_project(c->asper,question,c->lane_project,&answer,&cited,&cited_n)
                : asper_recall(c->asper, question, &answer, &cited, &cited_n);
  if (ae == ASPER_ERR_NOT_FOUND) {
    *out_block = core_strdup("memory: nothing relevant");
    goto out_fixed;
  }
  if (ae != ASPER_OK) {
    char line[96];
    asngn_log(c, ASNGN_LOG_WARN, "asper", "recall failed: %s: %s",
              asper_err_name(ae), asper_last_error(c->asper));
    snprintf(line, sizeof(line), "memory: unavailable (%s)",
             asper_err_name(ae));
    *out_block = core_strdup(line);
    goto out_fixed;
  }

  core_buf_init(&b);
  ok = asngn_buf_appends(&b, "memory: ") == ASNGN_OK &&
       asngn_buf_appends(&b, answer ? answer : "") == ASNGN_OK;
  for (i = 0; ok && i < cited_n && i < SIB_RECALL_CITES; i++) {
    const char *content = asper_record_content(cited[i]);
    size_t clen;
    if (!content) continue;
    clen = mod_utf8_clip(content, strlen(content), SIB_CITE_MAX_BYTES);
    ok = asngn_buf_appends(&b, "\n- ") == ASNGN_OK &&
         asngn_buf_append(&b, content, clen) == ASNGN_OK;
  }
  asper_free(answer);
  asper_records_free(cited, cited_n);

  if (!ok) {
    core_buf_free(&b);
    return asngn_seterr(c, ASNGN_ERR_NOMEM, "modules: out of memory");
  }
  *out_block = core_buf_detach(&b);
  if (!*out_block)
    return asngn_seterr(c, ASNGN_ERR_NOMEM, "modules: out of memory");
  return ASNGN_OK;

out_fixed:
  if (!*out_block)
    return asngn_seterr(c, ASNGN_ERR_NOMEM, "modules: out of memory");
  return ASNGN_OK;
}

/* ═══════════════════════ projects ═══════════════════════ */

asngn_err asngn_modules_project(asngn_ctx *c, const char *slug) {
  asper_err ae;

  if (!c) return ASNGN_ERR_INVALID;
  if (!c->asper_ok)
    return asngn_seterr(c, ASNGN_ERR_UNSUPPORTED, "asper disabled");

  ae = asper_project_select(c->asper, slug); /* NULL slug deselects */
  if (ae != ASPER_OK)
    return asngn_seterr(c, ASNGN_ERR_MODULE, "asper: %s: %s",
                        asper_err_name(ae), asper_last_error(c->asper));
  return ASNGN_OK;
}

/* Make ⁂ asper's active project match `want` (NULL = none). ⁂ asper holds a
 * single active project per context while every session carries its
 * own: sessions sync it on open and at each turn, so switching
 * sessions switches the project memory with them. No-op when they
 * already agree. */
asngn_err asngn_modules_project_sync(asngn_ctx *c, const char *want) {
  char *cur = NULL;
  bool same;
  if (c == NULL) return ASNGN_ERR_INVALID;
  if (!c->asper_ok) return ASNGN_ERR_UNSUPPORTED;
  if (c->owner) {
    char *copy=want ? core_strdup(want) : NULL;
    if (want && !copy) return ASNGN_ERR_NOMEM;
    free(c->lane_project);c->lane_project=copy;return ASNGN_OK;
  }
  if (asper_project_active(c->asper, &cur) != ASPER_OK) cur = NULL;
  same = (want == NULL && cur == NULL) ||
         (want != NULL && cur != NULL && strcmp(want, cur) == 0);
  if (cur != NULL) asper_free(cur);
  if (same) return ASNGN_OK;
  return asngn_modules_project(c, want);
}
