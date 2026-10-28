/*
 * http.c — the generic capped GET used by page-watch triggers, and the
 * resumable download used by "asterism setup" for model files.
 *
 * Only http and https, also across redirects; a GET body is capped in
 * memory and an oversized one fails instead of being truncated, so a
 * trigger never compares half a page. No context: neither carries a token.
 */

#include "aslink_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int scheme_ok(const char *url) {
  static const char *const schemes[] = {"http://", "https://"};
  size_t i, j;
  for (i = 0; i < 2; i++) {
    const char *s = schemes[i];
    for (j = 0; s[j] && tolower((unsigned char)url[j]) == s[j]; j++) {}
    if (!s[j] && url[j]) return 1;
  }
  return 0;
}

/* http(s), printable, bounded. */
static int url_ok(const char *url) {
  size_t i;
  if (!url || !scheme_ok(url) || strlen(url) > 8192) return 0;
  for (i = 0; url[i]; i++)
    if ((unsigned char)url[i] <= 0x20 || (unsigned char)url[i] == 0x7F) return 0;
  return 1;
}

aslink_err aslink_http_get(const char *url, size_t max_bytes, long timeout_ms,
                           char **out_body, size_t *out_len, long *out_status) {
  aslink_xfer x;
  if (out_body) *out_body = NULL;
  if (out_len) *out_len = 0;
  if (out_status) *out_status = 0;
  if (!url_ok(url) || !out_body || max_bytes == 0 || timeout_ms < 0)
    return ASLINK_ERR_INVALID;
  memset(&x, 0, sizeof x);
  x.url = url;
  x.connect_timeout_ms = 10000;
  x.timeout_ms = timeout_ms ? timeout_ms : 30000;
  x.max_body = max_bytes;
  x.follow_redirects = 5;
  return aslink_perform(&x, out_body, out_len, out_status, NULL);
}

/* Bytes already in the part file; 0 when it does not exist. */
static int part_size(const char *path, uint64_t *out) {
  core_err e = os_file_size(path, out);
  if (e == CORE_ERR_NOT_FOUND) *out = 0;
  return e == CORE_OK || e == CORE_ERR_NOT_FOUND;
}

aslink_err aslink_http_download(const char *url, const char *part_path, uint64_t max_bytes,
                                const volatile int *cancel, aslink_progress_fn progress,
                                void *progress_ud, long *out_status, aslink_status *st) {
  aslink_xfer x;
  aslink_err e = ASLINK_OK;
  long status = 0;
  int attempt;
  if (out_status) *out_status = 0;
  if (st) memset(st, 0, sizeof *st);
  if (!url_ok(url) || !part_path || !*part_path || max_bytes == 0) return ASLINK_ERR_INVALID;
  for (attempt = 0; attempt < 2; attempt++) {
    uint64_t have;
    FILE *f;
    char *body = NULL;
    if (!part_size(part_path, &have)) return ASLINK_ERR_IO;
    if (have > max_bytes) return ASLINK_ERR_TOO_LARGE;
    if (have == max_bytes) return ASLINK_OK; /* nothing left to fetch */
    if (progress) progress(progress_ud, have);
    if (!(f = os_fopen(part_path, have ? "ab" : "wb"))) return ASLINK_ERR_IO;
    memset(&x, 0, sizeof x);
    x.url = url;
    x.connect_timeout_ms = 15000;
    x.max_body = ASLINK_ERROR_BODY_MAX;
    x.sink = f;
    x.sink_max = max_bytes - have;
    x.range_from = have;
    x.follow_redirects = 10;
    x.cancel = cancel;
    x.progress = progress;
    x.progress_ud = progress_ud;
    e = aslink_perform(&x, &body, NULL, &status, st);
    free(body);
    if (fclose(f) != 0 && e == ASLINK_OK) e = ASLINK_ERR_IO;
    if (out_status) *out_status = status;
    /* the server ignored the Range: start over from byte 0 */
    if (e == ASLINK_OK && status == 200 && have > 0) {
      if (os_truncate(part_path, 0) != CORE_OK) return ASLINK_ERR_IO;
      continue;
    }
    break;
  }
  if (e != ASLINK_OK) return e;
  if (status >= 200 && status < 300) return ASLINK_OK;
  return status == 404 ? ASLINK_ERR_NOT_FOUND : status == 416 ? ASLINK_ERR_PROTOCOL : ASLINK_ERR_IO;
}
