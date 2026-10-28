/* Responses API event stream (SSE). Only response.completed is success; text
 * comes from output_text deltas, calls from completed function_call items. */
#include "chatgpt.h"
#include "tools.h"
#include "asmodel_json.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef asmodel_json_value json;

#define STREAM_MAX_LINE ((size_t)16 * 1024 * 1024)
#define STREAM_MAX_TEXT ((size_t)16 * 1024 * 1024)

static const json *get(const json *o, const char *k) { return asmodel_json_object_get(o, k); }

static const char *string(const json *v) {
  const char *s = asmodel_json_string_value(v);
  return s && strlen(s) == asmodel_json_string_length(v) ? s : NULL;
}

static int number(const json *v, int *out) {
  if (!asmodel_json_is_int(v) || asmodel_json_int_value(v) < 0 ||
      asmodel_json_int_value(v) > INT_MAX) return -1;
  *out = (int)asmodel_json_int_value(v);
  return 0;
}

void asmodel_chatgpt_sanitize(char *dst, size_t cap, const char *src) {
  const unsigned char *p = (const unsigned char *)(src ? src : "");
  size_t i = 0, j = 0;
  if (!dst || !cap) return;
  for (; p[i] && j + 1 < cap; i++) dst[j++] = p[i] < 0x20 || p[i] == 0x7f ? ' ' : (char)p[i];
  if (p[i]) { /* truncated: never end inside a UTF-8 sequence */
    size_t k = j;
    while (k > 0 && ((unsigned char)dst[k - 1] & 0xC0) == 0x80) k--;
    if (k > 0 && ((unsigned char)dst[k - 1] & 0x80)) {
      unsigned char lead = (unsigned char)dst[k - 1];
      size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
      if (j - (k - 1) < need) j = k - 1;
    }
  }
  dst[j] = 0;
}

static int grow(char **p, size_t *cap, size_t need) {
  size_t next;
  char *q;
  if (need <= *cap) return 0;
  next = *cap ? *cap : 256;
  while (next < need) {
    if (next > (size_t)-1 / 2) return -1;
    next *= 2;
  }
  q = (char *)realloc(*p, next);
  if (!q) return -1;
  *p = q; *cap = next;
  return 0;
}

static int append_text(chatgpt_stream *s, const char *t) {
  size_t n;
  if (!t) return -1;
  n = strlen(t);
  if (n > STREAM_MAX_TEXT - s->text_n) return -1;
  if (grow(&s->text, &s->text_cap, s->text_n + n + 1)) return -1;
  memcpy(s->text + s->text_n, t, n + 1);
  s->text_n += n;
  if (s->text_limit && s->text_n > s->text_limit) s->limit_hit = 1;
  return 0;
}

void chatgpt_stream_init(chatgpt_stream *s, asmodel_tool_calls *calls, size_t text_limit) {
  memset(s, 0, sizeof *s);
  s->calls = calls;
  s->text_limit = text_limit;
}

void chatgpt_stream_free(chatgpt_stream *s) {
  if (!s) return;
  free(s->line); free(s->data); free(s->text);
  s->line = s->data = s->text = NULL;
  s->line_n = s->line_cap = s->data_n = s->data_cap = s->text_n = s->text_cap = 0;
}

char *chatgpt_stream_take_text(chatgpt_stream *s) {
  char *t = s->text;
  s->text = NULL; s->text_n = s->text_cap = 0;
  return t;
}

static void copy_field(char *dst, size_t cap, const json *v) {
  const char *t = string(v);
  if (t) asmodel_chatgpt_sanitize(dst, cap, t);
}

static int usage(chatgpt_stream *s, const json *u) {
  const json *in, *out, *reason, *cached;
  if (!u || asmodel_json_typeof(u) == ASMODEL_JSON_NULL) return 0;
  if (asmodel_json_typeof(u) != ASMODEL_JSON_OBJECT) return -1;
  in = get(u, "input_tokens");
  out = get(u, "output_tokens");
  reason = get(get(u, "output_tokens_details"), "reasoning_tokens");
  cached = get(get(u, "input_tokens_details"), "cached_tokens");
  if ((in && number(in, &s->input_tokens)) || (out && number(out, &s->output_tokens)) ||
      (reason && number(reason, &s->reasoning_tokens)) ||
      (cached && number(cached, &s->cached_tokens))) return -1;
  s->usage_known = in && out;
  if (reason) s->reasoning_known = 1;
  return 0;
}

static int function_item(chatgpt_stream *s, const json *item) {
  json *one;
  int bad;
  if (!s->calls) return -1; /* no tools were offered */
  one = asmodel_json_array();
  if (!one || asmodel_json_array_push(one, asmodel_json_clone(item))) {
    asmodel_json_free(one);
    return -1;
  }
  bad = asmodel_tool_decode(one, 0, 1, s->calls);
  asmodel_json_free(one);
  if (!bad) s->calls_seen = 1;
  return bad;
}

/* Fallback when the stream carried no deltas/items: the terminal response. */
static int output_items(chatgpt_stream *s, const json *output) {
  int want_text = !s->text_seen, want_calls = !s->calls_seen;
  for (size_t i = 0; i < asmodel_json_array_len(output); i++) {
    const json *item = asmodel_json_array_at(output, i);
    const char *type = string(get(item, "type"));
    if (!type) continue;
    if (!strcmp(type, "function_call")) {
      if (want_calls && function_item(s, item)) return -1;
    } else if (!strcmp(type, "message") && want_text) {
      const json *content = get(item, "content");
      for (size_t j = 0; j < asmodel_json_array_len(content); j++) {
        const json *part = asmodel_json_array_at(content, j);
        const char *pt = string(get(part, "type"));
        if (pt && !strcmp(pt, "output_text") && append_text(s, string(get(part, "text"))))
          return -1;
        if (pt && !strcmp(pt, "refusal")) {
          s->refused = 1;
          copy_field(s->message, sizeof s->message, get(part, "refusal"));
        }
      }
    }
  }
  return 0;
}

static int terminal(chatgpt_stream *s, const json *root, int kind) {
  const json *r = get(root, "response"), *err, *details;
  if (asmodel_json_typeof(r) != ASMODEL_JSON_OBJECT) return -1;
  if (usage(s, get(r, "usage"))) return -1;
  err = get(r, "error");
  if (asmodel_json_typeof(err) == ASMODEL_JSON_OBJECT) {
    copy_field(s->code, sizeof s->code, get(err, "code"));
    copy_field(s->message, sizeof s->message, get(err, "message"));
    copy_field(s->param, sizeof s->param, get(err, "param"));
  }
  details = get(r, "incomplete_details");
  if (asmodel_json_typeof(details) == ASMODEL_JSON_OBJECT)
    copy_field(s->reason, sizeof s->reason, get(details, "reason"));
  if (kind == CHATGPT_STREAM_COMPLETED) {
    const char *status = string(get(r, "status"));
    if (status && strcmp(status, "completed")) return -1;
  }
  if (output_items(s, get(r, "output"))) return -1;
  s->terminal = kind;
  return 0;
}

static int dispatch(chatgpt_stream *s, const char *data, size_t n) {
  json *root = NULL;
  const char *type;
  int bad = 0;
  if (s->terminal) return 0; /* nothing after the terminal event counts */
  if (n == 6 && !memcmp(data, "[DONE]", 6)) return 0;
  if (asmodel_json_parse(data, n, &root) || asmodel_json_typeof(root) != ASMODEL_JSON_OBJECT) {
    asmodel_json_free(root);
    return -1;
  }
  type = string(get(root, "type"));
  if (!type) bad = -1;
  else if (!strcmp(type, "response.output_text.delta")) {
    bad = append_text(s, string(get(root, "delta")));
    s->text_seen = 1;
  } else if (!strcmp(type, "response.refusal.delta")) {
    s->refused = 1;
  } else if (!strcmp(type, "response.output_item.done")) {
    const json *item = get(root, "item");
    const char *it = string(get(item, "type"));
    if (it && !strcmp(it, "function_call")) bad = function_item(s, item);
  } else if (!strcmp(type, "response.completed")) {
    bad = terminal(s, root, CHATGPT_STREAM_COMPLETED);
  } else if (!strcmp(type, "response.incomplete")) {
    bad = terminal(s, root, CHATGPT_STREAM_INCOMPLETE);
  } else if (!strcmp(type, "response.failed")) {
    bad = terminal(s, root, CHATGPT_STREAM_FAILED);
  } else if (!strcmp(type, "error")) {
    const json *e = get(root, "error");
    if (asmodel_json_typeof(e) != ASMODEL_JSON_OBJECT) e = root;
    copy_field(s->code, sizeof s->code, get(e, "code"));
    copy_field(s->message, sizeof s->message, get(e, "message"));
    copy_field(s->param, sizeof s->param, get(e, "param"));
    s->terminal = CHATGPT_STREAM_ERROR;
  }
  /* Other event types (created, in_progress, reasoning, argument deltas...)
   * carry nothing the terminal event does not repeat. */
  asmodel_json_free(root);
  return bad;
}

static int line(chatgpt_stream *s, char *p, size_t n) {
  if (n && p[n - 1] == '\r') n--;
  if (n == 0) {
    int bad = 0;
    if (s->has_data) bad = dispatch(s, s->data ? s->data : "", s->data_n);
    s->data_n = 0; s->has_data = 0;
    return bad;
  }
  if (p[0] == ':') return 0; /* comment / keep-alive */
  if (n >= 5 && !memcmp(p, "data:", 5)) {
    p += 5; n -= 5;
    if (n && *p == ' ') { p++; n--; }
    if (n > STREAM_MAX_LINE - s->data_n - 1) return -1;
    if (grow(&s->data, &s->data_cap, s->data_n + n + 2)) return -1;
    if (s->has_data) s->data[s->data_n++] = '\n';
    memcpy(s->data + s->data_n, p, n);
    s->data_n += n;
    s->data[s->data_n] = 0;
    s->has_data = 1;
  }
  return 0; /* event:, id:, retry: are not needed: data carries "type" */
}

static int stop(const chatgpt_stream *s) {
  return s->bad || s->limit_hit ||
         s->terminal == CHATGPT_STREAM_FAILED || s->terminal == CHATGPT_STREAM_ERROR;
}

int chatgpt_stream_feed(chatgpt_stream *s, const char *p, size_t n) {
  if (stop(s)) return 1;
  for (size_t i = 0; i < n; i++) {
    if (p[i] == '\n') {
      if (line(s, s->line ? s->line : (char *)"", s->line_n)) s->bad = 1;
      s->line_n = 0;
      if (stop(s)) return 1;
      continue;
    }
    if (s->line_n >= STREAM_MAX_LINE || grow(&s->line, &s->line_cap, s->line_n + 2)) {
      s->bad = 1;
      return 1;
    }
    s->line[s->line_n++] = p[i];
    s->line[s->line_n] = 0;
  }
  return stop(s);
}

void chatgpt_stream_end(chatgpt_stream *s) {
  if (stop(s)) return;
  if (s->line_n) {
    if (line(s, s->line, s->line_n)) s->bad = 1;
    s->line_n = 0;
  }
  if (!s->bad && s->has_data) {
    if (dispatch(s, s->data, s->data_n)) s->bad = 1;
    s->has_data = 0; s->data_n = 0;
  }
}
