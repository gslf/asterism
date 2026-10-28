/*
 * delegate_events.c — defensive parsers of the Claude Code stream-json and
 * Codex `exec --json` event streams (shapes in delegate.h).
 *
 * Every line is untrusted output of another program: it is parsed with the
 * strict RFC 8259 codec, accessed only through type-checked getters, and
 * every kept text is capped on a UTF-8 boundary. Unknown event types and
 * fields are ignored; a line that is not a JSON object is counted.
 */

#include "delegate.h"

#include "asmodel_json.h"
#include "core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- small helpers ------------------------------------------------------ */

size_t dlg_utf8_cut(const char *s, size_t len, size_t cap) {
  size_t i, back = 0;
  if (!s || len <= cap) return len;
  i = cap;
  /* s[i] exists (i < len): step back over at most 3 continuation bytes so
   * the cut lands before the lead byte of a split sequence. */
  while (i > 0 && back < 3 && ((unsigned char)s[i] & 0xC0u) == 0x80u) {
    i--;
    back++;
  }
  return i;
}

static int lower_ascii(int ch) {
  return (ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch;
}

static int ci_contains(const char *hay, const char *needle) {
  size_t i, j, n;
  if (!hay || !needle) return 0;
  n = strlen(needle);
  if (n == 0) return 1;
  for (i = 0; hay[i] != '\0'; i++) {
    for (j = 0; j < n; j++) {
      if (hay[i + j] == '\0' ||
          lower_ascii((unsigned char)hay[i + j]) !=
              lower_ascii((unsigned char)needle[j]))
        break;
    }
    if (j == n) return 1;
  }
  return 0;
}

int dlg_text_is_limit(const char *text) {
  static const char *const needles[] = {
      "usage limit",    "rate limit",        "rate_limit",
      "rate-limit",     "hit your limit",    "limit reached",
      "limit exceeded", "too many requests", "quota"};
  size_t i;
  for (i = 0; i < sizeof needles / sizeof needles[0]; i++)
    if (ci_contains(text, needles[i])) return 1;
  return 0;
}

int dlg_text_is_auth(const char *text) {
  static const char *const needles[] = {
      "not logged in",          "/login",
      "invalid api key",        "authentication_failed",
      "authentication failed",  "unauthorized",
      "codex login",            "oauth_org_not_allowed",
      "token has expired",      "login required"};
  size_t i;
  for (i = 0; i < sizeof needles / sizeof needles[0]; i++)
    if (ci_contains(text, needles[i])) return 1;
  return 0;
}

/* Capped copy on a UTF-8 boundary; *cut (optional) reports truncation. */
static char *dup_capped(const char *s, size_t cap, int *cut) {
  size_t len, keep;
  if (cut) *cut = 0;
  if (!s) return NULL;
  len = strlen(s);
  keep = dlg_utf8_cut(s, len, cap);
  if (cut && keep < len) *cut = 1;
  return core_strndup(s, keep);
}

/* Replace *slot with a capped copy of v (NULL v leaves *slot alone). */
static void set_str(char **slot, const char *v, size_t cap) {
  char *copy;
  if (!v) return;
  copy = dup_capped(v, cap, NULL);
  if (!copy) return; /* allocation failure only loses text */
  free(*slot);
  *slot = copy;
}

static const asmodel_json_value *jget(const asmodel_json_value *o,
                                      const char *k) {
  if (asmodel_json_typeof(o) != ASMODEL_JSON_OBJECT) return NULL;
  return asmodel_json_object_get(o, k);
}

static const char *jstr(const asmodel_json_value *o, const char *k) {
  return asmodel_json_string_value(jget(o, k));
}

/* Non-negative integral member; 0 when absent or of another type. */
static int64_t jcount(const asmodel_json_value *o, const char *k) {
  const asmodel_json_value *v = jget(o, k);
  long long n;
  if (asmodel_json_typeof(v) != ASMODEL_JSON_NUMBER || !asmodel_json_is_int(v))
    return 0;
  n = asmodel_json_int_value(v);
  return n > 0 ? (int64_t)n : 0;
}

static int jnumber(const asmodel_json_value *o, const char *k, double *out) {
  const asmodel_json_value *v = jget(o, k);
  if (asmodel_json_typeof(v) != ASMODEL_JSON_NUMBER) return 0;
  *out = asmodel_json_double_value(v);
  return 1;
}

static int64_t sat_add(int64_t a, int64_t b) {
  return a > INT64_MAX - b ? INT64_MAX : a + b;
}

static void push_chunk(dlg_state *s, const char *text) {
  char *copy;
  if (!text || text[0] == '\0') return;
  copy = dup_capped(text, DLG_CHUNK_CAP, NULL);
  if (!copy) return;
  free(s->chunks[s->chunk_next]);
  s->chunks[s->chunk_next] = copy;
  s->chunk_next = (s->chunk_next + 1) % DLG_TAIL_CHUNKS;
  if (s->chunk_count < DLG_TAIL_CHUNKS) s->chunk_count++;
}

static void set_final(dlg_state *s, const char *text) {
  int cut = 0;
  char *copy;
  if (!text) return;
  copy = dup_capped(text, DLG_FINAL_CAP, &cut);
  if (!copy) return;
  free(s->final);
  s->final = copy;
  s->final_truncated = cut;
}

/* hit: the limit stopped (or will stop) the run; resets: unix seconds. */
static void note_limit(dlg_state *s, int hit, const char *message,
                       int64_t resets) {
  s->limit_seen = 1;
  if (hit) s->limit_hit = 1;
  /* A stopping limit's message wins over an earlier warning. */
  if (message && (hit || !s->limit_message))
    set_str(&s->limit_message, message, DLG_LIMIT_CAP);
  if (resets > 0) s->limit_resets_at = resets;
}

/* resetsAt is documented as a number; accept seconds or milliseconds. */
static int64_t epoch_seconds(double v) {
  if (!(v > 0) || v > 9.0e15) return 0;
  if (v > 1.0e11) v /= 1000.0;
  return (int64_t)v;
}

void dlg_state_init(dlg_state *s, dlg_agent agent) {
  memset(s, 0, sizeof *s);
  s->agent = agent;
}

void dlg_state_free(dlg_state *s) {
  size_t i;
  if (!s) return;
  free(s->session_id);
  free(s->model);
  free(s->subtype);
  free(s->final);
  free(s->error);
  free(s->limit_message);
  for (i = 0; i < DLG_TAIL_CHUNKS; i++) free(s->chunks[i]);
  memset(s, 0, sizeof *s);
}

char *dlg_tail_text(const dlg_state *s) {
  core_buf b;
  size_t i, idx;
  core_buf_init(&b);
  idx = (s->chunk_next + DLG_TAIL_CHUNKS - s->chunk_count) % DLG_TAIL_CHUNKS;
  for (i = 0; i < s->chunk_count; i++) {
    const char *c = s->chunks[(idx + i) % DLG_TAIL_CHUNKS];
    if (!c) continue;
    if ((b.len > 0 && core_buf_appends(&b, "\n\n") != CORE_OK) ||
        core_buf_appends(&b, c) != CORE_OK) {
      core_buf_free(&b);
      return NULL;
    }
  }
  return core_buf_detach(&b);
}

/* ---- Claude Code stream-json ------------------------------------------- */

/* Text blocks of an assistant message joined by blank lines (malloc'd;
 * NULL when there is none). */
static char *message_text(const asmodel_json_value *ev) {
  const asmodel_json_value *content = jget(jget(ev, "message"), "content");
  core_buf b;
  size_t i, n;
  if (asmodel_json_typeof(content) != ASMODEL_JSON_ARRAY) return NULL;
  core_buf_init(&b);
  n = asmodel_json_array_len(content);
  for (i = 0; i < n; i++) {
    const asmodel_json_value *blk = asmodel_json_array_at(content, i);
    const char *type = jstr(blk, "type"), *text = jstr(blk, "text");
    if (!type || strcmp(type, "text") != 0 || !text || text[0] == '\0')
      continue;
    if (b.len >= DLG_FINAL_CAP) break;
    if ((b.len > 0 && core_buf_appends(&b, "\n\n") != CORE_OK) ||
        core_buf_appends(&b, text) != CORE_OK) {
      core_buf_free(&b);
      return NULL;
    }
  }
  if (b.len == 0) {
    core_buf_free(&b);
    return NULL;
  }
  return core_buf_detach(&b);
}

static int is_auth_category(const char *e) {
  return e && (!strcmp(e, "authentication_failed") ||
               !strcmp(e, "oauth_org_not_allowed"));
}

static void claude_system(dlg_state *s, const asmodel_json_value *ev) {
  const char *sub = jstr(ev, "subtype");
  if (!sub) return;
  if (!strcmp(sub, "init")) {
    set_str(&s->session_id, jstr(ev, "session_id"), 256);
    set_str(&s->model, jstr(ev, "model"), 256);
  } else if (!strcmp(sub, "api_retry")) {
    const char *err = jstr(ev, "error");
    /* Claude Code retries by itself; this is a warning, not a stop. */
    if (err && !strcmp(err, "rate_limit"))
      note_limit(s, 0, "rate limited by the API; Claude Code is retrying", 0);
  }
}

static void claude_assistant(dlg_state *s, const asmodel_json_value *ev) {
  const asmodel_json_value *parent = jget(ev, "parent_tool_use_id");
  const char *err = jstr(ev, "error");
  char *text = message_text(ev);
  /* Subagent messages carry the id of the tool call that started them. */
  if (asmodel_json_typeof(parent) == ASMODEL_JSON_NULL) push_chunk(s, text);
  if (err) {
    const char *what = text ? text : err;
    if (!strcmp(err, "rate_limit") || !strcmp(err, "billing_error"))
      note_limit(s, 1, what, 0);
    else if (is_auth_category(err))
      s->auth_error = 1;
    set_str(&s->error, what, DLG_ERROR_CAP);
  }
  free(text);
}

static void claude_rate_limit(dlg_state *s, const asmodel_json_value *ev) {
  const asmodel_json_value *info = jget(ev, "rate_limit_info");
  const char *status = jstr(info, "status");
  const char *code = jstr(info, "errorCode");
  double v = 0;
  int64_t resets = jnumber(info, "resetsAt", &v) ? epoch_seconds(v) : 0;
  char msg[160];
  if (!status) return;
  if (!strcmp(status, "rejected")) {
    if (code)
      snprintf(msg, sizeof msg, "Claude plan usage limit reached (%.64s)",
               code);
    else
      snprintf(msg, sizeof msg, "Claude plan usage limit reached");
    note_limit(s, 1, msg, resets);
  } else if (!strcmp(status, "allowed_warning")) {
    if (jnumber(info, "utilization", &v) && v >= 0 && v <= 100) {
      double pct = v <= 1.0 ? v * 100.0 : v;
      snprintf(msg, sizeof msg,
               "approaching the Claude plan usage limit (%.0f%% used)", pct);
    } else {
      snprintf(msg, sizeof msg, "approaching the Claude plan usage limit");
    }
    note_limit(s, 0, msg, resets);
  }
}

static void claude_result(dlg_state *s, const asmodel_json_value *ev) {
  const char *subtype = jstr(ev, "subtype");
  const char *result = jstr(ev, "result");
  const asmodel_json_value *is_error = jget(ev, "is_error");
  const asmodel_json_value *usage = jget(ev, "usage");
  const asmodel_json_value *errors = jget(ev, "errors");
  double cost = 0;

  s->saw_result = 1;
  set_str(&s->subtype, subtype, 64);
  set_str(&s->session_id, jstr(ev, "session_id"), 256);
  s->failed = (asmodel_json_typeof(is_error) == ASMODEL_JSON_BOOL &&
               asmodel_json_bool_value(is_error)) ||
              (subtype && strcmp(subtype, "success") != 0);
  if (result) set_final(s, result);
  if (jget(ev, "num_turns")) s->turns = jcount(ev, "num_turns");
  if (jnumber(ev, "total_cost_usd", &cost) && cost >= 0 && cost < 1.0e9) {
    s->have_cost = 1;
    s->cost_usd = cost;
  }
  if (asmodel_json_typeof(usage) == ASMODEL_JSON_OBJECT) {
    int64_t cread = jcount(usage, "cache_read_input_tokens");
    int64_t cmade = jcount(usage, "cache_creation_input_tokens");
    s->have_usage = 1;
    s->input_tokens =
        sat_add(sat_add(jcount(usage, "input_tokens"), cread), cmade);
    s->output_tokens = jcount(usage, "output_tokens");
    s->cached_input_tokens = cread;
  }
  if (!s->failed) {
    free(s->error);
    s->error = NULL;
    return;
  }
  /* Error text: the result text, else the errors list, else the subtype. */
  if (result && result[0] != '\0') {
    set_str(&s->error, result, DLG_ERROR_CAP);
  } else if (asmodel_json_typeof(errors) == ASMODEL_JSON_ARRAY &&
             asmodel_json_array_len(errors) > 0) {
    core_buf b;
    size_t i, n = asmodel_json_array_len(errors);
    core_buf_init(&b);
    for (i = 0; i < n && b.len < DLG_ERROR_CAP; i++) {
      const char *e = asmodel_json_string_value(asmodel_json_array_at(errors, i));
      if (!e) continue;
      if (b.len > 0) (void)core_buf_appends(&b, "; ");
      (void)core_buf_appends(&b, e);
    }
    if (b.data) set_str(&s->error, b.data, DLG_ERROR_CAP);
    core_buf_free(&b);
  } else if (subtype) {
    char msg[96];
    snprintf(msg, sizeof msg, "Claude Code stopped: %s", subtype);
    set_str(&s->error, msg, DLG_ERROR_CAP);
  }
  if (jcount(ev, "api_error_status") == 429 || dlg_text_is_limit(s->error))
    note_limit(s, 1, s->error, 0);
  if (dlg_text_is_auth(s->error)) s->auth_error = 1;
}

static void claude_event(dlg_state *s, const asmodel_json_value *ev,
                         const char *type) {
  if (!strcmp(type, "system"))
    claude_system(s, ev);
  else if (!strcmp(type, "assistant"))
    claude_assistant(s, ev);
  else if (!strcmp(type, "rate_limit_event"))
    claude_rate_limit(s, ev);
  else if (!strcmp(type, "result"))
    claude_result(s, ev);
  /* user/tool_result, stream_event and everything newer: ignored. */
}

/* ---- Codex exec --json --------------------------------------------------- */

/* weight 0: item error (a warning inside the turn); 1: top-level error
 * event, which codex also emits for each transient retry ("Reconnecting...
 * 2/5 (...)"); 2: turn.failed, the only event that ends the turn. Later and
 * heavier messages win; only turn.failed makes a limit a stopping one. */
static void codex_error_text(dlg_state *s, const char *msg, int weight) {
  if (!msg) return;
  if (weight > 0 || !s->error) set_str(&s->error, msg, DLG_ERROR_CAP);
  if (dlg_text_is_limit(msg)) note_limit(s, weight == 2, msg, 0);
  if (dlg_text_is_auth(msg)) s->auth_error = 1;
}

static void codex_event(dlg_state *s, const asmodel_json_value *ev,
                        const char *type) {
  if (!strcmp(type, "thread.started")) {
    set_str(&s->session_id, jstr(ev, "thread_id"), 256);
  } else if (!strcmp(type, "turn.started")) {
    if (s->turns < INT64_MAX) s->turns++;
  } else if (!strcmp(type, "item.completed")) {
    const asmodel_json_value *item = jget(ev, "item");
    const char *itype = jstr(item, "type");
    if (!itype) return;
    if (!strcmp(itype, "agent_message")) {
      const char *text = jstr(item, "text");
      push_chunk(s, text);
      set_final(s, text);
    } else if (!strcmp(itype, "error")) {
      /* Non-fatal: the turn goes on. Keep it only as a hint. */
      codex_error_text(s, jstr(item, "message"), 0);
    }
  } else if (!strcmp(type, "turn.completed")) {
    const asmodel_json_value *usage = jget(ev, "usage");
    s->saw_result = 1;
    if (asmodel_json_typeof(usage) == ASMODEL_JSON_OBJECT) {
      s->have_usage = 1;
      s->input_tokens = sat_add(s->input_tokens, jcount(usage, "input_tokens"));
      s->output_tokens =
          sat_add(s->output_tokens, jcount(usage, "output_tokens"));
      s->cached_input_tokens =
          sat_add(s->cached_input_tokens, jcount(usage, "cached_input_tokens"));
    }
  } else if (!strcmp(type, "turn.failed")) {
    s->failed = 1;
    codex_error_text(s, jstr(jget(ev, "error"), "message"), 2);
  } else if (!strcmp(type, "error")) {
    codex_error_text(s, jstr(ev, "message"), 1);
  }
  /* item.started/item.updated and unknown types: ignored. */
}

/* ---- entry -------------------------------------------------------------- */

static int is_space(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

void dlg_feed_line(dlg_state *s, const char *line, size_t len) {
  asmodel_json_value *ev = NULL;
  const char *type;
  if (!s || !line) return;
  while (len > 0 && is_space(line[len - 1])) len--;
  while (len > 0 && is_space(line[0])) {
    line++;
    len--;
  }
  if (len == 0) return; /* blank lines are not events */
  if (line[0] != '{' || asmodel_json_parse(line, len, &ev) != 0 ||
      asmodel_json_typeof(ev) != ASMODEL_JSON_OBJECT ||
      (type = jstr(ev, "type")) == NULL) {
    asmodel_json_free(ev);
    s->malformed++;
    return;
  }
  s->events++;
  if (s->agent == DLG_CLAUDE)
    claude_event(s, ev, type);
  else
    codex_event(s, ev, type);
  asmodel_json_free(ev);
}
