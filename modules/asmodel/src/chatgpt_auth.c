/* ChatGPT plan credentials: one owner-only JSON record per auth file, the
 * stable host id, serialized refresh with rotation persisted before use, and
 * the local status/logout/model-list helpers. Tokens never reach diagnostics. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "chatgpt.h"
#include "chatgpt_auth.h"
#include "http.h"
#include "asmodel_json.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef asmodel_json_value json;

#define CG_REFRESH_MARGIN_S 300 /* refresh this long before access expiry */
#define CG_EXPIRY_SKEW_S 30
#define CG_LOCK_WAIT_MS 120000
#define CG_HTTP_TIMEOUT_MS 30000

static void say(char *error, size_t size, const char *fmt, const char *a, const char *b) {
  if (error && size) snprintf(error, size, fmt, a ? a : "", b ? b : "");
}

/* ── process-wide refresh mutex ─────────────────────────────────────────── */

#if defined(ASTERISM_NO_THREADS)
static int proc_trylock(void) { return 1; }
static void proc_unlock(void) {}
#elif defined(_WIN32)
static SRWLOCK g_refresh = SRWLOCK_INIT;
static int proc_trylock(void) { return TryAcquireSRWLockExclusive(&g_refresh) != 0; }
static void proc_unlock(void) { ReleaseSRWLockExclusive(&g_refresh); }
#else
static pthread_mutex_t g_refresh = PTHREAD_MUTEX_INITIALIZER;
static int proc_trylock(void) { return pthread_mutex_trylock(&g_refresh) == 0; }
static void proc_unlock(void) { pthread_mutex_unlock(&g_refresh); }
#endif

/* ── small helpers ──────────────────────────────────────────────────────── */

static const json *get(const json *o, const char *k) { return asmodel_json_object_get(o, k); }

static const char *jstr(const json *v) {
  const char *s = asmodel_json_string_value(v);
  return s && strlen(s) == asmodel_json_string_length(v) ? s : NULL;
}

static char *dupn(const char *s) { return s ? core_strdup(s) : NULL; }

static void wipe(char *s) {
  if (s) {
    volatile char *p = s;
    while (*p) *p++ = 0;
  }
  free(s);
}

char *cg_path_with(const char *path, const char *suffix) {
  size_t a = strlen(path), b = strlen(suffix);
  char *p = (char *)malloc(a + b + 1);
  if (!p) return NULL;
  memcpy(p, path, a);
  memcpy(p + a, suffix, b + 1);
  return p;
}

static core_err ensure_parent(const char *path) {
  const char *slash = strrchr(path, '/');
#ifdef _WIN32
  const char *back = strrchr(path, '\\');
  if (!slash || (back && back > slash)) slash = back;
#endif
  if (!slash || slash == path) return CORE_OK;
  char *dir = core_strndup(path, (size_t)(slash - path));
  if (!dir) return CORE_ERR_NOMEM;
  core_err e = os_file_exists(dir) ? CORE_OK : os_mkdir_p(dir);
  free(dir);
  return e;
}

char *cg_base64url(const uint8_t *data, size_t n) {
  char *s = core_base64_encode(data, n);
  size_t j = 0;
  if (!s) return NULL;
  for (size_t i = 0; s[i]; i++) {
    if (s[i] == '=') break;
    s[j++] = s[i] == '+' ? '-' : s[i] == '/' ? '_' : s[i];
  }
  s[j] = 0;
  return s;
}

static int b64url_decode(const char *s, size_t n, uint8_t **out, size_t *out_n) {
  char *std = (char *)malloc(n + 4);
  int ok;
  size_t j = 0;
  if (!std) return 0;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c == '-') c = '+';
    else if (c == '_') c = '/';
    else if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
      free(std);
      return 0;
    }
    std[j++] = c;
  }
  while (j % 4) std[j++] = '=';
  std[j] = 0;
  ok = core_base64_decode(std, out, out_n);
  free(std);
  return ok;
}

char *cg_random_token(size_t bytes) {
  uint8_t buf[64];
  if (bytes > sizeof buf) return NULL;
  os_random_bytes(buf, bytes);
  char *t = cg_base64url(buf, bytes);
  memset(buf, 0, sizeof buf);
  return t;
}

int cg_has_scope(const char *scopes, const char *want) {
  size_t n = strlen(want);
  for (const char *p = scopes ? scopes : ""; *p;) {
    while (*p == ' ') p++;
    size_t k = strcspn(p, " ");
    if (k == n && !memcmp(p, want, n)) return 1;
    p += k;
  }
  return 0;
}

static core_err pct(core_buf *b, const char *s) {
  static const char hex[] = "0123456789ABCDEF";
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    core_err e;
    if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
        *p == '-' || *p == '.' || *p == '_' || *p == '~')
      e = core_buf_appendc(b, (char)*p);
    else {
      char esc[3] = {'%', hex[*p >> 4], hex[*p & 15]};
      e = core_buf_append(b, esc, 3);
    }
    if (e) return e;
  }
  return CORE_OK;
}

core_err cg_form_add(core_buf *b, const char *key, const char *value) {
  core_err e = b->len ? core_buf_appendc(b, '&') : CORE_OK;
  if (!e) e = pct(b, key);
  if (!e) e = core_buf_appendc(b, '=');
  if (!e) e = pct(b, value ? value : "");
  return e;
}

/* ── record ─────────────────────────────────────────────────────────────── */

void cg_record_free(cg_record *r) {
  if (!r) return;
  free(r->issuer); free(r->resource); free(r->subject); free(r->email); free(r->plan);
  free(r->client_id); free(r->host_id); free(r->token_type); free(r->scope);
  wipe(r->id_token); wipe(r->access_token); wipe(r->refresh_token);
  memset(r, 0, sizeof *r);
}

void cg_tokens_free(cg_tokens *t) {
  if (!t) return;
  wipe(t->access_token); wipe(t->refresh_token); wipe(t->id_token);
  free(t->token_type); free(t->scope);
  memset(t, 0, sizeof *t);
}

void cg_identity_free(cg_identity *id) {
  if (!id) return;
  free(id->subject); free(id->email); free(id->plan);
  memset(id, 0, sizeof *id);
}

void cg_record_clear_tokens(cg_record *r) {
  wipe(r->access_token); wipe(r->refresh_token); wipe(r->id_token);
  r->access_token = r->refresh_token = r->id_token = NULL;
  r->expires_at = r->earliest_refresh_at = 0;
}

static int read_time(const json *v, int64_t *out) {
  if (!v) return 0;
  if (asmodel_json_is_int(v)) { *out = asmodel_json_int_value(v); return 0; }
  core_time t;
  if (jstr(v) && core_time_parse_rfc3339(jstr(v), &t)) { *out = t; return 0; }
  return -1;
}

asmodel_err cg_record_read(const char *path, cg_record *r) {
  char *text = NULL;
  size_t n = 0;
  json *root = NULL;
  asmodel_err rc = ASMODEL_ERR_INVALID;
  memset(r, 0, sizeof *r);
  core_err e = os_read_file(path, &text, &n);
  if (e == CORE_ERR_NOT_FOUND) return ASMODEL_ERR_NOT_FOUND;
  if (e != CORE_OK) return e == CORE_ERR_NOMEM ? ASMODEL_ERR_NOMEM : ASMODEL_ERR_INVALID;
#ifndef _WIN32
  {
    struct stat st; /* A copied record may arrive group-readable: tighten it. */
    if (stat(path, &st) == 0 && (st.st_mode & 077)) (void)chmod(path, 0600);
  }
#endif
  if (asmodel_json_parse(text, n, &root) || asmodel_json_typeof(root) != ASMODEL_JSON_OBJECT)
    goto done;
  {
    struct { const char *key; char **slot; } fields[] = {
      {"issuer", &r->issuer}, {"resource", &r->resource}, {"subject", &r->subject},
      {"email", &r->email}, {"plan", &r->plan}, {"client_id", &r->client_id},
      {"ext_agent_host_id", &r->host_id}, {"id_token", &r->id_token},
      {"access_token", &r->access_token}, {"refresh_token", &r->refresh_token},
      {"token_type", &r->token_type}, {"scope", &r->scope}};
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
      const json *v = get(root, fields[i].key);
      if (!v || asmodel_json_typeof(v) == ASMODEL_JSON_NULL) continue;
      if (!jstr(v)) goto done;
      if (!jstr(v)[0]) continue;
      if (!(*fields[i].slot = core_strdup(jstr(v)))) { rc = ASMODEL_ERR_NOMEM; goto done; }
    }
  }
  {
    const json *scopes = get(root, "scopes");
    if (asmodel_json_typeof(scopes) == ASMODEL_JSON_ARRAY) {
      core_buf b;
      core_buf_init(&b);
      for (size_t i = 0; i < asmodel_json_array_len(scopes); i++) {
        const char *s = jstr(asmodel_json_array_at(scopes, i));
        if (!s || strchr(s, ' ')) { core_buf_free(&b); goto done; }
        if ((b.len && core_buf_appendc(&b, ' ')) || core_buf_appends(&b, s)) {
          core_buf_free(&b); rc = ASMODEL_ERR_NOMEM; goto done;
        }
      }
      free(r->scope);
      r->scope = core_buf_detach(&b);
    }
  }
  {
    int64_t expires_in = 0;
    if (read_time(get(root, "expires_at"), &r->expires_at) ||
        read_time(get(root, "earliest_refresh_at"), &r->earliest_refresh_at) ||
        read_time(get(root, "saved_at"), &r->saved_at) ||
        read_time(get(root, "expires_in"), &expires_in)) goto done;
    /* The documented record keeps expires_in + saved_at only. */
    if (!r->expires_at && expires_in > 0 && r->saved_at > 0) r->expires_at = r->saved_at + expires_in;
  }
  rc = ASMODEL_OK;
done:
  asmodel_json_free(root);
  if (text) { memset(text, 0, n); free(text); }
  if (rc != ASMODEL_OK) cg_record_free(r);
  return rc;
}

static asmodel_err write_private(const char *path, const char *data, char *error, size_t size) {
  size_t n = strlen(data);
  char *tmp = cg_path_with(path, ".tmp");
  asmodel_err rc = ASMODEL_ERR_BACKEND;
  if (!tmp) return ASMODEL_ERR_NOMEM;
  if (ensure_parent(path) != CORE_OK) {
    say(error, size, "cannot create the directory of %s%s", path, NULL);
    free(tmp);
    return ASMODEL_ERR_BACKEND;
  }
#ifdef _WIN32
  /* Windows: the file inherits the per-user profile ACL of its directory. */
  if (os_write_file(tmp, data, n) != CORE_OK) goto fail;
#else
  {
    int fd;
    size_t off = 0;
    (void)unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) goto fail;
    if (fchmod(fd, 0600) != 0) { close(fd); goto fail; }
    while (off < n) {
      ssize_t w = write(fd, data + off, n - off);
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) { close(fd); goto fail; }
      off += (size_t)w;
    }
    if (fsync(fd) != 0) { close(fd); goto fail; }
    if (close(fd) != 0) goto fail;
  }
#endif
  if (os_file_replace(tmp, path) != CORE_OK) goto fail;
  free(tmp);
  return ASMODEL_OK;
fail:
  (void)os_remove_file(tmp);
  say(error, size, "cannot write credential file %s%s", path, NULL);
  free(tmp);
  return rc;
}

static int set_str(json *o, const char *key, const char *value) {
  if (!value || !value[0]) return 0;
  return asmodel_json_object_set(o, key, asmodel_json_string(value));
}

asmodel_err cg_record_write(const char *path, const cg_record *r, char *error, size_t size) {
  json *o = asmodel_json_object(), *scopes = NULL;
  int bad = !o;
  char *text = NULL;
  asmodel_err rc;
  char saved[CORE_RFC3339_SIZE];
  bad |= asmodel_json_object_set(o, "version", asmodel_json_int(1));
  bad |= set_str(o, "issuer", r->issuer) | set_str(o, "resource", r->resource) |
         set_str(o, "subject", r->subject) | set_str(o, "email", r->email) |
         set_str(o, "plan", r->plan) | set_str(o, "client_id", r->client_id) |
         set_str(o, "ext_agent_host_id", r->host_id) | set_str(o, "id_token", r->id_token) |
         set_str(o, "access_token", r->access_token) |
         set_str(o, "refresh_token", r->refresh_token) | set_str(o, "token_type", r->token_type);
  if (r->scope) {
    scopes = asmodel_json_array();
    bad |= !scopes;
    for (const char *p = r->scope; scopes && *p;) {
      while (*p == ' ') p++;
      size_t k = strcspn(p, " ");
      if (k) {
        char *one = core_strndup(p, k);
        bad |= asmodel_json_array_push(scopes, one ? asmodel_json_string(one) : NULL);
        free(one);
      }
      p += k;
    }
    bad |= asmodel_json_object_set(o, "scopes", scopes);
  }
  if (r->expires_at) {
    bad |= asmodel_json_object_set(o, "expires_at", asmodel_json_int(r->expires_at));
    if (r->saved_at && r->expires_at > r->saved_at)
      bad |= asmodel_json_object_set(o, "expires_in", asmodel_json_int(r->expires_at - r->saved_at));
  }
  if (r->earliest_refresh_at)
    bad |= asmodel_json_object_set(o, "earliest_refresh_at", asmodel_json_int(r->earliest_refresh_at));
  if (r->saved_at) {
    core_time_format_rfc3339(r->saved_at, saved);
    bad |= set_str(o, "saved_at", saved);
  }
  if (!bad) text = asmodel_json_write(o, 1);
  asmodel_json_free(o);
  if (!text) return ASMODEL_ERR_NOMEM;
  rc = write_private(path, text, error, size);
  wipe(text);
  return rc;
}

asmodel_err cg_record_apply(cg_record *r, cg_tokens *t, const cg_identity *id) {
  int64_t now = os_now_unix();
  wipe(r->access_token);
  r->access_token = t->access_token; t->access_token = NULL;
  if (t->refresh_token) { wipe(r->refresh_token); r->refresh_token = t->refresh_token; t->refresh_token = NULL; }
  if (t->id_token) { wipe(r->id_token); r->id_token = t->id_token; t->id_token = NULL; }
  if (t->token_type) { free(r->token_type); r->token_type = t->token_type; t->token_type = NULL; }
  if (t->scope) { free(r->scope); r->scope = t->scope; t->scope = NULL; }
  r->expires_at = t->expires_in > 0 ? now + t->expires_in : 0;
  r->earliest_refresh_at = t->earliest_refresh_at;
  r->saved_at = now;
  if (id) {
    char *sub = dupn(id->subject), *email = dupn(id->email), *plan = dupn(id->plan);
    if ((id->subject && !sub) || (id->email && !email) || (id->plan && !plan)) {
      free(sub); free(email); free(plan);
      return ASMODEL_ERR_NOMEM;
    }
    free(r->subject); r->subject = sub;
    if (email) { free(r->email); r->email = email; }
    if (plan) { free(r->plan); r->plan = plan; }
  }
  return ASMODEL_OK;
}

/* ── locks ──────────────────────────────────────────────────────────────── */

asmodel_err cg_lock_acquire(const char *auth_file, int64_t timeout_ms, volatile int *cancel,
                            cg_lock *out, char *error, size_t size) {
  int64_t start = os_monotonic_ms(), limit = timeout_ms > 0 ? timeout_ms : CG_LOCK_WAIT_MS;
  char *path = cg_path_with(auth_file, ".lock");
  memset(out, 0, sizeof *out);
  if (!path) return ASMODEL_ERR_NOMEM;
  (void)ensure_parent(path);
  while (!proc_trylock()) {
    if (cancel && *cancel) { free(path); return ASMODEL_ERR_CANCELLED; }
    if (os_monotonic_ms() - start >= limit) {
      say(error, size, "timed out waiting for the ChatGPT credential lock%s%s", NULL, NULL);
      free(path);
      return ASMODEL_ERR_TIMEOUT;
    }
    os_sleep_ms(5);
  }
  for (;;) {
    out->file = os_store_lock(path);
    if (out->file) break;
    if ((cancel && *cancel) || os_monotonic_ms() - start >= limit) {
      proc_unlock();
      if (cancel && *cancel) { free(path); return ASMODEL_ERR_CANCELLED; }
      say(error, size, "cannot lock %s (held by another process?)%s", path, NULL);
      free(path);
      return ASMODEL_ERR_TIMEOUT;
    }
    os_sleep_ms(10);
  }
  free(path);
  out->held = 1;
  return ASMODEL_OK;
}

void cg_lock_release(cg_lock *l) {
  if (!l || !l->held) return;
  fclose(l->file);
  proc_unlock();
  memset(l, 0, sizeof *l);
}

/* ── host id ────────────────────────────────────────────────────────────── */

static int host_id_valid(const char *s) {
  size_t n = strlen(s);
  if (n < 9 || n >= 160) return 0;
  for (size_t i = 0; i < n; i++)
    if ((unsigned char)s[i] <= 0x20 || (unsigned char)s[i] >= 0x7f) return 0;
  if (core_str_has_prefix(s, "urn:uuid:")) return core_uuid_valid(s + 9);
  return core_str_has_prefix(s, "urn:ietf:params:oauth:jwk-thumbprint:") ||
         core_str_has_prefix(s, "did:key:");
}

asmodel_err cg_host_id(const char *host_file, int create, char **out, char *error, size_t size) {
  char *text = NULL;
  core_err e = os_read_file(host_file, &text, NULL);
  *out = NULL;
  if (e == CORE_OK) {
    char *id = core_str_trim(text);
    if (!host_id_valid(id)) {
      say(error, size, "invalid ext_agent_host_id in %s%s", host_file, NULL);
      free(text);
      return ASMODEL_ERR_INVALID;
    }
    *out = core_strdup(id);
    free(text);
    return *out ? ASMODEL_OK : ASMODEL_ERR_NOMEM;
  }
  if (e != CORE_ERR_NOT_FOUND || !create) {
    if (e != CORE_ERR_NOT_FOUND) say(error, size, "cannot read %s%s", host_file, NULL);
    return e == CORE_ERR_NOT_FOUND ? ASMODEL_ERR_NOT_FOUND : ASMODEL_ERR_BACKEND;
  }
  {
    char uuid[37], line[64];
    asmodel_err rc;
    core_uuid_v4(uuid);
    snprintf(line, sizeof line, "urn:uuid:%s\n", uuid);
    rc = write_private(host_file, line, error, size);
    if (rc != ASMODEL_OK) return rc;
    line[strlen(line) - 1] = 0;
    *out = core_strdup(line);
    return *out ? ASMODEL_OK : ASMODEL_ERR_NOMEM;
  }
}

/* ── token endpoint ─────────────────────────────────────────────────────── */

static char *endpoint(const char *base, const char *suffix) {
  size_t n = strlen(base);
  while (n && base[n - 1] == '/') n--;
  char *head = core_strndup(base, n);
  char *url = head ? cg_path_with(head, suffix) : NULL;
  free(head);
  return url;
}

static const char *const TERMINAL_CODES[] = {
  "invalid_grant", "invalid_refresh_token", "token_expired", "refresh_token_expired",
  "refresh_token_invalidated", "refresh_token_reused", "invalid_client", NULL};

static void oauth_error(const json *root, char *code, size_t code_size, char *desc, size_t desc_size) {
  const json *e = get(root, "error");
  code[0] = desc[0] = 0;
  if (jstr(e)) {
    asmodel_chatgpt_sanitize(code, code_size, jstr(e));
    asmodel_chatgpt_sanitize(desc, desc_size, jstr(get(root, "error_description")));
  } else if (asmodel_json_typeof(e) == ASMODEL_JSON_OBJECT) {
    const char *c = jstr(get(e, "code"));
    asmodel_chatgpt_sanitize(code, code_size, c ? c : jstr(get(e, "type")));
    asmodel_chatgpt_sanitize(desc, desc_size, jstr(get(e, "message")));
  } else if (jstr(get(root, "detail"))) {
    asmodel_chatgpt_sanitize(desc, desc_size, jstr(get(root, "detail")));
  }
}

asmodel_err cg_token_request(const char *issuer, const char *form, int64_t timeout_ms,
                             volatile int *cancel, cg_tokens *out, char *err_code,
                             size_t err_code_size, int *terminal, char *error, size_t size) {
  static const char *const headers[] = {
    "Content-Type: application/x-www-form-urlencoded", "Accept: application/json", NULL};
  asmodel_http_request rq;
  asmodel_http_response rs;
  json *root = NULL;
  char transport[256] = {0};
  asmodel_err rc = ASMODEL_ERR_BACKEND;
  char *url = endpoint(issuer, CG_TOKEN_PATH);
  memset(out, 0, sizeof *out);
  if (terminal) *terminal = 0;
  if (err_code && err_code_size) err_code[0] = 0;
  if (!url) return ASMODEL_ERR_NOMEM;
  if (!asmodel_http_url_allowed(url)) {
    say(error, size, "refusing token endpoint %s: https required%s", url, NULL);
    free(url);
    return ASMODEL_ERR_INVALID;
  }
  memset(&rq, 0, sizeof rq);
  rq.method = "POST"; rq.url = url; rq.headers = headers; rq.body = form;
  rq.cancel = cancel; rq.timeout_ms = timeout_ms > 0 ? timeout_ms : CG_HTTP_TIMEOUT_MS;
  rq.max_body = 1024 * 1024;
  int h = asmodel_http_perform(&rq, &rs, transport, sizeof transport);
  free(url);
  if (h == ASMODEL_HTTP_CANCELLED) { rc = ASMODEL_ERR_CANCELLED; goto done; }
  if (h == ASMODEL_HTTP_TIMEOUT) {
    say(error, size, "ChatGPT token endpoint: %s%s", transport, NULL);
    rc = ASMODEL_ERR_TIMEOUT;
    goto done;
  }
  if (h != ASMODEL_HTTP_OK) {
    say(error, size, "ChatGPT token endpoint unreachable: %s%s", transport, NULL);
    rc = h == ASMODEL_HTTP_UNAVAILABLE ? ASMODEL_ERR_UNSUPPORTED : ASMODEL_ERR_BACKEND;
    goto done;
  }
  if (rs.body) (void)asmodel_json_parse(rs.body, rs.len, &root);
  if (rs.status == 200) {
    const json *exp = get(root, "expires_in");
    if (asmodel_json_typeof(root) != ASMODEL_JSON_OBJECT || !jstr(get(root, "access_token")) ||
        !jstr(get(root, "access_token"))[0] || (exp && !asmodel_json_is_int(exp))) {
      say(error, size, "malformed ChatGPT token response%s%s", NULL, NULL);
      goto done;
    }
    out->access_token = core_strdup(jstr(get(root, "access_token")));
    out->refresh_token = dupn(jstr(get(root, "refresh_token")));
    out->id_token = dupn(jstr(get(root, "id_token")));
    out->token_type = dupn(jstr(get(root, "token_type")));
    out->scope = dupn(jstr(get(root, "scope")));
    out->expires_in = exp ? asmodel_json_int_value(exp) : 0;
    (void)read_time(get(root, "earliest_refresh_at"), &out->earliest_refresh_at);
    if (!out->access_token) { rc = ASMODEL_ERR_NOMEM; goto done; }
    rc = ASMODEL_OK;
  } else {
    char code[64], desc[160], msg[400];
    oauth_error(root, code, sizeof code, desc, sizeof desc);
    for (size_t i = 0; TERMINAL_CODES[i]; i++)
      if (!strcmp(code, TERMINAL_CODES[i]) && terminal) *terminal = 1;
    if (err_code && err_code_size) snprintf(err_code, err_code_size, "%s", code);
    snprintf(msg, sizeof msg, "ChatGPT token endpoint HTTP %ld%s%s%s%s%s", rs.status,
             code[0] ? ": " : "", code, desc[0] ? " (" : "", desc, desc[0] ? ")" : "");
    if (rs.request_id[0] && strlen(msg) + strlen(rs.request_id) + 16 < sizeof msg)
      snprintf(msg + strlen(msg), sizeof msg - strlen(msg), " [request %s]", rs.request_id);
    say(error, size, "%s%s", msg, NULL);
    rc = terminal && *terminal ? ASMODEL_ERR_AUTH : ASMODEL_ERR_BACKEND;
  }
done:
  asmodel_json_free(root);
  if (rs.body) memset(rs.body, 0, rs.len);
  asmodel_http_response_free(&rs);
  if (rc != ASMODEL_OK) cg_tokens_free(out);
  return rc;
}

/* ── ID token ───────────────────────────────────────────────────────────── */

static json *jwt_part(const char *s, size_t n) {
  uint8_t *raw = NULL;
  size_t len = 0;
  json *v = NULL;
  if (!b64url_decode(s, n, &raw, &len)) return NULL;
  if (asmodel_json_parse((const char *)raw, len, &v) || asmodel_json_typeof(v) != ASMODEL_JSON_OBJECT) {
    asmodel_json_free(v);
    v = NULL;
  }
  free(raw);
  return v;
}

int cg_same_issuer(const char *a, const char *b) {
  size_t n = strlen(a), m = strlen(b);
  while (n && a[n - 1] == '/') n--;
  while (m && b[m - 1] == '/') m--;
  return n == m && !memcmp(a, b, n);
}

char *cg_jwt_claim(const char *jwt, const char *claim) {
  const char *d1, *d2;
  json *claims;
  char *value;
  if (!jwt || !(d1 = strchr(jwt, '.')) || !(d2 = strchr(d1 + 1, '.'))) return NULL;
  claims = jwt_part(d1 + 1, (size_t)(d2 - d1 - 1));
  value = dupn(jstr(get(claims, claim)));
  asmodel_json_free(claims);
  return value;
}

asmodel_err cg_validate_id_token(const char *jwt, const char *issuer, const char *client_id,
                                 const char *expected_nonce, cg_identity *out,
                                 char *error, size_t size) {
  const char *d1, *d2;
  json *header = NULL, *claims = NULL;
  asmodel_err rc = ASMODEL_ERR_AUTH;
  const json *aud, *exp;
  int aud_ok = 0;
  memset(out, 0, sizeof *out);
  if (!jwt || !(d1 = strchr(jwt, '.')) || !(d2 = strchr(d1 + 1, '.'))) {
    say(error, size, "ChatGPT ID token is malformed%s%s", NULL, NULL);
    return ASMODEL_ERR_AUTH;
  }
  header = jwt_part(jwt, (size_t)(d1 - jwt));
  claims = jwt_part(d1 + 1, (size_t)(d2 - d1 - 1));
  if (!header || !claims || !jstr(get(header, "alg"))) {
    say(error, size, "ChatGPT ID token is malformed%s%s", NULL, NULL);
    goto done;
  }
  if (!jstr(get(claims, "iss")) || !cg_same_issuer(jstr(get(claims, "iss")), issuer)) {
    say(error, size, "ChatGPT ID token issuer mismatch (expected %s)%s", issuer, NULL);
    goto done;
  }
  aud = get(claims, "aud");
  if (jstr(aud)) aud_ok = !strcmp(jstr(aud), client_id);
  else for (size_t i = 0; i < asmodel_json_array_len(aud); i++)
    if (jstr(asmodel_json_array_at(aud, i)) && !strcmp(jstr(asmodel_json_array_at(aud, i)), client_id))
      aud_ok = 1;
  if (!aud_ok) {
    say(error, size, "ChatGPT ID token audience is not the issued client id%s%s", NULL, NULL);
    goto done;
  }
  exp = get(claims, "exp");
  if (!asmodel_json_is_int(exp) || asmodel_json_int_value(exp) + CG_EXPIRY_SKEW_S < os_now_unix()) {
    say(error, size, "ChatGPT ID token is expired%s%s", NULL, NULL);
    goto done;
  }
  if (expected_nonce) {
    const char *nonce = jstr(get(claims, "nonce"));
    if (!nonce || strlen(nonce) != strlen(expected_nonce) ||
        !core_bytes_equal_ct(nonce, expected_nonce, strlen(nonce))) {
      say(error, size, "ChatGPT ID token nonce mismatch%s%s", NULL, NULL);
      goto done;
    }
  }
  if (!jstr(get(claims, "sub")) || !jstr(get(claims, "sub"))[0]) {
    say(error, size, "ChatGPT ID token has no subject%s%s", NULL, NULL);
    goto done;
  }
  out->subject = core_strdup(jstr(get(claims, "sub")));
  out->email = dupn(jstr(get(claims, "email")));
  /* Not documented for this flow: shown when present, never relied upon. */
  out->plan = dupn(jstr(get(get(claims, "https://api.openai.com/auth"), "chatgpt_plan_type")));
  rc = out->subject ? ASMODEL_OK : ASMODEL_ERR_NOMEM;
done:
  asmodel_json_free(header);
  asmodel_json_free(claims);
  if (rc != ASMODEL_OK) cg_identity_free(out);
  return rc;
}

/* ── inference access ───────────────────────────────────────────────────── */

static asmodel_err refresh(cg_record *r, int64_t timeout_ms, volatile int *cancel, char *error,
                           size_t size) {
  core_buf form;
  cg_tokens t;
  cg_identity id;
  int terminal = 0;
  char code[64];
  asmodel_err rc;
  core_buf_init(&form);
  if (cg_form_add(&form, "grant_type", "refresh_token") ||
      cg_form_add(&form, "client_id", r->client_id) ||
      cg_form_add(&form, "refresh_token", r->refresh_token) ||
      cg_form_add(&form, "resource", r->resource ? r->resource : ASMODEL_CHATGPT_RESOURCE)) {
    core_buf_free(&form);
    return ASMODEL_ERR_NOMEM;
  }
  rc = cg_token_request(r->issuer ? r->issuer : ASMODEL_CHATGPT_ISSUER, form.data, timeout_ms,
                        cancel, &t, code, sizeof code, &terminal, error, size);
  if (form.data) memset(form.data, 0, form.len);
  core_buf_free(&form);
  if (rc != ASMODEL_OK) return rc;
  memset(&id, 0, sizeof id);
  /* A refreshed ID token is kept only when it validates (no nonce here). */
  if (t.id_token && cg_validate_id_token(t.id_token, r->issuer ? r->issuer : ASMODEL_CHATGPT_ISSUER,
                                         r->client_id, NULL, &id, NULL, 0) != ASMODEL_OK) {
    wipe(t.id_token);
    t.id_token = NULL;
  }
  if (id.subject && r->subject && strcmp(id.subject, r->subject)) {
    cg_identity_free(&id);
    cg_tokens_free(&t);
    say(error, size, "ChatGPT refresh returned a different account; %s%s", CG_LOGIN_HINT, NULL);
    return ASMODEL_ERR_AUTH;
  }
  rc = cg_record_apply(r, &t, id.subject ? &id : NULL);
  cg_identity_free(&id);
  cg_tokens_free(&t);
  return rc;
}

asmodel_err asmodel_chatgpt_access(const char *auth_file, const char *stale, int64_t timeout_ms,
                                   volatile int *cancel, char **out_token,
                                   char *error, size_t size) {
  cg_lock lock;
  cg_record r;
  asmodel_err rc;
  *out_token = NULL;
  if (!auth_file || !auth_file[0]) {
    say(error, size, "no ChatGPT auth_file configured%s%s", NULL, NULL);
    return ASMODEL_ERR_INVALID;
  }
  rc = cg_lock_acquire(auth_file, timeout_ms, cancel, &lock, error, size);
  if (rc != ASMODEL_OK) return rc;
  rc = cg_record_read(auth_file, &r);
  if (rc == ASMODEL_ERR_NOT_FOUND || (rc == ASMODEL_OK && !r.access_token && !r.refresh_token)) {
    say(error, size, "not logged in to ChatGPT: %s%s", CG_LOGIN_HINT, NULL);
    rc = ASMODEL_ERR_AUTH;
    goto done;
  }
  if (rc != ASMODEL_OK) {
    say(error, size, "unreadable ChatGPT credential file %s; %s", auth_file, CG_LOGIN_HINT);
    rc = rc == ASMODEL_ERR_NOMEM ? rc : ASMODEL_ERR_AUTH;
    goto done;
  }
  if (!r.client_id || !strcmp(r.client_id, CG_DYNAMIC_CLIENT)) {
    say(error, size, "ChatGPT registration incomplete: %s%s", CG_LOGIN_HINT, NULL);
    rc = ASMODEL_ERR_AUTH;
    goto done;
  }
  if (!cg_has_scope(r.scope, CG_PLAN_SCOPE)) {
    say(error, size, "ChatGPT plan usage is not enabled for this sign-in: %s and allow plan usage%s",
        CG_LOGIN_HINT, NULL);
    rc = ASMODEL_ERR_UNSUPPORTED;
    goto done;
  }
  {
    int64_t now = os_now_unix();
    int expired = r.expires_at && now >= r.expires_at - CG_EXPIRY_SKEW_S;
    int soon = r.expires_at && now >= r.expires_at - CG_REFRESH_MARGIN_S;
    int forced = stale && r.access_token && !strcmp(stale, r.access_token);
    if (soon && !expired && r.earliest_refresh_at && now < r.earliest_refresh_at) soon = 0;
    if (!r.access_token || soon || forced) {
      if (!r.refresh_token) {
        if (r.access_token && !expired && !forced) goto use;
        say(error, size, "ChatGPT login expired: %s%s", CG_LOGIN_HINT, NULL);
        rc = ASMODEL_ERR_AUTH;
        goto done;
      }
      char why[400] = {0};
      rc = refresh(&r, timeout_ms, cancel, why, sizeof why);
      if (rc == ASMODEL_OK) {
        /* Persist the rotated refresh token before anything uses the new
         * access token: the old refresh token is already spent. */
        rc = cg_record_write(auth_file, &r, error, size);
        if (rc != ASMODEL_OK) {
          char detail[256];
          snprintf(detail, sizeof detail, "%s", error && size ? error : "");
          say(error, size, "rotated ChatGPT credentials could not be saved (%s); %s", detail,
              CG_LOGIN_HINT);
          goto done;
        }
      } else if (rc == ASMODEL_ERR_AUTH) {
        cg_record_clear_tokens(&r);
        (void)cg_record_write(auth_file, &r, NULL, 0);
        say(error, size, "ChatGPT login expired (%s): %s", why, CG_LOGIN_HINT);
        goto done;
      } else if (r.access_token && !expired && !forced && rc != ASMODEL_ERR_CANCELLED) {
        rc = ASMODEL_OK; /* transient refresh failure: the current token is still valid */
      } else {
        say(error, size, "%s%s", why, NULL);
        goto done;
      }
    }
  }
use:
  *out_token = core_strdup(r.access_token);
  rc = *out_token ? ASMODEL_OK : ASMODEL_ERR_NOMEM;
done:
  cg_record_free(&r);
  cg_lock_release(&lock);
  return rc;
}

/* ── status / logout / models ───────────────────────────────────────────── */

asmodel_err asmodel_chatgpt_status_read(const char *auth_file, asmodel_chatgpt_status *out) {
  cg_record r;
  asmodel_err rc;
  char *host_file, *host = NULL;
  if (!auth_file || !out) return ASMODEL_ERR_INVALID;
  memset(out, 0, sizeof *out);
  rc = cg_record_read(auth_file, &r);
  if (rc != ASMODEL_OK) return rc;
  out->logged_in = r.access_token || r.refresh_token;
  out->plan_usage = cg_has_scope(r.scope, CG_PLAN_SCOPE);
  out->expires_at = r.expires_at;
  out->saved_at = r.saved_at;
  snprintf(out->email, sizeof out->email, "%s", r.email ? r.email : "");
  snprintf(out->subject, sizeof out->subject, "%s", r.subject ? r.subject : "");
  snprintf(out->client_id, sizeof out->client_id, "%s", r.client_id ? r.client_id : "");
  snprintf(out->plan, sizeof out->plan, "%s", r.plan ? r.plan : "");
  host_file = cg_path_with(auth_file, ".host");
  if (host_file && cg_host_id(host_file, 0, &host, NULL, 0) == ASMODEL_OK)
    snprintf(out->host_id, sizeof out->host_id, "%s", host);
  else if (r.host_id)
    snprintf(out->host_id, sizeof out->host_id, "%s", r.host_id);
  free(host); free(host_file);
  cg_record_free(&r);
  return ASMODEL_OK;
}

static int revoke_grant(const cg_record *r, char *error, size_t size) {
  static const char *const json_headers[] = {"Accept: application/json", NULL};
  static const char *const form_headers[] = {
    "Content-Type: application/x-www-form-urlencoded", "Accept: application/json", NULL};
  asmodel_http_request rq;
  asmodel_http_response rs;
  json *root = NULL;
  char transport[256] = {0}, *discovery, *endpoint_url = NULL;
  core_buf form;
  int ok = 0;
  discovery = endpoint(r->issuer ? r->issuer : ASMODEL_CHATGPT_ISSUER,
                       "/.well-known/openid-configuration");
  if (!discovery || !asmodel_http_url_allowed(discovery)) { free(discovery); return 0; }
  memset(&rq, 0, sizeof rq);
  rq.method = "GET"; rq.url = discovery; rq.headers = json_headers;
  rq.timeout_ms = 15000; rq.max_body = 256 * 1024;
  if (asmodel_http_perform(&rq, &rs, transport, sizeof transport) == ASMODEL_HTTP_OK &&
      rs.status == 200 && rs.body && !asmodel_json_parse(rs.body, rs.len, &root) &&
      jstr(get(root, "revocation_endpoint")) &&
      asmodel_http_url_allowed(jstr(get(root, "revocation_endpoint"))))
    endpoint_url = core_strdup(jstr(get(root, "revocation_endpoint")));
  else
    say(error, size, "revocation endpoint not discovered%s%s", transport[0] ? ": " : "", transport);
  asmodel_json_free(root);
  asmodel_http_response_free(&rs);
  free(discovery);
  if (!endpoint_url) return 0;
  core_buf_init(&form);
  if (!cg_form_add(&form, "token", r->refresh_token) &&
      !cg_form_add(&form, "token_type_hint", "refresh_token") &&
      !cg_form_add(&form, "client_id", r->client_id)) {
    memset(&rq, 0, sizeof rq);
    rq.method = "POST"; rq.url = endpoint_url; rq.headers = form_headers; rq.body = form.data;
    rq.timeout_ms = 15000; rq.max_body = 64 * 1024;
    int h = asmodel_http_perform(&rq, &rs, transport, sizeof transport);
    ok = h == ASMODEL_HTTP_OK && rs.status == 200;
    if (!ok) {
      char status[32];
      snprintf(status, sizeof status, "HTTP %ld", rs.status);
      say(error, size, "revocation not confirmed: %s%s", h == ASMODEL_HTTP_OK ? status : transport, NULL);
    }
    asmodel_http_response_free(&rs);
  }
  if (form.data) memset(form.data, 0, form.len);
  core_buf_free(&form);
  free(endpoint_url);
  return ok;
}

asmodel_err asmodel_chatgpt_logout(const char *auth_file, int *out_revoked, char *error, size_t size) {
  cg_lock lock;
  cg_record r;
  asmodel_err rc;
  if (out_revoked) *out_revoked = 0;
  if (error && size) error[0] = 0;
  if (!auth_file || !auth_file[0]) return ASMODEL_ERR_INVALID;
  rc = cg_lock_acquire(auth_file, 0, NULL, &lock, error, size);
  if (rc != ASMODEL_OK) return rc;
  rc = cg_record_read(auth_file, &r);
  if (rc == ASMODEL_ERR_NOT_FOUND) { rc = ASMODEL_OK; goto unlock; }
  if (rc != ASMODEL_OK) goto unlock;
  if (r.refresh_token && r.client_id) {
    int revoked = revoke_grant(&r, error, size);
    if (out_revoked) *out_revoked = revoked;
  }
  /* Keep the account/client mapping and host id for the next sign-in. */
  cg_record_clear_tokens(&r);
  rc = cg_record_write(auth_file, &r, error, size);
  cg_record_free(&r);
unlock:
  cg_lock_release(&lock);
  return rc;
}

static asmodel_err models_once(const char *url, const char *token, char **out, long *status,
                               char *error, size_t size) {
  core_buf auth;
  asmodel_http_request rq;
  asmodel_http_response rs;
  json *root = NULL, *list = NULL;
  char transport[256] = {0};
  asmodel_err rc = ASMODEL_ERR_BACKEND;
  const char *headers[3];
  core_buf_init(&auth);
  if (core_buf_appends(&auth, "Authorization: Bearer ") || core_buf_appends(&auth, token)) {
    core_buf_free(&auth);
    return ASMODEL_ERR_NOMEM;
  }
  headers[0] = auth.data; headers[1] = "Accept: application/json"; headers[2] = NULL;
  memset(&rq, 0, sizeof rq);
  rq.method = "GET"; rq.url = url; rq.headers = headers; rq.timeout_ms = 30000;
  int h = asmodel_http_perform(&rq, &rs, transport, sizeof transport);
  memset(auth.data, 0, auth.len);
  core_buf_free(&auth);
  *status = rs.status;
  if (h != ASMODEL_HTTP_OK) {
    say(error, size, "ChatGPT model list: %s%s", transport, NULL);
    rc = h == ASMODEL_HTTP_TIMEOUT ? ASMODEL_ERR_TIMEOUT : ASMODEL_ERR_BACKEND;
    goto done;
  }
  if (rs.status != 200) {
    char code[64], desc[160], st[24];
    if (rs.body) (void)asmodel_json_parse(rs.body, rs.len, &root);
    oauth_error(root, code, sizeof code, desc, sizeof desc);
    snprintf(st, sizeof st, "HTTP %ld", rs.status);
    say(error, size, "ChatGPT model list %s: %s", st, code[0] ? code : desc);
    rc = rs.status == 401 ? ASMODEL_ERR_AUTH : rs.status == 429 ? ASMODEL_ERR_QUOTA : ASMODEL_ERR_BACKEND;
    goto done;
  }
  if (!rs.body || asmodel_json_parse(rs.body, rs.len, &root) ||
      asmodel_json_typeof(get(root, "models")) != ASMODEL_JSON_ARRAY) {
    say(error, size, "malformed ChatGPT model list%s%s", NULL, NULL);
    goto done;
  }
  list = asmodel_json_array();
  if (!list) { rc = ASMODEL_ERR_NOMEM; goto done; }
  {
    const json *models = get(root, "models");
    int bad = 0;
    for (size_t i = 0; i < asmodel_json_array_len(models); i++) {
      const json *m = asmodel_json_array_at(models, i);
      const char *vis = jstr(get(m, "visibility")), *slug = jstr(get(m, "slug"));
      const char *name = jstr(get(m, "display_name"));
      if (!vis || strcmp(vis, "list") || !slug) continue;
      json *o = asmodel_json_object();
      bad |= !o || asmodel_json_object_set(o, "slug", asmodel_json_string(slug)) ||
             asmodel_json_object_set(o, "display_name", asmodel_json_string(name ? name : slug));
      bad |= asmodel_json_array_push(list, o);
    }
    *out = bad ? NULL : asmodel_json_write(list, 0);
    rc = *out ? ASMODEL_OK : ASMODEL_ERR_NOMEM;
  }
done:
  asmodel_json_free(root);
  asmodel_json_free(list);
  asmodel_http_response_free(&rs);
  return rc;
}

asmodel_err asmodel_chatgpt_models(const char *auth_file, const char *base_url, char **out_json,
                                   char *error, size_t size) {
  char *token = NULL, *url;
  asmodel_err rc;
  long status = 0;
  if (!out_json) return ASMODEL_ERR_INVALID;
  *out_json = NULL;
  if (error && size) error[0] = 0;
  url = endpoint(base_url ? base_url : ASMODEL_CHATGPT_BASE_URL, "/models");
  if (!url) return ASMODEL_ERR_NOMEM;
  if (!asmodel_http_url_allowed(url)) { free(url); return ASMODEL_ERR_INVALID; }
  rc = asmodel_chatgpt_access(auth_file, NULL, 0, NULL, &token, error, size);
  if (rc == ASMODEL_OK) rc = models_once(url, token, out_json, &status, error, size);
  if (rc == ASMODEL_ERR_AUTH && status == 401) {
    char *fresh = NULL; /* exactly one refresh and resend, as for inference */
    rc = asmodel_chatgpt_access(auth_file, token, 0, NULL, &fresh, error, size);
    if (rc == ASMODEL_OK) rc = models_once(url, fresh, out_json, &status, error, size);
    wipe(fresh);
  }
  wipe(token);
  free(url);
  return rc;
}
