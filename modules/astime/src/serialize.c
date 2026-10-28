/*
 * serialize.c — canonical xCDN for routines and their state, and atomic
 * writes (tmp + fsync + os_file_replace).
 *
 * The routine writer is hand-made so the file reads like the documented
 * example: fixed key order, small objects on one line, durations and times
 * as plain strings. Strings are escaped exactly as xCDN-C's serializer
 * does, so parse(serialize(r)) == r.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astime_internal.h"
#include "xcdn.h"

static void put_str(core_buf *b, const char *s) {
  core_buf_appendc(b, '"');
  for (; s && *s; s++) {
    unsigned char ch = (unsigned char)*s;
    switch (ch) {
    case '\\': core_buf_appends(b, "\\\\"); break;
    case '"': core_buf_appends(b, "\\\""); break;
    case '\n': core_buf_appends(b, "\\n"); break;
    case '\r': core_buf_appends(b, "\\r"); break;
    case '\t': core_buf_appends(b, "\\t"); break;
    default:
      if (ch < 32)
        core_buf_printf(b, "\\u%04X", ch);
      else
        core_buf_appendc(b, (char)ch);
    }
  }
  core_buf_appendc(b, '"');
}

static void quiet_text(int start, int end, char out[32]) {
  snprintf(out, 32, "%02d:%02d-%02d:%02d", start / 60, start % 60, end / 60, end % 60);
}

astime_err astime_routine_serialize(const astime_routine *r, char **out_text) {
  core_buf b;
  char tmp[40], err[ASTIME_ERR_SIZE];
  size_t i;
  unsigned bit;
  bool first;
  astime_err e;
  if (!out_text) return ASTIME_ERR_INVALID;
  *out_text = NULL;
  e = astime_routine_validate(r, err);
  if (e != ASTIME_OK) return e;
  core_buf_init(&b);
  core_buf_appends(&b, "#routine {\n  name: ");
  put_str(&b, r->name);
  core_buf_printf(&b, ",\n  enabled: %s,\n", r->enabled ? "true" : "false");
  if (r->trigger.kind != ASTIME_TRIGGER_NONE) {
    core_buf_appends(&b, "  trigger: { ");
    switch (r->trigger.kind) {
    case ASTIME_TRIGGER_CRON:
      core_buf_appends(&b, "cron: ");
      put_str(&b, r->trigger.cron_text);
      break;
    case ASTIME_TRIGGER_EVERY:
      astime__format_duration(r->trigger.every_s, tmp);
      core_buf_appends(&b, "every: ");
      put_str(&b, tmp);
      break;
    default:
      astime__format_at(r->trigger.at, r->trigger.at_offset_s, tmp);
      core_buf_appends(&b, "at: ");
      put_str(&b, tmp);
      break;
    }
    core_buf_printf(&b, ", tz: \"%s\" },\n", astime_tz_name(r->trigger.tz));
  }
  if (r->watch_kind != ASTIME_WATCH_NONE) {
    core_buf_printf(&b, "  watch: { %s: ", r->watch_kind == ASTIME_WATCH_URL ? "url" : "file");
    put_str(&b, r->watch_target);
    astime__format_duration(r->watch_every_s, tmp);
    core_buf_appends(&b, ", every: ");
    put_str(&b, tmp);
    core_buf_appends(&b, " },\n");
  }
  core_buf_appends(&b, "  goal: ");
  put_str(&b, r->goal);
  core_buf_printf(&b, ",\n  mode: \"%s\",\n  profile: \"%s\",\n", astime_mode_name(r->mode),
                  astime_profile_name(r->profile));
  if (r->tools_set) {
    core_buf_appends(&b, "  tools: [");
    for (i = 0; i < r->tools_n; i++) {
      if (i) core_buf_appends(&b, ", ");
      put_str(&b, r->tools[i]);
    }
    core_buf_appends(&b, "],\n");
  }
  core_buf_appends(&b, "  providers: [");
  first = true;
  for (bit = ASTIME_PROVIDER_LOCAL; bit <= ASTIME_PROVIDER_CLAUDE_CODE; bit <<= 1) {
    if (!(r->providers & bit)) continue;
    core_buf_printf(&b, "%s\"%s\"", first ? "" : ", ", astime_provider_name(bit));
    first = false;
  }
  core_buf_printf(&b, "],\n  notify: \"%s\",\n", astime_notify_name(r->notify));
  if (r->quiet_set) {
    char q[32];
    quiet_text(r->quiet_start_min, r->quiet_end_min, q);
    core_buf_printf(&b, "  quiet_hours: \"%s\",\n", q);
  }
  if (r->deliver_channel) {
    core_buf_appends(&b, "  deliver: { channel: ");
    put_str(&b, r->deliver_channel);
    if (r->deliver_star) {
      core_buf_appends(&b, ", star: ");
      put_str(&b, r->deliver_star);
    }
    core_buf_appends(&b, " },\n");
  }
  if (r->max_runs_per_day > 0) core_buf_printf(&b, "  max_runs_per_day: %d,\n", r->max_runs_per_day);
  if (core_buf_appends(&b, "}\n") != CORE_OK) {
    core_buf_free(&b);
    return ASTIME_ERR_NOMEM;
  }
  *out_text = core_buf_detach(&b);
  return *out_text ? ASTIME_OK : ASTIME_ERR_NOMEM;
}

/* ── atomic write ─────────────────────────────────────────────────────── */

static astime_err write_atomic(const char *path, const char *text) {
  size_t plen = strlen(path), tlen = strlen(text);
  char *tmp = (char *)malloc(plen + sizeof ".tmp");
  FILE *f;
  bool ok;
  astime_err e;
  if (!tmp) return ASTIME_ERR_NOMEM;
  memcpy(tmp, path, plen);
  memcpy(tmp + plen, ".tmp", sizeof ".tmp");
  f = os_fopen(tmp, "wb");
  if (!f) {
    free(tmp);
    return ASTIME_ERR_IO;
  }
  ok = fwrite(text, 1, tlen, f) == tlen;
  if (ok) ok = os_fsync(f) == CORE_OK;
  if (fclose(f) != 0) ok = false;
  e = ok ? astime_from_core(os_file_replace(tmp, path)) : ASTIME_ERR_IO;
  if (e != ASTIME_OK) (void)os_remove_file(tmp);
  free(tmp);
  return e;
}

astime_err astime_routine_save(const char *dir, const astime_routine *r, char err[ASTIME_ERR_SIZE]) {
  char *text = NULL, *file = NULL, *path = NULL;
  astime_err e;
  if (!dir || !r) {
    astime__seterr(err, "routine: missing directory or routine");
    return ASTIME_ERR_INVALID;
  }
  e = astime_routine_validate(r, err);
  if (e != ASTIME_OK) return e;
  e = astime_routine_serialize(r, &text);
  if (e == ASTIME_OK) e = astime_from_core(os_mkdir_p(dir));
  if (e == ASTIME_OK) {
    size_t n = strlen(r->name);
    file = (char *)malloc(n + 6);
    if (file) {
      memcpy(file, r->name, n);
      memcpy(file + n, ".xcdn", 6);
      path = os_path_join(dir, file);
    }
    e = path ? write_atomic(path, text) : ASTIME_ERR_NOMEM;
  }
  if (e != ASTIME_OK) astime__seterr(err, "%s/%s.xcdn: cannot write (%s)", dir, r->name, astime_err_name(e));
  free(text);
  free(file);
  free(path);
  return e;
}

/* ── state ────────────────────────────────────────────────────────────── */

void astime_state_init(astime_state *st) {
  if (st) memset(st, 0, sizeof *st);
}

static char *state_path(const char *dir, const char *name) {
  core_buf b;
  core_buf_init(&b);
  if (core_buf_printf(&b, "%s/.state/%s.xcdn", dir, name) != CORE_OK) {
    core_buf_free(&b);
    return NULL;
  }
  return core_buf_detach(&b);
}

static void put_time(core_buf *b, const char *key, int64_t t) {
  char s[CORE_RFC3339_SIZE];
  if (!t) return;
  core_time_format_rfc3339((core_time)t, s);
  core_buf_printf(b, "  %s: \"%s\",\n", key, s);
}

astime_err astime_state_save(const char *dir, const char *name, const astime_state *st) {
  core_buf b;
  char *path, *sdir;
  astime_err e;
  if (!dir || !name || !st || !core_slug_valid(name)) return ASTIME_ERR_INVALID;
  sdir = os_path_join(dir, ".state");
  if (!sdir) return ASTIME_ERR_NOMEM;
  e = astime_from_core(os_mkdir_p(sdir));
  free(sdir);
  if (e != ASTIME_OK) return e;
  core_buf_init(&b);
  core_buf_appends(&b, "#routine_state {\n");
  put_time(&b, "armed_at", st->armed_at);
  if (st->trigger_sig[0]) {
    core_buf_appends(&b, "  trigger_sig: ");
    put_str(&b, st->trigger_sig);
    core_buf_appends(&b, ",\n");
  }
  if (st->watch_sig[0]) {
    core_buf_appends(&b, "  watch_sig: ");
    put_str(&b, st->watch_sig);
    core_buf_appends(&b, ",\n");
  }
  put_time(&b, "last_due_at", st->last_due_at);
  put_time(&b, "last_run_at", st->last_run_at);
  put_time(&b, "running_since", st->running_since);
  core_buf_printf(&b, "  last_status: \"%s\",\n", astime_status_name(st->last_status));
  if (st->last_error[0]) {
    core_buf_appends(&b, "  last_error: ");
    put_str(&b, st->last_error);
    core_buf_appends(&b, ",\n");
  }
  core_buf_printf(&b, "  runs_today: %d,\n", st->runs_today);
  if (st->runs_day[0]) core_buf_printf(&b, "  runs_day: \"%s\",\n", st->runs_day);
  core_buf_printf(&b, "  consecutive_failures: %d,\n", st->consecutive_failures);
  if (st->last_watch_hash[0]) core_buf_printf(&b, "  last_watch_hash: \"%s\",\n", st->last_watch_hash);
  put_time(&b, "last_watch_at", st->last_watch_at);
  put_time(&b, "next_due", st->next_due);
  put_time(&b, "defer_until", st->defer_until);
  if (core_buf_appends(&b, "}\n") != CORE_OK) {
    core_buf_free(&b);
    return ASTIME_ERR_NOMEM;
  }
  path = state_path(dir, name);
  e = path ? write_atomic(path, b.data) : ASTIME_ERR_NOMEM;
  free(path);
  core_buf_free(&b);
  return e;
}

static bool hex_ok(const char *s, size_t max) {
  size_t n = 0;
  for (; s[n]; n++)
    if (!((s[n] >= '0' && s[n] <= '9') || (s[n] >= 'a' && s[n] <= 'f'))) return false;
  return n > 0 && n <= max;
}

static bool day_ok(const char *s) {
  int i;
  for (i = 0; i < 10; i++) {
    if (i == 4 || i == 7) {
      if (s[i] != '-') return false;
    } else if (s[i] < '0' || s[i] > '9') {
      return false;
    }
  }
  return s[10] == '\0';
}

static const char *state_value(const xcdn_value_t *v) {
  if (!v) return NULL;
  if (v->type == XCDN_VAL_STRING || v->type == XCDN_VAL_DATETIME) return v->data.string;
  return NULL;
}

astime_err astime_state_load(const char *dir, const char *name, astime_state *out,
                             char err[ASTIME_ERR_SIZE]) {
  char *path, *text = NULL;
  xcdn_document_t *doc;
  xcdn_error_t xerr;
  const xcdn_node_t *root = NULL;
  astime_state st;
  astime_err e;
  size_t i;
  if (!dir || !name || !out) return ASTIME_ERR_INVALID;
  astime_state_init(out);
  path = state_path(dir, name);
  if (!path) return ASTIME_ERR_NOMEM;
  e = astime_from_core(os_read_file(path, &text, NULL));
  if (e != ASTIME_OK) {
    if (e != ASTIME_ERR_NOT_FOUND) astime__seterr(err, "%s: cannot read (%s)", path, astime_err_name(e));
    free(path);
    return e;
  }
  doc = xcdn_parse(text, &xerr);
  free(text);
  if (!doc) {
    astime__seterr(err, "%s:%zu: syntax error: %s", path, xerr.span.line, xerr.message);
    free(path);
    return ASTIME_ERR_PARSE;
  }
  for (i = 0; i < doc->values_len; i++)
    if (doc->values[i] && doc->values[i]->value && doc->values[i]->value->type == XCDN_VAL_OBJECT) {
      root = doc->values[i];
      break;
    }
  astime_state_init(&st);
  e = root ? ASTIME_OK : ASTIME_ERR_PARSE;
  if (!root) astime__seterr(err, "%s: no #routine_state object", path);
  for (i = 0; root && e == ASTIME_OK && i < xcdn_object_len(root->value); i++) {
    const char *k = xcdn_object_key_at(root->value, i);
    const xcdn_node_t *n = xcdn_object_node_at(root->value, i);
    const xcdn_value_t *v = n ? n->value : NULL;
    const char *s = state_value(v);
    int64_t *tp = NULL;
    if (!k || !v) continue;
    if (strcmp(k, "armed_at") == 0) tp = &st.armed_at;
    else if (strcmp(k, "last_due_at") == 0) tp = &st.last_due_at;
    else if (strcmp(k, "last_run_at") == 0) tp = &st.last_run_at;
    else if (strcmp(k, "running_since") == 0) tp = &st.running_since;
    else if (strcmp(k, "last_watch_at") == 0) tp = &st.last_watch_at;
    else if (strcmp(k, "next_due") == 0) tp = &st.next_due;
    else if (strcmp(k, "defer_until") == 0) tp = &st.defer_until;
    if (tp) {
      core_time t;
      if (!s || !core_time_parse_rfc3339(s, &t)) e = ASTIME_ERR_PARSE;
      else *tp = (int64_t)t;
    } else if (strcmp(k, "trigger_sig") == 0 || strcmp(k, "watch_sig") == 0) {
      if (!s || strlen(s) >= ASTIME_SIG_MAX) e = ASTIME_ERR_PARSE;
      else snprintf(k[0] == 't' ? st.trigger_sig : st.watch_sig, ASTIME_SIG_MAX, "%s", s);
    } else if (strcmp(k, "last_status") == 0) {
      int z, found = -1;
      for (z = ASTIME_STATUS_NONE; z <= ASTIME_STATUS_DEFERRED; z++)
        if (s && strcmp(s, astime_status_name((astime_status)z)) == 0) found = z;
      if (found < 0) e = ASTIME_ERR_PARSE;
      else st.last_status = (astime_status)found;
    } else if (strcmp(k, "last_error") == 0) {
      if (!s) e = ASTIME_ERR_PARSE;
      else astime__copy_short(st.last_error, sizeof st.last_error, s);
    } else if (strcmp(k, "runs_today") == 0 || strcmp(k, "consecutive_failures") == 0) {
      if (v->type != XCDN_VAL_INT || v->data.integer < 0 || v->data.integer > 1000000) e = ASTIME_ERR_PARSE;
      else if (k[0] == 'r') st.runs_today = (int)v->data.integer;
      else st.consecutive_failures = (int)v->data.integer;
    } else if (strcmp(k, "runs_day") == 0) {
      if (!s || !day_ok(s)) e = ASTIME_ERR_PARSE;
      else memcpy(st.runs_day, s, 11);
    } else if (strcmp(k, "last_watch_hash") == 0) {
      if (!s || !hex_ok(s, ASTIME_HASH_MAX - 1)) e = ASTIME_ERR_PARSE;
      else snprintf(st.last_watch_hash, sizeof st.last_watch_hash, "%s", s);
    }
    /* Unknown keys are ignored: the state is machine-written and newer
     * versions may add fields. */
    if (e != ASTIME_OK) astime__seterr(err, "%s: %s: invalid value", path, k);
  }
  xcdn_document_free(doc);
  free(path);
  if (e == ASTIME_OK) *out = st;
  return e;
}
