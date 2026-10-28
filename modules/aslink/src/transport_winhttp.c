/*
 * transport_winhttp.c — the WinHTTP backend of aslink (Windows without
 * libcurl), to the contract of transport_curl.c.
 *
 * Every hop of an exchange opens its own session in asynchronous mode: the
 * calling thread starts one operation at a time and waits for its
 * completion in slices of WAIT_SLICE_MS, checking the cancel flag and the
 * total deadline in between, so a held long poll stops within a slice.
 * Every request handle is closed and its HANDLE_CLOSING awaited before the
 * call returns: no callback outlives the state it writes, and every buffer
 * WinHTTP may still hold lives in that state.
 *
 * Redirects are followed here, not by WinHTTP, so the limit, the http/https
 * restriction and the Range header behave as with libcurl. Loopback hosts
 * skip proxy discovery; any other host uses the system proxy settings.
 * Multipart bodies are written part by part, files streamed from disk.
 */

#include "aslink_internal.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#ifndef ASTERISM_VERSION
#define ASTERISM_VERSION "0"
#endif

#define WAIT_SLICE_MS 100
#define CHUNK         (64u * 1024u)

/* Written by the callback, read after its event: the event orders them. */
typedef struct {
  HANDLE ev;     /* auto-reset: the pending operation completed */
  HANDLE closed; /* manual-reset: HANDLE_CLOSING arrived        */
  DWORD code;    /* WINHTTP_CALLBACK_STATUS_* of the completion */
  DWORD error;   /* its error, for REQUEST_ERROR                */
  DWORD n;       /* bytes available, read or written            */
} completion;

/* A multipart body: every byte but the files', which go in at their offsets. */
typedef struct {
  size_t at;
  const char *path;
  uint64_t size;
} form_file;

typedef struct {
  const aslink_xfer *x;
  HINTERNET session, connection, request;
  int armed;          /* the callback is set: await HANDLE_CLOSING */
  completion c;
  int64_t deadline;   /* with x->timeout_ms > 0                    */
  DWORD err;          /* the WinHTTP error behind an ASLINK_ERR_IO */
  int said;           /* st->message already written               */
  int upload_failed;  /* a file to upload could not be read        */
  char *buf;          /* CHUNK bytes for reads and file writes     */
  wchar_t *headers;   /* of the current hop                        */
  core_buf text;      /* the multipart body without the files      */
  form_file *files;
  size_t n_files;
  uint64_t form_len;
  char boundary[48];
  aslink_body b;
} xfer_state;

static void CALLBACK on_status(HINTERNET h, DWORD_PTR ctx, DWORD code, LPVOID info,
                               DWORD len) {
  completion *c = (completion *)ctx;
  (void)h;
  if (!c) return;
  switch (code) {
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING: SetEvent(c->closed); return;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
      c->error = ((WINHTTP_ASYNC_RESULT *)info)->dwError;
      break;
    case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
    case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE: c->n = *(DWORD *)info; break;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE: c->n = len; break;
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE: break;
    default: return;
  }
  c->code = code;
  SetEvent(c->ev);
}

static wchar_t *widen(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
  wchar_t *w;
  if (n <= 0) return NULL;
  w = (wchar_t *)malloc((size_t)n * sizeof *w);
  if (w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) <= 0) {
    free(w);
    w = NULL;
  }
  return w;
}

static char *narrow(const wchar_t *w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  char *s;
  if (n <= 0) return NULL;
  s = (char *)malloc((size_t)n);
  if (s && WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) {
    free(s);
    s = NULL;
  }
  return s;
}

static int clamp_ms(long v, int fallback) {
  if (v <= 0) return fallback;
  return v > INT_MAX ? INT_MAX : (int)v;
}

static aslink_err failed(xfer_state *s) {
  s->err = GetLastError();
  return ASLINK_ERR_IO;
}

/* Wait for the completion of the operation just started. */
static aslink_err await(xfer_state *s) {
  for (;;) {
    DWORD slice = WAIT_SLICE_MS, w;
    if (s->x->cancel && ASLINK_FLAG_GET(s->x->cancel)) return ASLINK_ERR_CANCELLED;
    if (s->x->timeout_ms > 0) {
      int64_t left = s->deadline - os_monotonic_ms();
      if (left <= 0) return ASLINK_ERR_TIMEOUT;
      if (left < (int64_t)slice) slice = (DWORD)left;
    }
    w = WaitForSingleObject(s->c.ev, slice);
    if (w == WAIT_OBJECT_0) break;
    if (w != WAIT_TIMEOUT) return failed(s);
  }
  if (s->c.code == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
    s->err = s->c.error;
    return ASLINK_ERR_IO;
  }
  return ASLINK_OK;
}

/* Close the hop's handles; the request's last callback is awaited. */
static void close_hop(xfer_state *s) {
  if (s->request) {
    WinHttpCloseHandle(s->request);
    if (s->armed) WaitForSingleObject(s->c.closed, INFINITE);
  }
  if (s->connection) WinHttpCloseHandle(s->connection);
  if (s->session) WinHttpCloseHandle(s->session);
  s->request = s->connection = s->session = NULL;
  s->armed = 0;
  free(s->headers);
  s->headers = NULL;
}

/* ── URLs ─────────────────────────────────────────────────────────────── */

/* Length of "scheme://authority" in url, 0 without a scheme. */
static size_t origin_len(const char *url) {
  const char *p = strstr(url, "://");
  if (!p) return 0;
  p += 3;
  return (size_t)(p - url) + strcspn(p, "/?#");
}

/* http:// or https://, any case: libcurl's protocol restriction. */
static int http_scheme(const char *url) {
  static const char *const schemes[] = {"http://", "https://"};
  size_t i, j;
  for (i = 0; i < 2; i++) {
    for (j = 0; schemes[i][j] && tolower((unsigned char)url[j]) == schemes[i][j]; j++) {}
    if (!schemes[i][j]) return 1;
  }
  return 0;
}

static int is_loopback(const char *url) {
  static const char *const hosts[] = {"localhost", "127.0.0.1", "[::1]"};
  const char *h = strstr(url, "://");
  size_t n, i, j;
  if (!h) return 0;
  h += 3;
  n = h[0] == '[' ? strcspn(h, "]") + 1 : strcspn(h, ":/?#");
  for (i = 0; i < sizeof hosts / sizeof hosts[0]; i++) {
    if (strlen(hosts[i]) != n) continue;
    for (j = 0; j < n && tolower((unsigned char)h[j]) == hosts[i][j]; j++) {}
    if (j == n) return 1;
  }
  return 0;
}

/* Location resolved against the current (absolute) URL, without its
 * fragment. Dot segments are left to the server. */
static char *resolve(const char *base, const char *loc) {
  core_buf b;
  const char *scheme_end = strstr(base, "://");
  size_t o = origin_len(base), plen = strcspn(base + o, "?#"), keep, i;
  int slash = 0;
  if (!scheme_end) return NULL;
  for (i = 0; isalnum((unsigned char)loc[i]) || loc[i] == '+' || loc[i] == '-' ||
              loc[i] == '.'; i++) {}
  if (i > 0 && loc[i] == ':' && isalpha((unsigned char)loc[0])) {
    keep = 0; /* absolute */
  } else if (loc[0] == '/' && loc[1] == '/') {
    keep = (size_t)(scheme_end - base) + 1; /* "scheme:" */
  } else if (loc[0] == '/') {
    keep = o;
  } else if (loc[0] == '?') {
    keep = o + plen;
  } else {
    /* relative to the base path's last segment */
    keep = o;
    for (i = plen; i > 0; i--)
      if (base[o + i - 1] == '/') {
        keep = o + i;
        break;
      }
    slash = keep == o;
  }
  core_buf_init(&b);
  if (core_buf_append(&b, base, keep) != CORE_OK ||
      (slash && core_buf_appendc(&b, '/') != CORE_OK) ||
      core_buf_append(&b, loc, strcspn(loc, "#")) != CORE_OK ||
      core_buf_appends(&b, "") != CORE_OK) {
    core_buf_free(&b);
    return NULL;
  }
  return core_buf_detach(&b);
}

static int is_redirect(DWORD status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/* ── response headers ─────────────────────────────────────────────────── */

/* A short header as ASCII; "" when absent or longer than cap. */
static void header(HINTERNET r, DWORD info, const wchar_t *name, char *dst, size_t cap) {
  wchar_t w[128];
  DWORD size = sizeof w, i, n;
  dst[0] = 0;
  if (!WinHttpQueryHeaders(r, info, name ? name : WINHTTP_HEADER_NAME_BY_INDEX, w, &size,
                           WINHTTP_NO_HEADER_INDEX))
    return;
  n = (DWORD)(size / sizeof(wchar_t));
  if (n >= cap) return;
  for (i = 0; i < n; i++) dst[i] = w[i] < 0x80 ? (char)w[i] : '?';
  dst[n] = 0;
}

static int64_t content_length(HINTERNET r) {
  char v[32];
  uint64_t n = 0;
  size_t i;
  header(r, WINHTTP_QUERY_CONTENT_LENGTH, NULL, v, sizeof v);
  if (!v[0]) return -1;
  for (i = 0; v[i]; i++) {
    if (v[i] < '0' || v[i] > '9' || n > ((uint64_t)INT64_MAX - 9) / 10) return -1;
    n = n * 10 + (uint64_t)(v[i] - '0');
  }
  return (int64_t)n;
}

/* The Location header, malloc'd UTF-8; NULL when absent. */
static char *location(HINTERNET r) {
  DWORD size = 0;
  wchar_t *w;
  char *s = NULL;
  WinHttpQueryHeaders(r, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, NULL, &size,
                      WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !size) return NULL;
  w = (wchar_t *)malloc((size_t)size + sizeof *w);
  if (!w) return NULL;
  if (WinHttpQueryHeaders(r, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, w, &size,
                          WINHTTP_NO_HEADER_INDEX))
    s = narrow(w);
  free(w);
  return s;
}

/* ── request body ─────────────────────────────────────────────────────── */

/* A Content-Disposition value: '"', CR and LF percent-encoded (HTML5). */
static core_err put_quoted(core_buf *b, const char *v) {
  core_err e = CORE_OK;
  for (; *v && e == CORE_OK; v++) {
    if (*v == '"') e = core_buf_appends(b, "%22");
    else if (*v == '\r') e = core_buf_appends(b, "%0D");
    else if (*v == '\n') e = core_buf_appends(b, "%0A");
    else e = core_buf_appendc(b, *v);
  }
  return e;
}

static aslink_err build_form(xfer_state *s, aslink_status *st) {
  const aslink_xfer *x = s->x;
  static const char hex[] = "0123456789abcdef";
  unsigned char rnd[12];
  core_buf *t = &s->text;
  core_err ce = CORE_OK;
  size_t i;
  uint64_t file_bytes = 0;
  os_random_bytes(rnd, sizeof rnd);
  memcpy(s->boundary, "----------------", 16);
  for (i = 0; i < sizeof rnd; i++) {
    s->boundary[16 + 2 * i] = hex[rnd[i] >> 4];
    s->boundary[17 + 2 * i] = hex[rnd[i] & 15];
  }
  s->boundary[16 + 2 * sizeof rnd] = 0;
  s->files = (form_file *)calloc(x->form_n, sizeof *s->files);
  if (!s->files) return ASLINK_ERR_NOMEM;
  for (i = 0; i < x->form_n && ce == CORE_OK; i++) {
    const aslink_form_field *f = &x->form[i];
    ce = core_buf_printf(t, "%s--%s\r\nContent-Disposition: form-data; name=\"",
                         i ? "\r\n" : "", s->boundary);
    if (ce == CORE_OK) ce = put_quoted(t, f->name);
    if (ce != CORE_OK) break;
    if (f->file_path) {
      form_file *ff = &s->files[s->n_files];
      if ((ce = core_buf_appends(t, "\"; filename=\"")) != CORE_OK ||
          (ce = put_quoted(t, f->file_name)) != CORE_OK ||
          (ce = core_buf_appends(t, "\"\r\nContent-Type: application/octet-stream\r\n\r\n")) !=
              CORE_OK)
        break;
      if (os_file_size(f->file_path, &ff->size) != CORE_OK) {
        aslink_status_msg(st, NULL, "cannot prepare the upload");
        s->said = 1;
        return ASLINK_ERR_IO;
      }
      ff->at = t->len;
      ff->path = f->file_path;
      file_bytes += ff->size;
      s->n_files++;
    } else {
      ce = core_buf_appends(t, "\"\r\n\r\n");
      if (ce == CORE_OK) ce = core_buf_appends(t, f->value ? f->value : "");
    }
  }
  if (ce == CORE_OK) ce = core_buf_printf(t, "\r\n--%s--\r\n", s->boundary);
  if (ce != CORE_OK) return ASLINK_ERR_NOMEM;
  s->form_len = file_bytes + t->len;
  if (s->form_len > 0xFFFFFFFFu) {
    aslink_status_msg(st, NULL, "the upload exceeds what WinHTTP can send at once");
    s->said = 1;
    return ASLINK_ERR_TOO_LARGE;
  }
  return ASLINK_OK;
}

static aslink_err write_all(xfer_state *s, const char *p, size_t n) {
  aslink_err e;
  while (n) {
    DWORD want = n > CHUNK ? CHUNK : (DWORD)n;
    if (!WinHttpWriteData(s->request, p, want, NULL)) return failed(s);
    if ((e = await(s)) != ASLINK_OK) return e;
    if (!s->c.n || s->c.n > want) {
      s->err = ERROR_WINHTTP_CONNECTION_ERROR;
      return ASLINK_ERR_IO;
    }
    p += s->c.n;
    n -= s->c.n;
  }
  return ASLINK_OK;
}

static aslink_err write_file(xfer_state *s, const form_file *f) {
  FILE *in = os_fopen(f->path, "rb");
  uint64_t left = f->size;
  aslink_err e = ASLINK_OK;
  if (!in) {
    s->upload_failed = 1;
    return ASLINK_ERR_IO;
  }
  while (left && e == ASLINK_OK) {
    size_t want = left > CHUNK ? CHUNK : (size_t)left;
    if (fread(s->buf, 1, want, in) != want) {
      s->upload_failed = 1; /* shorter than announced */
      e = ASLINK_ERR_IO;
      break;
    }
    e = write_all(s, s->buf, want);
    left -= want;
  }
  fclose(in);
  return e;
}

static aslink_err write_form(xfer_state *s) {
  size_t pos = 0, i;
  aslink_err e;
  for (i = 0; i < s->n_files; i++) {
    if ((e = write_all(s, s->text.data + pos, s->files[i].at - pos)) != ASLINK_OK ||
        (e = write_file(s, &s->files[i])) != ASLINK_OK)
      return e;
    pos = s->files[i].at;
  }
  return write_all(s, s->text.data + pos, s->text.len - pos);
}

/* ── one hop ──────────────────────────────────────────────────────────── */

static aslink_err open_hop(xfer_state *s, const char *url, int post) {
  const aslink_xfer *x = s->x;
  URL_COMPONENTS parts;
  wchar_t *wurl = widen(url), *agent = widen("asterism/" ASTERISM_VERSION), *host = NULL,
          *path = NULL;
  DWORD_PTR ctx = (DWORD_PTR)&s->c;
  aslink_err e = ASLINK_ERR_NOMEM;
  int connect_ms = clamp_ms(x->connect_timeout_ms, 60000);
  int total_ms = clamp_ms(x->timeout_ms, INT_MAX);
  if (!http_scheme(url)) {
    s->err = ERROR_WINHTTP_UNRECOGNIZED_SCHEME;
    e = ASLINK_ERR_IO;
    goto done;
  }
  if (!wurl || !agent) goto done;
  memset(&parts, 0, sizeof parts);
  parts.dwStructSize = sizeof parts;
  parts.dwHostNameLength = (DWORD)-1;
  parts.dwUrlPathLength = (DWORD)-1;
  parts.dwExtraInfoLength = (DWORD)-1;
  if (!WinHttpCrackUrl(wurl, 0, 0, &parts)) {
    e = failed(s);
    goto done;
  }
  if (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) {
    s->err = ERROR_WINHTTP_UNRECOGNIZED_SCHEME;
    e = ASLINK_ERR_IO;
    goto done;
  }
  host = (wchar_t *)malloc(((size_t)parts.dwHostNameLength + 1) * sizeof *host);
  path = (wchar_t *)malloc(((size_t)parts.dwUrlPathLength + parts.dwExtraInfoLength + 2) *
                           sizeof *path);
  if (!host || !path) goto done;
  memcpy(host, parts.lpszHostName, (size_t)parts.dwHostNameLength * sizeof *host);
  host[parts.dwHostNameLength] = 0;
  memcpy(path, parts.lpszUrlPath, (size_t)parts.dwUrlPathLength * sizeof *path);
  if (parts.dwExtraInfoLength)
    memcpy(path + parts.dwUrlPathLength, parts.lpszExtraInfo,
           (size_t)parts.dwExtraInfoLength * sizeof *path);
  path[parts.dwUrlPathLength + parts.dwExtraInfoLength] = 0;
  if (!path[0]) wcscpy(path, L"/");
  if (wcschr(path, L'#')) *wcschr(path, L'#') = 0;

  e = ASLINK_ERR_IO;
  s->session = WinHttpOpen(agent,
                           is_loopback(url) ? WINHTTP_ACCESS_TYPE_NO_PROXY
                                            : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
  if (!s->session ||
      !WinHttpSetTimeouts(s->session, connect_ms, connect_ms, total_ms, total_ms)) {
    e = failed(s);
    goto done;
  }
  s->connection = WinHttpConnect(s->session, host, parts.nPort, 0);
  if (!s->connection) {
    e = failed(s);
    goto done;
  }
  s->request = WinHttpOpenRequest(s->connection, post ? L"POST" : L"GET", path, NULL,
                                  WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE
                                                                         : 0);
  if (!s->request) {
    e = failed(s);
    goto done;
  }
  {
    DWORD off = WINHTTP_DISABLE_REDIRECTS, wait = (DWORD)total_ms;
    if (!WinHttpSetOption(s->request, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof off) ||
        !WinHttpSetOption(s->request, WINHTTP_OPTION_CONTEXT_VALUE, &ctx, sizeof ctx)) {
      e = failed(s);
      goto done;
    }
    /* Best effort: otherwise the headers must arrive within 90 s, too
     * short for the longest polls. */
    (void)WinHttpSetOption(s->request, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &wait,
                           sizeof wait);
  }
  ResetEvent(s->c.closed);
  if (WinHttpSetStatusCallback(s->request, on_status,
                               WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS |
                                   WINHTTP_CALLBACK_FLAG_HANDLES,
                               0) == WINHTTP_INVALID_STATUS_CALLBACK) {
    e = failed(s);
    goto done;
  }
  s->armed = 1;
  e = ASLINK_OK;

done:
  free(wurl);
  free(agent);
  free(host);
  free(path);
  return e;
}

/* Send the request of the open hop and wait for the response headers. */
static aslink_err send_hop(xfer_state *s, int post) {
  const aslink_xfer *x = s->x;
  core_buf h;
  core_err ce = CORE_OK;
  const char *json = post ? x->json_body : NULL;
  int form = post && x->form_n;
  size_t json_len = json ? strlen(json) : 0;
  DWORD total;
  aslink_err e;
  if ((uint64_t)json_len > 0xFFFFFFFFu) return ASLINK_ERR_TOO_LARGE;
  total = json ? (DWORD)json_len : form ? (DWORD)s->form_len : 0;
  core_buf_init(&h);
  if (json) ce = core_buf_appends(&h, "Content-Type: application/json\r\n");
  else if (form)
    ce = core_buf_printf(&h, "Content-Type: multipart/form-data; boundary=%s\r\n",
                         s->boundary);
  if (ce == CORE_OK && x->range_from)
    ce = core_buf_printf(&h, "Range: bytes=%llu-\r\n", (unsigned long long)x->range_from);
  if (ce == CORE_OK && h.len && !(s->headers = widen(h.data))) ce = CORE_ERR_NOMEM;
  core_buf_free(&h);
  if (ce != CORE_OK) return ASLINK_ERR_NOMEM;
  if (!WinHttpSendRequest(s->request,
                          s->headers ? s->headers : WINHTTP_NO_ADDITIONAL_HEADERS,
                          s->headers ? (DWORD)-1L : 0,
                          json ? (LPVOID)json : WINHTTP_NO_REQUEST_DATA,
                          json ? (DWORD)json_len : 0, total, (DWORD_PTR)&s->c))
    return failed(s);
  if ((e = await(s)) != ASLINK_OK) return e;
  if (form && (e = write_form(s)) != ASLINK_OK) return e;
  if (!WinHttpReceiveResponse(s->request, NULL)) return failed(s);
  return await(s);
}

/* ── errors ───────────────────────────────────────────────────────────── */

static aslink_err from_winhttp(DWORD err, const aslink_xfer *x) {
  switch (err) {
    case ERROR_WINHTTP_TIMEOUT: return ASLINK_ERR_TIMEOUT;
    case ERROR_WINHTTP_OPERATION_CANCELLED:
      return x->cancel && ASLINK_FLAG_GET(x->cancel) ? ASLINK_ERR_CANCELLED : ASLINK_ERR_IO;
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
    case ERROR_WINHTTP_REDIRECT_FAILED: return ASLINK_ERR_PROTOCOL;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY: return ASLINK_ERR_NOMEM;
    default: return ASLINK_ERR_IO;
  }
}

static const char *describe(DWORD err) {
  switch (err) {
    case ERROR_WINHTTP_TIMEOUT: return "timed out";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "could not resolve the host";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "could not connect";
    case ERROR_WINHTTP_CONNECTION_ERROR: return "connection reset";
    case ERROR_WINHTTP_SECURE_FAILURE: return "TLS failure";
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE: return "invalid server response";
    case ERROR_WINHTTP_INVALID_URL: return "invalid URL";
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: return "unsupported URL scheme";
    case ERROR_WINHTTP_OPERATION_CANCELLED: return "operation cancelled";
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY: return "out of memory";
    default: return "WinHTTP failure";
  }
}

/* ── the exchange ─────────────────────────────────────────────────────── */

aslink_err aslink_perform(const aslink_xfer *x, char **out_body, size_t *out_len,
                          long *out_status, aslink_status *st) {
  xfer_state s;
  char *url = NULL, *loc = NULL, *next = NULL, range[96];
  int post = x->json_body || x->form_n, hops = 0;
  DWORD status = 0, size = sizeof status;
  aslink_err e = ASLINK_ERR_NOMEM;

  if (out_body) *out_body = NULL;
  if (out_len) *out_len = 0;
  if (out_status) *out_status = 0;
  memset(&s, 0, sizeof s);
  s.x = x;
  aslink_body_init(&s.b, x);
  core_buf_init(&s.text);
  if (x->cancel && ASLINK_FLAG_GET(x->cancel)) {
    aslink_status_msg(st, NULL, "cancelled");
    return ASLINK_ERR_CANCELLED;
  }
  s.deadline = os_monotonic_ms() + x->timeout_ms;
  s.c.ev = CreateEventW(NULL, FALSE, FALSE, NULL);
  s.c.closed = CreateEventW(NULL, TRUE, FALSE, NULL);
  s.buf = (char *)malloc(CHUNK);
  url = core_strdup(x->url);
  if (!s.c.ev || !s.c.closed || !s.buf || !url) goto done;
  if (x->form_n && (e = build_form(&s, st)) != ASLINK_OK) goto done;

  for (;;) {
    if ((e = open_hop(&s, url, post)) != ASLINK_OK || (e = send_hop(&s, post)) != ASLINK_OK)
      goto done;
    size = sizeof status;
    if (!WinHttpQueryHeaders(s.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                             WINHTTP_NO_HEADER_INDEX)) {
      e = failed(&s);
      goto done;
    }
    if (st) st->http_status = (int)status;
    if (x->follow_redirects <= 0 || !is_redirect(status) || !(loc = location(s.request)))
      break;
    if (hops++ == x->follow_redirects) {
      free(loc);
      aslink_status_msg(st, x->token, "transport: more than %d redirects",
                        x->follow_redirects);
      s.said = 1;
      e = ASLINK_ERR_PROTOCOL;
      goto done;
    }
    next = resolve(url, loc);
    free(loc);
    if (!next) {
      e = ASLINK_ERR_NOMEM;
      goto done;
    }
    free(url);
    url = next;
    if (status == 301 || status == 302 || status == 303) post = 0; /* as libcurl */
    close_hop(&s);
  }

  header(s.request, WINHTTP_QUERY_CUSTOM, L"Content-Range", range, sizeof range);
  if (!aslink_body_route(&s.b, (long)status, range, content_length(s.request))) {
    e = aslink_body_error(&s.b, x->token, st);
    s.said = 1;
    goto done;
  }
  while (!s.b.skip) {
    DWORD want;
    if (!WinHttpQueryDataAvailable(s.request, NULL)) {
      e = failed(&s);
      goto done;
    }
    if ((e = await(&s)) != ASLINK_OK) goto done;
    if (!s.c.n) break;
    want = s.c.n > CHUNK ? CHUNK : s.c.n;
    if (!WinHttpReadData(s.request, s.buf, want, NULL)) {
      e = failed(&s);
      goto done;
    }
    if ((e = await(&s)) != ASLINK_OK) goto done;
    if (!s.c.n) break;
    if (!aslink_body_take(&s.b, s.buf, s.c.n)) {
      e = aslink_body_error(&s.b, x->token, st);
      s.said = 1;
      goto done;
    }
  }
  if ((e = aslink_body_finish(&s.b, out_body, out_len)) != ASLINK_OK) goto done;
  if (out_status) *out_status = (long)status;

done:
  close_hop(&s);
  if (e == ASLINK_ERR_IO && s.err) e = from_winhttp(s.err, x);
  if (e != ASLINK_OK && !s.said) {
    if (e == ASLINK_ERR_CANCELLED)
      aslink_status_msg(st, x->token, "cancelled");
    else if (s.upload_failed)
      aslink_status_msg(st, x->token, "cannot read the file to upload");
    else if (s.err)
      aslink_status_msg(st, x->token, "transport: %s (WinHTTP error %lu)", describe(s.err),
                        (unsigned long)s.err);
    else if (e == ASLINK_ERR_TIMEOUT)
      aslink_status_msg(st, x->token, "transport: timed out after %ld ms", x->timeout_ms);
    else if (e == ASLINK_ERR_NOMEM)
      aslink_status_msg(st, x->token, "out of memory");
    else
      aslink_status_msg(st, x->token, "transport failure");
  }
  if (s.c.ev) CloseHandle(s.c.ev);
  if (s.c.closed) CloseHandle(s.c.closed);
  aslink_body_free(&s.b);
  core_buf_free(&s.text);
  free(s.files);
  free(s.buf);
  free(url);
  return e;
}

int aslink_has_transport(void) { return 1; }
