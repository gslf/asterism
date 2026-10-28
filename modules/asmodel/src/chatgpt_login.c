/* "Sign in with ChatGPT" authorization-code flow with PKCE (S256).
 *
 * The callback listener is the project's only listening socket. It binds
 * 127.0.0.1 only, serves one connection at a time with a short read timeout,
 * answers anything but the expected redirect with an error page, and is
 * closed as soon as the login finishes or its session is freed. Headless
 * hosts can skip it: the user pastes the redirected URL (complete()). */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET cg_socket;
#define CG_BAD_SOCKET INVALID_SOCKET
#define cg_closesocket closesocket
#else
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int cg_socket;
#define CG_BAD_SOCKET (-1)
#define cg_closesocket close
#endif

#include "chatgpt.h"
#include "chatgpt_auth.h"
#include "http.h"

#include <stdlib.h>
#include <string.h>

#define CG_REQUEST_MAX 8192
#define CG_CONNECTION_MS 5000

struct asmodel_chatgpt_login {
  char *auth_file, *host_file, *issuer, *resource, *agent_name, *host_id;
  char *client_id;      /* saved issued client (reauthorization), else NULL */
  char *expect_subject; /* identity of the saved registration */
  int registering;
  char *state, *nonce, *verifier;
  char redirect_uri[64];
  char *url;
  int64_t http_timeout_ms;
  cg_socket listener;
  int wsa, finished;
};

static void say(char *error, size_t size, const char *fmt, const char *a, const char *b) {
  if (error && size) snprintf(error, size, fmt, a ? a : "", b ? b : "");
}

static void wipe(char *s) {
  if (s) {
    volatile char *p = s;
    while (*p) *p++ = 0;
  }
  free(s);
}

static int printable(const char *s, size_t max) {
  size_t n = s ? strlen(s) : 0;
  if (!n || n > max) return 0;
  for (size_t i = 0; i < n; i++)
    if ((unsigned char)s[i] <= 0x20 || (unsigned char)s[i] >= 0x7f) return 0;
  return 1;
}

static int same_secret(const char *a, const char *b) {
  size_t n = a ? strlen(a) : 0;
  return a && b && n && n == strlen(b) && core_bytes_equal_ct(a, b, n);
}

/* ── query strings ──────────────────────────────────────────────────────── */

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Decoded value of key; *bad on malformed or repeated keys. */
static char *query_value(const char *q, const char *key, int *bad) {
  size_t klen = strlen(key);
  char *found = NULL;
  for (const char *p = q; p && *p;) {
    size_t n = strcspn(p, "&#");
    const char *eq = memchr(p, '=', n);
    size_t kn = eq ? (size_t)(eq - p) : n;
    if (kn == klen && !memcmp(p, key, klen)) {
      const char *v = eq ? eq + 1 : p + n;
      size_t vn = (size_t)(p + n - v), j = 0;
      char *out;
      if (found) { *bad = 1; free(found); return NULL; }
      out = (char *)malloc(vn + 1);
      if (!out) { *bad = 1; return NULL; }
      for (size_t i = 0; i < vn; i++) {
        if (v[i] == '+') out[j++] = ' ';
        else if (v[i] == '%') {
          int hi = i + 2 < vn ? hexval(v[i + 1]) : -1;
          int lo = i + 2 < vn ? hexval(v[i + 2]) : -1;
          if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) { free(out); *bad = 1; return NULL; }
          out[j++] = (char)(hi * 16 + lo);
          i += 2;
        } else out[j++] = v[i];
      }
      out[j] = 0;
      if (!core_utf8_valid(out, j)) { free(out); *bad = 1; return NULL; }
      found = out;
    }
    p += n;
    if (*p == '#') break;
    if (*p == '&') p++;
  }
  return found;
}

/* ── loopback listener ──────────────────────────────────────────────────── */

static asmodel_err listen_loopback(asmodel_chatgpt_login *s, int port, int *out_port,
                                   char *error, size_t size) {
  struct sockaddr_in a;
  socklen_t len = sizeof a;
  char where[32];
  cg_socket fd;
#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    say(error, size, "Winsock initialization failed%s%s", NULL, NULL);
    return ASMODEL_ERR_BACKEND;
  }
  s->wsa = 1;
#endif
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == CG_BAD_SOCKET) {
    say(error, size, "cannot create the login callback socket%s%s", NULL, NULL);
    return ASMODEL_ERR_BACKEND;
  }
  /* Non-blocking: a connection reset between poll and accept cannot stall. */
#ifdef _WIN32
  {
    BOOL one = TRUE; /* nobody else may bind this port while we listen */
    u_long nb = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);
    (void)ioctlsocket(fd, FIONBIO, &nb);
  }
#else
  {
    int one = 1, flags = fcntl(fd, F_GETFL);
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  }
#endif
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* never any other interface */
  a.sin_port = htons((unsigned short)port);
  snprintf(where, sizeof where, "127.0.0.1:%d", port);
  if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 4) != 0 ||
      getsockname(fd, (struct sockaddr *)&a, &len) != 0) {
    cg_closesocket(fd);
    say(error, size, "cannot listen on %s for the login callback (port in use?)%s", where, NULL);
    return ASMODEL_ERR_BUSY;
  }
  s->listener = fd;
  *out_port = ntohs(a.sin_port);
  return ASMODEL_OK;
}

static void close_listener(asmodel_chatgpt_login *s) {
  if (s->listener != CG_BAD_SOCKET) cg_closesocket(s->listener);
  s->listener = CG_BAD_SOCKET;
}

/* 1 readable, 0 timeout, -1 error. */
static int readable(cg_socket fd, int64_t ms) {
  if (ms < 0) ms = 0;
  if (ms > 1000000) ms = 1000000;
#ifdef _WIN32
  fd_set set;
  struct timeval tv;
  FD_ZERO(&set);
  FD_SET(fd, &set);
  tv.tv_sec = (long)(ms / 1000);
  tv.tv_usec = (long)(ms % 1000) * 1000;
  int r = select(0, &set, NULL, NULL, &tv);
  return r > 0 ? 1 : r == 0 ? 0 : -1;
#else
  struct pollfd p;
  p.fd = fd; p.events = POLLIN; p.revents = 0;
  for (;;) {
    int r = poll(&p, 1, (int)ms);
    if (r < 0 && errno == EINTR) continue;
    return r > 0 ? 1 : r == 0 ? 0 : -1;
  }
#endif
}

static void send_all(cg_socket c, const char *p, size_t n) {
  while (n) {
#ifdef _WIN32
    int w = send(c, p, n > 65536 ? 65536 : (int)n, 0);
#elif defined(MSG_NOSIGNAL)
    ssize_t w = send(c, p, n, MSG_NOSIGNAL);
#else
    ssize_t w = send(c, p, n, 0);
#endif
    if (w <= 0) return;
    p += w;
    n -= (size_t)w;
  }
}

static void html_escape(core_buf *b, const char *t) {
  for (; *t; t++) {
    switch (*t) {
      case '<': (void)core_buf_appends(b, "&lt;"); break;
      case '>': (void)core_buf_appends(b, "&gt;"); break;
      case '&': (void)core_buf_appends(b, "&amp;"); break;
      case '"': (void)core_buf_appends(b, "&quot;"); break;
      default: (void)core_buf_appendc(b, *t);
    }
  }
}

static void respond(cg_socket c, int status, const char *text) {
  core_buf body, head;
  const char *reason = status == 200 ? "OK" : status == 400 ? "Bad Request" :
                       status == 404 ? "Not Found" : "Method Not Allowed";
  core_buf_init(&body);
  core_buf_init(&head);
  (void)core_buf_appends(&body, "<!doctype html><html><head><meta charset=\"utf-8\">"
                                "<title>asterism</title></head><body><p>");
  html_escape(&body, text);
  (void)core_buf_appends(&body, "</p></body></html>\n");
  (void)core_buf_printf(&head, "HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\n"
                        "Content-Length: %lu\r\nCache-Control: no-store\r\n"
                        "Referrer-Policy: no-referrer\r\nConnection: close\r\n\r\n",
                        status, reason, (unsigned long)body.len);
  if (head.data && body.data) {
    send_all(c, head.data, head.len);
    send_all(c, body.data, body.len);
  }
  core_buf_free(&body);
  core_buf_free(&head);
}

/* Read the request head; returns its length, 0 when nothing usable arrived. */
static size_t read_request(cg_socket c, char *buf, size_t cap) {
  size_t n = 0;
  int64_t start = os_monotonic_ms();
  while (n + 1 < cap) {
    int64_t left = CG_CONNECTION_MS - (os_monotonic_ms() - start);
    if (left <= 0 || readable(c, left) != 1) break;
#ifdef _WIN32
    int got = recv(c, buf + n, (int)(cap - 1 - n), 0);
#else
    ssize_t got = recv(c, buf + n, cap - 1 - n, 0);
#endif
    if (got <= 0) break;
    n += (size_t)got;
    buf[n] = 0;
    if (strstr(buf, "\r\n\r\n") || strstr(buf, "\n\n")) return n;
  }
  buf[n] = 0;
  return strchr(buf, '\n') ? n : 0;
}

/* ── callback handling ──────────────────────────────────────────────────── */

static asmodel_err save_registration(asmodel_chatgpt_login *s, const char *client_id,
                                     char *error, size_t size) {
  cg_lock lock;
  cg_record r;
  asmodel_err rc = cg_lock_acquire(s->auth_file, 30000, NULL, &lock, error, size);
  if (rc != ASMODEL_OK) return rc;
  /* A new registration replaces whatever this file held. The issued client id
   * is saved before the code exchange so a later attempt can reuse it. */
  memset(&r, 0, sizeof r);
  r.issuer = core_strdup(s->issuer);
  r.resource = core_strdup(s->resource);
  r.client_id = core_strdup(client_id);
  r.host_id = core_strdup(s->host_id);
  rc = r.issuer && r.resource && r.client_id && r.host_id
           ? cg_record_write(s->auth_file, &r, error, size) : ASMODEL_ERR_NOMEM;
  cg_record_free(&r);
  cg_lock_release(&lock);
  return rc;
}

static asmodel_err exchange(asmodel_chatgpt_login *s, const char *code, const char *client_id,
                            char *error, size_t size) {
  core_buf form;
  cg_tokens t;
  cg_identity id;
  cg_lock lock;
  cg_record r;
  asmodel_err rc;
  int terminal = 0, plan;
  char oauth_code[64];
  core_buf_init(&form);
  if (cg_form_add(&form, "grant_type", "authorization_code") ||
      cg_form_add(&form, "client_id", client_id) || cg_form_add(&form, "code", code) ||
      cg_form_add(&form, "code_verifier", s->verifier) ||
      cg_form_add(&form, "redirect_uri", s->redirect_uri) ||
      cg_form_add(&form, "resource", s->resource)) {
    core_buf_free(&form);
    return ASMODEL_ERR_NOMEM;
  }
  rc = cg_token_request(s->issuer, form.data, s->http_timeout_ms, NULL, &t, oauth_code,
                        sizeof oauth_code, &terminal, error, size);
  if (form.data) memset(form.data, 0, form.len);
  core_buf_free(&form);
  if (rc != ASMODEL_OK) {
    if (rc == ASMODEL_ERR_AUTH && !strcmp(oauth_code, "invalid_grant")) {
      char detail[300];
      snprintf(detail, sizeof detail, "%s", error && size ? error : "");
      say(error, size, "authorization code rejected (%s); %s again", detail, CG_LOGIN_HINT);
    }
    return rc;
  }
  if (!t.id_token) {
    cg_tokens_free(&t);
    say(error, size, "ChatGPT token response has no ID token%s%s", NULL, NULL);
    return ASMODEL_ERR_AUTH;
  }
  rc = cg_validate_id_token(t.id_token, s->issuer, client_id, s->nonce, &id, error, size);
  if (rc != ASMODEL_OK) { cg_tokens_free(&t); return rc; }
  if (!s->registering && s->expect_subject && strcmp(id.subject, s->expect_subject)) {
    cg_identity_free(&id);
    cg_tokens_free(&t);
    say(error, size, "signed in as a different ChatGPT account than the saved registration; "
        "use a new registration to switch accounts%s%s", NULL, NULL);
    return ASMODEL_ERR_AUTH;
  }
  if (!t.scope) t.scope = cg_jwt_claim(t.access_token, "scope");
  plan = cg_has_scope(t.scope, CG_PLAN_SCOPE);
  rc = cg_lock_acquire(s->auth_file, 30000, NULL, &lock, error, size);
  if (rc != ASMODEL_OK) { cg_identity_free(&id); cg_tokens_free(&t); return rc; }
  if (cg_record_read(s->auth_file, &r) != ASMODEL_OK) memset(&r, 0, sizeof r);
  free(r.issuer); r.issuer = core_strdup(s->issuer);
  free(r.resource); r.resource = core_strdup(s->resource);
  free(r.client_id); r.client_id = core_strdup(client_id);
  free(r.host_id); r.host_id = core_strdup(s->host_id);
  free(r.scope); r.scope = NULL; /* the new grant alone decides plan usage */
  free(r.plan); r.plan = NULL;
  if (!r.issuer || !r.resource || !r.client_id || !r.host_id) rc = ASMODEL_ERR_NOMEM;
  else rc = cg_record_apply(&r, &t, &id);
  if (rc == ASMODEL_OK) rc = cg_record_write(s->auth_file, &r, error, size);
  cg_record_free(&r);
  cg_lock_release(&lock);
  if (rc == ASMODEL_OK && !plan) {
    say(error, size, "signed in%s%s, but ChatGPT plan usage was not granted; run "
        "`asterism login chatgpt` again and allow plan usage",
        id.email ? " as " : "", id.email);
    rc = ASMODEL_ERR_UNSUPPORTED;
  }
  cg_identity_free(&id);
  cg_tokens_free(&t);
  return rc;
}

/* *stale: the query does not belong to this attempt (keep waiting). */
static asmodel_err handle_query(asmodel_chatgpt_login *s, const char *query, int *stale,
                                char *error, size_t size) {
  int bad = 0;
  char *state = query_value(query, "state", &bad), *code = query_value(query, "code", &bad);
  char *client = query_value(query, "client_id", &bad), *oerr = query_value(query, "error", &bad);
  char *desc = query_value(query, "error_description", &bad);
  asmodel_err rc;
  *stale = 0;
  if (bad || !same_secret(state, s->state)) {
    *stale = 1;
    say(error, size, "the redirect does not belong to this login attempt (state mismatch)%s%s",
        NULL, NULL);
    rc = ASMODEL_ERR_INVALID;
    goto done;
  }
  if (oerr) {
    char e1[64], e2[160];
    asmodel_chatgpt_sanitize(e1, sizeof e1, oerr);
    asmodel_chatgpt_sanitize(e2, sizeof e2, desc ? desc : "");
    if (!strcmp(oerr, "access_denied"))
      say(error, size, "ChatGPT sign-in was declined%s%s", e2[0] ? ": " : "", e2);
    else
      say(error, size, "ChatGPT sign-in failed: %s %s", e1, e2);
    rc = ASMODEL_ERR_AUTH;
    goto done;
  }
  if (!code || !code[0]) {
    say(error, size, "the redirect carries no authorization code%s%s", NULL, NULL);
    rc = ASMODEL_ERR_AUTH;
    goto done;
  }
  if (s->registering) {
    if (!printable(client, 128) || !strcmp(client, CG_DYNAMIC_CLIENT)) {
      say(error, size, "ChatGPT registration incomplete: the redirect carried no issued client_id; "
          "%s again%s", CG_LOGIN_HINT, NULL);
      rc = ASMODEL_ERR_AUTH;
      goto done;
    }
    rc = save_registration(s, client, error, size);
    if (rc == ASMODEL_OK) rc = exchange(s, code, client, error, size);
  } else {
    if (client && strcmp(client, s->client_id)) {
      say(error, size, "the redirect names a different client_id than the saved registration%s%s",
          NULL, NULL);
      rc = ASMODEL_ERR_AUTH;
      goto done;
    }
    rc = exchange(s, code, s->client_id, error, size);
  }
done:
  wipe(state); wipe(code); free(client); free(oerr); free(desc);
  return rc;
}

static void finish(asmodel_chatgpt_login *s) {
  s->finished = 1;
  close_listener(s);
}

/* ── public API ─────────────────────────────────────────────────────────── */

void asmodel_chatgpt_login_free(asmodel_chatgpt_login *s) {
  if (!s) return;
  close_listener(s);
#ifdef _WIN32
  if (s->wsa) WSACleanup();
#endif
  free(s->auth_file); free(s->host_file); free(s->issuer); free(s->resource);
  free(s->agent_name); free(s->host_id); free(s->client_id); free(s->expect_subject);
  wipe(s->state); wipe(s->nonce); wipe(s->verifier); wipe(s->url);
  free(s);
}

const char *asmodel_chatgpt_login_url(const asmodel_chatgpt_login *s) { return s ? s->url : NULL; }

const char *asmodel_chatgpt_login_redirect_uri(const asmodel_chatgpt_login *s) {
  return s ? s->redirect_uri : NULL;
}

static char *trimmed_base(const char *url) {
  size_t n = strlen(url);
  while (n && url[n - 1] == '/') n--;
  return core_strndup(url, n);
}

asmodel_err asmodel_chatgpt_login_begin(const asmodel_chatgpt_login_params *p,
                                        asmodel_chatgpt_login **out, char *error, size_t size) {
  asmodel_chatgpt_login *s;
  cg_lock lock;
  cg_record r;
  char *id_token_hint = NULL, *login_hint = NULL, *challenge = NULL;
  asmodel_err rc;
  int port = 0;
  if (error && size) error[0] = 0;
  if (!out) return ASMODEL_ERR_INVALID;
  *out = NULL;
  if (!p || !p->auth_file || !p->auth_file[0] || p->redirect_port < 0 ||
      p->redirect_port > 65535 || p->http_timeout_ms < 0) {
    say(error, size, "invalid ChatGPT login parameters%s%s", NULL, NULL);
    return ASMODEL_ERR_INVALID;
  }
  if (!asmodel_http_available()) {
    say(error, size, "asmodel built without an HTTP client%s%s", NULL, NULL);
    return ASMODEL_ERR_UNSUPPORTED;
  }
  if (!asmodel_http_url_allowed(p->issuer ? p->issuer : ASMODEL_CHATGPT_ISSUER) ||
      !printable(p->resource ? p->resource : ASMODEL_CHATGPT_RESOURCE, 512) ||
      (p->agent_name && !p->agent_name[0])) {
    say(error, size, "invalid ChatGPT issuer, resource or agent name%s%s", NULL, NULL);
    return ASMODEL_ERR_INVALID;
  }
  s = (asmodel_chatgpt_login *)calloc(1, sizeof *s);
  if (!s) return ASMODEL_ERR_NOMEM;
  s->listener = CG_BAD_SOCKET;
  s->http_timeout_ms = p->http_timeout_ms;
  s->auth_file = core_strdup(p->auth_file);
  s->host_file = p->host_file ? core_strdup(p->host_file) : cg_path_with(p->auth_file, ".host");
  s->issuer = trimmed_base(p->issuer ? p->issuer : ASMODEL_CHATGPT_ISSUER);
  s->resource = core_strdup(p->resource ? p->resource : ASMODEL_CHATGPT_RESOURCE);
  s->agent_name = core_strdup(p->agent_name ? p->agent_name : "asterism");
  if (!s->auth_file || !s->host_file || !s->issuer || !s->resource || !s->agent_name) {
    rc = ASMODEL_ERR_NOMEM;
    goto fail;
  }
  rc = cg_lock_acquire(s->auth_file, 30000, NULL, &lock, error, size);
  if (rc != ASMODEL_OK) goto fail;
  if (!p->new_registration && cg_record_read(s->auth_file, &r) == ASMODEL_OK) {
    if (r.client_id && strcmp(r.client_id, CG_DYNAMIC_CLIENT) &&
        (!r.issuer || cg_same_issuer(r.issuer, s->issuer))) {
      s->client_id = core_strdup(r.client_id);
      s->expect_subject = r.subject ? core_strdup(r.subject) : NULL;
      id_token_hint = r.id_token ? core_strdup(r.id_token) : NULL;
      login_hint = r.email ? core_strdup(r.email) : NULL;
    }
    cg_record_free(&r);
  }
  s->registering = s->client_id == NULL;
  /* The host id is chosen and persisted before the host's first sign-in. */
  rc = cg_host_id(s->host_file, 1, &s->host_id, error, size);
  cg_lock_release(&lock);
  if (rc != ASMODEL_OK) goto fail;
  s->state = cg_random_token(32);
  s->nonce = cg_random_token(32);
  s->verifier = cg_random_token(32);
  if (!s->state || !s->nonce || !s->verifier) { rc = ASMODEL_ERR_NOMEM; goto fail; }
  {
    uint8_t digest[32];
    core_sha256(s->verifier, strlen(s->verifier), digest);
    challenge = cg_base64url(digest, sizeof digest);
    if (!challenge) { rc = ASMODEL_ERR_NOMEM; goto fail; }
  }
  rc = listen_loopback(s, p->redirect_port, &port, error, size);
  if (rc != ASMODEL_OK) goto fail;
  snprintf(s->redirect_uri, sizeof s->redirect_uri, "http://127.0.0.1:%d%s", port, CG_CALLBACK_PATH);
  {
    core_buf q, u;
    core_err e;
    core_buf_init(&q);
    core_buf_init(&u);
    e = cg_form_add(&q, "client_id", s->registering ? CG_DYNAMIC_CLIENT : s->client_id);
    if (!e && s->registering) e = cg_form_add(&q, "agent_name_hint", s->agent_name);
    if (!e) e = cg_form_add(&q, "ext_agent_host_id", s->host_id);
    if (!e && id_token_hint) e = cg_form_add(&q, "id_token_hint", id_token_hint);
    if (!e && login_hint) e = cg_form_add(&q, "login_hint", login_hint);
    if (!e) e = cg_form_add(&q, "response_type", "code");
    if (!e) e = cg_form_add(&q, "redirect_uri", s->redirect_uri);
    if (!e) e = cg_form_add(&q, "scope", CG_SCOPES);
    if (!e) e = cg_form_add(&q, "resource", s->resource);
    if (!e) e = cg_form_add(&q, "state", s->state);
    if (!e) e = cg_form_add(&q, "nonce", s->nonce);
    if (!e) e = cg_form_add(&q, "code_challenge_method", "S256");
    if (!e) e = cg_form_add(&q, "code_challenge", challenge);
    if (!e) e = core_buf_appends(&u, s->issuer);
    if (!e) e = core_buf_appends(&u, CG_AUTHORIZE_PATH "?");
    if (!e) e = core_buf_append(&u, q.data, q.len);
    if (q.data) memset(q.data, 0, q.len);
    core_buf_free(&q);
    if (e) { core_buf_free(&u); rc = ASMODEL_ERR_NOMEM; goto fail; }
    s->url = core_buf_detach(&u);
  }
  wipe(id_token_hint); free(login_hint); free(challenge);
  *out = s;
  return ASMODEL_OK;
fail:
  wipe(id_token_hint); free(login_hint); free(challenge);
  asmodel_chatgpt_login_free(s);
  return rc;
}

asmodel_err asmodel_chatgpt_login_wait(asmodel_chatgpt_login *s, int64_t timeout_ms,
                                       char *error, size_t size) {
  int64_t start = os_monotonic_ms();
  char *req;
  if (error && size) error[0] = 0;
  if (!s || s->finished || s->listener == CG_BAD_SOCKET || timeout_ms < 0) {
    say(error, size, "the ChatGPT login session is not waiting%s%s", NULL, NULL);
    return ASMODEL_ERR_INVALID;
  }
  req = (char *)malloc(CG_REQUEST_MAX);
  if (!req) return ASMODEL_ERR_NOMEM;
  for (;;) {
    int64_t left = timeout_ms - (os_monotonic_ms() - start);
    struct sockaddr_in peer;
    socklen_t plen = sizeof peer;
    cg_socket c;
    int r = readable(s->listener, left > 0 ? left : 0);
    if (r < 0) {
      free(req);
      say(error, size, "login callback listener failed%s%s", NULL, NULL);
      return ASMODEL_ERR_BACKEND;
    }
    if (r == 0) {
      free(req);
      say(error, size, "no browser redirect received yet%s%s", NULL, NULL);
      return ASMODEL_ERR_TIMEOUT;
    }
    c = accept(s->listener, (struct sockaddr *)&peer, &plen);
    if (c == CG_BAD_SOCKET) continue;
    /* The connection itself is blocking; reads are bounded by poll. */
#ifdef _WIN32
    {
      u_long nb = 0;
      (void)ioctlsocket(c, FIONBIO, &nb);
    }
#else
    {
      int flags = fcntl(c, F_GETFL);
      (void)fcntl(c, F_SETFD, FD_CLOEXEC);
      if (flags >= 0) (void)fcntl(c, F_SETFL, flags & ~O_NONBLOCK);
    }
#endif
    if (peer.sin_family != AF_INET || (ntohl(peer.sin_addr.s_addr) >> 24) != 127) {
      cg_closesocket(c);
      continue;
    }
    if (!read_request(c, req, CG_REQUEST_MAX)) { cg_closesocket(c); continue; }
    if (strncmp(req, "GET ", 4)) {
      respond(c, 405, "Method not allowed.");
      cg_closesocket(c);
      continue;
    }
    {
      char *target = req + 4, *end = strpbrk(target, " \r\n"), *query;
      int stale = 0;
      asmodel_err rc;
      if (end) *end = 0;
      query = strchr(target, '?');
      if (query) *query++ = 0;
      if (strcmp(target, CG_CALLBACK_PATH)) {
        respond(c, 404, "Not found.");
        cg_closesocket(c);
        continue;
      }
      rc = handle_query(s, query ? query : "", &stale, error, size);
      if (stale) {
        respond(c, 400, "This sign-in redirect does not belong to the running asterism login.");
        cg_closesocket(c);
        continue;
      }
      if (rc == ASMODEL_OK)
        respond(c, 200, "Signed in with ChatGPT. You can close this tab and return to asterism.");
      else if (rc == ASMODEL_ERR_UNSUPPORTED)
        respond(c, 200, "Signed in, but ChatGPT plan usage was not granted. Return to asterism.");
      else
        respond(c, 400, "Sign-in did not complete. Return to asterism for details.");
      cg_closesocket(c);
      memset(req, 0, CG_REQUEST_MAX);
      free(req);
      finish(s);
      return rc;
    }
  }
}

asmodel_err asmodel_chatgpt_login_complete(asmodel_chatgpt_login *s, const char *pasted,
                                           char *error, size_t size) {
  char *copy, *p, *q;
  int stale = 0;
  asmodel_err rc;
  size_t ru;
  if (error && size) error[0] = 0;
  if (!s || s->finished || !pasted) {
    say(error, size, "the ChatGPT login session is finished%s%s", NULL, NULL);
    return ASMODEL_ERR_INVALID;
  }
  copy = core_strdup(pasted);
  if (!copy) return ASMODEL_ERR_NOMEM;
  p = core_str_trim(copy);
  ru = strlen(s->redirect_uri);
  if (!strncmp(p, s->redirect_uri, ru)) {
    q = p[ru] == '?' ? p + ru + 1 : NULL;
  } else if (core_str_has_prefix(p, "http://") || core_str_has_prefix(p, "https://")) {
    q = NULL;
  } else if (p[0] == '?') {
    q = p + 1;
  } else {
    q = strstr(p, "state=") && strchr(p, '=') ? p : NULL;
    if (!q) {
      wipe(copy);
      say(error, size, "paste the full address shown by the browser after sign-in (it starts "
          "with %s); a bare code cannot be verified%s", s->redirect_uri, NULL);
      return ASMODEL_ERR_INVALID;
    }
  }
  if (!q) {
    wipe(copy);
    say(error, size, "the pasted address is not this login's redirect (%s)%s", s->redirect_uri, NULL);
    return ASMODEL_ERR_INVALID;
  }
  rc = handle_query(s, q, &stale, error, size);
  wipe(copy);
  if (stale) return ASMODEL_ERR_INVALID; /* keep the session: paste again */
  finish(s);
  return rc;
}
