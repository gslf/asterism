/*
 * serve.h — "asterism serve": the always-on agent daemon.
 *
 * The daemon is a thin front-end over the public libasngn API, like the TUI,
 * MCP and ACP modes. It adds what an agent reachable from a phone needs:
 *
 *   - channels (aslink): Telegram long polling, outbound connections only;
 *   - an owner: one Telegram account paired with a one-time code. Every other
 *     sender is ignored; forwarded text becomes untrusted turn input;
 *   - remote approvals: the review message is rendered by the daemon from the
 *     engine's durable approval record, never from model text, and its buttons
 *     carry an HMAC bound to the approval, the decision and the chat;
 *   - routines (astime): scheduled and watched turns delivered to the owner;
 *   - stars: named agents, each with its own engine root (memory, tools,
 *     sessions, secrets) and optionally its own bot.
 *
 * Threads: one poller per bot, one scheduler, a small pool of turn runners
 * and one notifier for approvals. Engine event callbacks only enqueue.
 */

#ifndef ASNGN_SERVE_H
#define ASNGN_SERVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "asngn.h"
#include "aslink.h"
#include "astime.h"
#include "core.h"

#define SERVE_STARS_MAX   8
#define SERVE_RUNNERS     4
#define SERVE_JOBS_MAX    64
#define SERVE_SESSIONS_MAX 128

/* One named agent. The default star uses the daemon's engine root. */
typedef struct {
  char name[33];
  char root[1024];            /* engine root; "" = the daemon's root   */
  char config[1024];          /* config.xcdn override; "" = discover   */
  char token_env[65];         /* own bot; "" = shares the main bot     */
  asngn_usage_mode mode;
  asngn_security_profile profile;
  char identity[1025];        /* who this star is, prefixed to turns   */
} serve_star_config;

typedef struct {
  /* Telegram */
  char token_env[65];         /* environment variable holding the token */
  char token_secret[65];      /* or: vault secret allowed for "serve"    */
  char api_base[256];         /* "" = https://api.telegram.org          */
  int  poll_timeout_s;
  bool topics;                /* one topic per task in the private chat */
  /* speech-to-text: argv with "{input}" and "{output}" placeholders     */
  char *stt_argv[16];
  size_t stt_argc;
  char stt_output_ext[16];    /* appended to {output}, e.g. ".txt"     */
  /* routines */
  char routines_dir[1024];    /* relative to the root                  */
  int  routine_tick_s;
  /* stars */
  serve_star_config stars[SERVE_STARS_MAX];
  size_t stars_n;
  /* inbox for photos and documents, per session workspace */
  uint64_t max_download_bytes;
} serve_config;

void serve_config_defaults(serve_config *cfg);
/* Parses <root>/serve.xcdn or `path`; a missing default file keeps defaults.
 * err receives a one-line reason on failure. */
bool serve_config_load(serve_config *cfg, const char *root, const char *path,
                       char *err, size_t err_size);
void serve_config_free(serve_config *cfg);

/* ── persistent daemon state (<root>/serve/) ──────────────────────────── */

typedef struct {
  char dir[1100];
  uint8_t key[32];            /* HMAC key for buttons, 0600 file        */
  int64_t owner_id;           /* 0 = not paired                         */
  int64_t owner_chat;
  int64_t routine_thread;     /* topic for routine deliveries; 0 = chat */
  os_mutex mu;
} serve_state;

bool serve_state_open(serve_state *st, const char *root, char *err, size_t n);
void serve_state_close(serve_state *st);
bool serve_state_save(serve_state *st);
/* Writes a fresh pairing code (8 digits, valid 10 minutes) and returns it. */
bool serve_pairing_create(serve_state *st, char code[16], char *err, size_t n);
/* Consumes a matching, unexpired code: the sender becomes the owner. */
bool serve_pairing_claim(serve_state *st, const char *code, int64_t user_id,
                         int64_t chat_id);
/* Topic bindings: which star answers in a chat thread. */
bool serve_binding_get(serve_state *st, int64_t chat, int64_t thread,
                       char star[33]);
bool serve_binding_set(serve_state *st, int64_t chat, int64_t thread,
                       const char *star);
/* The project a thread works in, under its star ("" none). Switching the
 * thread's star with serve_binding_set clears it. */
bool serve_binding_project(serve_state *st, int64_t chat, int64_t thread,
                           const char *star, char project[41]);
bool serve_binding_set_project(serve_state *st, int64_t chat, int64_t thread,
                               const char *star, const char *project);
/* The first thread bound to a project, and unbinding every thread. */
bool serve_binding_project_thread(serve_state *st, const char *star,
                                  const char *project, int64_t *chat,
                                  int64_t *thread);
void serve_binding_drop_project(serve_state *st, const char *star,
                                const char *project);

/* Button tokens: "<kind>:<id>:<choice>:<mac>" within Telegram's 64 bytes. */
bool serve_token_make(const serve_state *st, char kind, const char *id,
                      char choice, int64_t chat, char *out, size_t out_size);
bool serve_token_check(const serve_state *st, const char *token, int64_t chat,
                       char *kind, char id[40], char *choice);

/* ── daily usage: how much ran locally ───────────────────────────────── */

typedef struct {
  int64_t day;                /* unix day                               */
  int64_t turns, local_turns, paid_turns, delegate_calls, routine_runs;
} serve_usage;

void serve_usage_note(serve_state *st, bool paid, size_t delegates,
                      bool routine);
bool serve_usage_get(serve_state *st, serve_usage *today, serve_usage *month);

/* ── strings ─────────────────────────────────────────────────────────── */

typedef enum {
  S_WELCOME, S_PAIRED, S_NOT_OWNER, S_PAIR_FIRST, S_BUSY, S_STOPPED,
  S_NOTHING_RUNNING, S_WORKING, S_APPROVAL_TITLE, S_APPROVAL_TOOL,
  S_APPROVAL_TAINT, S_APPROVAL_ARGS, S_ALLOW, S_DENY, S_ALWAYS, S_ALLOWED,
  S_DENIED, S_EXPIRED, S_HELP, S_TOPIC_CREATED, S_VOICE_HEARD,
  S_VOICE_NO_STT, S_ATTACHMENT_SAVED, S_ROUTINE_NONE, S_ROUTINE_ACTIVATE,
  S_ROUTINE_CANCEL, S_ROUTINE_ON, S_ROUTINE_OFF, S_ROUTINE_SAVED,
  S_ROUTINE_UNKNOWN, S_ROUTINE_RESULT, S_QUOTA, S_STAR_NOW, S_STAR_UNKNOWN,
  S_STATUS, S_ERROR, S_SKILL_SAVED, S_SKILL_REMOVED, S_SKILL_NONE,
  S_SKILL_OFFER, S_MEMORY_EMPTY, S_FORWARDED_FROM, S_ROUTINE_RUNNING,
  S_UNTRUSTED_NOTE, S_PROJECT_NONE, S_PROJECT_HERE, S_PROJECT_HERE_NONE,
  S_PROJECT_CREATED, S_PROJECT_BOUND, S_PROJECT_OFF, S_PROJECT_REMOVED,
  S_PROJECT_QUEUED, S_HELLO, S__COUNT
} serve_string;

const char *serve_s(serve_string key);

/* ── the daemon ──────────────────────────────────────────────────────── */

typedef struct serve_daemon serve_daemon;

typedef struct {
  const char *root;           /* engine root of the default star        */
  const char *config;         /* engine config of the default star      */
  const char *serve_config;   /* serve.xcdn; NULL = <root>/serve.xcdn   */
  const char *workspace;      /* optional workspace override            */
  int allow_degraded;
} serve_params;

/* Runs until SIGINT/SIGTERM or serve_daemon_stop. Returns an exit status. */
int serve_run(const serve_params *p);

#endif /* ASNGN_SERVE_H */
