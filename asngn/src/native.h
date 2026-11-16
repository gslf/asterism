/* Private native-action adapter. Execution authority stays in action_apply. */
#ifndef ASNGN_NATIVE_H
#define ASNGN_NATIVE_H
#include "execution.h"

#define ASNGN_NATIVE_HISTORY 64
#define ASNGN_NATIVE_BYTES (1024 * 1024)
typedef struct {
  asmodel_tool_schema schemas[64];
  const astools_selected_command *commands[64]; /* borrowed from this selection */
  asngn_step_kind kinds[64];
  char names[59][63], descriptions[59][1200];
  size_t count;
} asngn_native_contract;
typedef struct {
  asmodel_tool_call call;
  char *result;
} asngn_native_entry;
typedef struct {
  asngn_native_entry entries[ASNGN_NATIVE_HISTORY];
  size_t count, bytes;
  const struct asngn_native_thread *thread; /* earlier turns: their call ids are taken */
} asngn_native_history;

asngn_err asngn_native_contract_build(asngn_ctx *c, asngn_turn_state *t, bool call_now,
                                      asngn_native_contract *out);
asngn_err asngn_native_steps(asngn_ctx *c, asngn_turn_state *t,
                             const asngn_native_contract *contract, const asmodel_tool_calls *calls,
                             asngn_step steps[32]);
void asngn_native_history_free(asngn_native_history *h);
/* Copies a redacted, bounded observation of this action's working items. */
asngn_err asngn_native_record(asngn_turn_state *t, asngn_native_history *h,
                              const asmodel_tool_call *call, size_t work_before);
/* One message of the stored conversation. role: s system text, m memory of
 * older turns, u user, c tool call, r tool result, a assistant text. */
typedef struct {
  char role;
  char *id, *name, *text;
} asngn_thread_msg;
typedef struct asngn_native_thread {
  asngn_thread_msg *v;
  size_t n, cap, bytes;
} asngn_native_thread;
void asngn_native_thread_free(asngn_native_thread *th);
asngn_err asngn_native_thread_load(const asngn_session *s, asngn_native_thread *th);
asngn_err asngn_native_thread_add(asngn_native_thread *th, char role, const char *id,
                                  const char *name, const char *text);
bool asngn_native_thread_full(asngn_ctx *c, int slot, const asngn_native_thread *th);
/* Messages: the thread, this turn's calls and results, then the state. The
 * arrays are allocated here; free(*messages) and free(*blocks). */
asngn_err asngn_native_input(asngn_ctx *c, asngn_turn_state *t, const asngn_native_thread *th,
                             const char *rules, const asngn_native_history *h, core_buf *state,
                             int ceiling, const char *closing, asmodel_message **messages,
                             asmodel_block **blocks, asmodel_input *out);
/* The response pass over the same conversation, when the action phase did
 * not leave a usable final message. */
asngn_err asngn_native_answer(asngn_ctx *c, asngn_turn_state *t, const char *trailer, int cap,
                              asngn_token_fn cb, void *cb_ud, char **out, int *out_tokens);
#endif
