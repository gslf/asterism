/*
 * transport.c — the response rules shared by the two HTTP backends
 * (transport_curl.c, transport_winhttp.c), so both treat a body alike:
 * a 2xx goes to the sink when there is one, any other body stays in
 * memory under its cap, and a ranged request is checked before a byte of
 * it is taken.
 */

#include "aslink_internal.h"

#include <ctype.h>
#include <string.h>

void aslink_body_init(aslink_body *b, const aslink_xfer *x) {
  memset(b, 0, sizeof *b);
  b->x = x;
  core_buf_init(&b->mem);
}

void aslink_body_free(aslink_body *b) { core_buf_free(&b->mem); }

/* First byte of a "bytes first-last/length" Content-Range. */
static int range_first(const char *v, uint64_t *out) {
  static const char unit[] = "bytes";
  uint64_t n = 0;
  size_t i;
  int digits = 0;
  while (*v == ' ' || *v == '\t') v++;
  for (i = 0; unit[i]; i++)
    if (tolower((unsigned char)v[i]) != unit[i]) return 0;
  v += i;
  if (*v != ' ') return 0;
  while (*v == ' ') v++;
  for (; *v >= '0' && *v <= '9'; v++, digits++) {
    if (n > (UINT64_MAX - 9) / 10) return 0;
    n = n * 10 + (uint64_t)(*v - '0');
  }
  if (!digits || *v != '-') return 0;
  *out = n;
  return 1;
}

int aslink_body_route(aslink_body *b, long status, const char *content_range,
                      int64_t content_length) {
  const aslink_xfer *x = b->x;
  uint64_t first = 0;
  b->routed = 1;
  b->to_sink = x->sink && status >= 200 && status <= 299;
  b->cap = b->to_sink ? x->sink_max : x->sink ? ASLINK_ERROR_BODY_MAX : x->max_body;
  if (x->range_from && status == 206 &&
      (!content_range || !range_first(content_range, &first) || first != x->range_from)) {
    b->bad_range = 1;
    return 0;
  }
  if (x->range_from && x->sink && status == 200) {
    /* Range ignored: the sink holds a prefix, the caller restarts. */
    b->to_sink = 0;
    b->skip = 1;
    return 1;
  }
  if (content_length >= 0 && (uint64_t)content_length > b->cap) {
    b->over = 1;
    return 0;
  }
  return 1;
}

int aslink_body_take(aslink_body *b, const void *p, size_t n) {
  if (b->skip) return 0;
  if (n > b->cap || b->got > b->cap - n) {
    b->over = 1;
    return 0;
  }
  if (b->to_sink) {
    if (n && fwrite(p, 1, n, b->x->sink) != n) {
      b->write_failed = 1;
      return 0;
    }
  } else if (core_buf_append(&b->mem, p, n) != CORE_OK) {
    b->nomem = 1;
    return 0;
  }
  b->got += n;
  if (b->to_sink && b->x->progress)
    b->x->progress(b->x->progress_ud, b->x->range_from + b->got);
  return 1;
}

aslink_err aslink_body_error(const aslink_body *b, const char *token, aslink_status *st) {
  const aslink_xfer *x = b->x;
  if (b->over) {
    uint64_t cap = b->routed ? b->cap : x->sink ? x->sink_max : x->max_body;
    aslink_status_msg(st, token, "response exceeds the %llu byte cap",
                      (unsigned long long)cap);
    return ASLINK_ERR_TOO_LARGE;
  }
  if (b->bad_range) {
    aslink_status_msg(st, token, "the server did not answer the requested byte range");
    return ASLINK_ERR_PROTOCOL;
  }
  if (b->nomem) {
    aslink_status_msg(st, token, "out of memory");
    return ASLINK_ERR_NOMEM;
  }
  if (b->write_failed) {
    aslink_status_msg(st, token, "cannot write the downloaded file");
    return ASLINK_ERR_IO;
  }
  return ASLINK_OK;
}

aslink_err aslink_body_finish(aslink_body *b, char **out_body, size_t *out_len) {
  if (core_buf_appends(&b->mem, "") != CORE_OK) return ASLINK_ERR_NOMEM;
  if (out_len) *out_len = b->mem.len;
  if (out_body) *out_body = core_buf_detach(&b->mem);
  return ASLINK_OK;
}
