/* Driver for test_chatgpt.py: one scenario per invocation against the fake
 * OpenAI auth + Responses server. Prints one result line per operation and
 * never prints credential values. */
#include "asmodel.h"
#include "core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *base_url, *auth_file, *model, *schema;
  int tools, grammar_only, optional, max_tokens, cancel_on_progress, threads;
  asmodel_reasoning_mode reasoning;
  int64_t deadline_ms;
} options;

typedef struct {
  const options *o;
  volatile int cancel;
  int rc;
  char *text;
  asmodel_generation_info info;
  asmodel_tool_calls calls;
  int heartbeats, outputs;
} run;

static void on_token(const char *text, size_t len, void *ud) {
  run *r = (run *)ud;
  (void)text;
  if (len) { r->outputs++; return; }
  r->heartbeats++;
  if (r->o->cancel_on_progress) r->cancel = 1;
}

static void *generate(void *arg) {
  run *r = (run *)arg;
  const options *o = r->o;
  asmodel_spec spec;
  asmodel_provider p;
  asmodel_generate_params params;
  asmodel_tool_schema schema = {"read", "Read a file",
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
      "\"limit\":{\"type\":\"integer\"}},\"required\":[\"path\"]}"};
  asmodel_tools tools;
  asmodel_block sys = {ASMODEL_BLOCK_TEXT, "You are terse.", NULL, NULL};
  asmodel_block dev = {ASMODEL_BLOCK_TEXT, "Prefer JSON.", NULL, NULL};
  asmodel_block usr = {ASMODEL_BLOCK_TEXT, "Say hello with a star \xe2\x98\x85", NULL, NULL};
  asmodel_message messages[3] = {{ASMODEL_ROLE_SYSTEM, &sys, 1},
                                 {ASMODEL_ROLE_DEVELOPER, &dev, 1},
                                 {ASMODEL_ROLE_USER, &usr, 1}};
  asmodel_input input = {messages, 3};
  char error[256] = {0};
  memset(&spec, 0, sizeof spec);
  spec.id = "plan"; spec.backend = ASMODEL_BACKEND_OPENAI;
  spec.base_url = o->base_url; spec.remote_model = o->model;
  spec.remote_provider = ASMODEL_REMOTE_CHATGPT; spec.auth_file = o->auth_file;
  spec.context_tokens = 32768;
  if (asmodel_provider_create(&spec, &p, error, sizeof error) != 0) {
    fprintf(stderr, "create: %s\n", error);
    r->rc = -1;
    return NULL;
  }
  memset(&params, 0, sizeof params);
  params.max_tokens = o->max_tokens;
  params.temperature = 0.7; params.top_p = 0.9; /* must not reach the wire */
  params.reasoning = o->reasoning;
  params.reasoning_budget = o->reasoning == ASMODEL_REASONING_BUDGETED ? 512 : 0;
  params.deadline_ms = o->deadline_ms;
  params.output_schema = o->schema;
  params.require_constraint = (o->schema && !o->optional) || o->grammar_only;
  params.result_info = &r->info;
  params.request_id = "host-only-correlation";
  if (o->tools) {
    memset(&tools, 0, sizeof tools);
    tools.schemas = &schema; tools.count = 1; tools.choice = ASMODEL_TOOLS_REQUIRED;
    tools.output = &r->calls;
    params.tools = &tools;
    params.require_constraint = 0;
  }
  r->rc = p.generate(p.userdata, &input, o->grammar_only ? "root ::= \"ok\"" : NULL, &params,
                     on_token, r, &r->cancel, &r->text, NULL, NULL);
  p.destroy(p.userdata);
  return NULL;
}

static const char *finish_name(asmodel_finish_reason f) {
  static const char *names[] = {"unknown", "stop", "length", "cancelled", "error", "tool_calls"};
  return f >= ASMODEL_FINISH_UNKNOWN && f <= ASMODEL_FINISH_TOOL_CALLS ? names[f] : "?";
}

static void report(const run *r) {
  printf("GEN %s finish=%s usage_known=%d in=%d out=%d reasoning=%d cached=%d json=%d "
         "applied=%llu calls=%zu heartbeats=%d outputs=%d retry_ms=%lld code=%s\n",
         r->rc < 0 ? "CREATE_FAILED" : asmodel_err_name((asmodel_err)r->rc),
         finish_name(r->info.finish_reason), r->info.usage_known, r->info.input_tokens,
         r->info.output_tokens, r->info.reasoning_tokens, r->info.cached_input_tokens,
         r->info.json_output, (unsigned long long)r->info.applied, r->calls.count,
         r->heartbeats, r->outputs, (long long)r->info.retry_after_ms,
         r->info.error_code[0] ? r->info.error_code : "-");
  printf("TEXT %s\n", r->text ? r->text : "(null)");
  for (size_t i = 0; i < r->calls.count; i++)
    printf("CALL %s %s %s\n", r->calls.calls[i].id, r->calls.calls[i].name,
           r->calls.calls[i].arguments);
  printf("ERROR %s\n", r->info.error[0] ? r->info.error : "-");
}

static int cmd_generate(const options *o) {
  run runs[8];
  os_thread threads[8];
  int n = o->threads > 0 ? o->threads : 1, failed = 0;
  if (n > 8) n = 8;
  memset(runs, 0, sizeof runs);
  for (int i = 0; i < n; i++) runs[i].o = o;
  if (n == 1) generate(&runs[0]);
  else {
    for (int i = 0; i < n; i++)
      if (os_thread_start(&threads[i], generate, &runs[i]) != CORE_OK) return 3;
    for (int i = 0; i < n; i++) os_thread_join(&threads[i]);
  }
  for (int i = 0; i < n; i++) {
    report(&runs[i]);
    if (runs[i].rc < 0) failed = 1;
    free(runs[i].text);
    asmodel_tool_calls_clear(&runs[i].calls);
  }
  fflush(stdout);
  return failed ? 4 : 0;
}

static int read_line(char *buf, size_t cap) {
  if (!fgets(buf, (int)cap, stdin)) return 0;
  buf[strcspn(buf, "\r\n")] = 0;
  return 1;
}

static int cmd_login(const char *mode, const char *issuer, const char *auth_file, int fresh) {
  asmodel_chatgpt_login_params lp;
  asmodel_chatgpt_login *session = NULL;
  char error[512] = {0}, line[8192];
  asmodel_err rc;
  memset(&lp, 0, sizeof lp);
  lp.auth_file = auth_file; lp.issuer = issuer; lp.new_registration = fresh;
  lp.http_timeout_ms = 10000;
  rc = asmodel_chatgpt_login_begin(&lp, &session, error, sizeof error);
  if (rc != ASMODEL_OK) { printf("BEGIN %s %s\n", asmodel_err_name(rc), error); return 5; }
  printf("URL %s\n", asmodel_chatgpt_login_url(session));
  printf("REDIRECT %s\n", asmodel_chatgpt_login_redirect_uri(session));
  fflush(stdout);
  if (!strcmp(mode, "browser")) {
    int polls = 0;
    do { /* short slices, as a CLI that also watches stdin would do */
      rc = asmodel_chatgpt_login_wait(session, 200, error, sizeof error);
    } while (rc == ASMODEL_ERR_TIMEOUT && ++polls < 100);
  } else {
    rc = ASMODEL_ERR_INVALID;
    while (read_line(line, sizeof line)) {
      rc = asmodel_chatgpt_login_complete(session, line, error, sizeof error);
      printf("COMPLETE %s %s\n", asmodel_err_name(rc), error);
      fflush(stdout);
      if (rc != ASMODEL_ERR_INVALID) break;
    }
  }
  printf("RESULT %s %s\n", asmodel_err_name(rc), error[0] ? error : "-");
  fflush(stdout);
  /* The listener must already be closed; keep the session to prove it. */
  if (!strcmp(mode, "browser")) read_line(line, sizeof line);
  asmodel_chatgpt_login_free(session);
  return 0;
}

int main(int argc, char **argv) {
  options o;
  const char *cmd;
  if (argc < 5) {
    fprintf(stderr, "usage: %s CMD BASE_URL ISSUER AUTH_FILE [MODEL] [flags]\n", argv[0]);
    return 2;
  }
  cmd = argv[1];
  memset(&o, 0, sizeof o);
  o.base_url = argv[2]; o.auth_file = argv[4]; o.model = argc > 5 ? argv[5] : "gpt-text";
  o.max_tokens = 256;
  for (int i = 6; i < argc; i++) {
    if (!strcmp(argv[i], "--schema") && i + 1 < argc) o.schema = argv[++i];
    else if (!strcmp(argv[i], "--tools")) o.tools = 1;
    else if (!strcmp(argv[i], "--grammar-only")) o.grammar_only = 1;
    else if (!strcmp(argv[i], "--optional")) o.optional = 1;
    else if (!strcmp(argv[i], "--max") && i + 1 < argc) o.max_tokens = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--deadline") && i + 1 < argc) o.deadline_ms = atoll(argv[++i]);
    else if (!strcmp(argv[i], "--cancel")) o.cancel_on_progress = 1;
    else if (!strcmp(argv[i], "--threads") && i + 1 < argc) o.threads = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--reasoning-off")) o.reasoning = ASMODEL_REASONING_REQUIRED_OFF;
    else if (!strcmp(argv[i], "--reasoning-budget")) o.reasoning = ASMODEL_REASONING_BUDGETED;
    else { fprintf(stderr, "unknown flag %s\n", argv[i]); return 2; }
  }
  if (!strcmp(cmd, "login-browser")) return cmd_login("browser", argv[3], o.auth_file, 0);
  if (!strcmp(cmd, "login-paste")) return cmd_login("paste", argv[3], o.auth_file, 0);
  if (!strcmp(cmd, "login-new")) return cmd_login("browser", argv[3], o.auth_file, 1);
  if (!strcmp(cmd, "generate")) return cmd_generate(&o);
  if (!strcmp(cmd, "status")) {
    asmodel_chatgpt_status st;
    asmodel_err rc = asmodel_chatgpt_status_read(o.auth_file, &st);
    printf("STATUS %s logged_in=%d plan_usage=%d expires_at=%lld saved_at=%lld email=%s "
           "plan=%s client_id=%s host_id=%s subject=%s\n", asmodel_err_name(rc), st.logged_in,
           st.plan_usage, (long long)st.expires_at, (long long)st.saved_at,
           st.email[0] ? st.email : "-", st.plan[0] ? st.plan : "-",
           st.client_id[0] ? st.client_id : "-", st.host_id[0] ? st.host_id : "-",
           st.subject[0] ? st.subject : "-");
    return 0;
  }
  if (!strcmp(cmd, "logout")) {
    char error[256] = {0};
    int revoked = 0;
    asmodel_err rc = asmodel_chatgpt_logout(o.auth_file, &revoked, error, sizeof error);
    printf("LOGOUT %s revoked=%d %s\n", asmodel_err_name(rc), revoked, error[0] ? error : "-");
    return 0;
  }
  if (!strcmp(cmd, "models")) {
    char error[256] = {0}, *list = NULL;
    asmodel_err rc = asmodel_chatgpt_models(o.auth_file, o.base_url, &list, error, sizeof error);
    printf("MODELS %s %s\n", asmodel_err_name(rc), list ? list : error);
    free(list);
    return 0;
  }
  if (!strcmp(cmd, "caps")) {
    asmodel_capabilities caps;
    int rc = asmodel_remote_capabilities(ASMODEL_REMOTE_CHATGPT, 32768, 0, &caps);
    printf("CAPS %d %s %s flags=%llu embed=%d\n", rc, caps.provider, caps.profile,
           (unsigned long long)caps.flags,
           asmodel_remote_capabilities(ASMODEL_REMOTE_CHATGPT, 512, 1, &caps));
    return 0;
  }
  fprintf(stderr, "unknown command %s\n", cmd);
  return 2;
}
