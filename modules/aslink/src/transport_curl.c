/*
 * transport_curl.c — the libcurl backend of aslink.
 *
 * Every exchange gets its own easy handle, so concurrent calls from several
 * threads share nothing but the process-wide curl_global_init, which runs
 * exactly once (pthread_once / InitOnceExecuteOnce) and is never undone:
 * curl_global_cleanup cannot be made safe while other threads may still use
 * libcurl. Signals are off (CURLOPT_NOSIGNAL), protocols are restricted to
 * http/https, and the progress callback polls the context's cancel flag;
 * libcurl calls it at least about once per second while idle.
 */

#include "aslink_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#ifndef ASTERISM_VERSION
#define ASTERISM_VERSION "0"
#endif

static CURLcode g_init_rc = CURLE_FAILED_INIT;
static void global_init(void) { g_init_rc = curl_global_init(CURL_GLOBAL_DEFAULT); }

#if defined(ASTERISM_NO_THREADS)
static int g_init_done;
static int curl_ready(void) {
  if (!g_init_done) {
    g_init_done = 1;
    global_init();
  }
  return g_init_rc == CURLE_OK;
}
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK once_cb(PINIT_ONCE once, PVOID param, PVOID *ctx) {
  (void)once;
  (void)param;
  (void)ctx;
  global_init();
  return TRUE;
}
static int curl_ready(void) {
  InitOnceExecuteOnce(&g_once, once_cb, NULL, NULL);
  return g_init_rc == CURLE_OK;
}
#else
#include <pthread.h>
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int curl_ready(void) {
  pthread_once(&g_once, global_init);
  return g_init_rc == CURLE_OK;
}
#endif

typedef struct {
  const aslink_xfer *x;
  CURL *h;
  aslink_body b;
  char content_range[96]; /* of the latest response, ranged requests only */
} xfer_state;

static int64_t content_length(CURL *h) {
#if LIBCURL_VERSION_NUM >= 0x073700 /* 7.55.0 */
  curl_off_t n = -1;
  if (curl_easy_getinfo(h, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &n) != CURLE_OK) return -1;
  return n < 0 ? -1 : (int64_t)n;
#else
  double n = -1;
  if (curl_easy_getinfo(h, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &n) != CURLE_OK) return -1;
  return n < 0 ? -1 : (int64_t)n;
#endif
}

/* Routed on the first byte: the status is final by then. */
static size_t on_data(char *ptr, size_t sz, size_t nm, void *ud) {
  xfer_state *s = (xfer_state *)ud;
  size_t n = sz * nm;
  if (!s->b.routed) {
    long status = 0;
    curl_easy_getinfo(s->h, CURLINFO_RESPONSE_CODE, &status);
    if (!aslink_body_route(&s->b, status, s->content_range, content_length(s->h))) return 0;
  }
  return aslink_body_take(&s->b, ptr, n) ? n : 0;
}

/* Keeps the Content-Range of the latest response (redirects included). */
static size_t on_header(char *ptr, size_t sz, size_t nm, void *ud) {
  static const char name[] = "content-range:";
  xfer_state *s = (xfer_state *)ud;
  size_t n = sz * nm, i, k = sizeof name - 1;
  if (n >= 5 && memcmp(ptr, "HTTP/", 5) == 0) {
    s->content_range[0] = 0;
    return n;
  }
  if (n <= k) return n;
  for (i = 0; i < k; i++)
    if (tolower((unsigned char)ptr[i]) != name[i]) return n;
  ptr += k;
  n -= k;
  while (n && (ptr[n - 1] == '\r' || ptr[n - 1] == '\n')) n--;
  if (n >= sizeof s->content_range) n = sizeof s->content_range - 1;
  memcpy(s->content_range, ptr, n);
  s->content_range[n] = 0;
  return sz * nm;
}

static int on_progress(void *ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
  const xfer_state *s = (const xfer_state *)ud;
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  return s->x->cancel && ASLINK_FLAG_GET(s->x->cancel) ? 1 : 0;
}

static void restrict_protocols(CURL *h, int redirects) {
#if LIBCURL_VERSION_NUM >= 0x075500 /* 7.85.0: string form, the bitmask is deprecated */
  curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
  if (redirects) curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
  curl_easy_setopt(h, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
  if (redirects)
    curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
}

static aslink_err build_form(CURL *h, const aslink_xfer *x, curl_mime **out) {
  curl_mime *mime = curl_mime_init(h);
  size_t i;
  *out = mime;
  if (!mime) return ASLINK_ERR_NOMEM;
  for (i = 0; i < x->form_n; i++) {
    const aslink_form_field *f = &x->form[i];
    curl_mimepart *part = curl_mime_addpart(mime);
    if (!part || curl_mime_name(part, f->name) != CURLE_OK) return ASLINK_ERR_NOMEM;
    if (f->file_path) {
      if (curl_mime_filedata(part, f->file_path) != CURLE_OK ||
          curl_mime_filename(part, f->file_name) != CURLE_OK ||
          curl_mime_type(part, "application/octet-stream") != CURLE_OK)
        return ASLINK_ERR_IO;
    } else if (curl_mime_data(part, f->value ? f->value : "", CURL_ZERO_TERMINATED) !=
               CURLE_OK) {
      return ASLINK_ERR_NOMEM;
    }
  }
  return ASLINK_OK;
}

aslink_err aslink_perform(const aslink_xfer *x, char **out_body, size_t *out_len,
                          long *out_status, aslink_status *st) {
  CURL *h = NULL;
  curl_mime *mime = NULL;
  struct curl_slist *headers = NULL, *more;
  char errbuf[CURL_ERROR_SIZE], range[32];
  xfer_state s;
  CURLcode rc;
  long status = 0;
  aslink_err e = ASLINK_OK;
  uint64_t cap;

  if (out_body) *out_body = NULL;
  if (out_len) *out_len = 0;
  if (out_status) *out_status = 0;
  memset(&s, 0, sizeof s);
  s.x = x;
  aslink_body_init(&s.b, x);
  errbuf[0] = 0;
  if (x->cancel && ASLINK_FLAG_GET(x->cancel)) {
    aslink_status_msg(st, NULL, "cancelled");
    return ASLINK_ERR_CANCELLED;
  }
  if (!curl_ready()) {
    aslink_status_msg(st, NULL, "libcurl initialisation failed");
    return ASLINK_ERR_IO;
  }
  h = curl_easy_init();
  if (!h) return ASLINK_ERR_NOMEM;
  s.h = h;

  /* No "Expect: 100-continue" round trip on uploads. */
  headers = curl_slist_append(NULL, "Expect:");
  if (headers && x->json_body) {
    more = curl_slist_append(headers, "Content-Type: application/json");
    if (!more) {
      curl_slist_free_all(headers);
      headers = NULL;
    } else {
      headers = more;
    }
  }
  if (!headers) {
    e = ASLINK_ERR_NOMEM;
    goto done;
  }
  curl_easy_setopt(h, CURLOPT_URL, x->url);
  restrict_protocols(h, x->follow_redirects);
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(h, CURLOPT_USERAGENT, "asterism/" ASTERISM_VERSION);
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
  if (x->connect_timeout_ms > 0)
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, x->connect_timeout_ms);
  if (x->timeout_ms > 0) curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, x->timeout_ms);
  if (x->follow_redirects > 0) {
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, (long)x->follow_redirects);
  } else {
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
  }
  /* A 200 to a ranged download is routed, not refused for its length. */
  cap = x->sink ? x->sink_max : x->max_body;
  if (cap > 0 && cap < (uint64_t)INT64_MAX && !(x->range_from && x->sink))
    curl_easy_setopt(h, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)cap);
  if (x->range_from) {
    snprintf(range, sizeof range, "%llu-", (unsigned long long)x->range_from);
    curl_easy_setopt(h, CURLOPT_RANGE, range);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &s);
  }
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_data);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &s);
  curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, on_progress);
  curl_easy_setopt(h, CURLOPT_XFERINFODATA, &s);
  if (x->json_body) {
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, x->json_body);
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(x->json_body));
  } else if (x->form_n) {
    if ((e = build_form(h, x, &mime)) != ASLINK_OK) {
      aslink_status_msg(st, NULL, "cannot prepare the upload");
      goto done;
    }
    curl_easy_setopt(h, CURLOPT_MIMEPOST, mime);
  }

  rc = curl_easy_perform(h);
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
  if (st) st->http_status = (int)status;
  if (rc == CURLE_WRITE_ERROR && s.b.skip) rc = CURLE_OK;
  /* An empty ranged body never reached on_data: check its range anyway. */
  if (rc == CURLE_OK && x->range_from && !s.b.routed &&
      !aslink_body_route(&s.b, status, s.content_range, -1))
    rc = CURLE_WRITE_ERROR;
  switch (rc) {
    case CURLE_OK: break;
    case CURLE_ABORTED_BY_CALLBACK:
      e = x->cancel && ASLINK_FLAG_GET(x->cancel) ? ASLINK_ERR_CANCELLED : ASLINK_ERR_IO;
      break;
    case CURLE_OPERATION_TIMEDOUT: e = ASLINK_ERR_TIMEOUT; break;
    case CURLE_FILESIZE_EXCEEDED:
      s.b.over = 1;
      e = ASLINK_ERR_TOO_LARGE;
      break;
    case CURLE_WRITE_ERROR:
      e = aslink_body_error(&s.b, x->token, st);
      if (e != ASLINK_OK) goto done;
      e = ASLINK_ERR_IO;
      break;
    case CURLE_OUT_OF_MEMORY: e = ASLINK_ERR_NOMEM; break;
    case CURLE_TOO_MANY_REDIRECTS:
    case CURLE_UNSUPPORTED_PROTOCOL: e = ASLINK_ERR_PROTOCOL; break;
    default: e = ASLINK_ERR_IO; break;
  }
  if (e != ASLINK_OK) {
    if (e == ASLINK_ERR_CANCELLED)
      aslink_status_msg(st, x->token, "cancelled");
    else if (e == ASLINK_ERR_TOO_LARGE)
      (void)aslink_body_error(&s.b, x->token, st);
    else
      aslink_status_msg(st, x->token, "transport: %s%s%s", curl_easy_strerror(rc),
                        errbuf[0] ? ": " : "", errbuf);
    goto done;
  }
  if ((e = aslink_body_finish(&s.b, out_body, out_len)) != ASLINK_OK) goto done;
  if (out_status) *out_status = status;

done:
  aslink_body_free(&s.b);
  if (mime) curl_mime_free(mime);
  curl_slist_free_all(headers);
  curl_easy_cleanup(h);
  return e;
}

int aslink_has_transport(void) { return 1; }
