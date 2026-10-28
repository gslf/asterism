/*
 * transport_driver.c — drives aslink_perform against the two local servers
 * of test_transport.py. Usage: transport_driver <url-a> <url-b> <scratch-dir>.
 *
 * What a resumable download needs from the transport: Range with the 206
 * resume and the 200 restart, a ranged answer checked before it reaches
 * the part file, caps on a ranged sink, and redirects to a second host that
 * keep the Range. The exit code names the first failed step.
 */

#include "aslink.h"
#include "aslink_internal.h"
#include "core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOB_LEN 50000 /* the server's /blob: byte i is (7 i + 3) mod 251 */
#define PART     20000 /* bytes already in a part file                   */

#define CHECK(cond, code)                                                    \
  do {                                                                       \
    if (!(cond)) {                                                           \
      fprintf(stderr, "step %d failed at line %d: %s\n", code, __LINE__, #cond); \
      return code;                                                           \
    }                                                                        \
  } while (0)

static const char *url_a, *url_b, *scratch;
static unsigned char blob[BLOB_LEN];

/* base + path; a static buffer per slot. */
static const char *at(const char *base, const char *path) {
  static char slots[4][512];
  static unsigned next;
  char *p = slots[next++ % 4];
  snprintf(p, sizeof slots[0], "%s%s", base, path);
  return p;
}

typedef struct {
  const char *url;
  const char *dest;   /* sink file, NULL = keep the body in memory */
  const char *mode;   /* fopen mode of the sink                    */
  uint64_t from;      /* range_from                                */
  uint64_t sink_max;  /* 0 = BLOB_LEN                              */
  int redirects;
} get_req;

typedef struct {
  long status;
  char *body;
  size_t len;
  aslink_status st;
} get_res;

static aslink_err get(const get_req *rq, get_res *rs) {
  aslink_xfer x;
  FILE *f = NULL;
  aslink_err e;
  memset(rs, 0, sizeof *rs);
  if (rq->dest && !(f = os_fopen(rq->dest, rq->mode))) return ASLINK_ERR_IO;
  memset(&x, 0, sizeof x);
  x.url = rq->url;
  x.connect_timeout_ms = 5000;
  x.timeout_ms = 10000;
  x.max_body = 100000;
  x.sink = f;
  x.sink_max = rq->sink_max ? rq->sink_max : BLOB_LEN;
  x.range_from = rq->from;
  x.follow_redirects = rq->redirects;
  e = aslink_perform(&x, &rs->body, &rs->len, &rs->status, &rs->st);
  if (f && fclose(f) != 0 && e == ASLINK_OK) e = ASLINK_ERR_IO;
  return e;
}

/* The file holds exactly the first n bytes of the blob. */
static int holds(const char *path, size_t n) {
  char *got = NULL;
  size_t len = 0;
  int ok;
  if (os_read_file(path, &got, &len) != CORE_OK) return 0;
  ok = len == n && memcmp(got, blob, n) == 0;
  free(got);
  return ok;
}

static int make_part(const char *path) { return os_write_file(path, blob, PART) == CORE_OK; }

/* ── steps ────────────────────────────────────────────────────────────── */

static int step_fresh_and_resume(void) {
  char part[512];
  get_req rq;
  get_res rs;
  snprintf(part, sizeof part, "%s/model.part", scratch);
  memset(&rq, 0, sizeof rq);
  /* from scratch: a plain 200 */
  rq.url = at(url_a, "/blob");
  rq.dest = part;
  rq.mode = "wb";
  CHECK(get(&rq, &rs) == ASLINK_OK, 10);
  CHECK(rs.status == 200 && rs.len == 0 && rs.st.http_status == 200, 11);
  free(rs.body);
  CHECK(holds(part, BLOB_LEN), 12);
  /* resume: 206 from the part size, appended */
  CHECK(make_part(part), 13);
  rq.mode = "ab";
  rq.from = PART;
  rq.sink_max = BLOB_LEN - PART;
  CHECK(get(&rq, &rs) == ASLINK_OK, 14);
  CHECK(rs.status == 206 && rs.st.http_status == 206, 15);
  free(rs.body);
  CHECK(holds(part, BLOB_LEN), 16);
  /* one byte under the remaining length: the cap still holds */
  CHECK(make_part(part), 17);
  rq.sink_max = BLOB_LEN - PART - 1;
  CHECK(get(&rq, &rs) == ASLINK_ERR_TOO_LARGE, 18);
  /* already complete: 416, its body kept for the caller */
  CHECK(make_part(part), 19);
  rq.from = BLOB_LEN;
  rq.sink_max = 0;
  CHECK(get(&rq, &rs) == ASLINK_OK, 20);
  CHECK(rs.status == 416 && strcmp(rs.body, "range not satisfiable") == 0, 21);
  free(rs.body);
  CHECK(holds(part, PART), 22);
  return 0;
}

static int step_range_ignored(void) {
  char part[512];
  get_req rq;
  get_res rs;
  snprintf(part, sizeof part, "%s/ignored.part", scratch);
  CHECK(make_part(part), 30);
  memset(&rq, 0, sizeof rq);
  rq.url = at(url_a, "/blob-norange");
  rq.dest = part;
  rq.mode = "ab";
  rq.from = PART;
  /* sized for the rest only: the full 200 is reported, not refused */
  rq.sink_max = BLOB_LEN - PART;
  CHECK(get(&rq, &rs) == ASLINK_OK, 31);
  CHECK(rs.status == 200 && rs.len == 0, 32);
  free(rs.body);
  CHECK(holds(part, PART), 33); /* nothing appended */
  /* the caller restarts from byte 0 */
  rq.mode = "wb";
  rq.from = 0;
  rq.sink_max = 0;
  CHECK(get(&rq, &rs) == ASLINK_OK && rs.status == 200, 34);
  free(rs.body);
  CHECK(holds(part, BLOB_LEN), 35);
  /* a 206 at another offset never reaches the part file */
  CHECK(make_part(part), 36);
  rq.url = at(url_a, "/blob-wrong");
  rq.mode = "ab";
  rq.from = PART;
  CHECK(get(&rq, &rs) == ASLINK_ERR_PROTOCOL, 37);
  CHECK(rs.st.http_status == 206 && rs.st.message[0], 38);
  CHECK(holds(part, PART), 39);
  rq.url = at(url_a, "/blob-bare-206"); /* no Content-Range at all */
  CHECK(get(&rq, &rs) == ASLINK_ERR_PROTOCOL, 40);
  CHECK(holds(part, PART), 41);
  return 0;
}

static int step_range_in_memory(void) {
  get_req rq;
  get_res rs;
  memset(&rq, 0, sizeof rq);
  rq.url = at(url_a, "/blob");
  rq.from = BLOB_LEN - 10;
  CHECK(get(&rq, &rs) == ASLINK_OK, 50);
  CHECK(rs.status == 206 && rs.len == 10 && memcmp(rs.body, blob + BLOB_LEN - 10, 10) == 0,
        51);
  free(rs.body);
  /* without a sink a 200 is simply the whole body */
  rq.url = at(url_a, "/blob-norange");
  CHECK(get(&rq, &rs) == ASLINK_OK, 52);
  CHECK(rs.status == 200 && rs.len == BLOB_LEN && memcmp(rs.body, blob, BLOB_LEN) == 0, 53);
  free(rs.body);
  return 0;
}

static int step_redirects(void) {
  char part[512], *body = NULL;
  size_t len = 0;
  long status = 0;
  get_req rq;
  get_res rs;
  snprintf(part, sizeof part, "%s/moved.part", scratch);
  CHECK(make_part(part), 60);
  /* to the second host, the Range carried along */
  memset(&rq, 0, sizeof rq);
  rq.url = at(url_a, "/moved/blob");
  rq.dest = part;
  rq.mode = "ab";
  rq.from = PART;
  rq.redirects = 5;
  CHECK(get(&rq, &rs) == ASLINK_OK, 61);
  CHECK(rs.status == 206, 62);
  free(rs.body);
  CHECK(holds(part, BLOB_LEN), 63);
  /* not followed: the 302 itself, nothing in the part file */
  CHECK(make_part(part), 64);
  rq.redirects = 0;
  CHECK(get(&rq, &rs) == ASLINK_OK, 65);
  CHECK(rs.status == 302, 66);
  free(rs.body);
  CHECK(holds(part, PART), 67);
  /* a -> b -> a: two hops, over a limit of one */
  rq.url = at(url_a, "/moved/round-trip");
  rq.dest = NULL;
  rq.from = 0;
  rq.redirects = 1;
  CHECK(get(&rq, &rs) == ASLINK_ERR_PROTOCOL, 68);
  rq.redirects = 2;
  CHECK(get(&rq, &rs) == ASLINK_OK, 69);
  CHECK(rs.status == 200 && strcmp(rs.body, "back home") == 0, 70);
  free(rs.body);
  /* the page-watch GET across hosts */
  CHECK(aslink_http_get(at(url_a, "/moved/page"), 1000, 5000, &body, &len, &status) ==
            ASLINK_OK, 71);
  CHECK(status == 200 && strcmp(body, "second host") == 0, 72);
  free(body);
  /* b does not exist on a: the redirect really changed host */
  CHECK(aslink_http_get(at(url_b, "/page"), 1000, 5000, &body, &len, &status) == ASLINK_OK,
        73);
  CHECK(status == 200 && strcmp(body, "second host") == 0, 74);
  free(body);
  return 0;
}

/* aslink_http_download: what "asterism setup" uses for model files. */
static void on_progress(void *ud, uint64_t have) {
  uint64_t *seen = ud;
  if (seen[0] == UINT64_MAX) seen[0] = have; /* first report */
  seen[1] = have;                            /* last report  */
}

static int step_download(void) {
  char part[512];
  uint64_t seen[2];
  aslink_status st;
  long status;
  snprintf(part, sizeof part, "%s/download.part", scratch);
  remove(part);
  /* fresh, with progress up to the full length */
  seen[0] = UINT64_MAX;
  CHECK(aslink_http_download(at(url_a, "/blob"), part, BLOB_LEN, NULL, on_progress, seen,
                             &status, &st) == ASLINK_OK, 80);
  CHECK(status == 200 && holds(part, BLOB_LEN), 81);
  CHECK(seen[0] == 0 && seen[1] == BLOB_LEN, 82);
  /* complete already: no request at all */
  CHECK(aslink_http_download(at(url_a, "/blob"), part, BLOB_LEN, NULL, NULL, NULL, &status,
                             &st) == ASLINK_OK && status == 0, 83);
  /* resumed from the part */
  CHECK(make_part(part), 84);
  seen[0] = UINT64_MAX;
  CHECK(aslink_http_download(at(url_a, "/blob"), part, BLOB_LEN, NULL, on_progress, seen,
                             &status, &st) == ASLINK_OK, 85);
  CHECK(status == 206 && holds(part, BLOB_LEN) && seen[0] == PART && seen[1] == BLOB_LEN, 86);
  /* the server ignores the Range: restarted from zero */
  CHECK(make_part(part), 87);
  CHECK(aslink_http_download(at(url_a, "/blob-norange"), part, BLOB_LEN, NULL, NULL, NULL,
                             &status, &st) == ASLINK_OK, 88);
  CHECK(status == 200 && holds(part, BLOB_LEN), 89);
  /* longer than announced, and a missing file */
  remove(part);
  CHECK(aslink_http_download(at(url_a, "/blob"), part, BLOB_LEN - 1, NULL, NULL, NULL,
                             &status, &st) == ASLINK_ERR_TOO_LARGE, 90);
  remove(part);
  CHECK(aslink_http_download(at(url_a, "/nothing"), part, BLOB_LEN, NULL, NULL, NULL,
                             &status, &st) == ASLINK_ERR_NOT_FOUND && status == 404, 91);
  CHECK(aslink_http_download("ftp://x/blob", part, BLOB_LEN, NULL, NULL, NULL, &status,
                             &st) == ASLINK_ERR_INVALID, 92);
  remove(part);
  return 0;
}

int main(int argc, char **argv) {
  int rc;
  size_t i;
  if (argc != 4) return 2;
  url_a = argv[1];
  url_b = argv[2];
  scratch = argv[3];
  for (i = 0; i < BLOB_LEN; i++) blob[i] = (unsigned char)((i * 7 + 3) % 251);
  if ((rc = step_fresh_and_resume()) != 0) return rc;
  if ((rc = step_range_ignored()) != 0) return rc;
  if ((rc = step_range_in_memory()) != 0) return rc;
  if ((rc = step_redirects()) != 0) return rc;
  if ((rc = step_download()) != 0) return rc;
  fprintf(stderr, "transport driver: all steps passed\n");
  return 0;
}
