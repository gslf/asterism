/* Daemon state under <root>/serve/: the owner, topic bindings, the button
 * key, the pending pairing code and daily usage. Small xCDN files written by
 * atomic replace with owner-only permissions. */
#include "serve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "xcdn.h"

#define BINDINGS_MAX 256

typedef struct {
  int64_t chat, thread;
  char star[33];
  char project[41]; /* "" = none */
} binding;

/* Process-local tables beside the persisted fields (state is a singleton). */
static binding g_bind[BINDINGS_MAX];
static size_t g_bind_n;
static serve_usage g_today, g_month;

static void private_mode(const char *path) {
#if !defined(_WIN32)
  (void)chmod(path, 0600);
#else
  (void)path;
#endif
}

static bool write_private(const char *dir, const char *name, const void *data,
                          size_t len) {
  char path[1200], tmp[1220];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  if (os_write_file(tmp, data, len) != CORE_OK) return false;
  private_mode(tmp);
  if (os_file_replace(tmp, path) != CORE_OK) {
    os_remove_file(tmp);
    return false;
  }
  return true;
}

static xcdn_document_t *read_doc(const char *dir, const char *name) {
  char path[1200], *text = NULL;
  size_t len = 0;
  xcdn_error_t xe;
  xcdn_document_t *doc;
  snprintf(path, sizeof path, "%s/%s", dir, name);
  if (os_read_file(path, &text, &len) != CORE_OK) return NULL;
  doc = xcdn_parse_str(text, len, &xe);
  free(text);
  return doc;
}

static const xcdn_value_t *first_object(const xcdn_document_t *doc) {
  if (!doc || !doc->values_len || !doc->values[0]->value ||
      doc->values[0]->value->type != XCDN_VAL_OBJECT)
    return NULL;
  return doc->values[0]->value;
}

static int64_t int_field(const xcdn_value_t *obj, const char *key) {
  xcdn_node_t *n = obj ? xcdn_object_get(obj, key) : NULL;
  return n && n->value && n->value->type == XCDN_VAL_INT ? xcdn_value_as_int(n->value) : 0;
}

static void load_usage(const xcdn_value_t *obj, const char *key, serve_usage *u) {
  xcdn_node_t *n = obj ? xcdn_object_get(obj, key) : NULL;
  const xcdn_value_t *v = n ? n->value : NULL;
  memset(u, 0, sizeof *u);
  if (!v || v->type != XCDN_VAL_OBJECT) return;
  u->day = int_field(v, "day");
  u->turns = int_field(v, "turns");
  u->local_turns = int_field(v, "local");
  u->paid_turns = int_field(v, "paid");
  u->delegate_calls = int_field(v, "delegates");
  u->routine_runs = int_field(v, "routines");
}

static bool save_locked(serve_state *st) {
  core_buf b;
  bool ok;
  core_buf_init(&b);
  ok = core_buf_printf(&b,
           "#serve_state {\n  owner_id: %lld,\n  owner_chat: %lld,\n"
           "  routine_thread: %lld,\n  bindings: [\n",
           (long long)st->owner_id, (long long)st->owner_chat,
           (long long)st->routine_thread) == CORE_OK;
  for (size_t i = 0; ok && i < g_bind_n; i++)
    ok = core_buf_printf(&b, "    { chat: %lld, thread: %lld, star: \"%s\"%s%s%s },\n",
                         (long long)g_bind[i].chat, (long long)g_bind[i].thread,
                         g_bind[i].star, g_bind[i].project[0] ? ", project: \"" : "",
                         g_bind[i].project, g_bind[i].project[0] ? "\"" : "") == CORE_OK;
  ok = ok && core_buf_appends(&b, "  ],\n") == CORE_OK;
  const serve_usage *u[2] = {&g_today, &g_month};
  const char *names[2] = {"today", "month"};
  for (int i = 0; ok && i < 2; i++)
    ok = core_buf_printf(&b,
             "  %s: { day: %lld, turns: %lld, local: %lld, paid: %lld, "
             "delegates: %lld, routines: %lld },\n",
             names[i], (long long)u[i]->day, (long long)u[i]->turns,
             (long long)u[i]->local_turns, (long long)u[i]->paid_turns,
             (long long)u[i]->delegate_calls, (long long)u[i]->routine_runs) == CORE_OK;
  ok = ok && core_buf_appends(&b, "}\n") == CORE_OK;
  ok = ok && write_private(st->dir, "state.xcdn", b.data, b.len);
  core_buf_free(&b);
  return ok;
}

bool serve_state_save(serve_state *st) {
  os_mutex_lock(&st->mu);
  bool ok = save_locked(st);
  os_mutex_unlock(&st->mu);
  return ok;
}

bool serve_state_open(serve_state *st, const char *root, char *err, size_t n) {
  char path[1200];
  char *key = NULL;
  size_t key_len = 0;
  xcdn_document_t *doc;
  const xcdn_value_t *obj;
  memset(st, 0, sizeof *st);
  g_bind_n = 0;
  memset(&g_today, 0, sizeof g_today);
  memset(&g_month, 0, sizeof g_month);
  snprintf(st->dir, sizeof st->dir, "%s/serve", root);
  if (os_mkdir_p(st->dir) != CORE_OK) {
    snprintf(err, n, "cannot create %s", st->dir);
    return false;
  }
#if !defined(_WIN32)
  (void)chmod(st->dir, 0700);
#endif
  os_mutex_init(&st->mu);
  snprintf(path, sizeof path, "%s/button.key", st->dir);
  if (os_read_file(path, &key, &key_len) == CORE_OK && key_len == sizeof st->key) {
    memcpy(st->key, key, sizeof st->key);
  } else {
    os_random_bytes(st->key, sizeof st->key);
    if (!write_private(st->dir, "button.key", st->key, sizeof st->key)) {
      free(key);
      snprintf(err, n, "cannot write %s", path);
      return false;
    }
  }
  if (key) memset(key, 0, key_len);
  free(key);
  doc = read_doc(st->dir, "state.xcdn");
  obj = first_object(doc);
  if (obj) {
    xcdn_node_t *bn = xcdn_object_get(obj, "bindings");
    st->owner_id = int_field(obj, "owner_id");
    st->owner_chat = int_field(obj, "owner_chat");
    st->routine_thread = int_field(obj, "routine_thread");
    if (bn && bn->value && bn->value->type == XCDN_VAL_ARRAY)
      for (size_t i = 0; i < xcdn_array_len(bn->value) && g_bind_n < BINDINGS_MAX; i++) {
        xcdn_node_t *e = xcdn_array_get(bn->value, i);
        xcdn_node_t *s = e && e->value ? xcdn_object_get(e->value, "star") : NULL;
        if (!s || !s->value || s->value->type != XCDN_VAL_STRING ||
            !core_slug_valid(xcdn_value_as_string(s->value)))
          continue;
        g_bind[g_bind_n].chat = int_field(e->value, "chat");
        g_bind[g_bind_n].thread = int_field(e->value, "thread");
        snprintf(g_bind[g_bind_n].star, sizeof g_bind[g_bind_n].star, "%s",
                 xcdn_value_as_string(s->value));
        xcdn_node_t *p = xcdn_object_get(e->value, "project");
        g_bind[g_bind_n].project[0] = 0;
        if (p && p->value && p->value->type == XCDN_VAL_STRING &&
            core_slug_valid(xcdn_value_as_string(p->value)) &&
            strlen(xcdn_value_as_string(p->value)) < sizeof g_bind[g_bind_n].project)
          snprintf(g_bind[g_bind_n].project, sizeof g_bind[g_bind_n].project, "%s",
                   xcdn_value_as_string(p->value));
        g_bind_n++;
      }
    load_usage(obj, "today", &g_today);
    load_usage(obj, "month", &g_month);
  }
  if (doc) xcdn_document_free(doc);
  return true;
}

void serve_state_close(serve_state *st) {
  memset(st->key, 0, sizeof st->key);
  os_mutex_destroy(&st->mu);
}

static void code_hash(const char *code, char hex[65]) {
  uint8_t h[32];
  core_sha256(code, strlen(code), h);
  core_sha256_hex(h, 32, hex);
}

bool serve_pairing_create(serve_state *st, char code[16], char *err, size_t n) {
  uint8_t r[8];
  char hex[65], body[160];
  os_random_bytes(r, sizeof r);
  uint64_t v = 0;
  for (size_t i = 0; i < sizeof r; i++) v = v << 8 | r[i];
  snprintf(code, 16, "%08llu", (unsigned long long)(v % 100000000ULL));
  code_hash(code, hex);
  snprintf(body, sizeof body, "#serve_pairing { code_sha256: \"%s\", expires_at: %lld }\n",
           hex, (long long)(os_now_unix() + 600));
  if (!write_private(st->dir, "pairing.xcdn", body, strlen(body))) {
    snprintf(err, n, "cannot write %s/pairing.xcdn", st->dir);
    return false;
  }
  return true;
}

bool serve_pairing_claim(serve_state *st, const char *code, int64_t user_id,
                         int64_t chat_id) {
  char want[65], path[1200];
  xcdn_document_t *doc;
  const xcdn_value_t *obj;
  xcdn_node_t *h;
  bool ok = false;
  if (!code || strlen(code) != 8 || user_id <= 0) return false;
  os_mutex_lock(&st->mu);
  doc = read_doc(st->dir, "pairing.xcdn");
  obj = first_object(doc);
  h = obj ? xcdn_object_get(obj, "code_sha256") : NULL;
  code_hash(code, want);
  if (h && h->value && h->value->type == XCDN_VAL_STRING &&
      strlen(xcdn_value_as_string(h->value)) == 64 &&
      core_bytes_equal_ct(xcdn_value_as_string(h->value), want, 64) &&
      int_field(obj, "expires_at") >= os_now_unix()) {
    st->owner_id = user_id;
    st->owner_chat = chat_id;
    ok = save_locked(st);
    snprintf(path, sizeof path, "%s/pairing.xcdn", st->dir);
    os_remove_file(path); /* one use */
  }
  if (doc) xcdn_document_free(doc);
  os_mutex_unlock(&st->mu);
  return ok;
}

bool serve_binding_get(serve_state *st, int64_t chat, int64_t thread, char star[33]) {
  bool found = false;
  os_mutex_lock(&st->mu);
  for (size_t i = 0; i < g_bind_n && !found; i++)
    if (g_bind[i].chat == chat && g_bind[i].thread == thread) {
      memcpy(star, g_bind[i].star, 33);
      found = true;
    }
  os_mutex_unlock(&st->mu);
  return found;
}

bool serve_binding_set(serve_state *st, int64_t chat, int64_t thread, const char *star) {
  size_t i;
  bool ok;
  if (!core_slug_valid(star)) return false;
  os_mutex_lock(&st->mu);
  for (i = 0; i < g_bind_n; i++)
    if (g_bind[i].chat == chat && g_bind[i].thread == thread) break;
  if (i == g_bind_n && g_bind_n == BINDINGS_MAX) {
    os_mutex_unlock(&st->mu);
    return false;
  }
  if (i == g_bind_n || strcmp(g_bind[i].star, star) != 0) g_bind[i].project[0] = 0;
  g_bind[i].chat = chat;
  g_bind[i].thread = thread;
  snprintf(g_bind[i].star, sizeof g_bind[i].star, "%s", star);
  if (i == g_bind_n) g_bind_n++;
  ok = save_locked(st);
  os_mutex_unlock(&st->mu);
  return ok;
}

bool serve_binding_project(serve_state *st, int64_t chat, int64_t thread,
                           const char *star, char project[41]) {
  project[0] = 0;
  os_mutex_lock(&st->mu);
  for (size_t i = 0; i < g_bind_n; i++)
    if (g_bind[i].chat == chat && g_bind[i].thread == thread &&
        !strcmp(g_bind[i].star, star)) {
      memcpy(project, g_bind[i].project, 41);
      break;
    }
  os_mutex_unlock(&st->mu);
  return project[0] != 0;
}

bool serve_binding_set_project(serve_state *st, int64_t chat, int64_t thread,
                               const char *star, const char *project) {
  size_t i;
  bool ok;
  if (!core_slug_valid(star) || (project && *project && !core_slug_valid(project)) ||
      (project && strlen(project) > 40))
    return false;
  os_mutex_lock(&st->mu);
  for (i = 0; i < g_bind_n; i++)
    if (g_bind[i].chat == chat && g_bind[i].thread == thread) break;
  if (i == g_bind_n && g_bind_n == BINDINGS_MAX) {
    os_mutex_unlock(&st->mu);
    return false;
  }
  g_bind[i].chat = chat;
  g_bind[i].thread = thread;
  snprintf(g_bind[i].star, sizeof g_bind[i].star, "%s", star);
  snprintf(g_bind[i].project, sizeof g_bind[i].project, "%s", project ? project : "");
  if (i == g_bind_n) g_bind_n++;
  ok = save_locked(st);
  os_mutex_unlock(&st->mu);
  return ok;
}

bool serve_binding_project_thread(serve_state *st, const char *star, const char *project,
                                  int64_t *chat, int64_t *thread) {
  bool found = false;
  os_mutex_lock(&st->mu);
  for (size_t i = 0; i < g_bind_n && !found; i++)
    if (!strcmp(g_bind[i].star, star) && !strcmp(g_bind[i].project, project)) {
      *chat = g_bind[i].chat;
      *thread = g_bind[i].thread;
      found = true;
    }
  os_mutex_unlock(&st->mu);
  return found;
}

void serve_binding_drop_project(serve_state *st, const char *star, const char *project) {
  bool changed = false;
  os_mutex_lock(&st->mu);
  for (size_t i = 0; i < g_bind_n; i++)
    if (!strcmp(g_bind[i].star, star) && !strcmp(g_bind[i].project, project)) {
      g_bind[i].project[0] = 0;
      changed = true;
    }
  if (changed) (void)save_locked(st);
  os_mutex_unlock(&st->mu);
}

/* ── button tokens ────────────────────────────────────────────────────── */

static void token_mac(const serve_state *st, char kind, const char *id, char choice,
                      int64_t chat, char hex[17]) {
  char msg[160];
  uint8_t mac[32];
  snprintf(msg, sizeof msg, "asterism-button-v1|%c|%s|%c|%lld", kind, id, choice,
           (long long)chat);
  core_hmac_sha256(st->key, sizeof st->key, msg, strlen(msg), mac);
  core_sha256_hex(mac, 8, hex);
}

bool serve_token_make(const serve_state *st, char kind, const char *id, char choice,
                      int64_t chat, char *out, size_t size) {
  char mac[17];
  if (!id || strlen(id) > 39 || strchr(id, ':')) return false;
  token_mac(st, kind, id, choice, chat, mac);
  return snprintf(out, size, "%c:%s:%c:%s", kind, id, choice, mac) < (int)size &&
         strlen(out) <= 64;
}

bool serve_token_check(const serve_state *st, const char *token, int64_t chat,
                       char *kind, char id[40], char *choice) {
  const char *a, *b;
  char want[17];
  size_t idn;
  if (!token || strlen(token) < 7 || token[1] != ':') return false;
  a = token + 2;
  b = strchr(a, ':');
  if (!b || (idn = (size_t)(b - a)) == 0 || idn > 39 || b[1] == 0 || b[2] != ':' ||
      strlen(b + 3) != 16)
    return false;
  memcpy(id, a, idn);
  id[idn] = 0;
  token_mac(st, token[0], id, b[1], chat, want);
  if (!core_bytes_equal_ct(want, b + 3, 16)) return false;
  *kind = token[0];
  *choice = b[1];
  return true;
}

/* ── usage ────────────────────────────────────────────────────────────── */

void serve_usage_note(serve_state *st, bool paid, size_t delegates, bool routine) {
  int64_t day = os_now_unix() / 86400, month = day / 30;
  os_mutex_lock(&st->mu);
  if (g_today.day != day) {
    memset(&g_today, 0, sizeof g_today);
    g_today.day = day;
  }
  if (g_month.day != month) {
    memset(&g_month, 0, sizeof g_month);
    g_month.day = month;
  }
  serve_usage *u[2] = {&g_today, &g_month};
  for (int i = 0; i < 2; i++) {
    u[i]->turns++;
    if (paid || delegates) u[i]->paid_turns++;
    else u[i]->local_turns++;
    u[i]->delegate_calls += (int64_t)delegates;
    if (routine) u[i]->routine_runs++;
  }
  (void)save_locked(st);
  os_mutex_unlock(&st->mu);
}

bool serve_usage_get(serve_state *st, serve_usage *today, serve_usage *month) {
  int64_t day = os_now_unix() / 86400;
  os_mutex_lock(&st->mu);
  *today = g_today.day == day ? g_today : (serve_usage){.day = day};
  *month = g_month.day == day / 30 ? g_month : (serve_usage){.day = day / 30};
  os_mutex_unlock(&st->mu);
  return true;
}
