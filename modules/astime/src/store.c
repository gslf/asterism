/*
 * store.c — the routines directory as the daemon sees it: definitions,
 * per-routine state, the due list and run bookkeeping.
 *
 * One mutex guards everything; one lock file keeps a second process from
 * writing the same state. State is persisted with atomic replace whenever
 * it changes meaningfully (arming, runs); the cached next_due is refreshed
 * in memory on every astime_due call.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astime_internal.h"

#define ISSUES_MAX 200

typedef struct {
  astime_routine r;
  astime_state st;
  bool prev_valid; /* values before mark_started, for DEFERRED */
  int64_t prev_due, prev_watch;
} entry;

struct astime_store {
  os_mutex mu;
  char *dir;
  FILE *lock;
  entry *v;
  size_t n;
  astime_strings issues;
  int64_t grace;
};

static void issue(astime_store *s, const char *fmt, const char *a, const char *b) {
  if (s->issues.n >= ISSUES_MAX) return;
  (void)astime__strings_pushf(&s->issues, fmt, a, b);
}

/* A definition file with this name exists, valid or not. */
static bool on_disk(astime_store *s, const char *name) {
  core_buf b;
  bool yes;
  core_buf_init(&b);
  yes = core_buf_printf(&b, "%s/%s.xcdn", s->dir, name) == CORE_OK && os_file_exists(b.data) == 1;
  core_buf_free(&b);
  return yes;
}

static entry *find(astime_store *s, const char *name) {
  size_t i;
  if (!name) return NULL;
  for (i = 0; i < s->n; i++)
    if (strcmp(s->v[i].r.name, name) == 0) return &s->v[i];
  return NULL;
}

static void save_state(astime_store *s, entry *e) {
  astime_err rc = astime_state_save(s->dir, e->r.name, &e->st);
  if (rc != ASTIME_OK) issue(s, "%s: cannot save state (%s)", e->r.name, astime_err_name(rc));
}

static void refresh_next(astime_store *s, entry *e, int64_t now) {
  int64_t t;
  if (astime__next_due(&e->r, &e->st, now, astime__armed(&e->r, &e->st, now), s->grace, &t) ==
      ASTIME_OK)
    e->st.next_due = t;
  else
    e->st.next_due = 0;
}

/* State from disk for a routine this process did not know: corrupt files
 * are set aside and reset, an interrupted run is closed as failed. */
static void load_state(astime_store *s, entry *e) {
  char err[ASTIME_ERR_SIZE];
  astime_err rc;
  err[0] = '\0';
  rc = astime_state_load(s->dir, e->r.name, &e->st, err);
  if (rc == ASTIME_ERR_NOT_FOUND) {
    astime_state_init(&e->st);
    return;
  }
  if (rc != ASTIME_OK) {
    core_buf from, to;
    core_buf_init(&from);
    core_buf_init(&to);
    astime_state_init(&e->st);
    if (rc == ASTIME_ERR_PARSE &&
        core_buf_printf(&from, "%s/.state/%s.xcdn", s->dir, e->r.name) == CORE_OK &&
        core_buf_printf(&to, "%s.corrupt", from.data) == CORE_OK)
      (void)os_rename(from.data, to.data);
    issue(s, "%s; state reset (old file kept as .state/%s.xcdn.corrupt)", err, e->r.name);
    core_buf_free(&from);
    core_buf_free(&to);
    save_state(s, e);
    return;
  }
  if (e->st.running_since) {
    e->st.running_since = 0;
    e->st.last_status = ASTIME_STATUS_FAILED;
    astime__copy_short(e->st.last_error, sizeof e->st.last_error,
                       "interrupted: the daemon stopped during the run");
    e->st.consecutive_failures++;
    save_state(s, e);
  }
}

static int cmp_entry(const void *a, const void *b) {
  return strcmp(((const entry *)a)->r.name, ((const entry *)b)->r.name);
}

/* Replace the routine set with the directory contents; s->mu held. */
static astime_err load_all(astime_store *s) {
  astime_routine *list = NULL;
  size_t n = 0, i;
  entry *nv = NULL;
  astime_strings issues = {NULL, 0};
  astime_err e;
  e = astime_load_dir(s->dir, &list, &n, &issues);
  if (e != ASTIME_OK) {
    astime_strings_free(&issues);
    return e;
  }
  if (n) {
    nv = (entry *)calloc(n, sizeof *nv);
    if (!nv) {
      astime_routines_free(list, n);
      astime_strings_free(&issues);
      return ASTIME_ERR_NOMEM;
    }
  }
  astime_strings_free(&s->issues);
  s->issues = issues;
  for (i = 0; i < n; i++) {
    entry *old = find(s, list[i].name);
    nv[i].r = list[i];
    if (old) {
      nv[i].st = old->st;
      nv[i].prev_valid = old->prev_valid;
      nv[i].prev_due = old->prev_due;
      nv[i].prev_watch = old->prev_watch;
    }
  }
  free(list); /* routines moved into nv */
  for (i = 0; i < s->n; i++) astime_routine_free(&s->v[i].r);
  free(s->v);
  s->v = nv;
  s->n = n;
  return ASTIME_OK;
}

static astime_err reload_locked(astime_store *s, bool first) {
  size_t i, n_old = s->n;
  char **known = NULL;
  astime_err e;
  /* Names this process already tracks keep their in-memory state. */
  if (n_old) {
    known = (char **)calloc(n_old, sizeof *known);
    if (!known) return ASTIME_ERR_NOMEM;
    for (i = 0; i < n_old; i++) {
      known[i] = core_strdup(s->v[i].r.name);
      if (!known[i]) {
        os_free_names(known, i);
        return ASTIME_ERR_NOMEM;
      }
    }
  }
  e = load_all(s);
  if (e == ASTIME_OK) {
    for (i = 0; i < s->n; i++) {
      bool seen = false;
      size_t k;
      for (k = 0; !first && k < n_old && !seen; k++) seen = strcmp(known[k], s->v[i].r.name) == 0;
      if (!seen) load_state(s, &s->v[i]);
    }
    if (s->n > 1) qsort(s->v, s->n, sizeof *s->v, cmp_entry);
  }
  if (known) os_free_names(known, n_old);
  return e;
}

astime_err astime_store_open(const char *dir, astime_store **out) {
  astime_store *s;
  char *sdir = NULL, *lock = NULL;
  astime_err e;
  if (!dir || !out || !*dir) return ASTIME_ERR_INVALID;
  *out = NULL;
  s = (astime_store *)calloc(1, sizeof *s);
  if (!s) return ASTIME_ERR_NOMEM;
  s->grace = ASTIME_CATCH_UP_DEFAULT;
  s->dir = core_strdup(dir);
  sdir = os_path_join(dir, ".state");
  lock = sdir ? os_path_join(sdir, "lock") : NULL;
  if (!s->dir || !lock) {
    e = ASTIME_ERR_NOMEM;
    goto fail;
  }
  e = astime_from_core(os_mkdir_p(sdir));
  if (e != ASTIME_OK) goto fail;
  s->lock = os_store_lock(lock);
  if (!s->lock) {
    e = ASTIME_ERR_BUSY;
    goto fail;
  }
  os_mutex_init(&s->mu);
  e = reload_locked(s, true);
  if (e != ASTIME_OK) {
    os_mutex_destroy(&s->mu);
    goto fail;
  }
  free(sdir);
  free(lock);
  *out = s;
  return ASTIME_OK;
fail:
  if (s->lock) fclose(s->lock);
  free(s->dir);
  free(s);
  free(sdir);
  free(lock);
  return e;
}

void astime_store_close(astime_store *s) {
  size_t i;
  if (!s) return;
  for (i = 0; i < s->n; i++) astime_routine_free(&s->v[i].r);
  free(s->v);
  astime_strings_free(&s->issues);
  os_mutex_destroy(&s->mu);
  if (s->lock) fclose(s->lock);
  free(s->dir);
  free(s);
}

astime_err astime_store_reload(astime_store *s) {
  astime_err e;
  if (!s) return ASTIME_ERR_INVALID;
  os_mutex_lock(&s->mu);
  e = reload_locked(s, false);
  os_mutex_unlock(&s->mu);
  return e;
}

astime_err astime_store_issues(astime_store *s, astime_strings *out) {
  size_t i;
  astime_err e = ASTIME_OK;
  if (!s || !out) return ASTIME_ERR_INVALID;
  out->items = NULL;
  out->n = 0;
  os_mutex_lock(&s->mu);
  for (i = 0; i < s->issues.n && e == ASTIME_OK; i++)
    e = astime__strings_push(out, core_strdup(s->issues.items[i]));
  os_mutex_unlock(&s->mu);
  if (e != ASTIME_OK) astime_strings_free(out);
  return e;
}

void astime_store_set_catch_up(astime_store *s, int64_t grace_s) {
  if (!s) return;
  os_mutex_lock(&s->mu);
  s->grace = grace_s < 0 ? 0 : grace_s;
  os_mutex_unlock(&s->mu);
}

astime_err astime_store_list(astime_store *s, astime_strings *out) {
  size_t i;
  astime_err e = ASTIME_OK;
  if (!s || !out) return ASTIME_ERR_INVALID;
  out->items = NULL;
  out->n = 0;
  os_mutex_lock(&s->mu);
  for (i = 0; i < s->n && e == ASTIME_OK; i++)
    e = astime__strings_push(out, core_strdup(s->v[i].r.name));
  os_mutex_unlock(&s->mu);
  if (e != ASTIME_OK) astime_strings_free(out);
  return e;
}

astime_err astime_store_get(astime_store *s, const char *name, astime_routine *out_routine,
                            astime_state *out_state) {
  entry *e;
  astime_err rc = ASTIME_OK;
  if (!s || !name) return ASTIME_ERR_INVALID;
  if (out_routine) astime_routine_init(out_routine);
  os_mutex_lock(&s->mu);
  e = find(s, name);
  if (!e) {
    rc = ASTIME_ERR_NOT_FOUND;
  } else {
    if (out_routine) rc = astime_routine_copy(&e->r, out_routine);
    if (out_state) *out_state = e->st;
  }
  os_mutex_unlock(&s->mu);
  return rc;
}

/* Insert or replace the in-memory copy; s->mu held. */
static astime_err install(astime_store *s, const astime_routine *r) {
  entry *e = find(s, r->name), *nv;
  astime_routine copy;
  astime_err rc = astime_routine_copy(r, &copy);
  if (rc != ASTIME_OK) return rc;
  if (e) {
    astime_routine_free(&e->r);
    e->r = copy;
    return ASTIME_OK;
  }
  nv = (entry *)realloc(s->v, (s->n + 1) * sizeof *nv);
  if (!nv) {
    astime_routine_free(&copy);
    return ASTIME_ERR_NOMEM;
  }
  s->v = nv;
  memset(&s->v[s->n], 0, sizeof s->v[s->n]);
  s->v[s->n].r = copy;
  load_state(s, &s->v[s->n]);
  s->n++;
  if (s->n > 1) qsort(s->v, s->n, sizeof *s->v, cmp_entry);
  return ASTIME_OK;
}

astime_err astime_store_put(astime_store *s, const astime_routine *r, bool replace,
                            char err[ASTIME_ERR_SIZE]) {
  astime_err rc;
  if (!s || !r) return ASTIME_ERR_INVALID;
  rc = astime_routine_validate(r, err);
  if (rc != ASTIME_OK) return rc;
  os_mutex_lock(&s->mu);
  if (!replace && (find(s, r->name) || on_disk(s, r->name))) {
    astime__seterr(err, "a routine named \"%s\" already exists", r->name);
    rc = ASTIME_ERR_EXISTS;
  } else {
    rc = astime_routine_save(s->dir, r, err);
    if (rc == ASTIME_OK) rc = install(s, r);
  }
  os_mutex_unlock(&s->mu);
  return rc;
}

astime_err astime_store_set_enabled(astime_store *s, const char *name, bool enabled) {
  entry *e;
  astime_routine copy;
  astime_err rc;
  if (!s || !name) return ASTIME_ERR_INVALID;
  os_mutex_lock(&s->mu);
  e = find(s, name);
  if (!e) {
    os_mutex_unlock(&s->mu);
    return ASTIME_ERR_NOT_FOUND;
  }
  if (e->r.enabled == enabled) {
    os_mutex_unlock(&s->mu);
    return ASTIME_OK;
  }
  rc = astime_routine_copy(&e->r, &copy);
  if (rc == ASTIME_OK) {
    copy.enabled = enabled;
    rc = astime_routine_save(s->dir, &copy, NULL);
    if (rc == ASTIME_OK) {
      astime_routine_free(&e->r);
      e->r = copy;
    } else {
      astime_routine_free(&copy);
    }
  }
  os_mutex_unlock(&s->mu);
  return rc;
}

astime_err astime_store_remove(astime_store *s, const char *name) {
  entry *e;
  astime_err rc = ASTIME_OK;
  core_buf def, st;
  if (!s || !name) return ASTIME_ERR_INVALID;
  core_buf_init(&def);
  core_buf_init(&st);
  os_mutex_lock(&s->mu);
  e = find(s, name);
  if (!e) {
    rc = ASTIME_ERR_NOT_FOUND;
  } else if (e->st.running_since) {
    rc = ASTIME_ERR_BUSY;
  } else if (core_buf_printf(&def, "%s/%s.xcdn", s->dir, name) != CORE_OK ||
             core_buf_printf(&st, "%s/.state/%s.xcdn", s->dir, name) != CORE_OK) {
    rc = ASTIME_ERR_NOMEM;
  } else {
    rc = astime_from_core(os_remove_file(def.data));
    if (rc == ASTIME_ERR_NOT_FOUND) rc = ASTIME_OK;
    if (rc == ASTIME_OK) {
      size_t idx = (size_t)(e - s->v);
      (void)os_remove_file(st.data);
      astime_routine_free(&e->r);
      memmove(&s->v[idx], &s->v[idx + 1], (s->n - idx - 1) * sizeof *s->v);
      s->n--;
    }
  }
  os_mutex_unlock(&s->mu);
  core_buf_free(&def);
  core_buf_free(&st);
  return rc;
}

astime_err astime_store_unique_name(astime_store *s, const char *base, char out[ASTIME_NAME_MAX]) {
  char slug[ASTIME_NAME_MAX];
  int k;
  astime_err rc;
  if (!s || !base || !out) return ASTIME_ERR_INVALID;
  astime_slug_from_text(base, slug);
  os_mutex_lock(&s->mu);
  snprintf(out, ASTIME_NAME_MAX, "%s", slug);
  for (k = 2; (find(s, out) || on_disk(s, out)) && k < 10000; k++) {
    char suffix[8];
    size_t n = strlen(slug), sl = (size_t)snprintf(suffix, sizeof suffix, "-%d", k);
    if (n + sl > ASTIME_NAME_MAX - 1) n = ASTIME_NAME_MAX - 1 - sl;
    while (n > 1 && slug[n - 1] == '-') n--;
    snprintf(out, ASTIME_NAME_MAX, "%.*s%s", (int)n, slug, suffix);
  }
  rc = find(s, out) || on_disk(s, out) ? ASTIME_ERR_EXISTS : ASTIME_OK;
  os_mutex_unlock(&s->mu);
  return rc;
}

/* ── due and runs ─────────────────────────────────────────────────────── */

astime_err astime_due(astime_store *s, int64_t now, astime_due_list *out) {
  size_t i, cap = 0;
  astime_err rc = ASTIME_OK;
  if (!s || !out) return ASTIME_ERR_INVALID;
  out->items = NULL;
  out->n = 0;
  os_mutex_lock(&s->mu);
  astime__tz_refresh();
  for (i = 0; i < s->n; i++) {
    entry *e = &s->v[i];
    astime_routine *r = &e->r;
    astime_state *st = &e->st;
    char sig[ASTIME_SIG_MAX];
    bool dirty = false;
    astime__eval ev;
    if (!r->enabled) {
      if (st->armed_at) {
        st->armed_at = 0;
        dirty = true;
      }
    } else {
      astime__trigger_sig(r, sig);
      if (!st->armed_at || strcmp(st->trigger_sig, sig) != 0) {
        st->armed_at = now;
        memcpy(st->trigger_sig, sig, sizeof sig);
        dirty = true;
      }
    }
    astime__watch_sig(r, sig);
    if (strcmp(st->watch_sig, sig) != 0) {
      memcpy(st->watch_sig, sig, sizeof sig);
      st->last_watch_hash[0] = '\0';
      st->last_watch_at = 0;
      dirty = true;
    }
    refresh_next(s, e, now);
    if (dirty) save_state(s, e);
    if (!r->enabled || st->running_since) continue;
    astime__evaluate(r, st, now, st->armed_at, s->grace, &ev);
    if (!ev.due) continue;
    if (out->n == cap) {
      size_t nc = cap ? cap * 2 : 8;
      astime_due_item *items = (astime_due_item *)realloc(out->items, nc * sizeof *items);
      if (!items) {
        rc = ASTIME_ERR_NOMEM;
        break;
      }
      out->items = items;
      cap = nc;
    }
    {
      astime_due_item *it = &out->items[out->n++];
      int64_t end;
      memset(it, 0, sizeof *it);
      snprintf(it->name, sizeof it->name, "%s", r->name);
      it->reason = ev.reason;
      it->due_at = ev.slot;
      if (r->notify != ASTIME_NOTIFY_NEVER && astime_in_quiet_hours(r, now, &end))
        it->deliver_after = end;
      it->watch = r->watch_kind;
      memcpy(it->last_watch_hash, st->last_watch_hash, sizeof it->last_watch_hash);
    }
  }
  os_mutex_unlock(&s->mu);
  if (rc != ASTIME_OK) astime_due_list_free(out);
  return rc;
}

void astime_due_list_free(astime_due_list *l) {
  if (!l) return;
  free(l->items);
  l->items = NULL;
  l->n = 0;
}

astime_err astime_store_next_wakeup(astime_store *s, int64_t now, int64_t *out) {
  size_t i;
  bool any = false;
  if (!s || !out) return ASTIME_ERR_INVALID;
  os_mutex_lock(&s->mu);
  for (i = 0; i < s->n; i++) {
    entry *e = &s->v[i];
    int64_t t;
    if (e->st.running_since) continue;
    if (astime__next_due(&e->r, &e->st, now, astime__armed(&e->r, &e->st, now), s->grace, &t) !=
        ASTIME_OK)
      continue;
    if (!any || t < *out) *out = t;
    any = true;
  }
  os_mutex_unlock(&s->mu);
  return any ? ASTIME_OK : ASTIME_ERR_NOT_FOUND;
}

astime_err astime_mark_started(astime_store *s, const char *name, int64_t now) {
  entry *e;
  astime__eval ev;
  astime_err rc = ASTIME_OK;
  if (!s || !name) return ASTIME_ERR_INVALID;
  os_mutex_lock(&s->mu);
  astime__tz_refresh();
  e = find(s, name);
  if (!e) {
    rc = ASTIME_ERR_NOT_FOUND;
  } else if (e->st.running_since) {
    rc = ASTIME_ERR_BUSY;
  } else {
    astime__evaluate(&e->r, &e->st, now, astime__armed(&e->r, &e->st, now), s->grace, &ev);
    e->prev_valid = true;
    e->prev_due = e->st.last_due_at;
    e->prev_watch = e->st.last_watch_at;
    if (ev.slot_pending) e->st.last_due_at = ev.sched_slot;
    if (ev.watch_due) e->st.last_watch_at = now;
    e->st.running_since = now;
    e->st.last_run_at = now;
    e->st.defer_until = 0;
    refresh_next(s, e, now);
    rc = astime_state_save(s->dir, e->r.name, &e->st);
  }
  os_mutex_unlock(&s->mu);
  return rc;
}

static bool hash_ok(const char *h) {
  size_t n;
  for (n = 0; h[n]; n++)
    if (!((h[n] >= '0' && h[n] <= '9') || (h[n] >= 'a' && h[n] <= 'f'))) return false;
  return n <= ASTIME_HASH_MAX - 1;
}

astime_err astime_mark_finished(astime_store *s, const char *name, int64_t now,
                                astime_status status, const char *error,
                                const char *watch_hash) {
  entry *e;
  astime_err rc = ASTIME_OK;
  if (!s || !name || status < ASTIME_STATUS_OK || status > ASTIME_STATUS_DEFERRED)
    return ASTIME_ERR_INVALID;
  if (watch_hash && *watch_hash && !hash_ok(watch_hash)) return ASTIME_ERR_INVALID;
  os_mutex_lock(&s->mu);
  astime__tz_refresh();
  e = find(s, name);
  if (!e) {
    os_mutex_unlock(&s->mu);
    return ASTIME_ERR_NOT_FOUND;
  }
  {
    astime_state *st = &e->st;
    st->running_since = 0;
    st->last_status = status;
    if (status == ASTIME_STATUS_OK)
      st->last_error[0] = '\0';
    else
      astime__copy_short(st->last_error, sizeof st->last_error, error ? error : "");
    if (status == ASTIME_STATUS_FAILED) st->consecutive_failures++;
    if (status == ASTIME_STATUS_OK) st->consecutive_failures = 0;
    if (status == ASTIME_STATUS_OK || status == ASTIME_STATUS_FAILED) {
      char day[11];
      astime__day_text(now, e->r.trigger.tz, day);
      if (strcmp(day, st->runs_day) != 0) {
        memcpy(st->runs_day, day, sizeof day);
        st->runs_today = 0;
      }
      st->runs_today++;
    }
    if (status == ASTIME_STATUS_DEFERRED) {
      if (e->prev_valid) {
        st->last_due_at = e->prev_due;
        st->last_watch_at = e->prev_watch;
      }
      st->defer_until = now + ASTIME_DEFER_RETRY;
    } else {
      st->defer_until = 0;
    }
    if (watch_hash && *watch_hash) {
      snprintf(st->last_watch_hash, sizeof st->last_watch_hash, "%s", watch_hash);
      st->last_watch_at = now;
    }
    e->prev_valid = false;
    /* A one-shot that ran its slot successfully is done: disable it. */
    if (status == ASTIME_STATUS_OK && e->r.trigger.kind == ASTIME_TRIGGER_AT && e->r.enabled &&
        st->last_due_at == e->r.trigger.at) {
      astime_routine copy;
      rc = astime_routine_copy(&e->r, &copy);
      if (rc == ASTIME_OK) {
        copy.enabled = false;
        rc = astime_routine_save(s->dir, &copy, NULL);
        if (rc == ASTIME_OK) {
          astime_routine_free(&e->r);
          e->r = copy;
          st->armed_at = 0;
        } else {
          astime_routine_free(&copy);
          issue(s, "%s: cannot disable the finished one-shot routine (%s)", e->r.name,
                astime_err_name(rc));
        }
      }
    }
    refresh_next(s, e, now);
    {
      astime_err se = astime_state_save(s->dir, e->r.name, st);
      if (rc == ASTIME_OK) rc = se;
    }
  }
  os_mutex_unlock(&s->mu);
  return rc;
}
