/* ChatGPT plan provider: POST {base_url}/responses with the user's
 * "Sign in with ChatGPT" access token. Preview rules (OpenAI SIWC docs):
 * store:false and stream:true on every request; no system-role items (system
 * text goes to `instructions`); no temperature, top_p or max_output_tokens.
 * Only response.completed is success. One 401 refresh+resend; no retries. */
#include "chatgpt.h"
#include "http.h"
#include "input_json.h"
#include "tools.h"
#include "asmodel_json.h"
#include "core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef asmodel_json_value json;

typedef struct {
  char *base_url, *model, *auth_file, *responses_url;
  asmodel_capabilities caps;
} cg_provider;

static void wipe(char *s) {
  if (s) {
    volatile char *p = s;
    while (*p) *p++ = 0;
  }
  free(s);
}

/* Context admission with the module's byte estimate plus injected schemas. */
static int request_fits(const cg_provider *u, int output, const asmodel_input *input,
                        const char *schema, const asmodel_tools *tools) {
  asmodel_provider counter;
  memset(&counter, 0, sizeof counter);
  asmodel_token_count count = asmodel_provider_measure_prompt(&counter, input);
  if (count.admission_tokens < 0) return 0;
  if (u->caps.context_tokens <= 0) return 1;
  size_t need = (size_t)count.admission_tokens + (size_t)output + (schema ? strlen(schema) : 0);
  if (tools)
    for (size_t i = 0; i < tools->count; i++)
      need += strlen(tools->schemas[i].name) + strlen(tools->schemas[i].description) +
              strlen(tools->schemas[i].parameters) + 128;
  return need <= (size_t)u->caps.context_tokens;
}

static asmodel_err instructions(const asmodel_input *input, char **out) {
  core_buf b;
  core_buf_init(&b);
  *out = NULL;
  for (size_t i = 0; i < input->count; i++) {
    char *text = NULL;
    asmodel_err e;
    if (input->messages[i].role != ASMODEL_ROLE_SYSTEM) continue;
    e = asmodel_message_text(&input->messages[i], &text);
    if (e != ASMODEL_OK) { core_buf_free(&b); return e; }
    if (text[0] && ((b.len && core_buf_appends(&b, "\n\n")) || core_buf_appends(&b, text))) {
      free(text); core_buf_free(&b); return ASMODEL_ERR_NOMEM;
    }
    free(text);
  }
  *out = b.data; /* NULL when there is no system text */
  return ASMODEL_OK;
}

static int has_required(const json *v, const char *key) {
  const json *r = asmodel_json_object_get(v, "required");
  for (size_t i = 0; i < asmodel_json_array_len(r); i++) {
    const char *k = asmodel_json_string_value(asmodel_json_array_at(r, i));
    if (k && !strcmp(k, key)) return 1;
  }
  return 0;
}

/* Two object variants are exclusive when a property required by both has a
 * `const` in each and the constants differ: no instance can match both. */
static int exclusive(const json *a, const json *b) {
  const json *pa = asmodel_json_object_get(a, "properties");
  const json *pb = asmodel_json_object_get(b, "properties");
  for (size_t i = 0; i < asmodel_json_object_count(pa); i++) {
    const char *key = asmodel_json_object_key_at(pa, i);
    const json *ca = asmodel_json_object_get(asmodel_json_object_value_at(pa, i), "const");
    const json *cb = asmodel_json_object_get(asmodel_json_object_get(pb, key), "const");
    char *x, *y;
    int differ;
    if (!ca || !cb || !has_required(a, key) || !has_required(b, key)) continue;
    x = asmodel_json_write(ca, 0);
    y = asmodel_json_write(cb, 0);
    differ = x && y && strcmp(x, y) != 0;
    free(x); free(y);
    if (differ) return 1;
  }
  return 0;
}

/* OpenAI strict structured outputs need an object root and do not accept
 * oneOf. A non-object root is sent as {"value": <schema>} and unwrapped from
 * the output; a root oneOf becomes anyOf only when its variants are pairwise
 * exclusive, so the accepted language is unchanged. Anything else has no
 * faithful representation: UNSUPPORTED before dispatch. */
static asmodel_err strict_schema(const char *schema, json **out, int *wrapped) {
  json *root = NULL, *inner = NULL, *wrapper, *props, *required;
  const json *type, *one;
  int bad;
  *out = NULL;
  *wrapped = 0;
  if (asmodel_json_parse(schema, strlen(schema), &root) ||
      asmodel_json_typeof(root) != ASMODEL_JSON_OBJECT) {
    asmodel_json_free(root);
    return ASMODEL_ERR_INVALID;
  }
  type = asmodel_json_object_get(root, "type");
  one = asmodel_json_object_get(root, "oneOf");
  if (asmodel_json_string_value(type) && !strcmp(asmodel_json_string_value(type), "object") &&
      !one && !asmodel_json_object_get(root, "anyOf")) {
    *out = root;
    return ASMODEL_OK;
  }
  if (one) {
    size_t n = asmodel_json_array_len(one);
    if (asmodel_json_typeof(one) != ASMODEL_JSON_ARRAY || n < 1 || asmodel_json_object_count(root) != 1) {
      asmodel_json_free(root);
      return ASMODEL_ERR_UNSUPPORTED;
    }
    for (size_t i = 0; i < n; i++)
      for (size_t j = i + 1; j < n; j++)
        if (!exclusive(asmodel_json_array_at(one, i), asmodel_json_array_at(one, j))) {
          asmodel_json_free(root);
          return ASMODEL_ERR_UNSUPPORTED;
        }
    inner = asmodel_json_object();
    bad = !inner || asmodel_json_object_set(inner, "anyOf", asmodel_json_clone(one));
    asmodel_json_free(root);
    if (bad) { asmodel_json_free(inner); return ASMODEL_ERR_NOMEM; }
  } else {
    inner = root;
  }
  wrapper = asmodel_json_object();
  props = asmodel_json_object();
  required = asmodel_json_array();
  bad = !wrapper || !props || !required;
  bad |= asmodel_json_object_set(props, "value", inner);
  bad |= asmodel_json_array_push(required, asmodel_json_string("value"));
  bad |= asmodel_json_object_set(wrapper, "type", asmodel_json_string("object"));
  bad |= asmodel_json_object_set(wrapper, "properties", props);
  bad |= asmodel_json_object_set(wrapper, "required", required);
  bad |= asmodel_json_object_set(wrapper, "additionalProperties", asmodel_json_bool(0));
  if (bad) { asmodel_json_free(wrapper); return ASMODEL_ERR_NOMEM; }
  *out = wrapper;
  *wrapped = 1;
  return ASMODEL_OK;
}

/* The caller receives exactly the value its own schema describes. */
static asmodel_err unwrap(chatgpt_stream *st) {
  json *root = NULL;
  char *inner;
  if (!st->text || asmodel_json_parse(st->text, st->text_n, &root) ||
      asmodel_json_typeof(root) != ASMODEL_JSON_OBJECT || asmodel_json_object_count(root) != 1 ||
      !asmodel_json_object_get(root, "value")) {
    asmodel_json_free(root);
    return ASMODEL_ERR_BACKEND;
  }
  inner = asmodel_json_write(asmodel_json_object_get(root, "value"), 0);
  asmodel_json_free(root);
  if (!inner) return ASMODEL_ERR_NOMEM;
  free(st->text);
  st->text = inner;
  st->text_n = strlen(inner);
  st->text_cap = st->text_n + 1;
  return ASMODEL_OK;
}

static asmodel_err build_body(const cg_provider *u, const asmodel_input *input, json *format_schema,
                              const asmodel_tools *tools, asmodel_reasoning_mode reasoning,
                              asmodel_generation_info *info, char **out) {
  json *o = asmodel_json_object(), *items = NULL;
  asmodel_message kept[256];
  asmodel_input rest;
  char *text = NULL, *encoded = NULL;
  asmodel_err rc;
  int bad = !o;
  size_t n = 0;
  *out = NULL;
  rc = instructions(input, &text);
  if (rc != ASMODEL_OK) { asmodel_json_free(o); asmodel_json_free(format_schema); return rc; }
  for (size_t i = 0; i < input->count; i++)
    if (input->messages[i].role != ASMODEL_ROLE_SYSTEM) kept[n++] = input->messages[i];
  rest.messages = kept; rest.count = n;
  rc = asmodel_input_json(&rest, 1, &encoded);
  if (rc != ASMODEL_OK) goto done;
  rc = ASMODEL_ERR_NOMEM;
  if (asmodel_json_parse(encoded, strlen(encoded), &items)) goto done;
  bad |= asmodel_json_object_set(o, "model", asmodel_json_string(u->model));
  if (text) bad |= asmodel_json_object_set(o, "instructions", asmodel_json_string(text));
  bad |= asmodel_json_object_set(o, "input", items); items = NULL;
  bad |= asmodel_json_object_set(o, "store", asmodel_json_bool(0));
  bad |= asmodel_json_object_set(o, "stream", asmodel_json_bool(1));
  if (reasoning == ASMODEL_REASONING_REQUIRED_OFF || reasoning == ASMODEL_REASONING_REQUIRED_ON) {
    json *r = asmodel_json_object();
    int off = reasoning == ASMODEL_REASONING_REQUIRED_OFF;
    bad |= !r || asmodel_json_object_set(r, "effort", asmodel_json_string(off ? "none" : "medium"));
    bad |= asmodel_json_object_set(o, "reasoning", r);
    info->applied |= off ? ASMODEL_APPLIED_REASONING_OFF : ASMODEL_APPLIED_REASONING_ON;
  }
  if (format_schema) {
    json *format = asmodel_json_object(), *wrap = asmodel_json_object();
    bad |= !format || !wrap;
    bad |= asmodel_json_object_set(format, "type", asmodel_json_string("json_schema"));
    bad |= asmodel_json_object_set(format, "name", asmodel_json_string("asmodel_output"));
    bad |= asmodel_json_object_set(format, "strict", asmodel_json_bool(1));
    bad |= asmodel_json_object_set(format, "schema", format_schema);
    format_schema = NULL;
    bad |= asmodel_json_object_set(wrap, "format", format);
    bad |= asmodel_json_object_set(o, "text", wrap);
    info->json_output = 1;
    info->applied |= ASMODEL_APPLIED_CONSTRAINT;
  }
  if (tools) {
    char *list = NULL;
    json *parsed = NULL;
    rc = asmodel_tools_json(tools, 1, &list);
    if (rc != ASMODEL_OK) goto done;
    rc = ASMODEL_ERR_NOMEM;
    bad |= asmodel_json_parse(list, strlen(list), &parsed);
    free(list);
    /* The host validates arguments; strict mode would reject optional fields. */
    for (size_t i = 0; parsed && i < asmodel_json_array_len(parsed); i++)
      bad |= asmodel_json_object_set(asmodel_json_array_at(parsed, i), "strict", asmodel_json_bool(0));
    bad |= asmodel_json_object_set(o, "tools", parsed);
    bad |= asmodel_json_object_set(o, "tool_choice", asmodel_json_string(
        tools->choice == ASMODEL_TOOLS_REQUIRED ? "required" :
        tools->choice == ASMODEL_TOOLS_NONE ? "none" : "auto"));
  }
  if (!bad) *out = asmodel_json_write(o, 0);
  rc = *out ? ASMODEL_OK : ASMODEL_ERR_NOMEM;
done:
  asmodel_json_free(o);
  asmodel_json_free(items);
  asmodel_json_free(format_schema);
  free(text);
  free(encoded);
  return rc;
}

typedef struct {
  chatgpt_stream *stream;
  asmodel_token_fn fn;
  void *ud;
} sink_ctx;

static int sink(long status, const char *p, size_t n, void *ud) {
  sink_ctx *c = (sink_ctx *)ud;
  if (status < 200 || status >= 300) return 0;
  /* Zero-length progress heartbeat; text is delivered once, at the end. */
  if (n && c->fn) c->fn("", 0, c->ud);
  return chatgpt_stream_feed(c->stream, p, n);
}

static int is_quota_code(const char *code) {
  return !strcmp(code, "subscription_sharing_usage_limit_exceeded") ||
         !strcmp(code, "rate_limit_exceeded") || !strcmp(code, "insufficient_quota") ||
         !strcmp(code, "usage_limit_reached");
}

static void retry_after(const char *header, asmodel_generation_info *info) {
  char *end = NULL;
  long long s;
  if (!header || !header[0]) return;
  s = strtoll(header, &end, 10);
  if (end && !*end && s >= 0 && s <= 7LL * 24 * 3600) info->retry_after_ms = (int64_t)s * 1000;
}

static void append_request_id(asmodel_generation_info *info, const char *id) {
  size_t n = strlen(info->error);
  if (id && id[0] && n + strlen(id) + 14 < sizeof info->error)
    snprintf(info->error + n, sizeof info->error - n, " [request %s]", id);
}

static void append_retry(asmodel_generation_info *info) {
  size_t n = strlen(info->error);
  if (info->retry_after_ms > 0 && n + 40 < sizeof info->error)
    snprintf(info->error + n, sizeof info->error - n, "; retry after %lld s",
             (long long)(info->retry_after_ms / 1000));
}

/* Non-2xx before the stream opened: the request was not admitted. */
static asmodel_err http_error(const asmodel_http_response *rs, asmodel_generation_info *info) {
  json *root = NULL;
  char code[64] = {0}, msg[200] = {0}, param[96] = {0};
  asmodel_err rc = ASMODEL_ERR_BACKEND;
  if (rs->body) (void)asmodel_json_parse(rs->body, rs->len, &root);
  {
    const json *e = asmodel_json_object_get(root, "error");
    if (asmodel_json_typeof(e) == ASMODEL_JSON_OBJECT) {
      asmodel_chatgpt_sanitize(code, sizeof code, asmodel_json_string_value(asmodel_json_object_get(e, "code")));
      asmodel_chatgpt_sanitize(msg, sizeof msg, asmodel_json_string_value(asmodel_json_object_get(e, "message")));
      asmodel_chatgpt_sanitize(param, sizeof param, asmodel_json_string_value(asmodel_json_object_get(e, "param")));
    } else if (asmodel_json_string_value(e)) {
      asmodel_chatgpt_sanitize(code, sizeof code, asmodel_json_string_value(e));
    }
    if (!msg[0]) /* direct-admission bodies may be {"detail": "..."} */
      asmodel_chatgpt_sanitize(msg, sizeof msg,
                               asmodel_json_string_value(asmodel_json_object_get(root, "detail")));
    if (!msg[0] && !root && rs->body) asmodel_chatgpt_sanitize(msg, 120 < sizeof msg ? 120 : sizeof msg, rs->body);
  }
  asmodel_json_free(root);
  snprintf(info->error_code, sizeof info->error_code, "%s", code);
  retry_after(rs->retry_after, info);
  if (rs->status == 429 || is_quota_code(code)) {
    rc = ASMODEL_ERR_QUOTA;
    snprintf(info->error, sizeof info->error,
             "ChatGPT plan usage limit reached (HTTP %ld%s%s): %s; review usage at %s", rs->status,
             code[0] ? " " : "", code, msg[0] ? msg : "request refused", ASMODEL_CHATGPT_USAGE_URL);
    append_retry(info);
  } else if (rs->status == 401) {
    rc = ASMODEL_ERR_AUTH;
    snprintf(info->error, sizeof info->error,
             "ChatGPT rejected the credentials (HTTP 401%s%s)%s%s; run `asterism login chatgpt`",
             code[0] ? " " : "", code, msg[0] ? ": " : "", msg);
  } else if (rs->status == 403) {
    rc = !strncmp(code, "chatpass_v2_", 12) ? ASMODEL_ERR_AUTH : ASMODEL_ERR_UNSUPPORTED;
    snprintf(info->error, sizeof info->error, "ChatGPT plan usage not permitted (HTTP 403%s%s)%s%s",
             code[0] ? " " : "", code, msg[0] ? ": " : "", msg);
  } else if (rs->status == 400 && (!strncmp(param, "text.format", 11) ||
                                    !strcmp(code, "invalid_json_schema"))) {
    rc = ASMODEL_ERR_UNSUPPORTED; /* the constraint was refused, never dropped */
    snprintf(info->error, sizeof info->error,
             "OpenAI refused the output schema (HTTP 400%s%s%s%s)%s%s", code[0] ? " " : "", code,
             param[0] ? ", param " : "", param, msg[0] ? ": " : "", msg);
  } else if (!strcmp(code, "subscription_sharing_unsupported_capability")) {
    rc = ASMODEL_ERR_UNSUPPORTED;
    snprintf(info->error, sizeof info->error,
             "the ChatGPT plan route does not support this request%s%s%s%s%s",
             param[0] ? " (param " : "", param, param[0] ? ")" : "", msg[0] ? ": " : "", msg);
  } else {
    snprintf(info->error, sizeof info->error, "ChatGPT plan request failed (HTTP %ld%s%s)%s%s%s",
             rs->status, code[0] ? " " : "", code, msg[0] ? ": " : "", msg,
             rs->status >= 500 ? "; temporarily unavailable" : "");
    if (rs->status >= 500) append_retry(info);
  }
  append_request_id(info, rs->request_id);
  return rc;
}

static asmodel_err stream_result(const chatgpt_stream *st, int max_tokens,
                                 asmodel_generation_info *info, const char *request_id) {
  asmodel_err rc = ASMODEL_ERR_BACKEND;
  if (st->usage_known) {
    info->input_tokens = st->input_tokens;
    info->output_tokens = st->output_tokens;
    info->reasoning_tokens = st->reasoning_tokens;
    info->cached_input_tokens = st->cached_tokens;
    info->usage_known = 1;
  }
  if (st->bad) {
    snprintf(info->error, sizeof info->error, "malformed Responses event stream");
  } else if (st->limit_hit) {
    info->usage_known = 0; /* the stream was cut: consumption is unknown */
    info->finish_reason = ASMODEL_FINISH_LENGTH;
    snprintf(info->error, sizeof info->error,
             "output budget of %d tokens reached (client-side estimate: the ChatGPT plan route "
             "does not accept max_output_tokens)", max_tokens);
    rc = ASMODEL_ERR_LIMIT;
  } else if (st->terminal == CHATGPT_STREAM_COMPLETED) {
    if (st->refused && !st->text_n) {
      snprintf(info->error, sizeof info->error, "the model refused%s%s",
               st->message[0] ? ": " : "", st->message);
    } else {
      info->finish_reason = st->calls_seen ? ASMODEL_FINISH_TOOL_CALLS : ASMODEL_FINISH_STOP;
      rc = ASMODEL_OK;
    }
  } else if (st->terminal == CHATGPT_STREAM_INCOMPLETE) {
    if (!strcmp(st->reason, "max_output_tokens")) {
      info->finish_reason = ASMODEL_FINISH_LENGTH;
      snprintf(info->error, sizeof info->error, "completion reached its output limit");
      rc = ASMODEL_ERR_LIMIT;
    } else {
      snprintf(info->error, sizeof info->error, "response incomplete%s%s",
               st->reason[0] ? ": " : "", st->reason);
    }
  } else if (st->terminal == CHATGPT_STREAM_FAILED || st->terminal == CHATGPT_STREAM_ERROR) {
    snprintf(info->error_code, sizeof info->error_code, "%s", st->code);
    if (is_quota_code(st->code)) {
      rc = ASMODEL_ERR_QUOTA;
      snprintf(info->error, sizeof info->error,
               "ChatGPT plan usage limit reached (%s)%s%s; review usage at %s", st->code,
               st->message[0] ? ": " : "", st->message, ASMODEL_CHATGPT_USAGE_URL);
    } else {
      if (!strcmp(st->code, "subscription_sharing_unsupported_capability")) rc = ASMODEL_ERR_UNSUPPORTED;
      snprintf(info->error, sizeof info->error, "ChatGPT response failed%s%s%s%s%s%s%s",
               st->code[0] ? " (" : "", st->code, st->code[0] ? ")" : "",
               st->message[0] ? ": " : "", st->message, st->param[0] ? "; param " : "", st->param);
    }
  } else {
    info->usage_known = 0;
    snprintf(info->error, sizeof info->error, "stream ended before response.completed");
  }
  append_request_id(info, request_id);
  return rc;
}

static int cg_generate(void *ud, const asmodel_input *input, const char *grammar,
                       const asmodel_generate_params *params, asmodel_token_fn token_fn,
                       void *token_ud, volatile int *cancel, char **out_text,
                       int *out_in, int *out_gen) {
  cg_provider *u = (cg_provider *)ud;
  asmodel_generation_info local;
  asmodel_generation_info *info;
  asmodel_tool_calls *calls;
  chatgpt_stream st;
  asmodel_http_response rs;
  char *body = NULL, *token = NULL, why[400] = {0};
  const char *schema;
  json *format_schema = NULL;
  int rc = ASMODEL_ERR_BACKEND, streamed = 0, reasoning_known = 0, wrapped = 0;
  int64_t started = os_monotonic_ms();
  if (!ud || !params || !out_text) return ASMODEL_ERR_INVALID;
  memset(&local, 0, sizeof local);
  memset(&rs, 0, sizeof rs);
  memset(&st, 0, sizeof st);
  info = params->result_info ? params->result_info : &local;
  calls = params->tools ? params->tools->output : NULL;
  schema = params->output_schema;
  asmodel_tool_calls_clear(calls);
  *out_text = NULL;
  if (out_in) *out_in = 0;
  if (out_gen) *out_gen = 0;
  memset(info, 0, sizeof *info);
  info->finish_reason = ASMODEL_FINISH_ERROR;
  info->usage_known = 1; /* No request has been dispatched yet. */
  if (params->max_tokens <= 0 || params->deadline_ms < 0) { rc = ASMODEL_ERR_INVALID; goto done; }
  if (cancel && *cancel) { rc = ASMODEL_ERR_CANCELLED; goto done; }
  rc = asmodel_tools_validate(params->tools);
  if (rc != ASMODEL_OK) goto done;
  if (params->tools && (grammar || schema || params->require_constraint)) { rc = ASMODEL_ERR_INVALID; goto done; }
  rc = asmodel_input_validate(input);
  if (rc != ASMODEL_OK) goto done;
  if (params->reasoning == ASMODEL_REASONING_BUDGETED) {
    snprintf(info->error, sizeof info->error,
             "the ChatGPT plan profile has no reasoning budget control");
    rc = ASMODEL_ERR_UNSUPPORTED;
    goto done;
  }
  if (params->require_constraint && !schema) {
    snprintf(info->error, sizeof info->error,
             "the ChatGPT plan profile enforces JSON Schema only; the required constraint "
             "has no supported representation");
    rc = ASMODEL_ERR_UNSUPPORTED;
    goto done;
  }
  if (!request_fits(u, params->max_tokens, input, schema, params->tools)) {
    snprintf(info->error, sizeof info->error,
             "estimated request including output schema exceeds context budget");
    rc = ASMODEL_ERR_LIMIT;
    goto done;
  }
  if (schema) {
    rc = strict_schema(schema, &format_schema, &wrapped);
    if (rc == ASMODEL_ERR_UNSUPPORTED && !params->require_constraint) {
      rc = ASMODEL_OK; /* optional: sent unconstrained, APPLIED_CONSTRAINT stays clear */
    } else if (rc == ASMODEL_ERR_UNSUPPORTED) {
      snprintf(info->error, sizeof info->error,
               "the output schema has no faithful OpenAI strict form (root oneOf variants "
               "are not provably exclusive)");
    }
    if (rc != ASMODEL_OK) goto done;
  }
  rc = build_body(u, input, format_schema, params->tools, params->reasoning, info, &body);
  format_schema = NULL; /* consumed */
  if (rc != ASMODEL_OK) goto done;
  {
    int64_t remaining = params->deadline_ms ? params->deadline_ms - (os_monotonic_ms() - started) : 0;
    if (params->deadline_ms && remaining <= 0) { rc = ASMODEL_ERR_TIMEOUT; goto done; }
    rc = asmodel_chatgpt_access(u->auth_file, NULL, remaining, cancel, &token, why, sizeof why);
    if (rc != ASMODEL_OK) {
      snprintf(info->error, sizeof info->error, "%s", why);
      goto done;
    }
  }
  for (int attempt = 0; attempt < 2; attempt++) {
    core_buf auth;
    const char *headers[4];
    asmodel_http_request rq;
    sink_ctx ctx;
    char transport[256] = {0};
    int64_t remaining = params->deadline_ms ? params->deadline_ms - (os_monotonic_ms() - started) : 0;
    int h;
    if (params->deadline_ms && remaining <= 0) { rc = ASMODEL_ERR_TIMEOUT; goto done; }
    core_buf_init(&auth);
    if (core_buf_appends(&auth, "Authorization: Bearer ") || core_buf_appends(&auth, token)) {
      core_buf_free(&auth);
      rc = ASMODEL_ERR_NOMEM;
      goto done;
    }
    headers[0] = auth.data;
    headers[1] = "Content-Type: application/json";
    headers[2] = "Accept: text/event-stream";
    headers[3] = NULL;
    chatgpt_stream_free(&st);
    asmodel_tool_calls_clear(calls);
    chatgpt_stream_init(&st, calls, (size_t)params->max_tokens * 4);
    ctx.stream = &st; ctx.fn = token_fn; ctx.ud = token_ud;
    memset(&rq, 0, sizeof rq);
    rq.method = "POST"; rq.url = u->responses_url; rq.headers = headers; rq.body = body;
    rq.cancel = cancel; rq.timeout_ms = remaining; rq.sink = sink; rq.sink_ud = &ctx;
    info->usage_known = 0; /* dispatched: only the server can report consumption */
    asmodel_http_response_free(&rs);
    h = asmodel_http_perform(&rq, &rs, transport, sizeof transport);
    memset(auth.data, 0, auth.len);
    core_buf_free(&auth);
    if (h == ASMODEL_HTTP_OK && rs.status == 401 && attempt == 0) {
      /* Not admitted: refresh exactly once (unless another request already
       * did) and resend. No other failure is ever retried. */
      char *fresh = NULL;
      remaining = params->deadline_ms ? params->deadline_ms - (os_monotonic_ms() - started) : 0;
      if (params->deadline_ms && remaining <= 0) { rc = ASMODEL_ERR_TIMEOUT; goto done; }
      rc = asmodel_chatgpt_access(u->auth_file, token, remaining, cancel, &fresh, why, sizeof why);
      if (rc != ASMODEL_OK) {
        snprintf(info->error, sizeof info->error, "%s", why);
        goto done;
      }
      wipe(token);
      token = fresh;
      continue;
    }
    if (h == ASMODEL_HTTP_CANCELLED || (cancel && *cancel)) { rc = ASMODEL_ERR_CANCELLED; streamed = 1; goto done; }
    if (rs.status >= 200 && rs.status < 300) {
      streamed = 1;
      if (h == ASMODEL_HTTP_OK) chatgpt_stream_end(&st);
      if (h == ASMODEL_HTTP_TIMEOUT) {
        snprintf(info->error, sizeof info->error, "inference %s", transport);
        rc = ASMODEL_ERR_TIMEOUT;
      } else if (h == ASMODEL_HTTP_TRANSPORT && !st.terminal) {
        snprintf(info->error, sizeof info->error, "%s", transport[0] ? transport : "HTTP transport failed");
        rc = ASMODEL_ERR_BACKEND;
      } else {
        rc = stream_result(&st, params->max_tokens, info, rs.request_id);
      }
    } else if (h == ASMODEL_HTTP_OK) {
      rc = http_error(&rs, info);
    } else if (h == ASMODEL_HTTP_TIMEOUT) {
      snprintf(info->error, sizeof info->error, "inference %s", transport);
      rc = ASMODEL_ERR_TIMEOUT;
    } else {
      snprintf(info->error, sizeof info->error, "%s", transport[0] ? transport : "HTTP transport failed");
      rc = ASMODEL_ERR_BACKEND;
    }
    break;
  }
  if (rc != ASMODEL_OK && rc != ASMODEL_ERR_LIMIT) goto done;
  reasoning_known = st.reasoning_known;
  if (rc == ASMODEL_OK && params->reasoning == ASMODEL_REASONING_REQUIRED_OFF) {
    if (!reasoning_known) {
      snprintf(info->error, sizeof info->error,
               "provider did not report reasoning usage; reasoning-off postcondition cannot be verified");
      rc = ASMODEL_ERR_UNSUPPORTED;
    } else if (info->reasoning_tokens > 0) {
      snprintf(info->error, sizeof info->error,
               "provider violated reasoning-off contract (%d reasoning tokens)", info->reasoning_tokens);
      rc = ASMODEL_ERR_UNSUPPORTED;
    }
  }
  if (rc == ASMODEL_OK && wrapped) {
    rc = unwrap(&st);
    if (rc != ASMODEL_OK)
      snprintf(info->error, sizeof info->error, "structured output does not match the schema wrapper");
  }
  if (rc == ASMODEL_OK) {
    rc = asmodel_tools_accept(params->tools, info->finish_reason, input);
    if (rc != ASMODEL_OK) snprintf(info->error, sizeof info->error, "provider violated the native tool contract");
  }
done:
  if (streamed) { /* partial bytes never turn an error into success */
    *out_text = chatgpt_stream_take_text(&st);
    if (!*out_text && (rc == ASMODEL_OK || rc == ASMODEL_ERR_LIMIT)) {
      *out_text = core_strdup("");
      if (!*out_text) rc = ASMODEL_ERR_NOMEM;
    }
    if (*out_text && !**out_text && rc != ASMODEL_OK && rc != ASMODEL_ERR_LIMIT) {
      free(*out_text);
      *out_text = NULL;
    }
  }
  chatgpt_stream_free(&st);
  asmodel_http_response_free(&rs);
  asmodel_json_free(format_schema);
  wipe(token);
  free(body);
  if (cancel && *cancel) rc = ASMODEL_ERR_CANCELLED;
  if (rc != ASMODEL_OK) asmodel_tool_calls_clear(calls);
  if (rc != ASMODEL_OK && rc != ASMODEL_ERR_LIMIT) {
    if (!info->error[0])
      snprintf(info->error, sizeof info->error, "provider request failed: %s",
               asmodel_err_name((asmodel_err)rc));
    info->finish_reason = rc == ASMODEL_ERR_CANCELLED ? ASMODEL_FINISH_CANCELLED : ASMODEL_FINISH_ERROR;
  }
  if (out_in) *out_in = info->input_tokens;
  if (out_gen) *out_gen = info->output_tokens;
  if (token_fn && *out_text) token_fn(*out_text, strlen(*out_text), token_ud);
  return rc;
}

static int cg_count(void *ud, const char *text) {
  (void)ud;
  return text && *text ? (int)((strlen(text) + 3) / 4) : 0;
}

static int cg_capabilities(void *ud, asmodel_capabilities *out) {
  cg_provider *u = (cg_provider *)ud;
  if (!u || !out) return -1;
  *out = u->caps;
  return 0;
}

static void cg_destroy(void *ud) {
  cg_provider *u = (cg_provider *)ud;
  if (!u) return;
  free(u->base_url); free(u->model); free(u->auth_file); free(u->responses_url);
  free(u);
}

int asmodel_chatgpt_provider_create(const asmodel_spec *spec, asmodel_provider *out,
                                    char *error, size_t error_size) {
  cg_provider *u;
  const char *base;
  size_t n;
  if (out) memset(out, 0, sizeof *out);
  if (!spec || !out) return -1;
  base = spec->base_url && spec->base_url[0] ? spec->base_url : ASMODEL_CHATGPT_BASE_URL;
  if (!asmodel_http_available()) {
    if (error && error_size) snprintf(error, error_size, "asmodel built without an HTTP client");
    return -1;
  }
  if (spec->embedding || !spec->remote_model || !spec->remote_model[0] || !spec->auth_file ||
      !spec->auth_file[0] || !asmodel_http_url_allowed(base)) {
    if (error && error_size)
      snprintf(error, error_size, "provider chatgpt needs a model, an auth_file and an https "
               "base_url, and serves no embeddings");
    return -1;
  }
  u = (cg_provider *)calloc(1, sizeof *u);
  if (!u) return -1;
  n = strlen(base);
  while (n && base[n - 1] == '/') n--;
  u->base_url = core_strndup(base, n);
  u->model = core_strdup(spec->remote_model);
  u->auth_file = core_strdup(spec->auth_file);
  u->responses_url = u->base_url ? (char *)malloc(n + sizeof "/responses") : NULL;
  if (!u->base_url || !u->model || !u->auth_file || !u->responses_url ||
      asmodel_remote_capabilities(ASMODEL_REMOTE_CHATGPT, spec->context_tokens, 0, &u->caps)) {
    cg_destroy(u);
    return -1;
  }
  snprintf(u->responses_url, n + sizeof "/responses", "%s/responses", u->base_url);
  out->userdata = u;
  out->generate = cg_generate;
  out->count_tokens = cg_count;
  out->token_quality = ASMODEL_TOKENS_ESTIMATED;
  out->capabilities = cg_capabilities;
  out->destroy = cg_destroy;
  return 0;
}
