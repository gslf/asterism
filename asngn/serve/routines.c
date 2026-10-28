/* Routines in the daemon: the astime store, the scheduler tick, watch
 * fetches, the /routine command and its activation buttons. A routine turn is
 * owner-authored (HOST) unless it carries watched content, which is WEB and
 * therefore taints the turn. */
#include "daemon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WATCH_MAX_BYTES (512u * 1024u)
#define WATCH_TEXT_MAX  8000

/* Fetched watch content waiting for its turn, by routine name. */
typedef struct {
  char name[ASTIME_NAME_MAX];
  char hash[ASTIME_HASH_MAX];
  char *content;
  bool used;
} pending_watch;

typedef struct {
  astime_store *store;
  char dir[2200];
  pending_watch watch[16];
} routines;

static routines *R(struct serve_daemon *d) { return d->routines; }

bool serve_routines_open(struct serve_daemon *d, char *err, size_t n) {
  routines *r = calloc(1, sizeof *r);
  astime_err e;
  if (!r) return false;
  if (os_path_is_abs(d->cfg.routines_dir))
    snprintf(r->dir, sizeof r->dir, "%s", d->cfg.routines_dir);
  else
    snprintf(r->dir, sizeof r->dir, "%s/%s", d->stars[0].root, d->cfg.routines_dir);
  if ((e = astime_store_open(r->dir, &r->store)) != ASTIME_OK) {
    snprintf(err, n, "%s: %s", r->dir, astime_err_name(e));
    free(r);
    return false;
  }
  astime_strings issues = {0};
  if (astime_store_issues(r->store, &issues) == ASTIME_OK)
    for (size_t i = 0; i < issues.n; i++)
      fprintf(stderr, "asterism serve: routine: %s\n", issues.items[i]);
  astime_strings_free(&issues);
  d->routines = r;
  return true;
}

void serve_routines_close(struct serve_daemon *d) {
  routines *r = R(d);
  if (!r) return;
  for (size_t i = 0; i < 16; i++) free(r->watch[i].content);
  astime_store_close(r->store);
  free(r);
  d->routines = NULL;
}

size_t serve_routines_count(struct serve_daemon *d) {
  astime_strings names = {0};
  size_t n = 0;
  if (R(d) && astime_store_list(R(d)->store, &names) == ASTIME_OK) n = names.n;
  astime_strings_free(&names);
  return n;
}

static pending_watch *watch_slot(routines *r, const char *name, bool create) {
  for (size_t i = 0; i < 16; i++)
    if (r->watch[i].used && !strcmp(r->watch[i].name, name)) return &r->watch[i];
  if (!create) return NULL;
  for (size_t i = 0; i < 16; i++)
    if (!r->watch[i].used) {
      memset(&r->watch[i], 0, sizeof r->watch[i]);
      r->watch[i].used = true;
      snprintf(r->watch[i].name, sizeof r->watch[i].name, "%s", name);
      return &r->watch[i];
    }
  return NULL;
}

/* Fetches a watched target; *content gets readable text (may be NULL). */
static bool fetch(const astime_routine *rt, char hash[ASTIME_HASH_MAX], char **content) {
  *content = NULL;
  if (rt->watch_kind == ASTIME_WATCH_FILE) {
    size_t len = 0;
    if (astime_hash_file(rt->watch_target, hash) != ASTIME_OK) return false;
    if (os_read_file(rt->watch_target, content, &len) != CORE_OK) *content = NULL;
    return true;
  }
  char *body = NULL;
  size_t len = 0;
  long status = 0;
  if (aslink_http_get(rt->watch_target, WATCH_MAX_BYTES, 30000, &body, &len, &status) !=
          ASLINK_OK ||
      status < 200 || status >= 300) {
    free(body);
    return false;
  }
  astime_hash_bytes(body, len, hash);
  *content = body;
  return true;
}

void serve_routines_tick(struct serve_daemon *d) {
  routines *r = R(d);
  astime_due_list due = {0};
  int64_t now = os_now_unix();
  if (!r || astime_store_reload(r->store) != ASTIME_OK ||
      astime_due(r->store, now, &due) != ASTIME_OK)
    return;
  for (size_t i = 0; i < due.n; i++) {
    astime_due_item *it = &due.items[i];
    astime_routine rt;
    char hash[ASTIME_HASH_MAX] = "";
    char *content = NULL;
    astime_routine_init(&rt);
    if (astime_store_get(r->store, it->name, &rt, NULL) != ASTIME_OK) continue;
    if (astime_mark_started(r->store, it->name, now) != ASTIME_OK) {
      astime_routine_free(&rt);
      continue;
    }
    if (it->deliver_after > now) {
      /* Quiet hours: give the slot back; astime retries after the window. */
      astime_mark_finished(r->store, it->name, now, ASTIME_STATUS_DEFERRED, NULL, NULL);
      astime_routine_free(&rt);
      continue;
    }
    if (it->watch != ASTIME_WATCH_NONE) {
      if (!fetch(&rt, hash, &content)) {
        astime_mark_finished(r->store, it->name, now, ASTIME_STATUS_FAILED, "watch fetch", NULL);
        astime_routine_free(&rt);
        continue;
      }
      /* The first check only records a baseline; an unchanged target is skipped. */
      if (!it->last_watch_hash[0] || !strcmp(it->last_watch_hash, hash)) {
        astime_mark_finished(r->store, it->name, now, ASTIME_STATUS_SKIPPED, NULL, hash);
        free(content);
        astime_routine_free(&rt);
        continue;
      }
    }
    os_mutex_lock(&d->mu);
    pending_watch *w = watch_slot(r, it->name, true);
    if (w) {
      free(w->content);
      w->content = content;
      content = NULL;
      snprintf(w->hash, sizeof w->hash, "%s", hash);
    }
    os_mutex_unlock(&d->mu);
    free(content);
    serve_job job;
    memset(&job, 0, sizeof job);
    job.kind = JOB_ROUTINE;
    job.star = &d->stars[0];
    snprintf(job.routine, sizeof job.routine, "%s", it->name);
    if (!serve_enqueue(d, &job))
      astime_mark_finished(r->store, it->name, now, ASTIME_STATUS_DEFERRED, "queue full", NULL);
    astime_routine_free(&rt);
  }
  astime_due_list_free(&due);
}

char *serve_routines_turn_text(struct serve_daemon *d, const char *name,
                               asngn_origin *origin, char star[33]) {
  routines *r = R(d);
  astime_routine rt;
  core_buf b;
  *origin = ASNGN_ORIGIN_HOST;
  star[0] = 0;
  if (!r) return NULL;
  astime_routine_init(&rt);
  if (astime_store_get(r->store, name, &rt, NULL) != ASTIME_OK) return NULL;
  if (rt.deliver_star) snprintf(star, 33, "%s", rt.deliver_star);
  core_buf_init(&b);
  core_buf_appends(&b, rt.goal ? rt.goal : "");
  os_mutex_lock(&d->mu);
  pending_watch *w = watch_slot(r, name, false);
  if (w && w->content) {
    size_t n = strlen(w->content);
    if (n > WATCH_TEXT_MAX) n = WATCH_TEXT_MAX;
    while (n && !core_utf8_valid(w->content, n)) n--;
    core_buf_appends(&b, "\n\n");
    core_buf_printf(&b, serve_s(S_UNTRUSTED_NOTE),
                    rt.watch_target ? rt.watch_target : "watch");
    core_buf_appendc(&b, '\n');
    core_buf_append(&b, w->content, n);
    core_buf_appends(&b, "\n\nIf nothing is relevant to the goal, answer only: SKIP");
    *origin = ASNGN_ORIGIN_WEB;
    free(w->content);
    w->content = NULL;
  }
  os_mutex_unlock(&d->mu);
  astime_routine_free(&rt);
  return core_buf_detach(&b);
}

void serve_routines_prepare(struct serve_daemon *d, const char *name, asngn_session *s) {
  astime_routine rt;
  if (!R(d)) return;
  astime_routine_init(&rt);
  if (astime_store_get(R(d)->store, name, &rt, NULL) == ASTIME_OK) {
    /* astime and asngn order their modes and profiles the same way. */
    (void)asngn_session_set_mode(s, (asngn_usage_mode)rt.mode);
    (void)asngn_session_set_security_profile(s, (asngn_security_profile)rt.profile);
  }
  astime_routine_free(&rt);
}

bool serve_routines_delivers(struct serve_daemon *d, const char *name) {
  astime_routine rt;
  bool deliver = true;
  if (!R(d)) return true;
  astime_routine_init(&rt);
  if (astime_store_get(R(d)->store, name, &rt, NULL) == ASTIME_OK)
    deliver = rt.notify != ASTIME_NOTIFY_NEVER;
  astime_routine_free(&rt);
  return deliver;
}

void serve_routines_finished(struct serve_daemon *d, const char *name, bool ok,
                             const char *answer, const char *error) {
  routines *r = R(d);
  char hash[ASTIME_HASH_MAX] = "";
  if (!r) return;
  os_mutex_lock(&d->mu);
  pending_watch *w = watch_slot(r, name, false);
  if (w) {
    snprintf(hash, sizeof hash, "%s", w->hash);
    free(w->content);
    w->used = false;
  }
  os_mutex_unlock(&d->mu);
  bool skipped = ok && answer && !strncmp(answer, "SKIP", 4);
  astime_mark_finished(r->store, name, os_now_unix(),
                       !ok ? ASTIME_STATUS_FAILED : skipped ? ASTIME_STATUS_SKIPPED
                                                            : ASTIME_STATUS_OK,
                       error, hash[0] ? hash : NULL);
}

/* ── /routine ─────────────────────────────────────────────────────────── */

static void preview(struct serve_daemon *d, serve_star *star, aslink_ctx *bot, int64_t chat,
                    int64_t thread, const char *name, bool buttons) {
  routines *r = R(d);
  astime_routine rt;
  astime_state st;
  char *text = NULL;
  astime_routine_init(&rt);
  astime_state_init(&st);
  if (astime_store_get(r->store, name, &rt, &st) != ASTIME_OK ||
      astime_dry_run(&rt, &st, os_now_unix(), &text) != ASTIME_OK) {
    serve_sendf(star, bot, chat, thread, false, serve_s(S_ROUTINE_UNKNOWN), name);
    astime_routine_free(&rt);
    return;
  }
  char ty[72], tn[72];
  aslink_button b[2];
  aslink_keyboard kb = {b, 2, 2};
  aslink_send_opts opts;
  memset(&opts, 0, sizeof opts);
  if (buttons && serve_token_make(&d->st, 'r', name, 'y', chat, ty, sizeof ty) &&
      serve_token_make(&d->st, 'r', name, 'n', chat, tn, sizeof tn)) {
    b[0] = (aslink_button){serve_s(S_ROUTINE_ACTIVATE), ty, NULL};
    b[1] = (aslink_button){serve_s(S_ROUTINE_CANCEL), tn, NULL};
    opts.keyboard = &kb;
  }
  char **parts = NULL;
  size_t n = 0;
  if (aslink_split_text(text, 0, &parts, &n) == ASLINK_OK)
    for (size_t i = 0; i < n; i++)
      (void)aslink_send_message(bot, chat, thread, parts[i], i + 1 == n ? &opts : NULL, NULL,
                                NULL);
  aslink_strings_free(parts, n);
  astime_free(text);
  astime_routine_free(&rt);
}

static void list(struct serve_daemon *d, serve_star *star, aslink_ctx *bot, int64_t chat,
                 int64_t thread) {
  routines *r = R(d);
  astime_strings names = {0};
  core_buf b;
  int64_t now = os_now_unix();
  if (astime_store_list(r->store, &names) != ASTIME_OK || !names.n) {
    serve_send(star, bot, chat, thread, serve_s(S_ROUTINE_NONE), false);
    astime_strings_free(&names);
    return;
  }
  core_buf_init(&b);
  for (size_t i = 0; i < names.n; i++) {
    astime_routine rt;
    astime_state st;
    char *when = NULL, next[ASTIME_TIME_TEXT_SIZE] = "—";
    int64_t due;
    astime_routine_init(&rt);
    astime_state_init(&st);
    if (astime_store_get(r->store, names.items[i], &rt, &st) != ASTIME_OK) continue;
    (void)astime_describe_trigger(&rt.trigger, &when);
    if (rt.enabled && astime_next_due(&rt, &st, now, &due) == ASTIME_OK)
      astime_format_time(due, rt.trigger.tz, next);
    core_buf_printf(&b, "%s %s — %s · %s · %s\n", rt.enabled ? "▶️" : "⏸", rt.name,
                    when ? when : (rt.watch_target ? rt.watch_target : "?"), next,
                    astime_status_name(st.last_status));
    astime_free(when);
    astime_routine_free(&rt);
  }
  serve_send(star, bot, chat, thread, b.data, false);
  core_buf_free(&b);
  astime_strings_free(&names);
}

static bool word(const char **args, const char *w) {
  size_t n = strlen(w);
  if (strncmp(*args, w, n) || ((*args)[n] != ' ' && (*args)[n] != 0)) return false;
  *args += n;
  while (**args == ' ') (*args)++;
  return true;
}

void serve_routines_command(struct serve_daemon *d, serve_star *star, aslink_ctx *bot,
                            int64_t chat, int64_t thread, const char *args) {
  routines *r = R(d);
  char err[ASTIME_ERR_SIZE] = "";
  if (!r) {
    serve_sendf(star, bot, chat, thread, false, serve_s(S_ERROR), "routines");
    return;
  }
  (void)astime_store_reload(r->store);
  if (!*args) {
    list(d, star, bot, chat, thread);
  } else if (word(&args, "add")) {
    astime_routine rt;
    astime_routine_init(&rt);
    if (astime_routine_from_chat(args, os_now_unix(), ASTIME_TZ_LOCAL, &rt, err) !=
        ASTIME_OK) {
      serve_send(star, bot, chat, thread, err, false);
      return;
    }
    char unique[ASTIME_NAME_MAX];
    if (astime_store_unique_name(r->store, rt.name, unique) == ASTIME_OK) {
      free(rt.name);
      rt.name = core_strdup(unique);
    }
    if (star != &d->stars[0]) rt.deliver_star = core_strdup(star->cfg.name);
    if (!rt.name || astime_store_put(r->store, &rt, false, err) != ASTIME_OK) {
      serve_send(star, bot, chat, thread, err[0] ? err : "routine", false);
    } else {
      serve_sendf(star, bot, chat, thread, false, serve_s(S_ROUTINE_SAVED), rt.name);
      preview(d, star, bot, chat, thread, rt.name, true);
    }
    astime_routine_free(&rt);
  } else if (word(&args, "try")) {
    preview(d, star, bot, chat, thread, args, false);
  } else if (word(&args, "on")) {
    if (astime_store_set_enabled(r->store, args, true) == ASTIME_OK)
      serve_sendf(star, bot, chat, thread, false, serve_s(S_ROUTINE_ON), args);
    else serve_sendf(star, bot, chat, thread, false, serve_s(S_ROUTINE_UNKNOWN), args);
  } else if (word(&args, "off")) {
    if (astime_store_set_enabled(r->store, args, false) == ASTIME_OK)
      serve_sendf(star, bot, chat, thread, false, serve_s(S_ROUTINE_OFF), args);
    else serve_sendf(star, bot, chat, thread, false, serve_s(S_ROUTINE_UNKNOWN), args);
  } else if (word(&args, "run")) {
    serve_job job;
    memset(&job, 0, sizeof job);
    job.kind = JOB_ROUTINE;
    job.star = &d->stars[0];
    snprintf(job.routine, sizeof job.routine, "%s", args);
    if (astime_store_get(r->store, args, NULL, NULL) != ASTIME_OK ||
        astime_mark_started(r->store, args, os_now_unix()) != ASTIME_OK) {
      serve_sendf(star, bot, chat, thread, false, serve_s(S_ROUTINE_UNKNOWN), args);
      return;
    }
    serve_sendf(star, bot, chat, thread, false, serve_s(S_ROUTINE_RUNNING), args);
    if (!serve_enqueue(d, &job))
      astime_mark_finished(r->store, args, os_now_unix(), ASTIME_STATUS_DEFERRED, "queue full",
                           NULL);
  } else if (word(&args, "rm")) {
    if (astime_store_remove(r->store, args) == ASTIME_OK)
      serve_sendf(star, bot, chat, thread, false, serve_s(S_ROUTINE_OFF), args);
    else serve_sendf(star, bot, chat, thread, false, serve_s(S_ROUTINE_UNKNOWN), args);
  } else {
    preview(d, star, bot, chat, thread, args, false);
  }
}

void serve_routines_callback(struct serve_daemon *d, aslink_ctx *bot, int64_t chat,
                             int64_t thread, int64_t message_id, const char *name,
                             char choice) {
  routines *r = R(d);
  if (!r) return;
  (void)aslink_edit_keyboard(bot, chat, message_id, NULL, NULL);
  if (choice == 'y' && astime_store_set_enabled(r->store, name, true) == ASTIME_OK)
    serve_sendf(&d->stars[0], bot, chat, thread, false, serve_s(S_ROUTINE_ON), name);
  else if (choice == 'n' && astime_store_remove(r->store, name) == ASTIME_OK)
    serve_sendf(&d->stars[0], bot, chat, thread, false, serve_s(S_ROUTINE_OFF), name);
  else serve_sendf(&d->stars[0], bot, chat, thread, false, serve_s(S_ROUTINE_UNKNOWN), name);
}
