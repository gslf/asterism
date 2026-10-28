/*
 * delegate.h — internals of the delegate tool (tool_delegate.c):
 * the event-stream parsers of the two official agent CLIs and the
 * supervised streaming runner that feeds them.
 *
 * Parsers (delegate_events.c) are pure: one JSON line in, state updated,
 * never a failure. Unknown event types, unknown fields and lines that are
 * not JSON are counted and ignored, because both CLIs add event types
 * between releases. Memory is bounded: texts are capped at fixed sizes and
 * only the last DLG_TAIL_CHUNKS assistant texts are kept.
 *
 * Event shapes (verified against the vendors' documentation, Oct 2026):
 *   Claude Code  `claude -p --output-format stream-json --verbose`
 *     {type:"system",subtype:"init",session_id,model,...}
 *     {type:"system",subtype:"api_retry",error:"rate_limit"|...,attempt}
 *     {type:"assistant",message:{content:[{type:"text",text}|
 *        {type:"tool_use",...}]},parent_tool_use_id,error?}
 *     {type:"user",message:{content:[{type:"tool_result",...}]}}
 *     {type:"rate_limit_event",rate_limit_info:{status:"allowed"|
 *        "allowed_warning"|"rejected",resetsAt?,utilization?}}
 *     {type:"result",subtype:"success"|"error_*",is_error,result?,errors?,
 *        num_turns,total_cost_usd,usage:{input_tokens,output_tokens,
 *        cache_read_input_tokens,cache_creation_input_tokens},session_id,
 *        api_error_status?}
 *   Codex        `codex exec --json`
 *     {type:"thread.started",thread_id} {type:"turn.started"}
 *     {type:"item.started"|"item.updated"|"item.completed",item:{id,type:
 *        "agent_message"|"reasoning"|"command_execution"|"file_change"|
 *        "mcp_tool_call"|"web_search"|"todo_list"|"error",...}}
 *     {type:"turn.completed",usage:{input_tokens,cached_input_tokens,
 *        output_tokens,reasoning_output_tokens}}
 *     {type:"turn.failed",error:{message}}  (ends the turn)
 *     {type:"error",message}  (also emitted for every transient retry,
 *        "Reconnecting... 2/5 (...)": not by itself a failure)
 * Both shapes were also checked against the installed CLIs (Claude Code
 * 2.1.310, codex-cli 0.164.0) run without a login.
 */

#ifndef ASTOOLS_STD_DELEGATE_H
#define ASTOOLS_STD_DELEGATE_H

#include <stddef.h>
#include <stdint.h>

/* ---- bounds ------------------------------------------------------------ */

#define DLG_TAIL_CHUNKS 6          /* last assistant texts kept           */
#define DLG_CHUNK_CAP 8192         /* bytes per kept assistant text       */
#define DLG_FINAL_CAP 65536        /* bytes of the final answer kept      */
#define DLG_ERROR_CAP 4096         /* bytes of an error message           */
#define DLG_LIMIT_CAP 1024         /* bytes of a limit message            */
#define DLG_LINE_CAP (8u << 20)    /* longest event line parsed (8 MiB)   */
#define DLG_STDERR_TAIL 4096       /* stderr bytes reported on failure    */

typedef enum { DLG_CLAUDE = 0, DLG_CODEX = 1 } dlg_agent;

typedef struct {
  dlg_agent agent;
  /* identity reported by the CLI */
  char *session_id; /* claude session_id / codex thread_id */
  char *model;
  /* outcome */
  int saw_result;   /* claude: a result event; codex: turn.completed   */
  int failed;       /* claude: last result is_error; codex: turn.failed */
  char *subtype;    /* claude: last result subtype                     */
  char *final;      /* claude: result text; codex: last agent_message  */
  int final_truncated; /* final was cut at DLG_FINAL_CAP                */
  char *error;      /* error text reported by the CLI                   */
  int auth_error;   /* the CLI reported a missing or rejected login     */
  /* last assistant texts of the main conversation (ring) */
  char *chunks[DLG_TAIL_CHUNKS];
  size_t chunk_next, chunk_count;
  /* accounting */
  int64_t turns;
  int have_usage;
  int64_t input_tokens, output_tokens, cached_input_tokens;
  int have_cost;
  double cost_usd;
  /* usage / rate limits */
  int limit_seen, limit_hit;
  char *limit_message;
  int64_t limit_resets_at; /* unix seconds; 0 = unknown */
  /* stream health */
  uint64_t events, malformed;
} dlg_state;

void dlg_state_init(dlg_state *s, dlg_agent agent);
void dlg_state_free(dlg_state *s);
/* Feed one output line (no trailing newline required). Never fails: a line
 * that is not a JSON object counts as malformed; allocation failures only
 * lose text. */
void dlg_feed_line(dlg_state *s, const char *line, size_t len);
/* The last kept assistant texts joined oldest first ("" when none);
 * malloc'd, NULL on allocation failure. */
char *dlg_tail_text(const dlg_state *s);

/* Length of the longest prefix of s[0..len) that is at most cap bytes and
 * does not split a UTF-8 sequence. */
size_t dlg_utf8_cut(const char *s, size_t len, size_t cap);
/* 1 when text names a usage or rate limit (ASCII case-insensitive). */
int dlg_text_is_limit(const char *text);
/* 1 when text says the CLI is not logged in or its login was rejected. */
int dlg_text_is_auth(const char *text);

/* ---- supervised streaming runner (POSIX, delegate_run.c) --------------- */

typedef void (*dlg_line_fn)(void *ud, const char *line, size_t len);

typedef struct {
  char *const *argv;    /* argv[0] absolute; never searched on PATH  */
  char *const *envp;    /* complete child environment                 */
  const char *cwd;      /* child working directory                    */
  const char *input;    /* written to stdin then closed; NULL = /dev/null */
  size_t input_n;
  int64_t timeout_ms;   /* 0 = none; then SIGTERM, grace, SIGKILL      */
  int64_t grace_ms;     /* SIGTERM -> SIGKILL                          */
  size_t max_line;      /* longer stdout lines are skipped and counted */
  size_t stderr_cap;    /* bytes of stderr tail kept                  */
  dlg_line_fn on_line;  /* called per complete stdout line            */
  void *ud;
} dlg_run_opts;

typedef struct {
  int exit_code;        /* exit status, -signal, or -1                */
  int timed_out;        /* timeout_ms elapsed; the child was stopped   */
  int exec_errno;       /* nonzero when exec itself failed             */
  uint64_t stdout_bytes, stderr_bytes;
  uint64_t lines, oversized_lines;
  char *stderr_tail;    /* malloc'd, NUL-terminated (may be "")        */
  size_t stderr_tail_n;
  int64_t duration_ms;
} dlg_run_res;

/* Spawn argv (same process group as the caller so the enclosing runtime
 * can still reap the whole tree), stream stdout lines
 * to on_line with bounded memory and keep a stderr tail. Returns 0 when
 * the child ran (whatever its exit status), -1 with emsg when it could not
 * be started or supervised (res->exec_errno set when exec failed). Free
 * res with dlg_run_res_free in both cases. */
int dlg_run(const dlg_run_opts *o, dlg_run_res *res, char *emsg,
            size_t emsg_sz);
void dlg_run_res_free(dlg_run_res *res);

#endif /* ASTOOLS_STD_DELEGATE_H */
