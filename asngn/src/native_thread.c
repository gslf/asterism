/*
 * native_thread.c — the conversation of a native-action session, as sent.
 *
 * A model server keeps the state of the last prompt it read and resumes from
 * the longest prefix a new prompt shares with it. A session is therefore
 * cheap only when every request starts with the previous one. The thread is
 * that prefix: the system text the session started with, then every finished
 * turn as the messages the model saw (user message, tool calls, results,
 * answer). A turn only ever appends to it. What changes between rounds
 * (runtime state, the working contract) is sent after it, in the last
 * message, and is never stored.
 *
 * sessions/<slug>/native.thread, replaced atomically at each commit:
 *
 *   asterism-native-thread 1\n
 *   <role> <id bytes> <name bytes> <text bytes>\n<id><name><text>\n   (repeated)
 *
 * role: s system text, m memory of older turns, u user, c tool call,
 * r tool result, a assistant text.
 *
 * The thread is bounded by dropping its oldest turns in one large step, since
 * every cut makes the server read the rest again. The memory module still
 * holds everything; its account of the session is sent again after a cut.
 *
 * MIT License — per aspera ad astra.
 */
#include "native.h"
#include <stdlib.h>
#include <string.h>

#define THREAD_MAGIC "asterism-native-thread 1\n"
#define THREAD_FILE "native.thread"
/* asmodel accepts 256 messages and 256 calls; leave room for one turn. */
#define THREAD_MAX_MESSAGES 150
#define THREAD_KEEP_TURNS 2

void asngn_native_thread_free(asngn_native_thread *th) {
  if (!th) return;
  for (size_t i = 0; i < th->n; i++) {
    free(th->v[i].id);
    free(th->v[i].name);
    free(th->v[i].text);
  }
  free(th->v);
  memset(th, 0, sizeof *th);
}

static char *dup_n(const char *s, size_t n) {
  char *p = malloc(n + 1);
  if (!p) return NULL;
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

asngn_err asngn_native_thread_add(asngn_native_thread *th, char role, const char *id,
                                  const char *name, const char *text) {
  if (th->n == th->cap) {
    size_t cap = th->cap ? th->cap * 2 : 32;
    asngn_thread_msg *v = realloc(th->v, cap * sizeof *v);
    if (!v) return ASNGN_ERR_NOMEM;
    th->v = v;
    th->cap = cap;
  }
  asngn_thread_msg *m = &th->v[th->n];
  memset(m, 0, sizeof *m);
  m->role = role;
  m->id = id ? core_strdup(id) : NULL;
  m->name = name ? core_strdup(name) : NULL;
  m->text = core_strdup(text ? text : "");
  if ((id && !m->id) || (name && !m->name) || !m->text) {
    free(m->id);
    free(m->name);
    free(m->text);
    return ASNGN_ERR_NOMEM;
  }
  th->bytes += strlen(m->text) + (id ? strlen(id) : 0) + (name ? strlen(name) : 0) + 16;
  th->n++;
  return ASNGN_OK;
}

static bool role_ok(char r) { return r == 's' || r == 'm' || r == 'u' || r == 'c' || r == 'r' || r == 'a'; }

/* A missing or damaged file is an empty thread: the session then starts a
 * new one from the memory module's account, which is never lost. */
asngn_err asngn_native_thread_load(const asngn_session *s, asngn_native_thread *th) {
  memset(th, 0, sizeof *th);
  if (!s || !s->dir) return ASNGN_OK;
  char *path = os_path_join(s->dir, THREAD_FILE), *data = NULL;
  size_t len = 0;
  if (!path) return ASNGN_ERR_NOMEM;
  if (os_read_file(path, &data, &len) != ASNGN_OK) {
    free(path);
    return ASNGN_OK;
  }
  free(path);
  size_t off = strlen(THREAD_MAGIC);
  asngn_err e = ASNGN_OK;
  bool bad = len < off || memcmp(data, THREAD_MAGIC, off) != 0;
  while (!bad && e == ASNGN_OK && off < len) {
    char role = data[off];
    unsigned long long a = 0, b = 0, t = 0;
    int used = 0;
    if (!role_ok(role) || sscanf(data + off + 1, " %llu %llu %llu%n", &a, &b, &t, &used) != 3 ||
        data[off + 1 + (size_t)used] != '\n') {
      bad = true;
      break;
    }
    off += 2 + (size_t)used;
    if (a > len || b > len || t > len || a + b + t + 1 > len - off || data[off + a + b + t] != '\n') {
      bad = true;
      break;
    }
    char *id = a ? dup_n(data + off, a) : NULL, *name = b ? dup_n(data + off + a, b) : NULL;
    char *text = dup_n(data + off + a + b, t);
    if ((a && !id) || (b && !name) || !text) e = ASNGN_ERR_NOMEM;
    else if (strlen(text) != t || !core_utf8_valid(text, t)) bad = true;
    else e = asngn_native_thread_add(th, role, id, name, text);
    free(id);
    free(name);
    free(text);
    off += a + b + t + 1;
  }
  free(data);
  if (bad || e != ASNGN_OK) asngn_native_thread_free(th);
  return e;
}

static asngn_err thread_save(const asngn_session *s, const asngn_native_thread *th) {
  core_buf b;
  core_buf_init(&b);
  asngn_err e = asngn_buf_appends(&b, THREAD_MAGIC);
  for (size_t i = 0; e == ASNGN_OK && i < th->n; i++) {
    const asngn_thread_msg *m = &th->v[i];
    size_t a = m->id ? strlen(m->id) : 0, n = m->name ? strlen(m->name) : 0, t = strlen(m->text);
    e = asngn_buf_printf(&b, "%c %zu %zu %zu\n", m->role, a, n, t);
    if (e == ASNGN_OK && a) e = asngn_buf_append(&b, m->id, a);
    if (e == ASNGN_OK && n) e = asngn_buf_append(&b, m->name, n);
    if (e == ASNGN_OK) e = asngn_buf_append(&b, m->text, t);
    if (e == ASNGN_OK) e = asngn_buf_appendc(&b, '\n');
  }
  char *path = e == ASNGN_OK ? os_path_join(s->dir, THREAD_FILE) : NULL;
  char *tmp = path ? os_path_join(s->dir, THREAD_FILE ".tmp") : NULL;
  if (e == ASNGN_OK && (!path || !tmp)) e = ASNGN_ERR_NOMEM;
  if (e == ASNGN_OK && (e = os_write_file(tmp, b.data ? b.data : "", b.len)) != ASNGN_OK) {}
  if (e == ASNGN_OK) e = os_file_replace(tmp, path);
  free(path);
  free(tmp);
  core_buf_free(&b);
  return e;
}

/* Drop the oldest turns, keeping the last THREAD_KEEP_TURNS, and forget the
 * system text and the memory message: the next turn takes both afresh. */
static void thread_cut(asngn_native_thread *th) {
  size_t users = 0, first = th->n;
  for (size_t i = th->n; i-- > 0;)
    if (th->v[i].role == 'u' && ++users == THREAD_KEEP_TURNS) {
      first = i;
      break;
    }
  if (users < THREAD_KEEP_TURNS) first = th->n; /* one oversized turn: keep nothing */
  size_t out = 0;
  th->bytes = 0;
  for (size_t i = 0; i < th->n; i++) {
    asngn_thread_msg *m = &th->v[i];
    if (i < first) {
      free(m->id);
      free(m->name);
      free(m->text);
      continue;
    }
    th->bytes += strlen(m->text) + (m->id ? strlen(m->id) : 0) + (m->name ? strlen(m->name) : 0) + 16;
    th->v[out++] = *m;
  }
  th->n = out;
}

/* Bytes the stored thread may hold: a quarter of the generator's context,
 * taken at the provider's own bytes-per-token guess. */
static size_t thread_budget(asngn_ctx *c, int slot) {
  int ctx = slot >= 0 && (size_t)slot < c->models_n && c->models[slot].cfg.ctx > 0
                ? c->models[slot].cfg.ctx : 32768;
  int probe = asngn_models_count_tokens(c, slot, "0123456789012345678901234567890123456789"
                                                 "0123456789012345678901234567890123456789");
  double bpt = probe > 0 ? 80.0 / (double)probe : 1.0;
  if (bpt < 1.0) bpt = 1.0;
  if (bpt > 6.0) bpt = 6.0;
  return (size_t)((double)ctx * bpt / 4.0);
}

bool asngn_native_thread_full(asngn_ctx *c, int slot, const asngn_native_thread *th) {
  return th->n > THREAD_MAX_MESSAGES || th->bytes > thread_budget(c, slot);
}

void asngn_native_pending_free(asngn_turn_state *t) {
  if (!t || !t->native_pending) return;
  asngn_native_thread_free(t->native_pending);
  free(t->native_pending);
  t->native_pending = NULL;
}

/* The turn is committed: its messages join the thread. A failure here costs
 * speed, never the turn: the next one starts a thread from memory. */
asngn_err asngn_native_thread_commit(asngn_ctx *c, asngn_turn_state *t, const char *answer) {
  asngn_native_thread *p = t ? t->native_pending : NULL;
  if (!c || !p || !t->s || !t->s->dir) return ASNGN_OK;
  if (answer && *answer) {
    char *shown = asngn_context_text(t->s, answer);
    asngn_err e = shown ? asngn_native_thread_add(p, 'a', NULL, NULL, shown) : ASNGN_ERR_NOMEM;
    free(shown);
    if (e != ASNGN_OK) return e;
  }
  if (asngn_native_thread_full(c, t->gen_slot, p)) thread_cut(p);
  asngn_err e = thread_save(t->s, p);
  asngn_native_pending_free(t);
  return e;
}
