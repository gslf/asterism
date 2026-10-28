/* daemon.h — internal shape of the serve daemon, shared by its files. */
#ifndef ASNGN_SERVE_DAEMON_H
#define ASNGN_SERVE_DAEMON_H

#include "serve.h"

typedef struct serve_star {
  serve_star_config cfg;
  char root[1100];
  asngn_ctx *ctx;
  aslink_ctx *bot;       /* own bot, or the main bot (shared)           */
  bool own_bot;
  struct serve_daemon *d;
  os_thread poller;
  bool poller_started;
} serve_star;

/* One conversation: a star and a Telegram thread, or a routine. */
typedef struct {
  bool used;
  serve_star *star;
  char slug[64];
  asngn_session *s;
  asngn_task *task;      /* running turn, or NULL                        */
  aslink_ctx *bot;
  int64_t chat, thread;
  bool routine;
  char routine_name[65];
  char project[41];      /* registered project the session works in      */
  /* last finished turn, for /skill save */
  char *last_request, *last_answer;
  char steps[512];       /* "fs.list grep.search ..." of the last turn   */
  char turn_steps[512];  /* collected while the turn runs                */
  size_t turn_delegates;
  char skill_used[65];   /* skill hint injected into the running turn   */
} serve_session;

typedef enum { JOB_MESSAGE = 0, JOB_ROUTINE } serve_job_kind;

typedef struct {
  serve_job_kind kind;
  serve_star *star;
  aslink_ctx *bot;
  int64_t chat, thread, reply_to;
  char *text;            /* owned                                        */
  asngn_origin origin;
  char forward_origin[160];
  /* attachment */
  char file_id[256], file_name[256], file_mime[128];
  int64_t file_size;
  bool voice;
  /* routine */
  char routine[65];
} serve_job;

typedef struct {
  serve_star *star;
  char slug[64];
  char confirm_id[40];
} serve_approval_job;

/* An approval message on Telegram, to resolve its button later. */
typedef struct {
  bool used;
  char id[40];
  serve_star *star;
  aslink_ctx *bot;
  int64_t chat, message_id;
  bool tainted;
} serve_pending_approval;

struct serve_daemon {
  serve_config cfg;
  serve_state st;
  serve_star stars[SERVE_STARS_MAX];
  size_t stars_n;
  aslink_ctx *main_bot;
  volatile int stop;
  os_mutex mu;
  os_cond cv;
  serve_job jobs[SERVE_JOBS_MAX];
  size_t jobs_head, jobs_n;
  serve_approval_job approvals[32];
  size_t approvals_n;
  serve_pending_approval pending[32];
  serve_session sessions[SERVE_SESSIONS_MAX];
  os_thread runners[SERVE_RUNNERS], notifier, scheduler;
  int runners_n;
  bool notifier_started, scheduler_started;
  void *routines;        /* astime store                                 */
  char *paid_tiers[8];
  size_t paid_tiers_n;
};

/* daemon.c */
bool serve_enqueue(struct serve_daemon *d, serve_job *job); /* takes text */
void serve_send(serve_star *star, aslink_ctx *bot, int64_t chat, int64_t thread,
                const char *text, bool html);
void serve_sendf(serve_star *star, aslink_ctx *bot, int64_t chat, int64_t thread,
                 bool html, const char *fmt, ...);
serve_star *serve_star_find(struct serve_daemon *d, const char *name);

/* routines.c */
bool serve_routines_open(struct serve_daemon *d, char *err, size_t n);
void serve_routines_close(struct serve_daemon *d);
void serve_routines_tick(struct serve_daemon *d);
size_t serve_routines_count(struct serve_daemon *d);
void serve_routines_command(struct serve_daemon *d, serve_star *star, aslink_ctx *bot,
                            int64_t chat, int64_t thread, const char *args);
void serve_routines_callback(struct serve_daemon *d, aslink_ctx *bot, int64_t chat,
                             int64_t thread, int64_t message_id, const char *name,
                             char choice);
/* Builds the turn text of a due routine; NULL when nothing changed. */
char *serve_routines_turn_text(struct serve_daemon *d, const char *name,
                               asngn_origin *origin, char star[33]);
void serve_routines_finished(struct serve_daemon *d, const char *name, bool ok,
                             const char *answer, const char *error);
/* Applies the routine's mode and profile to its session. */
void serve_routines_prepare(struct serve_daemon *d, const char *name, asngn_session *s);
/* false when the routine's notify policy is "never". */
bool serve_routines_delivers(struct serve_daemon *d, const char *name);

/* skills.c */
void serve_skills_command(struct serve_daemon *d, serve_star *star, aslink_ctx *bot,
                          int64_t chat, int64_t thread, serve_session *sess,
                          const char *args);
/* A procedure hint for a new request, or NULL; *name gets the skill.
 * *untrusted is set when the procedure came from a delegated agent and the
 * owner has not saved it: the turn is then tainted. */
char *serve_skills_hint(serve_star *star, const char *request, char name[65],
                        bool *untrusted);
void serve_skills_outcome(struct serve_daemon *d, serve_star *star, const char *name,
                          bool ok);
/* After a delegated task succeeded, keep what the stronger agent did as a
 * provisional playbook for the local model. */
void serve_skills_playbook(struct serve_daemon *d, serve_star *star, const char *request,
                           const char *steps, const char *answer);

#endif
