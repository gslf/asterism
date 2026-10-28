/* serve.xcdn: channels, speech, routines and stars. Every key is optional;
 * unknown keys are errors so a typo never silently disables a guard. */
#include "serve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xcdn.h"

static bool fail(char *err, size_t n, const char *what, const char *key) {
  snprintf(err, n, "serve.xcdn: %s%s%s", what, key ? ": " : "", key ? key : "");
  return false;
}

static bool known(const xcdn_value_t *obj, const char *const *keys, char *err,
                  size_t n) {
  for (size_t i = 0; i < xcdn_object_len(obj); i++) {
    const char *k = xcdn_object_key_at(obj, i);
    bool ok = false;
    for (size_t j = 0; keys[j] && !ok; j++) ok = !strcmp(k, keys[j]);
    if (!ok) return fail(err, n, "unknown key", k);
  }
  return true;
}

static const xcdn_value_t *field(const xcdn_value_t *obj, const char *key) {
  xcdn_node_t *node = obj ? xcdn_object_get(obj, key) : NULL;
  return node ? node->value : NULL;
}

static bool str(const xcdn_value_t *obj, const char *key, char *out, size_t size,
                char *err, size_t n) {
  const xcdn_value_t *v = field(obj, key);
  if (!v) return true;
  if (v->type != XCDN_VAL_STRING) return fail(err, n, "expected a string", key);
  const char *s = xcdn_value_as_string(v);
  if (strlen(s) >= size) return fail(err, n, "value too long", key);
  snprintf(out, size, "%s", s);
  return true;
}

static bool seconds(const xcdn_value_t *obj, const char *key, int lo, int hi,
                    int *out, char *err, size_t n) {
  const xcdn_value_t *v = field(obj, key);
  int64_t s;
  if (!v) return true;
  if (v->type == XCDN_VAL_INT) s = xcdn_value_as_int(v);
  else if ((v->type == XCDN_VAL_DURATION || v->type == XCDN_VAL_STRING) &&
           core_duration_parse(xcdn_value_as_string(v), &s)) {
  } else return fail(err, n, "expected seconds or an ISO 8601 duration", key);
  if (s < lo || s > hi) return fail(err, n, "duration out of range", key);
  *out = (int)s;
  return true;
}

static bool flag(const xcdn_value_t *obj, const char *key, bool *out, char *err,
                 size_t n) {
  const xcdn_value_t *v = field(obj, key);
  if (!v) return true;
  if (v->type != XCDN_VAL_BOOL) return fail(err, n, "expected true or false", key);
  *out = xcdn_value_as_bool(v);
  return true;
}

static bool mode_of(const char *s, asngn_usage_mode *out) {
  for (int m = ASNGN_USAGE_CHAT; m <= ASNGN_USAGE_AUTOMATE; m++)
    if (!strcmp(s, asngn_usage_mode_name((asngn_usage_mode)m))) {
      *out = (asngn_usage_mode)m;
      return true;
    }
  return false;
}

static bool profile_of(const char *s, asngn_security_profile *out) {
  for (int p = ASNGN_SECURITY_CHAT; p <= ASNGN_SECURITY_AUTOMATION_CI; p++)
    if (!strcmp(s, asngn_security_profile_name((asngn_security_profile)p))) {
      *out = (asngn_security_profile)p;
      return true;
    }
  return false;
}

void serve_config_defaults(serve_config *cfg) {
  memset(cfg, 0, sizeof *cfg);
  snprintf(cfg->token_env, sizeof cfg->token_env, "ASTERISM_TELEGRAM_TOKEN");
  cfg->poll_timeout_s = 50;
  cfg->topics = true;
  snprintf(cfg->routines_dir, sizeof cfg->routines_dir, "routines");
  cfg->routine_tick_s = 30;
  cfg->max_download_bytes = 20u * 1024u * 1024u;
  cfg->stars_n = 1;
  snprintf(cfg->stars[0].name, sizeof cfg->stars[0].name, "default");
  cfg->stars[0].mode = ASNGN_USAGE_CHAT;
  cfg->stars[0].profile = ASNGN_SECURITY_CHAT;
}

void serve_config_free(serve_config *cfg) {
  for (size_t i = 0; i < cfg->stt_argc; i++) free(cfg->stt_argv[i]);
  cfg->stt_argc = 0;
}

static bool load_star(serve_star_config *star, const xcdn_value_t *obj,
                      char *err, size_t n) {
  static const char *const keys[] = {"name", "root", "config", "token_env",
                                     "mode", "profile", "identity", NULL};
  char mode[16] = "", profile[24] = "";
  if (!obj || obj->type != XCDN_VAL_OBJECT) return fail(err, n, "a star must be an object", NULL);
  if (!known(obj, keys, err, n) ||
      !str(obj, "name", star->name, sizeof star->name, err, n) ||
      !str(obj, "root", star->root, sizeof star->root, err, n) ||
      !str(obj, "config", star->config, sizeof star->config, err, n) ||
      !str(obj, "token_env", star->token_env, sizeof star->token_env, err, n) ||
      !str(obj, "mode", mode, sizeof mode, err, n) ||
      !str(obj, "profile", profile, sizeof profile, err, n) ||
      !str(obj, "identity", star->identity, sizeof star->identity, err, n))
    return false;
  if (!core_slug_valid(star->name)) return fail(err, n, "invalid star name", star->name);
  star->mode = ASNGN_USAGE_CHAT;
  star->profile = ASNGN_SECURITY_CHAT;
  if (*mode && !mode_of(mode, &star->mode)) return fail(err, n, "unknown mode", mode);
  if (*profile && !profile_of(profile, &star->profile))
    return fail(err, n, "unknown profile", profile);
  if (!*profile && star->mode == ASNGN_USAGE_CODING) star->profile = ASNGN_SECURITY_CODING_SANDBOXED;
  if (!*profile && star->mode == ASNGN_USAGE_AUTOMATE) star->profile = ASNGN_SECURITY_AUTOMATION_CI;
  return true;
}

static bool load(serve_config *cfg, const xcdn_value_t *obj, char *err, size_t n) {
  static const char *const top[] = {"telegram", "speech", "routines",
                                    "downloads", "stars", NULL};
  static const char *const tg[] = {"token_env", "token_secret", "api_base", "poll_timeout",
                                    "topics", NULL};
  static const char *const sp[] = {"argv", "output_ext", NULL};
  static const char *const rt[] = {"dir", "tick", NULL};
  static const char *const dl[] = {"max_bytes", NULL};
  const xcdn_value_t *v;
  if (!known(obj, top, err, n)) return false;
  if ((v = field(obj, "telegram")) != NULL) {
    if (v->type != XCDN_VAL_OBJECT || !known(v, tg, err, n) ||
        !str(v, "token_env", cfg->token_env, sizeof cfg->token_env, err, n) ||
        !str(v, "token_secret", cfg->token_secret, sizeof cfg->token_secret, err, n) ||
        !str(v, "api_base", cfg->api_base, sizeof cfg->api_base, err, n) ||
        !seconds(v, "poll_timeout", 1, 120, &cfg->poll_timeout_s, err, n) ||
        !flag(v, "topics", &cfg->topics, err, n))
      return v->type != XCDN_VAL_OBJECT ? fail(err, n, "expected an object", "telegram") : false;
  }
  if ((v = field(obj, "speech")) != NULL) {
    const xcdn_value_t *argv;
    if (v->type != XCDN_VAL_OBJECT) return fail(err, n, "expected an object", "speech");
    if (!known(v, sp, err, n) ||
        !str(v, "output_ext", cfg->stt_output_ext, sizeof cfg->stt_output_ext, err, n))
      return false;
    argv = field(v, "argv");
    if (argv) {
      size_t len = argv->type == XCDN_VAL_ARRAY ? xcdn_array_len(argv) : 0;
      if (!len || len > 16) return fail(err, n, "speech.argv needs 1..16 strings", NULL);
      for (size_t i = 0; i < len; i++) {
        xcdn_node_t *node = xcdn_array_get(argv, i);
        if (!node || !node->value || node->value->type != XCDN_VAL_STRING)
          return fail(err, n, "speech.argv entries must be strings", NULL);
        cfg->stt_argv[i] = core_strdup(xcdn_value_as_string(node->value));
        if (!cfg->stt_argv[i]) return fail(err, n, "out of memory", NULL);
        cfg->stt_argc = i + 1;
      }
    }
  }
  if ((v = field(obj, "routines")) != NULL) {
    if (v->type != XCDN_VAL_OBJECT || !known(v, rt, err, n) ||
        !str(v, "dir", cfg->routines_dir, sizeof cfg->routines_dir, err, n) ||
        !seconds(v, "tick", 5, 3600, &cfg->routine_tick_s, err, n))
      return v->type != XCDN_VAL_OBJECT ? fail(err, n, "expected an object", "routines") : false;
  }
  if ((v = field(obj, "downloads")) != NULL) {
    const xcdn_value_t *m;
    if (v->type != XCDN_VAL_OBJECT || !known(v, dl, err, n))
      return v->type != XCDN_VAL_OBJECT ? fail(err, n, "expected an object", "downloads") : false;
    if ((m = field(v, "max_bytes")) != NULL) {
      if (m->type != XCDN_VAL_INT || xcdn_value_as_int(m) < 1024 ||
          xcdn_value_as_int(m) > 2000LL * 1024 * 1024)
        return fail(err, n, "downloads.max_bytes out of range", NULL);
      cfg->max_download_bytes = (uint64_t)xcdn_value_as_int(m);
    }
  }
  if ((v = field(obj, "stars")) != NULL) {
    size_t len = v->type == XCDN_VAL_ARRAY ? xcdn_array_len(v) : 0;
    if (!len || len > SERVE_STARS_MAX)
      return fail(err, n, "stars needs 1..8 entries", NULL);
    cfg->stars_n = 0;
    for (size_t i = 0; i < len; i++) {
      xcdn_node_t *node = xcdn_array_get(v, i);
      serve_star_config *star = &cfg->stars[cfg->stars_n];
      memset(star, 0, sizeof *star);
      if (!load_star(star, node ? node->value : NULL, err, n)) return false;
      for (size_t j = 0; j < cfg->stars_n; j++)
        if (!strcmp(cfg->stars[j].name, star->name))
          return fail(err, n, "duplicate star", star->name);
      cfg->stars_n++;
    }
    if (cfg->stars[0].root[0])
      return fail(err, n, "the first star is the default and uses the daemon root", NULL);
  }
  return true;
}

bool serve_config_load(serve_config *cfg, const char *root, const char *path,
                       char *err, size_t n) {
  char *own = NULL, *text = NULL;
  size_t len = 0;
  xcdn_error_t xe;
  xcdn_document_t *doc;
  const xcdn_value_t *obj = NULL;
  bool ok;
  if (!path) {
    own = os_path_join(root, "serve.xcdn");
    if (!own) return fail(err, n, "out of memory", NULL);
    if (!os_file_exists(own)) {
      free(own);
      return true;
    }
    path = own;
  }
  if (os_read_file(path, &text, &len) != CORE_OK) {
    snprintf(err, n, "serve.xcdn: cannot read %s", path);
    free(own);
    return false;
  }
  free(own);
  doc = xcdn_parse_str(text, len, &xe);
  free(text);
  if (!doc) {
    snprintf(err, n, "serve.xcdn: line %zu: %s", xe.span.line, xe.message);
    return false;
  }
  for (size_t i = 0; i < doc->values_len && !obj; i++)
    if (xcdn_node_has_tag(doc->values[i], "serve")) obj = doc->values[i]->value;
  if (!obj && doc->values_len) obj = doc->values[0]->value;
  ok = obj && obj->type == XCDN_VAL_OBJECT ? load(cfg, obj, err, n)
                                           : fail(err, n, "expected #serve { ... }", NULL);
  xcdn_document_free(doc);
  return ok;
}
