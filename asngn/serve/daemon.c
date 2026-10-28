/* The serve daemon: stars, Telegram pollers, the turn queue, streaming
 * drafts, remote approvals and shutdown. Engine callbacks only enqueue; no
 * libasngn call is ever made while d->mu is held, because the engine may emit
 * events (and so take d->mu in the sink) from inside any call. */
#include "daemon.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "asvault.h"
#include "xcdn.h"

static volatile sig_atomic_t g_signal;

static void on_signal(int sig) { g_signal = sig; }

/* ── sending ──────────────────────────────────────────────────────────── */

void serve_send(serve_star *star, aslink_ctx *bot, int64_t chat, int64_t thread,
                const char *text, bool html) {
  aslink_send_opts opts;
  char **parts = NULL;
  size_t n = 0;
  (void)star;
  if (!bot || !text || !*text) return;
  memset(&opts, 0, sizeof opts);
  opts.no_preview = 1;
  if (html) {
    /* Host-rendered HTML stays short; send it whole. */
    opts.parse_mode = "HTML";
    (void)aslink_send_message(bot, chat, thread, text, &opts, NULL, NULL);
    return;
  }
  if (aslink_split_text(text, 0, &parts, &n) != ASLINK_OK) return;
  for (size_t i = 0; i < n; i++)
    (void)aslink_send_message(bot, chat, thread, parts[i], &opts, NULL, NULL);
  aslink_strings_free(parts, n);
}

void serve_sendf(serve_star *star, aslink_ctx *bot, int64_t chat, int64_t thread,
                 bool html, const char *fmt, ...) {
  core_buf b;
  va_list ap;
  core_buf_init(&b);
  va_start(ap, fmt);
  if (core_buf_vprintf(&b, fmt, ap) == CORE_OK) serve_send(star, bot, chat, thread, b.data, html);
  va_end(ap);
  core_buf_free(&b);
}

static char *escape(const char *s) {
  char *e = aslink_html_escape(s ? s : "");
  return e ? e : core_strdup("");
}

serve_star *serve_star_find(struct serve_daemon *d, const char *name) {
  for (size_t i = 0; i < d->stars_n; i++)
    if (!strcmp(d->stars[i].cfg.name, name)) return &d->stars[i];
  return NULL;
}

/* ── the job queue ────────────────────────────────────────────────────── */

bool serve_enqueue(struct serve_daemon *d, serve_job *job) {
  bool ok = false;
  os_mutex_lock(&d->mu);
  if (d->jobs_n < SERVE_JOBS_MAX && !d->stop) {
    d->jobs[(d->jobs_head + d->jobs_n) % SERVE_JOBS_MAX] = *job;
    d->jobs_n++;
    ok = true;
    os_cond_broadcast(&d->cv);
  }
  os_mutex_unlock(&d->mu);
  if (!ok) free(job->text);
  job->text = NULL;
  return ok;
}

static bool dequeue(struct serve_daemon *d, serve_job *out) {
  os_mutex_lock(&d->mu);
  while (!d->stop && d->jobs_n == 0) os_cond_timedwait(&d->cv, &d->mu, 500);
  bool ok = !d->stop && d->jobs_n > 0;
  if (ok) {
    *out = d->jobs[d->jobs_head];
    d->jobs_head = (d->jobs_head + 1) % SERVE_JOBS_MAX;
    d->jobs_n--;
  }
  os_mutex_unlock(&d->mu);
  return ok;
}

/* ── sessions ─────────────────────────────────────────────────────────── */

static void slug_for(char out[64], int64_t chat, int64_t thread) {
  snprintf(out, 64, "tg-%s%lld-%lld", chat < 0 ? "n" : "",
           (long long)(chat < 0 ? -chat : chat), (long long)thread);
}

/* Finds or reserves the conversation slot; opens the engine session outside
 * the lock. NULL when the table is full or the session cannot open. */
static serve_session *session_get(struct serve_daemon *d, serve_star *star,
                                  const char *slug, aslink_ctx *bot, int64_t chat,
                                  int64_t thread) {
  serve_session *slot = NULL;
  os_mutex_lock(&d->mu);
  for (size_t i = 0; i < SERVE_SESSIONS_MAX && !slot; i++)
    if (d->sessions[i].used && d->sessions[i].star == star &&
        !strcmp(d->sessions[i].slug, slug))
      slot = &d->sessions[i];
  if (!slot)
    for (size_t i = 0; i < SERVE_SESSIONS_MAX && !slot; i++)
      if (!d->sessions[i].used) {
        slot = &d->sessions[i];
        memset(slot, 0, sizeof *slot);
        slot->used = true;
        slot->star = star;
        snprintf(slot->slug, sizeof slot->slug, "%s", slug);
      }
  if (slot) {
    slot->bot = bot;
    slot->chat = chat;
    slot->thread = thread;
  }
  bool need_open = slot && !slot->s;
  os_mutex_unlock(&d->mu);
  if (!slot || !need_open) return slot;
  asngn_session *s = NULL;
  asngn_session_peek_info peek;
  asngn_project_info proj;
  bool peeked = asngn_session_peek(star->ctx, slug, &peek) == ASNGN_OK;
  bool fresh = !peeked || peek.turns == 0;
  if (asngn_session_open(star->ctx, slug, &s) != ASNGN_OK) {
    os_mutex_lock(&d->mu);
    slot->used = false;
    os_mutex_unlock(&d->mu);
    return NULL;
  }
  /* A new conversation takes the star's mode; a resumed one keeps its own. */
  if (fresh) {
    (void)asngn_session_set_mode(s, star->cfg.mode);
    (void)asngn_session_set_security_profile(s, star->cfg.profile);
  }
  os_mutex_lock(&d->mu);
  slot->s = s;
  if (peeked && peek.project[0] && asngn_project_get(star->ctx, peek.project, &proj) == ASNGN_OK)
    snprintf(slot->project, sizeof slot->project, "%s", proj.name);
  os_mutex_unlock(&d->mu);
  return slot;
}

/* The thread's project becomes the session's before each turn: its folder
 * is the workspace. Leaving a project restores the star's mode. False when
 * it cannot apply (reported in the thread). */
static bool apply_project(struct serve_daemon *d, serve_star *star, serve_session *sess,
                          const serve_job *job) {
  char want[41] = "";
  asngn_project_info p;
  if (job->kind == JOB_MESSAGE)
    (void)serve_binding_project(&d->st, job->chat, job->thread, star->cfg.name, want);
  if (want[0] && asngn_project_get(star->ctx, want, &p) != ASNGN_OK) {
    /* removed from another frontend: the thread is free again */
    (void)serve_binding_set_project(&d->st, job->chat, job->thread, star->cfg.name, "");
    want[0] = 0;
  }
  if (!strcmp(want, sess->project)) return true;
  if (asngn_session_project(sess->s, want[0] ? want : NULL) != ASNGN_OK) {
    serve_sendf(star, job->bot, job->chat, job->thread, false, serve_s(S_ERROR),
                asngn_last_error(star->ctx));
    return false;
  }
  if (!want[0]) {
    (void)asngn_session_set_mode(sess->s, star->cfg.mode);
    (void)asngn_session_set_security_profile(sess->s, star->cfg.profile);
  }
  os_mutex_lock(&d->mu);
  snprintf(sess->project, sizeof sess->project, "%s", want);
  os_mutex_unlock(&d->mu);
  return true;
}

static serve_session *session_by_slug(struct serve_daemon *d, serve_star *star,
                                      const char *slug) {
  for (size_t i = 0; i < SERVE_SESSIONS_MAX; i++)
    if (d->sessions[i].used && d->sessions[i].star == star &&
        !strcmp(d->sessions[i].slug, slug))
      return &d->sessions[i];
  return NULL;
}

/* ── engine events: approvals and step tracking ───────────────────────── */

static const xcdn_value_t *obj_field(const xcdn_value_t *obj, const char *key) {
  xcdn_node_t *n = obj && obj->type == XCDN_VAL_OBJECT ? xcdn_object_get(obj, key) : NULL;
  return n ? n->value : NULL;
}

static const char *obj_str(const xcdn_value_t *obj, const char *key) {
  const xcdn_value_t *v = obj_field(obj, key);
  return v && (v->type == XCDN_VAL_STRING || v->type == XCDN_VAL_UUID)
             ? xcdn_value_as_string(v) : NULL;
}

static void on_event(const char *event, void *ud) {
  serve_star *star = ud;
  struct serve_daemon *d = star->d;
  bool confirm = strstr(event, "kind: \"confirm\"") != NULL;
  bool tool = strstr(event, "kind: \"tool_call\"") != NULL;
  if (!confirm && !tool) return;
  xcdn_error_t xe;
  xcdn_document_t *doc = xcdn_parse_str(event, strlen(event), &xe);
  const xcdn_value_t *root = doc && doc->values_len ? doc->values[0]->value : NULL;
  const char *session = obj_str(root, "session");
  const xcdn_value_t *data = obj_field(root, "data");
  if (session && confirm) {
    const char *id = obj_str(data, "confirm_id");
    os_mutex_lock(&d->mu);
    if (id && strlen(id) < 40 && d->approvals_n < 32) {
      serve_approval_job *a = &d->approvals[d->approvals_n++];
      a->star = star;
      snprintf(a->slug, sizeof a->slug, "%s", session);
      snprintf(a->confirm_id, sizeof a->confirm_id, "%s", id);
      os_cond_broadcast(&d->cv);
    }
    os_mutex_unlock(&d->mu);
  } else if (session && tool) {
    const char *t = obj_str(data, "tool"), *c = obj_str(data, "command");
    os_mutex_lock(&d->mu);
    serve_session *sess = session_by_slug(d, star, session);
    if (sess && t) {
      size_t len = strlen(sess->turn_steps);
      if (len + strlen(t) + (c ? strlen(c) : 0) + 3 < sizeof sess->turn_steps)
        snprintf(sess->turn_steps + len, sizeof sess->turn_steps - len, "%s%s%s%s",
                 len ? " " : "", t, c ? "." : "", c ? c : "");
      if (!strncmp(t, "delegate", 8)) sess->turn_delegates++;
    }
    os_mutex_unlock(&d->mu);
  }
  if (doc) xcdn_document_free(doc);
}

/* ── speech to text (POSIX) ───────────────────────────────────────────── */

static char *replace_all(const char *s, const char *from, const char *to) {
  core_buf b;
  size_t fl = strlen(from);
  core_buf_init(&b);
  while (*s) {
    if (!strncmp(s, from, fl)) {
      core_buf_appends(&b, to);
      s += fl;
    } else core_buf_appendc(&b, *s++);
  }
  return core_buf_detach(&b);
}

static char *transcribe(struct serve_daemon *d, const char *audio) {
#if defined(_WIN32)
  (void)d; (void)audio;
  return NULL;
#else
  char out_base[1700], out_file[1800];
  char *argv[17] = {0};
  char *text = NULL;
  size_t len = 0;
  if (!d->cfg.stt_argc) return NULL;
  snprintf(out_base, sizeof out_base, "%s.stt", audio);
  snprintf(out_file, sizeof out_file, "%s%s", out_base, d->cfg.stt_output_ext);
  for (size_t i = 0; i < d->cfg.stt_argc; i++) {
    char *a = replace_all(d->cfg.stt_argv[i], "{input}", audio);
    argv[i] = a ? replace_all(a, "{output}", out_base) : NULL;
    free(a);
  }
  pid_t pid = fork();
  if (pid == 0) {
    /* stdout is the transcript when the tool prints it instead. */
    FILE *f = fopen(out_file, "wb");
    if (f) {
      dup2(fileno(f), 1);
      fclose(f);
    }
    execvp(argv[0], argv);
    _exit(127);
  }
  int status = 0;
  if (pid > 0) {
    int64_t deadline = os_monotonic_ms() + 300000;
    while (waitpid(pid, &status, WNOHANG) == 0) {
      if (os_monotonic_ms() > deadline) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        break;
      }
      os_sleep_ms(100);
    }
  }
  for (size_t i = 0; i < d->cfg.stt_argc; i++) free(argv[i]);
  if (pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
      os_read_file(out_file, &text, &len) == CORE_OK) {
    core_str_trim(text);
    if (!*text || !core_utf8_valid(text, strlen(text))) {
      free(text);
      text = NULL;
    }
  }
  os_remove_file(out_file);
  return text;
#endif
}

/* ── running a turn ───────────────────────────────────────────────────── */

typedef struct {
  os_mutex mu;
  core_buf out;
  char status[256];
  bool changed;
} stream_sink;

static void on_stream(const asngn_stream_event *ev, void *ud) {
  stream_sink *s = ud;
  os_mutex_lock(&s->mu);
  if (ev->kind == ASNGN_STREAM_OUTPUT) (void)core_buf_appends(&s->out, ev->text);
  else snprintf(s->status, sizeof s->status, "%s", ev->text);
  s->changed = true;
  os_mutex_unlock(&s->mu);
}

static bool tier_paid(struct serve_daemon *d, const char *tier) {
  for (size_t i = 0; i < d->paid_tiers_n; i++)
    if (!strcmp(d->paid_tiers[i], tier)) return true;
  return false;
}

static char *draft_text(stream_sink *s) {
  core_buf b;
  core_buf_init(&b);
  os_mutex_lock(&s->mu);
  if (s->out.len) {
    /* Drafts show the tail; the final message carries everything. */
    size_t start = s->out.len > 3500 ? s->out.len - 3500 : 0;
    while (start < s->out.len && (s->out.data[start] & 0xC0) == 0x80) start++;
    core_buf_appends(&b, s->out.data + start);
  } else if (s->status[0]) {
    core_buf_printf(&b, "⏳ %s", s->status);
  }
  s->changed = false;
  os_mutex_unlock(&s->mu);
  return core_buf_detach(&b);
}

static char *compose(serve_star *star, serve_session *sess, serve_job *job, char *text,
                     asngn_origin *origin) {
  core_buf b;
  char *hint = NULL;
  bool untrusted = false;
  core_buf_init(&b);
  if (star->cfg.identity[0]) core_buf_printf(&b, "[%s]\n", star->cfg.identity);
  if (job->kind == JOB_MESSAGE && job->origin == ASNGN_ORIGIN_OWNER && text &&
      (hint = serve_skills_hint(star, text, sess->skill_used, &untrusted)) != NULL) {
    core_buf_printf(&b, "%s\n", hint);
    /* A playbook written by a delegated agent is advice, not the owner's word. */
    if (untrusted) *origin = ASNGN_ORIGIN_DELEGATE;
  }
  free(hint);
  if (job->origin == ASNGN_ORIGIN_FORWARDED) {
    core_buf_printf(&b, serve_s(S_FORWARDED_FROM),
                    job->forward_origin[0] ? job->forward_origin : "?");
    core_buf_appendc(&b, '\n');
  }
  if (text) core_buf_appends(&b, text);
  return core_buf_detach(&b);
}

/* A project's inbox stays out of its repository's status. */
static void exclude_inbox(const asngn_workspace_info *ws) {
  char path[1300], *text = NULL;
  size_t len = 0;
  snprintf(path, sizeof path, "%s/.git/info", ws->repository_root);
  if (!ws->repository_root[0] || !os_is_dir(path)) return;
  snprintf(path, sizeof path, "%s/.git/info/exclude", ws->repository_root);
  if (os_read_file(path, &text, &len) != CORE_OK) len = 0;
  if (!text || !strstr(text, ".asterism/")) {
    FILE *f = os_fopen(path, "ab");
    if (f) {
      fputs(len && text[len - 1] != '\n' ? "\n.asterism/\n" : ".asterism/\n", f);
      fclose(f);
    }
  }
  free(text);
}

/* Downloads an attachment into the session workspace inbox; in a project,
 * into <project>/.asterism/inbox. */
static bool fetch_attachment(struct serve_daemon *d, serve_session *sess, serve_job *job,
                             char *rel, size_t rel_size, char *abs, size_t abs_size) {
  asngn_workspace_info ws;
  char name[200] = "", dir[1200];
  const char *src = job->file_name[0] ? job->file_name : NULL;
  if (asngn_session_workspace(sess->s, &ws) != ASNGN_OK) return false;
  /* Keep a safe basename; never trust the sender's path. */
  if (src) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < sizeof name; i++) {
      char ch = src[i];
      bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_';
      name[j++] = ok ? ch : '_';
    }
    name[j] = 0;
  }
  if (!name[0] || name[0] == '.') {
    char id[37];
    core_uuid_v4(id);
    snprintf(name, sizeof name, "%.8s%s", id,
             job->voice ? ".oga" : strstr(job->file_mime, "image/") ? ".jpg" : ".bin");
  }
  const char *inbox = sess->project[0] ? ".asterism/inbox" : "inbox";
  snprintf(dir, sizeof dir, "%s/%s", ws.canonical_root, inbox);
  if (os_mkdir_p(dir) != CORE_OK) return false;
  if (sess->project[0]) exclude_inbox(&ws);
  snprintf(rel, rel_size, "%s/%s", inbox, name);
  snprintf(abs, abs_size, "%s/%s", dir, name);
  return aslink_download_file(job->bot, job->file_id, abs, d->cfg.max_download_bytes, NULL) ==
         ASLINK_OK;
}

static void run_job(struct serve_daemon *d, serve_job *job) {
  serve_star *star = job->star;
  char slug[64];
  asngn_origin origin = job->origin;
  char *text = job->text;
  job->text = NULL;
  if (job->kind == JOB_ROUTINE) {
    char who[33] = "";
    free(text);
    text = serve_routines_turn_text(d, job->routine, &origin, who);
    if (!text) return; /* nothing new to look at */
    if (who[0] && serve_star_find(d, who)) star = serve_star_find(d, who);
    job->bot = star->bot;
    job->chat = d->st.owner_chat;
    job->thread = d->st.routine_thread;
    snprintf(slug, sizeof slug, "rt-%.60s", job->routine);
  } else {
    slug_for(slug, job->chat, job->thread);
  }
  serve_session *sess = session_get(d, star, slug, job->bot, job->chat, job->thread);
  if (!sess) {
    serve_sendf(star, job->bot, job->chat, job->thread, false, serve_s(S_ERROR),
                "session");
    free(text);
    return;
  }
  if (job->kind == JOB_ROUTINE) serve_routines_prepare(d, job->routine, sess->s);
  os_mutex_lock(&d->mu);
  bool busy = sess->task != NULL;
  if (!busy) {
    sess->routine = job->kind == JOB_ROUTINE;
    snprintf(sess->routine_name, sizeof sess->routine_name, "%s", job->routine);
    sess->turn_steps[0] = 0;
    sess->turn_delegates = 0;
    sess->skill_used[0] = 0;
  }
  os_mutex_unlock(&d->mu);
  if (busy) {
    if (job->kind == JOB_MESSAGE)
      serve_send(star, job->bot, job->chat, job->thread, serve_s(S_BUSY), false);
    free(text);
    return;
  }
  if (!apply_project(d, star, sess, job)) {
    free(text);
    return;
  }
  /* Attachments and voice notes land in the session workspace first. */
  if (job->file_id[0]) {
    char rel[300], abs[1600];
    if (fetch_attachment(d, sess, job, rel, sizeof rel, abs, sizeof abs)) {
      if (job->voice) {
        char *heard = transcribe(d, abs);
        if (!heard) {
          serve_send(star, job->bot, job->chat, job->thread, serve_s(S_VOICE_NO_STT), false);
          free(text);
          return;
        }
        serve_sendf(star, job->bot, job->chat, job->thread, false, serve_s(S_VOICE_HEARD), heard);
        free(text);
        text = heard;
      } else {
        core_buf b;
        core_buf_init(&b);
        if (text && *text) core_buf_printf(&b, "%s\n", text);
        core_buf_printf(&b, serve_s(S_ATTACHMENT_SAVED), rel,
                        job->file_mime[0] ? job->file_mime : "file",
                        (unsigned long long)job->file_size);
        free(text);
        text = core_buf_detach(&b);
      }
    } else {
      serve_sendf(star, job->bot, job->chat, job->thread, false, serve_s(S_ERROR), "download");
      free(text);
      return;
    }
  }
  if (!text || !*text) {
    free(text);
    return;
  }
  char *turn_text = compose(star, sess, job, text, &origin);
  stream_sink sink;
  memset(&sink, 0, sizeof sink);
  os_mutex_init(&sink.mu);
  core_buf_init(&sink.out);
  asngn_submit_opts opts;
  memset(&opts, 0, sizeof opts);
  opts.origin = origin;
  asngn_task *task = NULL;
  asngn_err e;
  bool queued = false;
  for (;;) {
    e = turn_text ? asngn_submit_stream(sess->s, turn_text, &opts, on_stream, &sink, &task)
                  : ASNGN_ERR_NOMEM;
    /* One turn per project: wait for the other thread's turn to end. */
    if (e != ASNGN_ERR_BUSY || !sess->project[0] || d->stop ||
        !asngn_project_busy(star->ctx, sess->project, NULL, 0))
      break;
    if (!queued)
      serve_sendf(star, job->bot, job->chat, job->thread, false, serve_s(S_PROJECT_QUEUED),
                  sess->project);
    queued = true;
    while (!d->stop && asngn_project_busy(star->ctx, sess->project, NULL, 0))
      os_sleep_ms(500);
  }
  free(turn_text);
  if (e != ASNGN_OK) {
    serve_sendf(star, job->bot, job->chat, job->thread, false, serve_s(S_ERROR),
                e == ASNGN_ERR_BUSY ? serve_s(S_BUSY) : asngn_last_error(star->ctx));
    goto done;
  }
  os_mutex_lock(&d->mu);
  sess->task = task;
  os_mutex_unlock(&d->mu);
  int64_t draft = (int64_t)(core_fnv1a64(slug, strlen(slug)) & 0x3fffffffffffLL) +
                  (os_monotonic_ms() & 0xffff) + 1;
  aslink_draft_opts dopts;
  memset(&dopts, 0, sizeof dopts);
  dopts.can_stop = 1;
  bool drafts = job->chat > 0;
  int64_t last_push = 0;
  asngn_turn_result result;
  memset(&result, 0, sizeof result);
  for (;;) {
    e = asngn_task_wait(task, 400, &result);
    if (e != ASNGN_ERR_BUSY) break;
    if (d->stop) asngn_task_cancel(task);
    int64_t now = os_monotonic_ms();
    if (now - last_push >= 1500) {
      last_push = now;
      char *preview = draft_text(&sink);
      if (!drafts || aslink_send_draft(job->bot, job->chat, job->thread, draft,
                                       preview, &dopts, NULL) != ASLINK_OK) {
        drafts = false;
        (void)aslink_send_chat_action(job->bot, job->chat, job->thread, "typing", NULL);
      }
      free(preview);
    }
  }
  os_mutex_lock(&d->mu);
  sess->task = NULL;
  char steps[512];
  size_t delegates = sess->turn_delegates;
  char skill[65];
  memcpy(steps, sess->turn_steps, sizeof steps);
  memcpy(skill, sess->skill_used, sizeof skill);
  os_mutex_unlock(&d->mu);
  bool ok = e == ASNGN_OK;
  if (ok) {
    const char *answer = result.answer && *result.answer ? result.answer : NULL;
    bool skip = job->kind == JOB_ROUTINE &&
                ((answer && !strncmp(answer, "SKIP", 4)) || !serve_routines_delivers(d, job->routine));
    if (job->kind == JOB_ROUTINE && !skip)
      serve_sendf(star, job->bot, job->chat, job->thread, true, serve_s(S_ROUTINE_RESULT),
                  job->routine);
    if (answer && !skip) serve_send(star, job->bot, job->chat, job->thread, answer, false);
    serve_usage_note(&d->st, tier_paid(d, result.tier), delegates, job->kind == JOB_ROUTINE);
    os_mutex_lock(&d->mu);
    free(sess->last_request);
    free(sess->last_answer);
    sess->last_request = core_strdup(text);
    sess->last_answer = core_strdup(answer ? answer : "");
    snprintf(sess->steps, sizeof sess->steps, "%s", steps);
    os_mutex_unlock(&d->mu);
    if (delegates && answer && job->kind == JOB_MESSAGE)
      serve_skills_playbook(d, star, text, steps, answer);
  } else if (e != ASNGN_ERR_CANCELLED) {
    serve_sendf(star, job->bot, job->chat, job->thread, false, serve_s(S_ERROR),
                asngn_err_name(e));
  }
  if (skill[0]) serve_skills_outcome(d, star, skill, ok);
  if (job->kind == JOB_ROUTINE)
    serve_routines_finished(d, job->routine, ok, result.answer, ok ? NULL : asngn_err_name(e));
  asngn_turn_result_free(&result);
  asngn_task_free(task);
done:
  core_buf_free(&sink.out);
  os_mutex_destroy(&sink.mu);
  free(text);
}

static void *runner_main(void *arg) {
  struct serve_daemon *d = arg;
  serve_job job;
  while (dequeue(d, &job)) run_job(d, &job);
  return NULL;
}

/* ── approvals ────────────────────────────────────────────────────────── */

static void send_approval(struct serve_daemon *d, serve_approval_job *job) {
  os_mutex_lock(&d->mu);
  serve_session *sess = session_by_slug(d, job->star, job->slug);
  asngn_session *s = sess ? sess->s : NULL;
  aslink_ctx *bot = sess ? sess->bot : NULL;
  int64_t chat = sess ? sess->chat : 0, thread = sess ? sess->thread : 0;
  os_mutex_unlock(&d->mu);
  if (!s || !bot || !chat) return; /* not ours: TUI or MCP will answer */
  asngn_approval *a = NULL;
  if (asngn_approval_get(s, &a) != ASNGN_OK) return;
  if (strcmp(a->id, job->confirm_id) || a->status != ASNGN_APPROVAL_PENDING) {
    asngn_approval_free(a);
    return;
  }
  bool tainted = a->taint != ASNGN_ORIGIN_HOST && a->taint != ASNGN_ORIGIN_OWNER;
  char *tool = escape(a->tool_ref), *cmd = escape(a->command);
  char *at = strchr(tool, '@');
  if (at) *at = 0;
  size_t n = strlen(a->arguments);
  if (n > 2500) n = 2500;
  while (n && !core_utf8_valid(a->arguments, n)) n--;
  char *cut = core_strndup(a->arguments, n);
  char *args = escape(cut);
  core_buf b;
  core_buf_init(&b);
  core_buf_printf(&b, "%s\n", serve_s(S_APPROVAL_TITLE));
  core_buf_printf(&b, serve_s(S_APPROVAL_TOOL), tool, cmd,
                  asngn_security_profile_name(a->profile));
  core_buf_appendc(&b, '\n');
  if (tainted) {
    core_buf_printf(&b, serve_s(S_APPROVAL_TAINT), asngn_origin_name(a->taint));
    core_buf_appendc(&b, '\n');
  }
  core_buf_printf(&b, "%s\n<pre>%s%s</pre>\nsha256 <code>%.16s</code>",
                  serve_s(S_APPROVAL_ARGS), args, n < strlen(a->arguments) ? "\n…" : "",
                  a->arguments_sha256);
  char ty[72], tn[72], ta[72];
  aslink_button buttons[3];
  size_t nb = 0;
  if (serve_token_make(&d->st, 'a', a->id, 'y', chat, ty, sizeof ty) &&
      serve_token_make(&d->st, 'a', a->id, 'n', chat, tn, sizeof tn) &&
      serve_token_make(&d->st, 'a', a->id, 'w', chat, ta, sizeof ta)) {
    buttons[nb++] = (aslink_button){serve_s(S_ALLOW), ty, NULL};
    buttons[nb++] = (aslink_button){serve_s(S_DENY), tn, NULL};
    /* A tainted review is one-shot; offering "always" would mislead. */
    if (!tainted) buttons[nb++] = (aslink_button){serve_s(S_ALWAYS), ta, NULL};
  }
  aslink_keyboard kb = {buttons, nb, 2};
  aslink_send_opts opts;
  memset(&opts, 0, sizeof opts);
  opts.parse_mode = "HTML";
  opts.keyboard = nb ? &kb : NULL;
  int64_t message_id = 0;
  /* Recorded before the message goes out: a button pressed as soon as it
   * shows must find its approval. The message id follows the send. */
  os_mutex_lock(&d->mu);
  size_t slot = 0;
  for (size_t i = 0; i < 32; i++)
    if (!d->pending[i].used) { slot = i; break; }
  d->pending[slot] = (serve_pending_approval){true, "", job->star, bot, chat, 0, tainted};
  snprintf(d->pending[slot].id, sizeof d->pending[slot].id, "%s", a->id);
  os_mutex_unlock(&d->mu);
  bool sent = aslink_send_message(bot, chat, thread, b.data, &opts, &message_id, NULL) == ASLINK_OK;
  os_mutex_lock(&d->mu);
  if (d->pending[slot].used && !strcmp(d->pending[slot].id, a->id)) {
    if (sent) d->pending[slot].message_id = message_id;
    else d->pending[slot].used = false;
  }
  os_mutex_unlock(&d->mu);
  core_buf_free(&b);
  free(tool); free(cmd); free(cut); free(args);
  asngn_approval_free(a);
}

static void *notifier_main(void *arg) {
  struct serve_daemon *d = arg;
  for (;;) {
    serve_approval_job job;
    os_mutex_lock(&d->mu);
    while (!d->stop && d->approvals_n == 0) os_cond_timedwait(&d->cv, &d->mu, 500);
    bool have = !d->stop && d->approvals_n > 0;
    if (have) {
      job = d->approvals[0];
      memmove(d->approvals, d->approvals + 1, (d->approvals_n - 1) * sizeof *d->approvals);
      d->approvals_n--;
    }
    os_mutex_unlock(&d->mu);
    if (!have) break;
    send_approval(d, &job);
  }
  return NULL;
}

static void resolve_approval(struct serve_daemon *d, aslink_ctx *from,
                             const aslink_update *u, const char *id, char choice) {
  serve_pending_approval p;
  bool found = false;
  memset(&p, 0, sizeof p);
  os_mutex_lock(&d->mu);
  for (size_t i = 0; i < 32 && !found; i++)
    if (d->pending[i].used && !strcmp(d->pending[i].id, id)) {
      p = d->pending[i];
      d->pending[i].used = false;
      found = true;
    }
  os_mutex_unlock(&d->mu);
  aslink_ctx *bot = found ? p.bot : from;
  if (!found || (choice == 'w' && p.tainted) ||
      asngn_confirm(p.star->ctx, id, choice != 'n', choice == 'w') != ASNGN_OK) {
    (void)aslink_answer_callback(from, u->callback_id, serve_s(S_EXPIRED), 0, NULL);
    return;
  }
  const char *verdict = serve_s(choice == 'n' ? S_DENIED : S_ALLOWED);
  (void)aslink_answer_callback(bot, u->callback_id, verdict, 0, NULL);
  (void)aslink_edit_keyboard(bot, p.chat, p.message_id, NULL, NULL);
  (void)aslink_send_message(bot, p.chat, u->thread_id, verdict, NULL, NULL, NULL);
}

/* ── commands ─────────────────────────────────────────────────────────── */

static bool is_cmd(const char *word, const char *name) { return !strcmp(word, name); }

static serve_star *star_for(struct serve_daemon *d, aslink_ctx *bot, int64_t chat,
                            int64_t thread) {
  char name[33];
  for (size_t i = 0; i < d->stars_n; i++)
    if (d->stars[i].own_bot && d->stars[i].bot == bot) return &d->stars[i];
  if (serve_binding_get(&d->st, chat, thread, name) && serve_star_find(d, name))
    return serve_star_find(d, name);
  return &d->stars[0];
}

static void cmd_status(struct serve_daemon *d, serve_star *star, aslink_ctx *bot,
                       int64_t chat, int64_t thread) {
  size_t running = 0;
  core_buf b, busy;
  core_buf_init(&busy);
  os_mutex_lock(&d->mu);
  for (size_t i = 0; i < SERVE_SESSIONS_MAX; i++)
    if (d->sessions[i].used && d->sessions[i].task) {
      running++;
      if (d->sessions[i].project[0])
        core_buf_printf(&busy, "\n• project %s: working", d->sessions[i].project);
    }
  os_mutex_unlock(&d->mu);
  core_buf_init(&b);
  core_buf_printf(&b, serve_s(S_STATUS), running, d->stars_n,
                  serve_routines_count(d));
  if (busy.len) core_buf_appends(&b, busy.data);
  core_buf_free(&busy);
  asngn_model_info models[16];
  size_t n = 0;
  if (asngn_get_models(star->ctx, models, 16, &n) == ASNGN_OK) {
    for (size_t i = 0; i < n && i < 16; i++)
      core_buf_printf(&b, "\n• %s: %s [%s]%s", models[i].id, models[i].file, models[i].roles,
                      models[i].resident ? " ●" : "");
  }
  serve_send(star, bot, chat, thread, b.data, false);
  core_buf_free(&b);
}

static void cmd_quota(struct serve_daemon *d, serve_star *star, aslink_ctx *bot,
                      int64_t chat, int64_t thread) {
  serve_usage t, m;
  serve_usage_get(&d->st, &t, &m);
  long long tp = t.turns ? t.local_turns * 100 / t.turns : 100;
  long long mp = m.turns ? m.local_turns * 100 / m.turns : 100;
  serve_sendf(star, bot, chat, thread, false, serve_s(S_QUOTA),
              (long long)t.turns, (long long)t.local_turns, tp, (long long)t.paid_turns,
              (long long)t.delegate_calls, (long long)t.routine_runs, (long long)m.turns,
              (long long)m.local_turns, mp, (long long)m.paid_turns);
  asngn_consumption c;
  if (asngn_get_consumption(star->ctx, &c) == ASNGN_OK)
    serve_sendf(star, bot, chat, thread, false,
                "tokens today: %lld in / %lld out (%lld calls, %lld unknown)",
                (long long)c.today.known_input_tokens, (long long)c.today.known_output_tokens,
                (long long)c.today.calls, (long long)c.today.unknown_calls);
}

static void cmd_new(struct serve_daemon *d, serve_star *star, aslink_ctx *bot,
                    int64_t chat, int64_t thread, const char *title) {
  int64_t topic = 0;
  char name[129];
  snprintf(name, sizeof name, "%s", title && *title ? title : "asterism");
  if (!d->cfg.topics || aslink_create_topic(bot, chat, name, &topic, NULL) != ASLINK_OK) {
    serve_sendf(star, bot, chat, thread, false, serve_s(S_ERROR), "topics");
    return;
  }
  (void)serve_binding_set(&d->st, chat, topic, star->cfg.name);
  serve_sendf(star, bot, chat, topic, false, serve_s(S_TOPIC_CREATED), name);
}

static void cmd_stop(struct serve_daemon *d, serve_star *star, aslink_ctx *bot,
                     int64_t chat, int64_t thread) {
  char slug[64];
  slug_for(slug, chat, thread);
  os_mutex_lock(&d->mu);
  serve_session *sess = session_by_slug(d, star, slug);
  asngn_task *task = sess ? sess->task : NULL;
  os_mutex_unlock(&d->mu);
  /* The runner frees the task; cancel only signals it. */
  if (task) asngn_task_cancel(task);
  serve_send(star, bot, chat, thread,
             serve_s(task ? S_STOPPED : S_NOTHING_RUNNING), false);
}

static void cmd_star(struct serve_daemon *d, serve_star *star, aslink_ctx *bot,
                     int64_t chat, int64_t thread, const char *name) {
  if (!name || !*name) {
    serve_sendf(star, bot, chat, thread, false, serve_s(S_STAR_NOW), star->cfg.name);
    return;
  }
  serve_star *to = serve_star_find(d, name);
  if (!to || to->own_bot) {
    char list[300] = "";
    for (size_t i = 0; i < d->stars_n; i++)
      if (!d->stars[i].own_bot || i == 0)
        snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", i ? ", " : "",
                 d->stars[i].cfg.name);
    serve_sendf(star, bot, chat, thread, false, serve_s(S_STAR_UNKNOWN), name, list);
    return;
  }
  (void)serve_binding_set(&d->st, chat, thread, to->cfg.name);
  serve_sendf(to, bot, chat, thread, false, serve_s(S_STAR_NOW), to->cfg.name);
}

/* Next space-separated word of *args into out; advances *args past it. */
static void next_word(const char **args, char *out, size_t size) {
  size_t n = 0;
  const char *p = *args;
  while (*p == ' ') p++;
  while (p[n] && p[n] != ' ' && n + 1 < size) {
    out[n] = p[n];
    n++;
  }
  out[n] = 0;
  p += n;
  while (*p && *p != ' ') p++; /* an over-long word is cut, never split */
  while (*p == ' ') p++;
  *args = p;
}

static void project_list(serve_star *star, aslink_ctx *bot, int64_t chat, int64_t thread,
                         const char *here) {
  asngn_project_info *v = NULL;
  size_t n = 0;
  core_buf b;
  if (asngn_projects_list(star->ctx, &v, &n) != ASNGN_OK) {
    serve_sendf(star, bot, chat, thread, false, serve_s(S_ERROR), asngn_last_error(star->ctx));
    return;
  }
  if (n == 0) {
    serve_send(star, bot, chat, thread, serve_s(S_PROJECT_NONE), false);
    return;
  }
  core_buf_init(&b);
  core_buf_appends(&b, "Projects:");
  for (size_t i = 0; i < n; i++)
    core_buf_printf(&b, "\n• %s — %s%s", v[i].name, v[i].path,
                    !strcmp(v[i].name, here) ? "  ← this thread" : "");
  core_buf_appends(&b, "\n\n");
  if (here[0]) {
    for (size_t i = 0; i < n; i++)
      if (!strcmp(v[i].name, here)) core_buf_printf(&b, serve_s(S_PROJECT_HERE), v[i].name, v[i].path);
  } else {
    core_buf_appends(&b, serve_s(S_PROJECT_HERE_NONE));
  }
  serve_send(star, bot, chat, thread, b.data, false);
  core_buf_free(&b);
  asngn_free(v);
}

/* /project [list] | new <name> [folder] | <name> | off | rm <name> */
static void cmd_project(struct serve_daemon *d, serve_star *star, aslink_ctx *bot,
                        int64_t chat, int64_t thread, const char *args) {
  char word[48], name[48], here[41];
  asngn_project_info p;
  int64_t topic = 0;
  next_word(&args, word, sizeof word);
  (void)serve_binding_project(&d->st, chat, thread, star->cfg.name, here);
  if (!word[0] || !strcmp(word, "list")) {
    project_list(star, bot, chat, thread, here);
  } else if (!strcmp(word, "off")) {
    if (serve_binding_set_project(&d->st, chat, thread, star->cfg.name, ""))
      serve_send(star, bot, chat, thread, serve_s(S_PROJECT_OFF), false);
    else serve_sendf(star, bot, chat, thread, false, serve_s(S_ERROR), "bindings");
  } else if (!strcmp(word, "new")) {
    next_word(&args, name, sizeof name);
    if (asngn_project_add(star->ctx, name, *args ? args : NULL, &p) != ASNGN_OK) {
      serve_sendf(star, bot, chat, thread, false, serve_s(S_ERROR), asngn_last_error(star->ctx));
      return;
    }
    /* A thread of its own when topics work here; else this thread. */
    if (!d->cfg.topics || aslink_create_topic(bot, chat, p.name, &topic, NULL) != ASLINK_OK ||
        topic == 0)
      topic = thread;
    if (!serve_binding_set_project(&d->st, chat, topic, star->cfg.name, p.name)) {
      serve_sendf(star, bot, chat, thread, false, serve_s(S_ERROR), "bindings");
      return;
    }
    serve_sendf(star, bot, chat, topic, false, serve_s(S_PROJECT_CREATED), p.name, p.path);
    if (topic != thread)
      serve_sendf(star, bot, chat, thread, false, serve_s(S_TOPIC_CREATED), p.name);
  } else if (!strcmp(word, "rm")) {
    next_word(&args, name, sizeof name);
    if (asngn_project_get(star->ctx, name, &p) != ASNGN_OK ||
        asngn_project_remove(star->ctx, name) != ASNGN_OK) {
      serve_sendf(star, bot, chat, thread, false, serve_s(S_ERROR), asngn_last_error(star->ctx));
      return;
    }
    serve_binding_drop_project(&d->st, star->cfg.name, p.name);
    serve_sendf(star, bot, chat, thread, false, serve_s(S_PROJECT_REMOVED), p.name, p.path);
  } else if (asngn_project_get(star->ctx, word, &p) != ASNGN_OK) {
    serve_sendf(star, bot, chat, thread, false, serve_s(S_ERROR), asngn_last_error(star->ctx));
  } else if (!serve_binding_set_project(&d->st, chat, thread, star->cfg.name, p.name)) {
    serve_sendf(star, bot, chat, thread, false, serve_s(S_ERROR), "bindings");
  } else {
    serve_sendf(star, bot, chat, thread, false, serve_s(S_PROJECT_BOUND), p.name, p.path);
  }
}

static void cmd_memory(serve_star *star, aslink_ctx *bot, int64_t chat, int64_t thread,
                       const char *q) {
  char *answer = NULL;
  if (q && *q && asngn_recall(star->ctx, q, &answer) == ASNGN_OK && answer && *answer)
    serve_send(star, bot, chat, thread, answer, false);
  else serve_send(star, bot, chat, thread, serve_s(S_MEMORY_EMPTY), false);
  asngn_free(answer);
}

static bool handle_command(struct serve_daemon *d, aslink_ctx *bot, const aslink_update *u) {
  char word[40];
  const char *text = u->text, *args;
  size_t n = 0;
  while (text[1 + n] && text[1 + n] != ' ' && text[1 + n] != '@' && n + 1 < sizeof word) {
    word[n] = text[1 + n];
    n++;
  }
  word[n] = 0;
  args = strchr(text, ' ');
  args = args ? args + 1 : "";
  while (*args == ' ') args++;
  serve_star *star = star_for(d, bot, u->chat_id, u->thread_id);
  int64_t chat = u->chat_id, thread = u->thread_id;
  if (is_cmd(word, "start") || is_cmd(word, "help")) {
    if (is_cmd(word, "start")) serve_send(star, bot, chat, thread, serve_s(S_WELCOME), false);
    serve_send(star, bot, chat, thread, serve_s(S_HELP), false);
  } else if (is_cmd(word, "new")) cmd_new(d, star, bot, chat, thread, args);
  else if (is_cmd(word, "stop")) cmd_stop(d, star, bot, chat, thread);
  else if (is_cmd(word, "status")) cmd_status(d, star, bot, chat, thread);
  else if (is_cmd(word, "quota")) cmd_quota(d, star, bot, chat, thread);
  else if (is_cmd(word, "routine") || is_cmd(word, "routines"))
    serve_routines_command(d, star, bot, chat, thread, args);
  else if (is_cmd(word, "memory")) cmd_memory(star, bot, chat, thread, args);
  else if (is_cmd(word, "star")) cmd_star(d, star, bot, chat, thread, args);
  else if (is_cmd(word, "project") || is_cmd(word, "projects"))
    cmd_project(d, star, bot, chat, thread, args);
  else if (is_cmd(word, "skill")) {
    char slug[64];
    slug_for(slug, chat, thread);
    os_mutex_lock(&d->mu);
    serve_session *sess = session_by_slug(d, star, slug);
    os_mutex_unlock(&d->mu);
    serve_skills_command(d, star, bot, chat, thread, sess, args);
  } else return false;
  return true;
}

/* ── update dispatch ──────────────────────────────────────────────────── */

static void dispatch(struct serve_daemon *d, aslink_ctx *bot, const aslink_update *u) {
  if (u->kind == ASLINK_UPDATE_STOP) {
    char slug[64];
    slug_for(slug, u->chat_id, u->thread_id);
    os_mutex_lock(&d->mu);
    asngn_task *task = NULL;
    for (size_t i = 0; i < SERVE_SESSIONS_MAX && !task; i++)
      if (d->sessions[i].used && !strcmp(d->sessions[i].slug, slug))
        task = d->sessions[i].task;
    os_mutex_unlock(&d->mu);
    if (task && u->chat_id == d->st.owner_chat) asngn_task_cancel(task);
    return;
  }
  if (u->kind == ASLINK_UPDATE_CALLBACK) {
    char kind, choice, id[40];
    if (u->from_id != d->st.owner_id || !u->callback_data ||
        !serve_token_check(&d->st, u->callback_data, u->chat_id, &kind, id, &choice)) {
      (void)aslink_answer_callback(bot, u->callback_id, serve_s(S_EXPIRED), 0, NULL);
      return;
    }
    if (kind == 'a') resolve_approval(d, bot, u, id, choice);
    else if (kind == 'r') {
      (void)aslink_answer_callback(bot, u->callback_id, NULL, 0, NULL);
      serve_routines_callback(d, bot, u->chat_id, u->thread_id, u->callback_message_id, id,
                              choice);
    }
    return;
  }
  if (u->kind != ASLINK_UPDATE_MESSAGE || strcmp(u->chat_type, "private") || u->from_is_bot)
    return;
  const char *text = u->text ? u->text : "";
  if (!d->st.owner_id) {
    if (!strncmp(text, "/start ", 7) &&
        serve_pairing_claim(&d->st, text + 7, u->from_id, u->chat_id)) {
      serve_send(&d->stars[0], bot, u->chat_id, u->thread_id, serve_s(S_PAIRED), false);
      serve_send(&d->stars[0], bot, u->chat_id, u->thread_id, serve_s(S_HELP), false);
    } else {
      serve_send(&d->stars[0], bot, u->chat_id, u->thread_id, serve_s(S_PAIR_FIRST), false);
    }
    return;
  }
  if (u->from_id != d->st.owner_id) {
    if (!strncmp(text, "/start", 6))
      serve_send(&d->stars[0], bot, u->chat_id, u->thread_id, serve_s(S_NOT_OWNER), false);
    return;
  }
  if (text[0] == '/' && !u->forwarded && handle_command(d, bot, u)) return;
  serve_job job;
  memset(&job, 0, sizeof job);
  job.kind = JOB_MESSAGE;
  job.star = star_for(d, bot, u->chat_id, u->thread_id);
  job.bot = bot;
  job.chat = u->chat_id;
  job.thread = u->thread_id;
  job.reply_to = u->message_id;
  job.origin = u->forwarded ? ASNGN_ORIGIN_FORWARDED : ASNGN_ORIGIN_OWNER;
  snprintf(job.forward_origin, sizeof job.forward_origin, "%s", u->forward_origin);
  job.text = core_strdup(text);
  const char *file = u->voice_file_id[0] ? u->voice_file_id
                     : u->audio_file_id[0] ? u->audio_file_id
                     : u->document_file_id[0] ? u->document_file_id
                     : u->photo_file_id[0] ? u->photo_file_id : NULL;
  if (file) {
    snprintf(job.file_id, sizeof job.file_id, "%s", file);
    job.voice = u->voice_file_id[0] || u->audio_file_id[0];
    snprintf(job.file_name, sizeof job.file_name, "%s", u->document_name);
    snprintf(job.file_mime, sizeof job.file_mime, "%s",
             u->document_mime[0] ? u->document_mime
             : u->audio_mime[0] ? u->audio_mime
             : u->photo_file_id[0] ? "image/jpeg" : job.voice ? "audio/ogg" : "");
    job.file_size = u->file_size;
  }
  if (!job.text) return;
  if (!serve_enqueue(d, &job))
    serve_sendf(job.star, bot, u->chat_id, u->thread_id, false, serve_s(S_ERROR), "queue");
}

static void set_commands(aslink_ctx *bot) {
  static const aslink_command cmds[] = {
    {"new", "New thread for a new task"}, {"stop", "Cancel the task in this thread"},
    {"status", "What is running"}, {"quota", "Local versus paid usage"},
    {"routine", "Scheduled and watched tasks"}, {"memory", "Search the memory"},
    {"star", "Which agent answers here"}, {"project", "Folders the agent works in"},
    {"skill", "Saved procedures"},
    {"help", "Commands"}};
  (void)aslink_set_commands(bot, cmds, sizeof cmds / sizeof cmds[0], NULL);
}

static void *poller_main(void *arg) {
  serve_star *star = arg;
  struct serve_daemon *d = star->d;
  aslink_ctx *bot = star->bot;
  int64_t offset = 0;
  int backoff = 1;
  set_commands(bot);
  while (!d->stop) {
    aslink_update *u = NULL;
    size_t n = 0;
    aslink_status st;
    aslink_err e = aslink_get_updates(bot, offset, d->cfg.poll_timeout_s, &u, &n, &st);
    if (e == ASLINK_ERR_CANCELLED || d->stop) break;
    if (e != ASLINK_OK) {
      int wait = e == ASLINK_ERR_RATE_LIMITED && st.retry_after_s > 0 ? st.retry_after_s : backoff;
      if (e == ASLINK_ERR_UNAUTHORIZED)
        fprintf(stderr, "asterism serve: telegram rejected the token of star %s\n",
                star->cfg.name);
      for (int i = 0; i < wait * 10 && !d->stop; i++) os_sleep_ms(100);
      if (backoff < 60) backoff *= 2;
      continue;
    }
    backoff = 1;
    for (size_t i = 0; i < n; i++) {
      if (u[i].update_id >= offset) offset = u[i].update_id + 1;
      dispatch(d, bot, &u[i]);
    }
    aslink_updates_free(u, n);
  }
  return NULL;
}

static void *scheduler_main(void *arg) {
  struct serve_daemon *d = arg;
  while (!d->stop) {
    if (d->st.owner_chat) serve_routines_tick(d);
    for (int i = 0; i < d->cfg.routine_tick_s * 10 && !d->stop; i++) os_sleep_ms(100);
  }
  return NULL;
}

/* ── lifecycle ────────────────────────────────────────────────────────── */

/* The main bot's token may live in the vault, released only to "serve". */
static char *vault_token(struct serve_daemon *d, const char *name, char *err, size_t n) {
  char dir[1200];
  asvault *v = NULL;
  char *value = NULL;
  snprintf(dir, sizeof dir, "%s/vault", d->stars[0].root[0] ? d->stars[0].root : ".");
  if (asvault_open(dir, &v) != ASVAULT_OK ||
      asvault_get(v, name, "serve", &value) != ASVAULT_OK) {
    snprintf(err, n, "vault secret %s: %s (store it with: asterism vault set %s --tool serve)",
             name, v ? asvault_last_error(v) : "cannot open the vault", name);
    value = NULL;
  }
  asvault_close(v);
  return value;
}

static aslink_ctx *open_bot(struct serve_daemon *d, const char *env, char *err, size_t n) {
  char *secret = NULL;
  const char *token = getenv(env);
  aslink_telegram_params p;
  aslink_ctx *bot = NULL;
  aslink_bot_info info;
  aslink_status st;
  aslink_err e;
  if (env == d->cfg.token_env && d->cfg.token_secret[0]) {
    if (!(secret = vault_token(d, d->cfg.token_secret, err, n))) return NULL;
    token = secret;
  }
  if (!token || !*token) {
    snprintf(err, n, "set %s to the bot token from @BotFather", env);
    return NULL;
  }
  memset(&p, 0, sizeof p);
  p.token = token;
  p.api_base = d->cfg.api_base[0] ? d->cfg.api_base : NULL;
  p.request_timeout_ms = 30000;
  e = aslink_telegram_open(&p, &bot);
  asvault_free_secret(secret); /* aslink keeps its own copy */
  if (e != ASLINK_OK) {
    snprintf(err, n, "telegram: %s", aslink_err_name(e));
    return NULL;
  }
  memset(&st, 0, sizeof st);
  if ((e = aslink_get_me(bot, &info, &st)) != ASLINK_OK) {
    snprintf(err, n, "telegram getMe: %s %s", aslink_err_name(e), st.message);
    aslink_close(bot);
    return NULL;
  }
  return bot;
}

static bool open_star(struct serve_daemon *d, serve_star *star, const serve_params *p,
                      bool primary, char *err, size_t n) {
  asngn_open_params op;
  char cfgpath[1100];
  memset(&op, 0, sizeof op);
  star->d = d;
  if (primary) snprintf(star->root, sizeof star->root, "%s", p->root);
  else if (os_path_is_abs(star->cfg.root)) snprintf(star->root, sizeof star->root, "%s", star->cfg.root);
  else snprintf(star->root, sizeof star->root, "%s/%s", p->root, star->cfg.root);
  if (!primary && os_mkdir_p(star->root) != CORE_OK) {
    snprintf(err, n, "cannot create %s", star->root);
    return false;
  }
  op.engine_root = star->root;
  op.config_path = primary ? p->config : NULL;
  if (!primary && star->cfg.config[0]) {
    if (os_path_is_abs(star->cfg.config)) snprintf(cfgpath, sizeof cfgpath, "%s", star->cfg.config);
    else snprintf(cfgpath, sizeof cfgpath, "%s/%s", p->root, star->cfg.config);
    op.config_path = cfgpath;
  }
  op.workspace_root = primary ? p->workspace : NULL;
  op.allow_degraded = p->allow_degraded;
  if (asngn_open(&op, &star->ctx) != ASNGN_OK) {
    snprintf(err, n, "star %s: cannot open the engine at %s", star->cfg.name, star->root);
    return false;
  }
  asngn_set_event_sink(star->ctx, on_event, star);
  if (star->cfg.token_env[0]) {
    star->bot = open_bot(d, star->cfg.token_env, err, n);
    if (!star->bot) return false;
    star->own_bot = true;
  } else {
    star->bot = d->main_bot;
  }
  return true;
}

static void shutdown_all(struct serve_daemon *d) {
  os_mutex_lock(&d->mu);
  d->stop = 1;
  os_cond_broadcast(&d->cv);
  for (size_t i = 0; i < SERVE_SESSIONS_MAX; i++)
    if (d->sessions[i].used && d->sessions[i].task) asngn_task_cancel(d->sessions[i].task);
  os_mutex_unlock(&d->mu);
  if (d->main_bot) aslink_cancel(d->main_bot);
  for (size_t i = 0; i < d->stars_n; i++)
    if (d->stars[i].own_bot) aslink_cancel(d->stars[i].bot);
  for (size_t i = 0; i < d->stars_n; i++)
    if (d->stars[i].poller_started) os_thread_join(&d->stars[i].poller);
  for (int i = 0; i < d->runners_n; i++) os_thread_join(&d->runners[i]);
  if (d->notifier_started) os_thread_join(&d->notifier);
  if (d->scheduler_started) os_thread_join(&d->scheduler);
  for (size_t i = 0; i < d->jobs_n; i++) free(d->jobs[(d->jobs_head + i) % SERVE_JOBS_MAX].text);
  for (size_t i = 0; i < SERVE_SESSIONS_MAX; i++) {
    serve_session *s = &d->sessions[i];
    if (!s->used) continue;
    if (s->s) asngn_session_close(s->s);
    free(s->last_request);
    free(s->last_answer);
  }
  serve_routines_close(d);
  for (size_t i = 0; i < d->stars_n; i++) {
    if (d->stars[i].ctx) {
      asngn_set_event_sink(d->stars[i].ctx, NULL, NULL);
      asngn_close(d->stars[i].ctx);
    }
    if (d->stars[i].own_bot) aslink_close(d->stars[i].bot);
  }
  aslink_close(d->main_bot);
  for (size_t i = 0; i < d->paid_tiers_n; i++) free(d->paid_tiers[i]);
}

/* The first start after "asterism setup" (it leaves serve/hello holding the
 * model tier): tell the owner the agent is up. */
static void send_hello(struct serve_daemon *d) {
  char path[1200], host[256] = "";
  char *tier = NULL;
  snprintf(path, sizeof path, "%s/hello", d->st.dir);
  if (!d->st.owner_chat || os_read_file(path, &tier, NULL) != CORE_OK) return;
  core_str_trim(tier);
#if defined(_WIN32)
  snprintf(host, sizeof host, "%s", getenv("COMPUTERNAME") ? getenv("COMPUTERNAME") : "");
#else
  if (gethostname(host, sizeof host - 1) != 0) host[0] = '\0';
#endif
  serve_sendf(&d->stars[0], d->main_bot, d->st.owner_chat, 0, false, serve_s(S_HELLO),
              host[0] ? host : "this computer", tier[0] ? ", models tier " : "", tier);
  (void)os_remove_file(path);
  free(tier);
}

int serve_run(const serve_params *p) {
  struct serve_daemon *d = calloc(1, sizeof *d);
  char err[512] = "";
  int status = 1;
  if (!d) return 1;
#if !defined(_WIN32)
  /* A service starts in "/": relative paths in module configs (registry
   * roots, memory) must mean the engine root, as they do from a shell there. */
  if (chdir(p->root) != 0) {
    fprintf(stderr, "asterism serve: cannot enter %s\n", p->root);
    free(d);
    return 1;
  }
#endif
  serve_config_defaults(&d->cfg);
  os_mutex_init(&d->mu);
  os_cond_init(&d->cv);
  d->paid_tiers[d->paid_tiers_n++] = core_strdup("chatgpt");
  if (!serve_config_load(&d->cfg, p->root, p->serve_config, err, sizeof err) ||
      !serve_state_open(&d->st, p->root, err, sizeof err)) {
    fprintf(stderr, "asterism serve: %s\n", err);
    goto out;
  }
  d->main_bot = open_bot(d, d->cfg.token_env, err, sizeof err);
  if (!d->main_bot) {
    fprintf(stderr, "asterism serve: %s\n", err);
    goto out_state;
  }
  d->stars_n = d->cfg.stars_n;
  for (size_t i = 0; i < d->stars_n; i++) {
    d->stars[i].cfg = d->cfg.stars[i];
    if (!open_star(d, &d->stars[i], p, i == 0, err, sizeof err)) {
      fprintf(stderr, "asterism serve: %s\n", err);
      goto out_stars;
    }
  }
  if (!serve_routines_open(d, err, sizeof err))
    fprintf(stderr, "asterism serve: routines disabled: %s\n", err);
#if !defined(_WIN32)
  {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
  }
#else
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
#endif
  for (int i = 0; i < SERVE_RUNNERS; i++)
    if (os_thread_start(&d->runners[d->runners_n], runner_main, d) == CORE_OK) d->runners_n++;
  d->notifier_started = os_thread_start(&d->notifier, notifier_main, d) == CORE_OK;
  d->scheduler_started = os_thread_start(&d->scheduler, scheduler_main, d) == CORE_OK;
  /* One poller per bot: the main bot through the first star, own bots too. */
  for (size_t i = 0; i < d->stars_n; i++)
    if (i == 0 || d->stars[i].own_bot)
      d->stars[i].poller_started =
          os_thread_start(&d->stars[i].poller, poller_main, &d->stars[i]) == CORE_OK;
  fprintf(stderr, "asterism serve: ready (%zu star%s, %s)\n", d->stars_n,
          d->stars_n == 1 ? "" : "s", d->st.owner_id ? "paired" : "waiting for pairing");
  send_hello(d);
  while (!g_signal) os_sleep_ms(200);
  fprintf(stderr, "asterism serve: stopping\n");
  status = 0;
out_stars:
  shutdown_all(d);
out_state:
  serve_state_close(&d->st);
out:
  serve_config_free(&d->cfg);
  os_cond_destroy(&d->cv);
  os_mutex_destroy(&d->mu);
  free(d);
  return status;
}
