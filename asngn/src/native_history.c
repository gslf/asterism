/* Bounded, correlated observations. Raw tool evidence remains in the usual journal. */
#include "asmodel_json.h"
#include "native.h"
#include <stdlib.h>
#include <string.h>

void asngn_native_history_free(asngn_native_history *h) {
  for (size_t i = 0; i < h->count; i++) {
    free(h->entries[i].call.id);
    free(h->entries[i].call.name);
    free(h->entries[i].call.arguments);
    free(h->entries[i].result);
  }
  memset(h, 0, sizeof *h);
}

/* Keep both ends: compiler/test diagnostics often occur late in the output.
 * This is an excerpt, never a verifier receipt or proof of collection. */
static asngn_err excerpt(core_buf *out, const char *text) {
  size_t n = strlen(text), first = n, last = n;
  if (n > 60000) {
    first = 30000;
    last = n - 30000;
    while (first && !core_utf8_valid(text, first))
      first--;
    while (last < n && ((unsigned char)text[last] & 0xc0) == 0x80)
      last++;
  }
  asngn_err e = asngn_buf_append(out, text, first);
  if (e == ASNGN_OK && first < n)
    e = asngn_buf_appends(out, "\n[observation excerpt: middle omitted]\n");
  if (e == ASNGN_OK && first < n) e = asngn_buf_appends(out, text + last);
  return e;
}

asngn_err asngn_native_record(asngn_turn_state *t, asngn_native_history *h,
                              const asmodel_tool_call *call, size_t work_before) {
  if (h->count >= ASNGN_NATIVE_HISTORY) return ASNGN_ERR_CONTEXT;
  asngn_native_entry *entry = &h->entries[h->count++];
  /* A request may not carry the same call id twice, and a server may reuse
   * ids from one response to the next. Keep the server's id while it is
   * new to this conversation; otherwise give the call one of ours. The id
   * chosen here is the one stored, so the call reads the same later. */
  char own[32];
  const char *id = call->id;
  size_t taken = h->count - 1, tries = 0;
  for (size_t i = 0; h->thread && i < h->thread->n; i++) taken += h->thread->v[i].role == 'c';
  for (bool clash = true; clash && tries < 4096; tries++) {
    clash = false;
    for (size_t i = 0; !clash && i + 1 < h->count; i++) clash = !strcmp(h->entries[i].call.id, id);
    for (size_t i = 0; !clash && h->thread && i < h->thread->n; i++)
      clash = h->thread->v[i].role == 'c' && !strcmp(h->thread->v[i].id, id);
    if (clash) {
      snprintf(own, sizeof own, "h%zu", ++taken);
      id = own;
    }
  }
  entry->call.id = core_strdup(id);
  entry->call.name = core_strdup(call->name);
  entry->call.arguments = asngn_context_text(t->s, call->arguments);
  if (!entry->call.id || !entry->call.name || !entry->call.arguments) return ASNGN_ERR_NOMEM;
  /* Redaction may alter syntax. Never forward malformed or unredacted arguments. */
  asmodel_json_value *args = NULL;
  int bad = asmodel_json_parse(entry->call.arguments, strlen(entry->call.arguments), &args);
  asmodel_json_free(args);
  if (bad) return ASNGN_ERR_PROTOCOL;
  core_buf b;
  core_buf_init(&b);
  asngn_err e = ASNGN_OK;
  for (size_t i = work_before; i < t->work_n && e == ASNGN_OK; i++) {
    if (b.len > 60000) {
      e = asngn_buf_appends(&b, "\n[remaining observations omitted]\n");
      break;
    }
    e = excerpt(&b, t->work[i].text);
    if (e == ASNGN_OK) e = asngn_buf_appendc(&b, '\n');
  }
  if (e == ASNGN_OK && !b.len)
    e = asngn_buf_appends(&b,
                          "Control accepted; no tool result or verification receipt was produced.");
  if (e == ASNGN_OK) entry->result = asngn_context_text(t->s, b.data);
  core_buf_free(&b);
  if (e != ASNGN_OK) return e;
  if (!entry->result) return ASNGN_ERR_NOMEM;
  h->bytes += strlen(entry->call.arguments) + strlen(entry->result) + 256;
  return h->bytes <= ASNGN_NATIVE_BYTES ? ASNGN_OK : ASNGN_ERR_CONTEXT;
}

asngn_err asngn_native_input(asngn_ctx *c, asngn_turn_state *t, const asngn_native_thread *th,
                             const char *rules, const asngn_native_history *h, core_buf *state,
                             int ceiling, const char *closing, asmodel_message **messages,
                             asmodel_block **blocks, asmodel_input *out) {
  /* Everything that changes from round to round is in this last message, so
   * that what comes before it only grows: a server that keeps the state of
   * the previous prompt then reads just the new calls, their results and
   * this state. System and developer messages must lead, so it is a user
   * message. Nothing in it depends on a routing verdict. */
  asngn_err e = asngn_buf_printf(
      state,
      "[asterism runtime state, not from the user] artifact_written=%s; "
      "verification_current=%s; authorization_blocked=%s; remaining_actions=%d; "
      "tools=%zu (registry %llu, %zu not shown); output ceiling %d tokens.\n",
      t->artifact_written ? "true" : "false", asngn_verification_current(t) ? "true" : "false",
      t->authorization_blocked ? "true" : "false", c->cfg.max_steps - t->steps,
      astools_selection_count(t->tool_selection),
      (unsigned long long)astools_selection_revision(t->tool_selection),
      astools_selection_omitted(t->tool_selection), ceiling);
  if (e == ASNGN_OK && c->cfg.classifier != ASNGN_CLASSIFIER_OFF)
    e = asngn_buf_printf(state, "Response style: %s\n", asngn_detail_directive(t->detail));
  if (e == ASNGN_OK) e = asngn_work_render(t->s, state);
  if (e == ASNGN_OK && closing) e = asngn_buf_printf(state, "\n%s\n", closing);
  if (e != ASNGN_OK) return e;
  if (!th->n || th->v[0].role != 's') return ASNGN_ERR_INVALID;
  size_t cap = th->n + 2 * h->count + 3, n = 0;
  asmodel_message *m = calloc(cap, sizeof *m);
  asmodel_block *b = calloc(cap, sizeof *b);
  if (!m || !b) {
    free(m);
    free(b);
    return ASNGN_ERR_NOMEM;
  }
  b[n] = (asmodel_block){.kind = ASMODEL_BLOCK_TEXT, .text = th->v[0].text};
  m[n] = (asmodel_message){ASMODEL_ROLE_SYSTEM, &b[n], 1};
  n++;
  b[n] = (asmodel_block){.kind = ASMODEL_BLOCK_TEXT, .text = rules};
  m[n] = (asmodel_message){ASMODEL_ROLE_DEVELOPER, &b[n], 1};
  n++;
  for (size_t i = 1; i < th->n; i++, n++) {
    const asngn_thread_msg *v = &th->v[i];
    switch (v->role) {
    case 'c':
      b[n] = (asmodel_block){.kind = ASMODEL_BLOCK_TOOL_CALL, .id = v->id, .name = v->name, .text = v->text};
      m[n] = (asmodel_message){ASMODEL_ROLE_ASSISTANT, &b[n], 1};
      break;
    case 'r':
      b[n] = (asmodel_block){.kind = ASMODEL_BLOCK_TOOL_RESULT, .id = v->id, .text = v->text};
      m[n] = (asmodel_message){ASMODEL_ROLE_TOOL, &b[n], 1};
      break;
    case 'a':
      b[n] = (asmodel_block){.kind = ASMODEL_BLOCK_TEXT, .text = v->text};
      m[n] = (asmodel_message){ASMODEL_ROLE_ASSISTANT, &b[n], 1};
      break;
    default: /* u, m */
      b[n] = (asmodel_block){.kind = ASMODEL_BLOCK_TEXT, .text = v->text};
      m[n] = (asmodel_message){ASMODEL_ROLE_USER, &b[n], 1};
      break;
    }
  }
  for (size_t i = 0; i < h->count; i++) {
    const asngn_native_entry *v = &h->entries[i];
    b[n] = (asmodel_block){.kind = ASMODEL_BLOCK_TOOL_CALL,
                           .id = v->call.id, .name = v->call.name, .text = v->call.arguments};
    m[n] = (asmodel_message){ASMODEL_ROLE_ASSISTANT, &b[n], 1};
    n++;
    b[n] = (asmodel_block){.kind = ASMODEL_BLOCK_TOOL_RESULT, .id = v->call.id, .text = v->result};
    m[n] = (asmodel_message){ASMODEL_ROLE_TOOL, &b[n], 1};
    n++;
  }
  b[n] = (asmodel_block){.kind = ASMODEL_BLOCK_TEXT, .text = state->data};
  m[n] = (asmodel_message){ASMODEL_ROLE_USER, &b[n], 1};
  n++;
  *messages = m;
  *blocks = b;
  *out = (asmodel_input){m, n};
  return ASNGN_OK;
}
