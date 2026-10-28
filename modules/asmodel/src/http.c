/* Bounded HTTP exchanges for the plan provider (see http.h). */
#include "http.h"
#include "core.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ASMODEL_WITH_CURL
#include <curl/curl.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif

#define HTTP_DEFAULT_MAX_BODY ((size_t)16 * 1024 * 1024)

void asmodel_http_response_free(asmodel_http_response *rs) {
  if (!rs) return;
  free(rs->body);
  memset(rs, 0, sizeof *rs);
}

static int host_is_loopback(const char *host, size_t n) {
  if (n == 9 && !memcmp(host, "127.0.0.1", 9)) return 1;
  if (n == 5 && !memcmp(host, "[::1]", 5)) return 1;
  if (n != 9) return 0;
  for (size_t i = 0; i < n; i++)
    if (tolower((unsigned char)host[i]) != "localhost"[i]) return 0;
  return 1;
}

int asmodel_http_url_allowed(const char *url) {
  if (!url) return 0;
  for (const char *p = url; *p; p++)
    if ((unsigned char)*p <= 0x20 || (unsigned char)*p >= 0x7f) return 0;
  if (core_str_has_prefix(url, "https://")) return url[8] != '\0' && url[8] != '/';
  if (!core_str_has_prefix(url, "http://")) return 0;
  const char *host = url + 7;
  size_t n = strcspn(host, ":/?#");
  if (host[0] == '[') {
    const char *end = strchr(host, ']');
    if (!end) return 0;
    n = (size_t)(end - host) + 1;
  }
  return host_is_loopback(host, n);
}

static void copy_header_value(char *dst, size_t cap, const char *v, size_t n) {
  while (n && (*v == ' ' || *v == '\t')) { v++; n--; }
  while (n && (v[n - 1] == '\r' || v[n - 1] == '\n' || v[n - 1] == ' ' ||
               v[n - 1] == '\t')) n--;
  if (n >= cap) n = cap - 1;
  size_t j = 0;
  for (size_t i = 0; i < n; i++)
    if ((unsigned char)v[i] >= 0x20 && (unsigned char)v[i] < 0x7f) dst[j++] = v[i];
  dst[j] = 0;
}

static int capture(asmodel_http_response *rs, size_t max, const char *p, size_t n) {
  if (n > max || rs->len > max - n) return -1;
  char *next = (char *)realloc(rs->body, rs->len + n + 1);
  if (!next) return -1;
  rs->body = next;
  memcpy(rs->body + rs->len, p, n);
  rs->len += n;
  rs->body[rs->len] = 0;
  return 0;
}

#ifdef ASMODEL_WITH_CURL

int asmodel_http_available(void) { return 1; }

typedef struct {
  const asmodel_http_request *rq;
  asmodel_http_response *rs;
  CURL *curl;
  size_t max;
  int aborted, overflow;
} curl_ctx;

static size_t on_write(char *p, size_t sz, size_t nm, void *ud) {
  curl_ctx *c = (curl_ctx *)ud;
  size_t n = sz * nm;
  long status = 0;
  curl_easy_getinfo(c->curl, CURLINFO_RESPONSE_CODE, &status);
  c->rs->status = status;
  /* A streamed success body belongs to the sink; errors are kept for parsing. */
  if (!(c->rq->sink && status >= 200 && status < 300) && capture(c->rs, c->max, p, n)) {
    c->overflow = 1;
    return 0;
  }
  if (c->rq->sink && c->rq->sink(status, p, n, c->rq->sink_ud)) {
    c->aborted = 1;
    return 0;
  }
  return n;
}

static size_t on_header(char *p, size_t sz, size_t nm, void *ud) {
  curl_ctx *c = (curl_ctx *)ud;
  size_t n = sz * nm;
  const char *colon = memchr(p, ':', n);
  if (colon) {
    size_t k = (size_t)(colon - p);
    char name[32];
    if (k < sizeof name) {
      for (size_t i = 0; i < k; i++) name[i] = (char)tolower((unsigned char)p[i]);
      name[k] = 0;
      if (!strcmp(name, "retry-after"))
        copy_header_value(c->rs->retry_after, sizeof c->rs->retry_after, colon + 1, n - k - 1);
      else if (!strcmp(name, "x-request-id") ||
               (!strcmp(name, "openai-request-id") && !c->rs->request_id[0]))
        copy_header_value(c->rs->request_id, sizeof c->rs->request_id, colon + 1, n - k - 1);
    }
  }
  return n;
}

static int on_progress(void *ud, curl_off_t a, curl_off_t b, curl_off_t d, curl_off_t e) {
  curl_ctx *c = (curl_ctx *)ud;
  (void)a; (void)b; (void)d; (void)e;
  return c->rq->cancel && *c->rq->cancel ? 1 : 0;
}

int asmodel_http_perform(const asmodel_http_request *rq, asmodel_http_response *rs,
                         char *error, size_t error_size) {
  struct curl_slist *headers = NULL;
  curl_ctx c;
  CURLcode cc;
  int rc = ASMODEL_HTTP_TRANSPORT;
  if (error && error_size) error[0] = 0;
  if (!rq || !rs || !rq->url || !rq->method) return ASMODEL_HTTP_TRANSPORT;
  memset(rs, 0, sizeof *rs);
  memset(&c, 0, sizeof c);
  c.rq = rq; c.rs = rs; c.max = rq->max_body ? rq->max_body : HTTP_DEFAULT_MAX_BODY;
  if (rq->cancel && *rq->cancel) return ASMODEL_HTTP_CANCELLED;
  c.curl = curl_easy_init();
  if (!c.curl) {
    if (error && error_size) snprintf(error, error_size, "curl init failed");
    return ASMODEL_HTTP_TRANSPORT;
  }
  for (size_t i = 0; rq->headers && rq->headers[i]; i++) {
    struct curl_slist *next = curl_slist_append(headers, rq->headers[i]);
    if (!next) goto done;
    headers = next;
  }
  {
    struct curl_slist *next = curl_slist_append(headers, "Expect:");
    if (!next) goto done;
    headers = next;
  }
  curl_easy_setopt(c.curl, CURLOPT_URL, rq->url);
  curl_easy_setopt(c.curl, CURLOPT_HTTPHEADER, headers);
  if (!strcmp(rq->method, "POST"))
    curl_easy_setopt(c.curl, CURLOPT_POSTFIELDS, rq->body ? rq->body : "");
  else
    curl_easy_setopt(c.curl, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(c.curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(c.curl, CURLOPT_WRITEFUNCTION, on_write);
  curl_easy_setopt(c.curl, CURLOPT_WRITEDATA, &c);
  curl_easy_setopt(c.curl, CURLOPT_HEADERFUNCTION, on_header);
  curl_easy_setopt(c.curl, CURLOPT_HEADERDATA, &c);
  curl_easy_setopt(c.curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c.curl, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(c.curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  if (rq->timeout_ms > 0)
    curl_easy_setopt(c.curl, CURLOPT_TIMEOUT_MS,
                     rq->timeout_ms > LONG_MAX ? LONG_MAX : (long)rq->timeout_ms);
  curl_easy_setopt(c.curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(c.curl, CURLOPT_XFERINFOFUNCTION, on_progress);
  curl_easy_setopt(c.curl, CURLOPT_XFERINFODATA, &c);
  cc = curl_easy_perform(c.curl);
  {
    long status = 0;
    curl_easy_getinfo(c.curl, CURLINFO_RESPONSE_CODE, &status);
    rs->status = status;
  }
  if (cc == CURLE_OK) rc = ASMODEL_HTTP_OK;
  else if (c.aborted) rc = ASMODEL_HTTP_ABORTED;
  else if (rq->cancel && *rq->cancel) rc = ASMODEL_HTTP_CANCELLED;
  else if (cc == CURLE_OPERATION_TIMEDOUT && rq->timeout_ms > 0) {
    rc = ASMODEL_HTTP_TIMEOUT;
    if (error && error_size)
      snprintf(error, error_size, "deadline expired after %lld ms", (long long)rq->timeout_ms);
  } else if (error && error_size) {
    snprintf(error, error_size, "HTTP transport: %s",
             c.overflow ? "response too large" : curl_easy_strerror(cc));
  }
done:
  curl_slist_free_all(headers);
  curl_easy_cleanup(c.curl);
  return rc;
}

#elif defined(_WIN32)

int asmodel_http_available(void) { return 1; }

static wchar_t *widen(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
  wchar_t *w;
  if (n <= 0) return NULL;
  w = (wchar_t *)malloc((size_t)n * sizeof *w);
  if (w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) <= 0) {
    free(w); w = NULL;
  }
  return w;
}

/* WinHTTP timeouts apply per operation; spend the remaining total budget. */
static int budget(HINTERNET h, int64_t started, int64_t total) {
  int64_t remaining;
  int t;
  if (total <= 0) return 1;
  remaining = total - (os_monotonic_ms() - started);
  if (remaining <= 0) { SetLastError(ERROR_WINHTTP_TIMEOUT); return 0; }
  t = remaining > INT_MAX ? INT_MAX : (int)remaining;
  return WinHttpSetTimeouts(h, t, t, t, t) != 0;
}

static void query_header(HINTERNET request, DWORD info, const wchar_t *name,
                         char *dst, size_t cap) {
  wchar_t wide[256];
  DWORD size = sizeof wide;
  char narrow[512];
  int got;
  if (!WinHttpQueryHeaders(request, info, name ? name : WINHTTP_HEADER_NAME_BY_INDEX,
                           wide, &size, WINHTTP_NO_HEADER_INDEX)) return;
  got = WideCharToMultiByte(CP_UTF8, 0, wide, (int)(size / sizeof(wchar_t)), narrow,
                            (int)sizeof narrow - 1, NULL, NULL);
  if (got <= 0) return;
  narrow[got] = 0;
  copy_header_value(dst, cap, narrow, (size_t)got);
}

int asmodel_http_perform(const asmodel_http_request *rq, asmodel_http_response *rs,
                         char *error, size_t error_size) {
  HINTERNET session = NULL, connection = NULL, request = NULL;
  URL_COMPONENTS parts;
  wchar_t *wurl = NULL, *wheaders = NULL, *wmethod = NULL;
  core_buf hdr;
  DWORD status = 0, status_size = sizeof status, available = 0;
  int rc = ASMODEL_HTTP_TRANSPORT, aborted = 0;
  int64_t started = os_monotonic_ms();
  size_t max, body_len;
  if (error && error_size) error[0] = 0;
  if (!rq || !rs || !rq->url || !rq->method) return ASMODEL_HTTP_TRANSPORT;
  memset(rs, 0, sizeof *rs);
  core_buf_init(&hdr);
  max = rq->max_body ? rq->max_body : HTTP_DEFAULT_MAX_BODY;
  body_len = rq->body ? strlen(rq->body) : 0;
  if (rq->cancel && *rq->cancel) return ASMODEL_HTTP_CANCELLED;
  wurl = widen(rq->url);
  wmethod = widen(rq->method);
  if (!wurl || !wmethod) goto done;
  for (size_t i = 0; rq->headers && rq->headers[i]; i++)
    if (core_buf_appends(&hdr, rq->headers[i]) || core_buf_appends(&hdr, "\r\n")) goto done;
  if (hdr.data && !(wheaders = widen(hdr.data))) goto done;
  memset(&parts, 0, sizeof parts);
  parts.dwStructSize = sizeof parts;
  parts.dwHostNameLength = (DWORD)-1;
  parts.dwUrlPathLength = (DWORD)-1;
  parts.dwExtraInfoLength = (DWORD)-1;
  if (!WinHttpCrackUrl(wurl, 0, 0, &parts)) goto done;
  session = WinHttpOpen(L"asmodel", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) goto done;
  {
    int t = rq->timeout_ms > 0 ? (rq->timeout_ms > INT_MAX ? INT_MAX : (int)rq->timeout_ms) : 0;
    if (!WinHttpSetTimeouts(session, 10000, 10000, t, t)) goto done;
  }
  {
    wchar_t *host = (wchar_t *)malloc(((size_t)parts.dwHostNameLength + 1) * sizeof *host);
    if (!host) goto done;
    memcpy(host, parts.lpszHostName, (size_t)parts.dwHostNameLength * sizeof *host);
    host[parts.dwHostNameLength] = 0;
    connection = WinHttpConnect(session, host, parts.nPort, 0);
    free(host);
  }
  if (!connection) goto done;
  {
    size_t path_n = (size_t)parts.dwUrlPathLength + parts.dwExtraInfoLength;
    wchar_t *path = (wchar_t *)malloc((path_n + 1) * sizeof *path);
    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    if (!path) goto done;
    memcpy(path, parts.lpszUrlPath, (size_t)parts.dwUrlPathLength * sizeof *path);
    if (parts.dwExtraInfoLength)
      memcpy(path + parts.dwUrlPathLength, parts.lpszExtraInfo,
             (size_t)parts.dwExtraInfoLength * sizeof *path);
    path[path_n] = 0;
    request = WinHttpOpenRequest(connection, wmethod, path, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    free(path);
  }
  if (!request) goto done;
  {
    DWORD feature = WINHTTP_DISABLE_REDIRECTS;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &feature, sizeof feature))
      goto done;
  }
  if (body_len > 0xffffffffu) goto done;
  if (!budget(request, started, rq->timeout_ms) ||
      !WinHttpSendRequest(request, wheaders ? wheaders : WINHTTP_NO_ADDITIONAL_HEADERS,
                          wheaders ? (DWORD)-1L : 0, (void *)(rq->body ? rq->body : ""),
                          (DWORD)body_len, (DWORD)body_len, 0) ||
      !budget(request, started, rq->timeout_ms) ||
      !WinHttpReceiveResponse(request, NULL) ||
      !budget(request, started, rq->timeout_ms)) goto done;
  if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                           WINHTTP_NO_HEADER_INDEX)) goto done;
  rs->status = (long)status;
  query_header(request, WINHTTP_QUERY_RETRY_AFTER, NULL, rs->retry_after, sizeof rs->retry_after);
  query_header(request, WINHTTP_QUERY_CUSTOM, L"x-request-id", rs->request_id, sizeof rs->request_id);
  if (!rs->request_id[0])
    query_header(request, WINHTTP_QUERY_CUSTOM, L"openai-request-id", rs->request_id,
                 sizeof rs->request_id);
  do {
    char *chunk;
    DWORD got = 0;
    if (rq->cancel && *rq->cancel) { rc = ASMODEL_HTTP_CANCELLED; goto done; }
    if (!budget(request, started, rq->timeout_ms) ||
        !WinHttpQueryDataAvailable(request, &available) ||
        !budget(request, started, rq->timeout_ms)) goto done;
    if (!available) break;
    chunk = (char *)malloc(available);
    if (!chunk) goto done;
    if (!WinHttpReadData(request, chunk, available, &got) ||
        !budget(request, started, rq->timeout_ms) ||
        (!(rq->sink && rs->status >= 200 && rs->status < 300) && capture(rs, max, chunk, got))) {
      free(chunk); goto done;
    }
    if (got && rq->sink && rq->sink(rs->status, chunk, got, rq->sink_ud)) {
      free(chunk); aborted = 1; rc = ASMODEL_HTTP_ABORTED; goto done;
    }
    free(chunk);
  } while (available);
  rc = ASMODEL_HTTP_OK;
done:
  if (rc == ASMODEL_HTTP_TRANSPORT && !aborted) {
    DWORD err = GetLastError();
    if (rq->cancel && *rq->cancel) rc = ASMODEL_HTTP_CANCELLED;
    else if (rq->timeout_ms > 0 && (err == ERROR_WINHTTP_TIMEOUT ||
                                    os_monotonic_ms() - started >= rq->timeout_ms)) {
      rc = ASMODEL_HTTP_TIMEOUT;
      if (error && error_size)
        snprintf(error, error_size, "deadline expired after %lld ms", (long long)rq->timeout_ms);
    } else if (error && error_size && !error[0]) {
      snprintf(error, error_size, "WinHTTP transport error %lu", (unsigned long)err);
    }
  }
  if (request) WinHttpCloseHandle(request);
  if (connection) WinHttpCloseHandle(connection);
  if (session) WinHttpCloseHandle(session);
  free(wurl); free(wmethod); free(wheaders); core_buf_free(&hdr);
  return rc;
}

#else

int asmodel_http_available(void) { return 0; }

int asmodel_http_perform(const asmodel_http_request *rq, asmodel_http_response *rs,
                         char *error, size_t error_size) {
  (void)rq;
  (void)capture;
  (void)copy_header_value;
  if (rs) memset(rs, 0, sizeof *rs);
  if (error && error_size) snprintf(error, error_size, "asmodel built without an HTTP client");
  return ASMODEL_HTTP_UNAVAILABLE;
}

#endif
