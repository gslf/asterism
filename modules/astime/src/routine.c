/*
 * routine.c — #routine definitions: parse, validate, copy, load.
 *
 * Parsing checks shapes and types and rejects unknown keys; the semantic
 * rules live in one validator shared with routines built in code (the
 * daemon creates them from chat). Messages carry "file:line: key.path:"
 * when the source text is known: xCDN keeps no positions in its AST, so the
 * line comes from a small key locator over the text.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astime_internal.h"
#include "xcdn.h"

#define GOAL_MAX 4000
#define TARGET_MAX 2048
#define TOOLS_MAX 64

/* ── lifecycle ────────────────────────────────────────────────────────── */

void astime_routine_init(astime_routine *r) {
  if (!r) return;
  memset(r, 0, sizeof *r);
  r->trigger.kind = ASTIME_TRIGGER_NONE;
  r->trigger.tz = ASTIME_TZ_LOCAL;
  r->mode = ASTIME_MODE_CHAT;
  r->profile = ASTIME_PROFILE_CHAT;
  r->providers = ASTIME_PROVIDER_LOCAL;
  r->notify = ASTIME_NOTIFY_ALWAYS;
}

void astime_routine_free(astime_routine *r) {
  size_t i;
  if (!r) return;
  free(r->name);
  free(r->watch_target);
  free(r->goal);
  for (i = 0; i < r->tools_n; i++) free(r->tools[i]);
  free(r->tools);
  free(r->deliver_channel);
  free(r->deliver_star);
  astime_routine_init(r);
}

static bool dup_opt(const char *s, char **out) {
  *out = NULL;
  if (!s) return true;
  *out = core_strdup(s);
  return *out != NULL;
}

astime_err astime_routine_copy(const astime_routine *src, astime_routine *dst) {
  size_t i;
  if (!src || !dst) return ASTIME_ERR_INVALID;
  *dst = *src;
  dst->name = dst->watch_target = dst->goal = NULL;
  dst->deliver_channel = dst->deliver_star = NULL;
  dst->tools = NULL;
  dst->tools_n = 0;
  if (!dup_opt(src->name, &dst->name) || !dup_opt(src->watch_target, &dst->watch_target) ||
      !dup_opt(src->goal, &dst->goal) || !dup_opt(src->deliver_channel, &dst->deliver_channel) ||
      !dup_opt(src->deliver_star, &dst->deliver_star))
    goto nomem;
  if (src->tools_n) {
    dst->tools = (char **)calloc(src->tools_n, sizeof *dst->tools);
    if (!dst->tools) goto nomem;
    for (i = 0; i < src->tools_n; i++) {
      dst->tools[i] = core_strdup(src->tools[i]);
      if (!dst->tools[i]) goto nomem;
      dst->tools_n = i + 1;
    }
  }
  return ASTIME_OK;
nomem:
  astime_routine_free(dst);
  return ASTIME_ERR_NOMEM;
}

/* ── messages ─────────────────────────────────────────────────────────── */

typedef struct {
  const char *src;  /* file name, may be NULL */
  const char *text; /* source text for line lookup, may be NULL */
  char *err;
} vctx;

static astime_err vfail(const vctx *v, const char *path, const char *fmt, ...) {
  char msg[ASTIME_ERR_SIZE];
  size_t line = 0;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  if (v->text && path) line = astime__locate_line(v->text, path);
  if (v->src && line)
    astime__seterr(v->err, "%s:%zu: %s: %s", v->src, line, path, msg);
  else if (v->src && path)
    astime__seterr(v->err, "%s: %s: %s", v->src, path, msg);
  else if (v->src)
    astime__seterr(v->err, "%s: %s", v->src, msg);
  else if (path)
    astime__seterr(v->err, "%s: %s", path, msg);
  else
    astime__seterr(v->err, "%s", msg);
  return ASTIME_ERR_INVALID;
}

/* ── validation ───────────────────────────────────────────────────────── */

static bool url_ok(const char *s) {
  const char *host;
  size_t i;
  if (core_str_has_prefix(s, "https://"))
    host = s + 8;
  else if (core_str_has_prefix(s, "http://"))
    host = s + 7;
  else
    return false;
  if (!*host || *host == '/') return false;
  for (i = 0; s[i]; i++)
    if ((unsigned char)s[i] <= ' ' || s[i] == 0x7F) return false;
  return true;
}

static bool text_ok(const char *s, size_t max) {
  size_t n = strlen(s);
  return n <= max && core_utf8_valid(s, n);
}

static bool single_line(const char *s) {
  for (; *s; s++)
    if ((unsigned char)*s < 0x20 || *s == 0x7F) return false;
  return true;
}

static astime_err validate(const vctx *v, const astime_routine *r) {
  const astime_trigger *t = &r->trigger;
  size_t i, j;
  if (!r->name) return vfail(v, "name", "missing (required)");
  if (!core_slug_valid(r->name))
    return vfail(v, "name", "\"%s\" is not a slug ([a-z0-9][a-z0-9-]{0,63})", r->name);
  if (!r->goal || core_str_blank(r->goal)) return vfail(v, "goal", "missing (required)");
  if (!text_ok(r->goal, GOAL_MAX))
    return vfail(v, "goal", "must be UTF-8 text of at most %d bytes", GOAL_MAX);
  if (t->kind == ASTIME_TRIGGER_NONE && r->watch_kind == ASTIME_WATCH_NONE)
    return vfail(v, "trigger", "missing: a routine needs a trigger, a watch, or both");
  if (t->tz != ASTIME_TZ_LOCAL && t->tz != ASTIME_TZ_UTC)
    return vfail(v, "trigger.tz", "must be \"local\" or \"utc\"");
  switch (t->kind) {
  case ASTIME_TRIGGER_CRON: {
    astime_cron c;
    char cerr[ASTIME_ERR_SIZE];
    if (astime_cron_parse(t->cron_text, &c, cerr) != ASTIME_OK)
      return vfail(v, "trigger.cron", "%s", cerr + (strncmp(cerr, "cron: ", 6) == 0 ? 6 : 0));
    if (memcmp(&c, &t->cron, sizeof c) != 0)
      return vfail(v, "trigger.cron", "parsed form does not match \"%s\"", t->cron_text);
    if (!astime__cron_satisfiable(&c))
      return vfail(v, "trigger.cron", "\"%s\" never fires (no such calendar date)", t->cron_text);
    break;
  }
  case ASTIME_TRIGGER_EVERY:
    if (t->every_s < ASTIME_MIN_INTERVAL)
      return vfail(v, "trigger.every", "must be at least PT1M");
    if (t->every_s > (int64_t)366 * 86400)
      return vfail(v, "trigger.every", "must be at most P366D");
    break;
  case ASTIME_TRIGGER_AT:
    if (t->at < INT64_C(946684800) || t->at > INT64_C(253402300799))
      return vfail(v, "trigger.at", "time out of range (years 2000-9999)");
    if (t->at_offset_s % 60 != 0 || t->at_offset_s < -86399 || t->at_offset_s > 86399)
      return vfail(v, "trigger.at", "invalid UTC offset");
    break;
  case ASTIME_TRIGGER_NONE:
    break;
  default:
    return vfail(v, "trigger", "unknown trigger kind");
  }
  if (r->watch_kind != ASTIME_WATCH_NONE) {
    if (r->watch_kind != ASTIME_WATCH_URL && r->watch_kind != ASTIME_WATCH_FILE)
      return vfail(v, "watch", "unknown watch kind");
    if (!r->watch_target || !*r->watch_target)
      return vfail(v, "watch", "needs \"url\" or \"file\"");
    if (strlen(r->watch_target) > TARGET_MAX || !core_utf8_valid(r->watch_target, strlen(r->watch_target)))
      return vfail(v, r->watch_kind == ASTIME_WATCH_URL ? "watch.url" : "watch.file",
                   "must be UTF-8 of at most %d bytes", TARGET_MAX);
    if (r->watch_kind == ASTIME_WATCH_URL && !url_ok(r->watch_target))
      return vfail(v, "watch.url", "\"%s\" is not an http(s) URL", r->watch_target);
    if (r->watch_kind == ASTIME_WATCH_FILE &&
        (!os_path_is_abs(r->watch_target) || !single_line(r->watch_target)))
      return vfail(v, "watch.file", "\"%s\" is not an absolute path", r->watch_target);
    if (r->watch_every_s < ASTIME_MIN_INTERVAL)
      return vfail(v, "watch.every", "%s",
                   r->watch_every_s ? "must be at least PT1M" : "missing (required with a watch)");
  } else if (r->watch_target) {
    return vfail(v, "watch", "target set without a watch kind");
  }
  if (r->mode < ASTIME_MODE_CHAT || r->mode > ASTIME_MODE_AUTOMATE)
    return vfail(v, "mode", "must be \"chat\", \"coding\" or \"automate\"");
  if (r->profile < ASTIME_PROFILE_CHAT || r->profile > ASTIME_PROFILE_AUTOMATION_CI)
    return vfail(v, "profile", "unknown profile");
  if (r->tools_n && !r->tools) return vfail(v, "tools", "missing list");
  if (r->tools_n > TOOLS_MAX) return vfail(v, "tools", "at most %d entries", TOOLS_MAX);
  for (i = 0; i < r->tools_n; i++) {
    if (!r->tools[i] || !core_slug_valid(r->tools[i]))
      return vfail(v, "tools", "\"%s\" is not a tool package id", r->tools[i] ? r->tools[i] : "");
    for (j = 0; j < i; j++)
      if (strcmp(r->tools[i], r->tools[j]) == 0)
        return vfail(v, "tools", "\"%s\" listed twice", r->tools[i]);
  }
  if (!r->tools_set && r->tools_n) return vfail(v, "tools", "entries without tools_set");
  if (r->providers == 0) return vfail(v, "providers", "must allow at least one provider");
  if (r->providers & ~(ASTIME_PROVIDER_LOCAL | ASTIME_PROVIDER_PAID))
    return vfail(v, "providers", "unknown provider bit");
  if (r->notify < ASTIME_NOTIFY_ALWAYS || r->notify > ASTIME_NOTIFY_NEVER)
    return vfail(v, "notify", "must be \"always\", \"on_change\" or \"never\"");
  if (r->quiet_set &&
      (r->quiet_start_min < 0 || r->quiet_start_min > 1439 || r->quiet_end_min < 0 ||
       r->quiet_end_min > 1440))
    return vfail(v, "quiet_hours", "times out of range");
  if (r->deliver_channel && !core_slug_valid(r->deliver_channel))
    return vfail(v, "deliver.channel", "\"%s\" is not a channel name", r->deliver_channel);
  if (r->deliver_star && !core_slug_valid(r->deliver_star))
    return vfail(v, "deliver.star", "\"%s\" is not a star name", r->deliver_star);
  if (r->deliver_star && !r->deliver_channel)
    return vfail(v, "deliver.channel", "missing (required with deliver.star)");
  if (r->max_runs_per_day < 0 || r->max_runs_per_day > 1440)
    return vfail(v, "max_runs_per_day", "must be between 1 and 1440");
  return ASTIME_OK;
}

astime_err astime_routine_validate(const astime_routine *r, char err[ASTIME_ERR_SIZE]) {
  vctx v;
  if (!r) {
    astime__seterr(err, "routine: missing");
    return ASTIME_ERR_INVALID;
  }
  v.src = NULL;
  v.text = NULL;
  v.err = err;
  return validate(&v, r);
}

/* ── parsing ──────────────────────────────────────────────────────────── */

static const char *TOP_KEYS =
    "name, enabled, trigger, watch, goal, mode, profile, tools, providers, notify, "
    "quiet_hours, deliver, max_runs_per_day";

static const char *type_name(xcdn_value_type_t t) {
  switch (t) {
  case XCDN_VAL_NULL: return "null";
  case XCDN_VAL_BOOL: return "a boolean";
  case XCDN_VAL_INT: return "an integer";
  case XCDN_VAL_FLOAT: return "a float";
  case XCDN_VAL_DECIMAL: return "a decimal";
  case XCDN_VAL_STRING: return "a string";
  case XCDN_VAL_BYTES: return "bytes";
  case XCDN_VAL_DATETIME: return "a datetime";
  case XCDN_VAL_DURATION: return "a duration";
  case XCDN_VAL_UUID: return "a uuid";
  case XCDN_VAL_ARRAY: return "a list";
  case XCDN_VAL_OBJECT: return "an object";
  }
  return "an unknown value";
}

static astime_err type_fail(const vctx *v, const char *path, const char *want,
                            const xcdn_value_t *val) {
  return vfail(v, path, "expected %s, got %s", want, type_name(val->type));
}

static astime_err get_string(const vctx *v, const char *path, const xcdn_value_t *val,
                             const char **out) {
  if (val->type != XCDN_VAL_STRING || !val->data.string)
    return type_fail(v, path, "a string", val);
  if (!core_utf8_valid(val->data.string, strlen(val->data.string)))
    return vfail(v, path, "not valid UTF-8");
  *out = val->data.string;
  return ASTIME_OK;
}

static astime_err set_string(const vctx *v, const char *path, const xcdn_value_t *val,
                             char **dst) {
  const char *s = NULL;
  astime_err e = get_string(v, path, val, &s);
  if (e != ASTIME_OK) return e;
  free(*dst);
  *dst = core_strdup(s);
  return *dst ? ASTIME_OK : ASTIME_ERR_NOMEM;
}

static astime_err get_duration(const vctx *v, const char *path, const xcdn_value_t *val,
                               int64_t *out) {
  const char *s;
  if ((val->type != XCDN_VAL_STRING && val->type != XCDN_VAL_DURATION) || !val->data.string)
    return type_fail(v, path, "an ISO 8601 duration such as \"PT30M\"", val);
  s = val->data.string;
  if (!core_duration_parse(s, out) || *out <= 0)
    return vfail(v, path, "invalid ISO 8601 duration \"%s\" (e.g. \"PT30M\", \"PT2H\", \"P1D\")", s);
  return ASTIME_OK;
}

static astime_err get_enum(const vctx *v, const char *path, const xcdn_value_t *val,
                           const char *const *names, int n, int *out) {
  const char *s = NULL;
  int i;
  core_buf b;
  astime_err e = get_string(v, path, val, &s);
  if (e != ASTIME_OK) return e;
  for (i = 0; i < n; i++)
    if (strcmp(s, names[i]) == 0) {
      *out = i;
      return ASTIME_OK;
    }
  core_buf_init(&b);
  for (i = 0; i < n; i++) core_buf_printf(&b, "%s\"%s\"", i ? (i + 1 == n ? " or " : ", ") : "", names[i]);
  e = vfail(v, path, "unknown value \"%s\" (expected %s)", s, b.data ? b.data : "");
  core_buf_free(&b);
  return e;
}

static const char *const MODES[] = {"chat", "coding", "automate"};
static const char *const PROFILES[] = {"chat", "coding-readonly", "coding-sandboxed", "automation-ci"};
static const char *const NOTIFY[] = {"always", "on_change", "never"};
static const char *const PROVIDERS[] = {"local", "chatgpt", "codex", "claude-code"};
static const char *const TZS[] = {"local", "utc"};

static astime_err parse_trigger(const vctx *v, const xcdn_value_t *obj, astime_routine *r) {
  size_t i, kinds = 0;
  astime_trigger *t = &r->trigger;
  if (obj->type != XCDN_VAL_OBJECT) return type_fail(v, "trigger", "an object", obj);
  for (i = 0; i < xcdn_object_len(obj); i++) {
    const char *k = xcdn_object_key_at(obj, i);
    const xcdn_node_t *n = xcdn_object_node_at(obj, i);
    const xcdn_value_t *val = n ? n->value : NULL;
    char path[80];
    astime_err e;
    if (!k || !val) continue;
    snprintf(path, sizeof path, "trigger.%s", k);
    if (strcmp(k, "cron") == 0) {
      const char *s;
      char cerr[ASTIME_ERR_SIZE];
      astime_tz keep = t->tz;
      if ((e = get_string(v, path, val, &s)) != ASTIME_OK) return e;
      if (astime_trigger_set_cron(t, s, keep, cerr) != ASTIME_OK)
        return vfail(v, path, "%s", cerr + (strncmp(cerr, "cron: ", 6) == 0 ? 6 : 0));
      kinds++;
    } else if (strcmp(k, "every") == 0) {
      if ((e = get_duration(v, path, val, &t->every_s)) != ASTIME_OK) return e;
      t->kind = ASTIME_TRIGGER_EVERY;
      kinds++;
    } else if (strcmp(k, "at") == 0) {
      if ((val->type != XCDN_VAL_STRING && val->type != XCDN_VAL_DATETIME) || !val->data.string)
        return type_fail(v, path, "an RFC 3339 time such as \"2026-11-02T09:00:00+01:00\"", val);
      if (!astime__parse_at(val->data.string, &t->at, &t->at_offset_s))
        return vfail(v, path, "invalid RFC 3339 time \"%s\" (e.g. \"2026-11-02T09:00:00+01:00\")",
                     val->data.string);
      t->kind = ASTIME_TRIGGER_AT;
      kinds++;
    } else if (strcmp(k, "tz") == 0) {
      int z = 0;
      if ((e = get_enum(v, path, val, TZS, 2, &z)) != ASTIME_OK) return e;
      t->tz = (astime_tz)z;
    } else {
      return vfail(v, path, "unknown key \"%s\" (trigger keys: cron, every, at, tz)", k);
    }
  }
  if (kinds == 0) return vfail(v, "trigger", "needs one of \"cron\", \"every\" or \"at\"");
  if (kinds > 1) return vfail(v, "trigger", "\"cron\", \"every\" and \"at\" are exclusive");
  return ASTIME_OK;
}

static astime_err parse_watch(const vctx *v, const xcdn_value_t *obj, astime_routine *r) {
  size_t i;
  if (obj->type != XCDN_VAL_OBJECT) return type_fail(v, "watch", "an object", obj);
  for (i = 0; i < xcdn_object_len(obj); i++) {
    const char *k = xcdn_object_key_at(obj, i);
    const xcdn_node_t *n = xcdn_object_node_at(obj, i);
    const xcdn_value_t *val = n ? n->value : NULL;
    char path[80];
    astime_err e;
    if (!k || !val) continue;
    snprintf(path, sizeof path, "watch.%s", k);
    if (strcmp(k, "url") == 0 || strcmp(k, "file") == 0) {
      if (r->watch_kind != ASTIME_WATCH_NONE)
        return vfail(v, path, "\"url\" and \"file\" are exclusive");
      if ((e = set_string(v, path, val, &r->watch_target)) != ASTIME_OK) return e;
      r->watch_kind = k[0] == 'u' ? ASTIME_WATCH_URL : ASTIME_WATCH_FILE;
    } else if (strcmp(k, "every") == 0) {
      if ((e = get_duration(v, path, val, &r->watch_every_s)) != ASTIME_OK) return e;
    } else {
      return vfail(v, path, "unknown key \"%s\" (watch keys: url, file, every)", k);
    }
  }
  if (r->watch_kind == ASTIME_WATCH_NONE) return vfail(v, "watch", "needs \"url\" or \"file\"");
  return ASTIME_OK;
}

static astime_err parse_deliver(const vctx *v, const xcdn_value_t *obj, astime_routine *r) {
  size_t i;
  if (obj->type != XCDN_VAL_OBJECT) return type_fail(v, "deliver", "an object", obj);
  for (i = 0; i < xcdn_object_len(obj); i++) {
    const char *k = xcdn_object_key_at(obj, i);
    const xcdn_node_t *n = xcdn_object_node_at(obj, i);
    const xcdn_value_t *val = n ? n->value : NULL;
    char path[80];
    astime_err e;
    if (!k || !val) continue;
    snprintf(path, sizeof path, "deliver.%s", k);
    if (strcmp(k, "channel") == 0) {
      if ((e = set_string(v, path, val, &r->deliver_channel)) != ASTIME_OK) return e;
    } else if (strcmp(k, "star") == 0) {
      if ((e = set_string(v, path, val, &r->deliver_star)) != ASTIME_OK) return e;
    } else {
      return vfail(v, path, "unknown key \"%s\" (deliver keys: channel, star)", k);
    }
  }
  if (!r->deliver_channel) return vfail(v, "deliver.channel", "missing (required)");
  return ASTIME_OK;
}

static astime_err parse_tools(const vctx *v, const xcdn_value_t *arr, astime_routine *r) {
  size_t i, n;
  if (arr->type != XCDN_VAL_ARRAY) return type_fail(v, "tools", "a list of strings", arr);
  n = xcdn_array_len(arr);
  if (n > TOOLS_MAX) return vfail(v, "tools", "at most %d entries", TOOLS_MAX);
  r->tools_set = true;
  if (n == 0) return ASTIME_OK;
  r->tools = (char **)calloc(n, sizeof *r->tools);
  if (!r->tools) return ASTIME_ERR_NOMEM;
  for (i = 0; i < n; i++) {
    const xcdn_node_t *node = xcdn_array_get(arr, i);
    const xcdn_value_t *val = node ? node->value : NULL;
    if (!val || val->type != XCDN_VAL_STRING || !val->data.string)
      return vfail(v, "tools", "entry %zu: expected a string", i + 1);
    r->tools[i] = core_strdup(val->data.string);
    if (!r->tools[i]) return ASTIME_ERR_NOMEM;
    r->tools_n = i + 1;
  }
  return ASTIME_OK;
}

static astime_err parse_providers(const vctx *v, const xcdn_value_t *arr, astime_routine *r) {
  size_t i, n;
  if (arr->type != XCDN_VAL_ARRAY) return type_fail(v, "providers", "a list of strings", arr);
  n = xcdn_array_len(arr);
  r->providers = 0;
  for (i = 0; i < n; i++) {
    const xcdn_node_t *node = xcdn_array_get(arr, i);
    const xcdn_value_t *val = node ? node->value : NULL;
    int k, found = -1;
    if (!val || val->type != XCDN_VAL_STRING || !val->data.string)
      return vfail(v, "providers", "entry %zu: expected a string", i + 1);
    for (k = 0; k < 4; k++)
      if (strcmp(val->data.string, PROVIDERS[k]) == 0) found = k;
    if (found < 0)
      return vfail(v, "providers", "unknown provider \"%s\" (expected \"local\", \"chatgpt\", "
                                   "\"codex\" or \"claude-code\")", val->data.string);
    if (r->providers & (1u << found))
      return vfail(v, "providers", "\"%s\" listed twice", val->data.string);
    r->providers |= 1u << found;
  }
  if (r->providers == 0) return vfail(v, "providers", "must allow at least one provider");
  return ASTIME_OK;
}

static astime_err parse_object(const vctx *v, const xcdn_value_t *obj, astime_routine *r) {
  size_t i;
  bool profile_set = false;
  for (i = 0; i < xcdn_object_len(obj); i++) {
    const char *k = xcdn_object_key_at(obj, i);
    const xcdn_node_t *n = xcdn_object_node_at(obj, i);
    const xcdn_value_t *val = n ? n->value : NULL;
    astime_err e = ASTIME_OK;
    int z = 0;
    if (!k || !val) continue;
    if (strcmp(k, "name") == 0) {
      e = set_string(v, k, val, &r->name);
    } else if (strcmp(k, "enabled") == 0) {
      if (val->type != XCDN_VAL_BOOL) return type_fail(v, k, "true or false", val);
      r->enabled = val->data.boolean;
    } else if (strcmp(k, "trigger") == 0) {
      e = parse_trigger(v, val, r);
    } else if (strcmp(k, "watch") == 0) {
      e = parse_watch(v, val, r);
    } else if (strcmp(k, "goal") == 0) {
      e = set_string(v, k, val, &r->goal);
    } else if (strcmp(k, "mode") == 0) {
      if ((e = get_enum(v, k, val, MODES, 3, &z)) == ASTIME_OK) r->mode = (astime_mode)z;
    } else if (strcmp(k, "profile") == 0) {
      if ((e = get_enum(v, k, val, PROFILES, 4, &z)) == ASTIME_OK) r->profile = (astime_profile)z;
      profile_set = true;
    } else if (strcmp(k, "tools") == 0) {
      e = parse_tools(v, val, r);
    } else if (strcmp(k, "providers") == 0) {
      e = parse_providers(v, val, r);
    } else if (strcmp(k, "notify") == 0) {
      if ((e = get_enum(v, k, val, NOTIFY, 3, &z)) == ASTIME_OK) r->notify = (astime_notify)z;
    } else if (strcmp(k, "quiet_hours") == 0) {
      const char *s;
      if ((e = get_string(v, k, val, &s)) == ASTIME_OK) {
        if (!astime__parse_quiet(s, &r->quiet_start_min, &r->quiet_end_min))
          return vfail(v, k, "expected \"HH:MM-HH:MM\" (e.g. \"22:00-07:00\"), got \"%s\"", s);
        r->quiet_set = true;
      }
    } else if (strcmp(k, "deliver") == 0) {
      e = parse_deliver(v, val, r);
    } else if (strcmp(k, "max_runs_per_day") == 0) {
      if (val->type != XCDN_VAL_INT) return type_fail(v, k, "an integer", val);
      if (val->data.integer < 1 || val->data.integer > 1440)
        return vfail(v, k, "%lld out of range 1-1440", (long long)val->data.integer);
      r->max_runs_per_day = (int)val->data.integer;
    } else {
      return vfail(v, k, "unknown key \"%s\" (routine keys: %s)", k, TOP_KEYS);
    }
    if (e != ASTIME_OK) return e;
  }
  if (!profile_set) /* the engine's mapping: chat, coding-sandboxed, automation-ci */
    r->profile = r->mode == ASTIME_MODE_CHAT     ? ASTIME_PROFILE_CHAT
                 : r->mode == ASTIME_MODE_CODING ? ASTIME_PROFILE_CODING_SANDBOXED
                                                 : ASTIME_PROFILE_AUTOMATION_CI;
  return ASTIME_OK;
}

astime_err astime_routine_parse(const char *text, const char *source, const char *expected_name,
                                astime_routine *out, char err[ASTIME_ERR_SIZE]) {
  xcdn_document_t *doc;
  xcdn_error_t xerr;
  const xcdn_node_t *root = NULL;
  astime_routine r;
  astime_err e;
  vctx v;
  size_t i;
  v.src = source;
  v.text = text;
  v.err = err;
  if (!text || !out) {
    astime__seterr(err, "routine: missing text");
    return ASTIME_ERR_INVALID;
  }
  astime_routine_init(out);
  doc = xcdn_parse(text, &xerr);
  if (!doc) {
    astime__seterr(err, "%s:%zu:%zu: syntax error: %s", source ? source : "<routine>",
                   xerr.span.line, xerr.span.column, xerr.message);
    return xerr.kind == XCDN_ERR_OUT_OF_MEMORY ? ASTIME_ERR_NOMEM : ASTIME_ERR_PARSE;
  }
  for (i = 0; i < doc->values_len; i++) {
    const xcdn_node_t *n = doc->values[i];
    if (n && n->value && n->value->type == XCDN_VAL_OBJECT) {
      if (root) {
        xcdn_document_free(doc);
        return vfail(&v, NULL, "more than one routine in one file");
      }
      root = n;
    }
  }
  if (!root) {
    xcdn_document_free(doc);
    return vfail(&v, NULL, "no #routine object found");
  }
  if (xcdn_node_tag_count(root) > 0 &&
      (xcdn_node_tag_count(root) > 1 || !xcdn_node_has_tag(root, "routine"))) {
    e = vfail(&v, NULL, "unexpected tag #%s (expected #routine)",
              xcdn_node_has_tag(root, "routine") ? xcdn_node_tag_at(root, 1) : xcdn_node_tag_at(root, 0));
    xcdn_document_free(doc);
    return e;
  }
  astime_routine_init(&r);
  e = parse_object(&v, root->value, &r);
  xcdn_document_free(doc);
  if (e == ASTIME_OK) e = validate(&v, &r);
  if (e == ASTIME_OK && expected_name && strcmp(r.name, expected_name) != 0)
    e = vfail(&v, "name", "\"%s\" must equal the file name \"%s\"", r.name, expected_name);
  if (e != ASTIME_OK) {
    if (e == ASTIME_ERR_NOMEM) astime__seterr(err, "%s: out of memory", source ? source : "routine");
    astime_routine_free(&r);
    return e;
  }
  *out = r;
  return ASTIME_OK;
}

/* "dir/name.xcdn" -> "name" (malloc'd), NULL when not *.xcdn. */
static char *name_from_path(const char *path) {
  const char *base = path, *p;
  size_t n;
  for (p = path; *p; p++)
    if (*p == '/' || *p == '\\') base = p + 1;
  n = strlen(base);
  if (n <= 5 || strcmp(base + n - 5, ".xcdn") != 0) return NULL;
  return core_strndup(base, n - 5);
}

astime_err astime_routine_load(const char *path, astime_routine *out, char err[ASTIME_ERR_SIZE]) {
  char *text = NULL, *name;
  astime_err e;
  if (!path || !out) {
    astime__seterr(err, "routine: missing path");
    return ASTIME_ERR_INVALID;
  }
  astime_routine_init(out);
  name = name_from_path(path);
  if (!name) {
    astime__seterr(err, "%s: routine files are named <name>.xcdn", path);
    return ASTIME_ERR_INVALID;
  }
  e = astime_from_core(os_read_file(path, &text, NULL));
  if (e != ASTIME_OK) {
    astime__seterr(err, "%s: cannot read (%s)", path, astime_err_name(e));
    free(name);
    return e;
  }
  if (!core_slug_valid(name)) {
    astime__seterr(err, "%s: file name \"%s\" is not a slug ([a-z0-9][a-z0-9-]{0,63})", path, name);
    e = ASTIME_ERR_INVALID;
  } else {
    e = astime_routine_parse(text, path, name, out, err);
  }
  free(text);
  free(name);
  return e;
}

static int cmp_names(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

astime_err astime_load_dir(const char *dir, astime_routine **out, size_t *out_n,
                           astime_strings *issues) {
  char **names = NULL;
  size_t n = 0, i, got = 0;
  astime_routine *list = NULL;
  astime_err e;
  if (!dir || !out || !out_n) return ASTIME_ERR_INVALID;
  *out = NULL;
  *out_n = 0;
  e = astime_from_core(os_list_dir(dir, &names, &n));
  if (e != ASTIME_OK) return e;
  if (n > 1) qsort(names, n, sizeof *names, cmp_names); /* names may be NULL */
  if (n) {
    list = (astime_routine *)calloc(n, sizeof *list);
    if (!list) {
      os_free_names(names, n);
      return ASTIME_ERR_NOMEM;
    }
  }
  for (i = 0; i < n; i++) {
    size_t len = strlen(names[i]);
    char *path, err[ASTIME_ERR_SIZE];
    if (names[i][0] == '.' || len <= 5 || strcmp(names[i] + len - 5, ".xcdn") != 0) continue;
    path = os_path_join(dir, names[i]);
    if (!path) {
      e = ASTIME_ERR_NOMEM;
      break;
    }
    err[0] = '\0';
    e = astime_routine_load(path, &list[got], err);
    free(path);
    if (e == ASTIME_ERR_NOMEM) break;
    if (e == ASTIME_OK) {
      got++;
    } else if (issues && astime__strings_pushf(issues, "%s", err) != ASTIME_OK) {
      e = ASTIME_ERR_NOMEM;
      break;
    }
    e = ASTIME_OK;
  }
  os_free_names(names, n);
  if (e != ASTIME_OK) {
    astime_routines_free(list, got);
    return e;
  }
  if (got == 0) {
    free(list);
    list = NULL;
  }
  *out = list;
  *out_n = got;
  return ASTIME_OK;
}

void astime_routines_free(astime_routine *list, size_t n) {
  size_t i;
  if (!list) return;
  for (i = 0; i < n; i++) astime_routine_free(&list[i]);
  free(list);
}
