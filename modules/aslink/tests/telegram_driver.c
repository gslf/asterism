/*
 * telegram_driver.c — drives aslink against the scripted Bot API server of
 * test_telegram.py. Usage: telegram_driver <base-url> <scratch-dir>.
 *
 * The server checks every request body; this side checks results, error
 * mapping, timing (long poll, cancel, concurrency) and that no message
 * leaks a token. The exit code names the first failed step.
 */

#include "aslink.h"
#include "aslink_internal.h" /* ASLINK_FLAG_SET / ASLINK_FLAG_GET */
#include "core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#define GOOD "123456:TEST-token_abc"
#define RATE "222222:RATE-limited"
#define BAD  "333333:WRONG-token"
#define HANG "444444:HANG-forever"
#define SLOW "555555:SLOW-server"
#define JUNK "666666:JUNK-replies"

#define CHECK(cond, code)                                                    \
  do {                                                                       \
    if (!(cond)) {                                                           \
      fprintf(stderr, "step %d failed at line %d: %s\n", code, __LINE__, #cond); \
      return code;                                                           \
    }                                                                        \
  } while (0)

static const char *base_url;
static const char *scratch;

static aslink_ctx *open_ctx_connect(const char *token, const char *base,
                                    long request_timeout_ms, long connect_timeout_ms) {
  aslink_telegram_params p;
  aslink_ctx *c = NULL;
  memset(&p, 0, sizeof p);
  p.api_base = base ? base : base_url;
  p.token = token;
  p.connect_timeout_ms = connect_timeout_ms;
  p.request_timeout_ms = request_timeout_ms;
  p.transfer_timeout_ms = 10000;
  return aslink_telegram_open(&p, &c) == ASLINK_OK ? c : NULL;
}

static aslink_ctx *open_ctx(const char *token, const char *base, long request_timeout_ms) {
  return open_ctx_connect(token, base, request_timeout_ms, 2000);
}

/* No secret half of any test token may reach a message. */
static int clean(const aslink_status *st) {
  static const char *const secrets[] = {"TEST-token", "RATE-limited", "WRONG-token",
                                        "HANG-forever", "SLOW-server", "JUNK-replies"};
  size_t i;
  for (i = 0; i < sizeof secrets / sizeof secrets[0]; i++) {
    if (strstr(st->message, secrets[i])) {
      fprintf(stderr, "token leaked: %s\n", st->message);
      return 0;
    }
  }
  return 1;
}

/* name inside the scratch directory; a static buffer per slot. */
static const char *path_in(const char *name) {
  static char slots[8][512];
  static unsigned next;
  char *p = slots[next++ % 8];
  snprintf(p, sizeof slots[0], "%s/%s", scratch, name);
  return p;
}

/* Only the expected entries in the scratch directory: no temp leftovers. */
static size_t scratch_entries(void) {
  char **names = NULL;
  size_t n = 0;
  if (os_list_dir(scratch, &names, &n) != CORE_OK) return (size_t)-1;
  os_free_names(names, n);
  return n;
}

/* ── steps ────────────────────────────────────────────────────────────── */

static int step_get_me(void) {
  aslink_ctx *c = open_ctx(GOOD, NULL, 0);
  aslink_bot_info bot;
  aslink_status st;
  CHECK(c != NULL, 10);
  CHECK(aslink_get_me(c, &bot, &st) == ASLINK_OK, 11);
  CHECK(bot.id == 999 && strcmp(bot.username, "asterism_test_bot") == 0, 12);
  CHECK(bot.has_topics_enabled == 1 && bot.allows_users_to_create_topics == 0, 13);
  CHECK(st.http_status == 200, 14);
  aslink_close(c);
  return 0;
}

static int step_updates(void) {
  aslink_ctx *c = open_ctx(GOOD, NULL, 0);
  aslink_update *u = NULL;
  aslink_status st;
  size_t n = 0;
  int64_t t0;
  CHECK(c != NULL, 20);
  CHECK(aslink_get_updates(c, 0, 0, &u, &n, &st) == ASLINK_OK, 21);
  CHECK(n == 3, 22);
  CHECK(u[0].update_id == 100 && u[0].kind == ASLINK_UPDATE_MESSAGE, 23);
  CHECK(u[0].text && strcmp(u[0].text, "/status") == 0 && u[0].from_id == 5, 24);
  CHECK(u[1].kind == ASLINK_UPDATE_CALLBACK && strcmp(u[1].callback_data, "ap:1:ok") == 0,
        25);
  CHECK(u[2].kind == ASLINK_UPDATE_STOP && u[2].draft_id == 77, 26);
  aslink_updates_free(u, n);
  /* long poll: the server holds the request for the whole timeout */
  t0 = os_monotonic_ms();
  CHECK(aslink_get_updates(c, 103, 1, &u, &n, &st) == ASLINK_OK, 27);
  CHECK(n == 0 && u == NULL, 28);
  CHECK(os_monotonic_ms() - t0 >= 800, 29);
  aslink_close(c);
  return 0;
}

static int step_send_and_edit(void) {
  static const aslink_button buttons[] = {
    {"Yes \xe2\x9c\x93", "ap:1:ok", NULL},
    {"No", "ap:1:no", NULL},
    {"Evidenza", NULL, "https://example.org/e"},
  };
  static const aslink_command cmds[] = {{"status", "Task status"}, {"stop", "Stop"}};
  aslink_keyboard kb = {buttons, 3, 2};
  aslink_send_opts o;
  aslink_draft_opts d = {NULL, 1, 0};
  aslink_ctx *c = open_ctx(GOOD, NULL, 0);
  aslink_status st;
  int64_t id = 0, thread = 0;
  char *html;
  CHECK(c != NULL, 30);
  memset(&o, 0, sizeof o);
  html = aslink_html_escape("sending <report> & co?");
  CHECK(html != NULL, 31);
  o.parse_mode = "HTML";
  o.keyboard = &kb;
  o.reply_to = 42;
  o.silent = 1;
  o.no_preview = 1;
  {
    char text[256];
    snprintf(text, sizeof text, "<b>Approve</b> %s", html);
    CHECK(aslink_send_message(c, 5, 9, text, &o, &id, &st) == ASLINK_OK, 32);
  }
  free(html);
  CHECK(id == 500, 33);
  memset(&o, 0, sizeof o);
  CHECK(aslink_edit_message(c, 5, 500, "Approvato \xe2\x9c\x85", &o, &st) == ASLINK_OK, 34);
  CHECK(aslink_edit_keyboard(c, 5, 500, NULL, &st) == ASLINK_OK, 35);
  CHECK(aslink_send_draft(c, 5, 9, 77, "Sto scrivendo", &d, &st) == ASLINK_OK, 36);
  CHECK(aslink_send_chat_action(c, 5, 9, "typing", &st) == ASLINK_OK, 37);
  CHECK(aslink_answer_callback(c, "cbq-1", "Fatto", 0, &st) == ASLINK_OK, 38);
  CHECK(aslink_create_topic(c, 5, "Task: report", &thread, &st) == ASLINK_OK, 39);
  CHECK(thread == 321, 40);
  CHECK(aslink_set_commands(c, cmds, 2, &st) == ASLINK_OK, 41);
  /* local validation never reaches the server */
  CHECK(aslink_send_message(c, 5, 0, "", NULL, NULL, &st) == ASLINK_ERR_INVALID, 42);
  aslink_close(c);
  return 0;
}

static int step_errors(void) {
  aslink_ctx *c;
  aslink_status st;
  aslink_bot_info bot;
  int64_t t0;
  /* 429 */
  c = open_ctx(RATE, NULL, 0);
  CHECK(c != NULL, 50);
  CHECK(aslink_send_message(c, 5, 0, "flood", NULL, NULL, &st) == ASLINK_ERR_RATE_LIMITED,
        51);
  CHECK(st.retry_after_s == 7 && st.http_status == 429 && st.telegram_code == 429, 52);
  CHECK(clean(&st), 53);
  aslink_close(c);
  /* 401, with the token echoed back by the server: redacted */
  c = open_ctx(BAD, NULL, 0);
  CHECK(c != NULL, 54);
  CHECK(aslink_get_me(c, &bot, &st) == ASLINK_ERR_UNAUTHORIZED, 55);
  CHECK(st.http_status == 401 && st.telegram_code == 401, 56);
  CHECK(strstr(st.message, "<redacted>") != NULL && clean(&st), 57);
  aslink_close(c);
  /* timeout */
  c = open_ctx(SLOW, NULL, 300);
  CHECK(c != NULL, 58);
  t0 = os_monotonic_ms();
  CHECK(aslink_get_me(c, &bot, &st) == ASLINK_ERR_TIMEOUT, 59);
  CHECK(os_monotonic_ms() - t0 < 1500 && clean(&st), 60);
  aslink_close(c);
  /* malformed replies */
  c = open_ctx(JUNK, NULL, 0);
  CHECK(c != NULL, 61);
  CHECK(aslink_get_me(c, &bot, &st) == ASLINK_ERR_PROTOCOL, 62);
  CHECK(aslink_send_chat_action(c, 5, 0, "typing", &st) == ASLINK_ERR_IO, 63);
  CHECK(st.http_status == 502 && clean(&st), 64);
  aslink_close(c);
  /* nothing listens on port 1; Windows retries a refused connect for about
   * two seconds, longer than the usual connect timeout */
  c = open_ctx_connect(GOOD, "http://127.0.0.1:1", 0, 10000);
  CHECK(c != NULL, 65);
  CHECK(aslink_get_me(c, &bot, &st) == ASLINK_ERR_IO, 66);
  CHECK(st.http_status == 0 && st.message[0] && clean(&st), 67);
  aslink_close(c);
  return 0;
}

static int step_files(void) {
  aslink_ctx *c = open_ctx(GOOD, NULL, 0);
  aslink_status st;
  const char *dest = path_in("download.txt"), *upload = path_in("report.txt");
  char *got = NULL;
  size_t len = 0, before;
  static const char payload[] = "line one\nline \xe2\x9c\x93\n\x00\x01\x02 binary";
  CHECK(c != NULL, 70);
  before = scratch_entries();
  CHECK(aslink_download_file(c, "doc-ok", dest, 1000, &st) == ASLINK_OK, 71);
  CHECK(os_read_file(dest, &got, &len) == CORE_OK, 72);
  CHECK(len == 11 && memcmp(got, "hello world", 11) == 0, 73);
  free(got);
#ifndef _WIN32
  {
    struct stat sb;
    CHECK(stat(dest, &sb) == 0 && (sb.st_mode & 077) == 0, 87); /* owner-only */
  }
#endif
  CHECK(scratch_entries() == before + 1, 74);
  /* announced size over the cap: refused before downloading */
  CHECK(aslink_download_file(c, "doc-big", path_in("big.bin"), 1000, &st) ==
            ASLINK_ERR_TOO_LARGE, 75);
  /* size not announced, Content-Length over the cap */
  CHECK(aslink_download_file(c, "doc-liar", path_in("liar.bin"), 1000, &st) ==
            ASLINK_ERR_TOO_LARGE, 76);
  CHECK(clean(&st), 77);
  /* no Content-Length: the stream itself is capped */
  CHECK(aslink_download_file(c, "doc-stream", path_in("stream.bin"), 1000, &st) ==
            ASLINK_ERR_TOO_LARGE, 78);
  CHECK(aslink_download_file(c, "doc-gone", path_in("gone.bin"), 1000, &st) ==
            ASLINK_ERR_NOT_FOUND, 79);
  CHECK(st.http_status == 404 && clean(&st), 80);
  CHECK(aslink_download_file(c, "doc-bad", path_in("bad.bin"), 1000, &st) ==
            ASLINK_ERR_PROTOCOL, 81);
  CHECK(aslink_download_file(c, "doc-abs", path_in("abs.bin"), 1000, &st) ==
            ASLINK_ERR_PROTOCOL, 82);
  CHECK(scratch_entries() == before + 1, 83); /* nothing partial left behind */
  /* upload */
  CHECK(os_write_file(upload, payload, sizeof payload - 1) == CORE_OK, 84);
  CHECK(aslink_send_document(c, 5, 9, upload, "The report is ready \xe2\x9c\x93", &st) ==
            ASLINK_OK, 85);
  CHECK(aslink_send_document(c, 5, 0, path_in("missing.txt"), NULL, &st) ==
            ASLINK_ERR_NOT_FOUND, 86);
  aslink_close(c);
  return 0;
}

/* ── threads ──────────────────────────────────────────────────────────── */

typedef struct {
  aslink_ctx *c;
  int64_t offset;
  int timeout_s;
  aslink_err result;
  size_t n;
  int64_t finished_ms;
  volatile int done;
} poll_job;

static void *poll_thread(void *arg) {
  poll_job *j = (poll_job *)arg;
  aslink_update *u = NULL;
  aslink_status st;
  j->result = aslink_get_updates(j->c, j->offset, j->timeout_s, &u, &j->n, &st);
  aslink_updates_free(u, j->n);
  j->finished_ms = os_monotonic_ms();
  ASLINK_FLAG_SET(&j->done);
  return NULL;
}

static int step_cancel(void) {
  poll_job j;
  os_thread t;
  int64_t cancelled_at;
  aslink_status st;
  memset(&j, 0, sizeof j);
  j.c = open_ctx(HANG, NULL, 0);
  j.timeout_s = 50;
  CHECK(j.c != NULL, 90);
  CHECK(os_thread_start(&t, poll_thread, &j) == CORE_OK, 91);
  os_sleep_ms(400);
  CHECK(!ASLINK_FLAG_GET(&j.done), 92);
  cancelled_at = os_monotonic_ms();
  aslink_cancel(j.c);
  os_thread_join(&t);
  fprintf(stderr, "cancel latency: %lld ms\n", (long long)(j.finished_ms - cancelled_at));
  CHECK(j.result == ASLINK_ERR_CANCELLED, 93);
  CHECK(j.finished_ms - cancelled_at < 2000, 94);
  /* sticky */
  CHECK(aslink_send_message(j.c, 5, 0, "late", NULL, NULL, &st) == ASLINK_ERR_CANCELLED, 95);
  aslink_close(j.c);
  return 0;
}

/* One context, one thread long-polling, sends running meanwhile. */
static int step_concurrency(void) {
  poll_job j;
  os_thread t;
  aslink_status st;
  int64_t t0;
  int i;
  memset(&j, 0, sizeof j);
  j.c = open_ctx(GOOD, NULL, 0);
  j.offset = 200;
  j.timeout_s = 2;
  CHECK(j.c != NULL, 100);
  CHECK(os_thread_start(&t, poll_thread, &j) == CORE_OK, 101);
  os_sleep_ms(200);
  t0 = os_monotonic_ms();
  for (i = 0; i < 5; i++) {
    char text[32];
    int64_t id = 0;
    snprintf(text, sizeof text, "concurrent %d", i);
    CHECK(aslink_send_message(j.c, 5, 0, text, NULL, &id, &st) == ASLINK_OK, 102);
    CHECK(id == 700 + i, 103);
  }
  CHECK(os_monotonic_ms() - t0 < 1500, 104);
  CHECK(!ASLINK_FLAG_GET(&j.done), 105); /* the poll was in flight the whole time */
  os_thread_join(&t);
  CHECK(j.result == ASLINK_OK && j.n == 0, 106);
  aslink_close(j.c);
  return 0;
}

static int step_http_get(void) {
  char url[256], *body = NULL;
  size_t len = 0;
  long status = 0;
  snprintf(url, sizeof url, "%s/page/redirect", base_url);
  CHECK(aslink_http_get(url, 1000, 5000, &body, &len, &status) == ASLINK_OK, 110);
  CHECK(status == 200 && len == 10 && strcmp(body, "hello page") == 0, 111);
  free(body);
  snprintf(url, sizeof url, "%s/page/hop/5", base_url);
  CHECK(aslink_http_get(url, 1000, 5000, &body, &len, &status) == ASLINK_OK, 112);
  CHECK(strcmp(body, "landed") == 0, 113);
  free(body);
  snprintf(url, sizeof url, "%s/page/hop/6", base_url);
  CHECK(aslink_http_get(url, 1000, 5000, &body, &len, &status) == ASLINK_ERR_PROTOCOL, 114);
  CHECK(body == NULL, 115);
  snprintf(url, sizeof url, "%s/page/big", base_url);
  CHECK(aslink_http_get(url, 1000, 5000, &body, &len, &status) == ASLINK_ERR_TOO_LARGE, 116);
  snprintf(url, sizeof url, "%s/page/big-stream", base_url);
  CHECK(aslink_http_get(url, 1000, 5000, &body, &len, &status) == ASLINK_ERR_TOO_LARGE, 117);
  snprintf(url, sizeof url, "%s/page/to-file", base_url);
  CHECK(aslink_http_get(url, 1000, 5000, &body, &len, &status) == ASLINK_ERR_PROTOCOL, 118);
  snprintf(url, sizeof url, "%s/page/missing", base_url);
  CHECK(aslink_http_get(url, 1000, 5000, &body, &len, &status) == ASLINK_OK, 119);
  CHECK(status == 404, 120);
  free(body);
  CHECK(aslink_http_get("ftp://127.0.0.1/x", 1000, 0, &body, NULL, NULL) ==
            ASLINK_ERR_INVALID, 121);
  return 0;
}

int main(int argc, char **argv) {
  int rc;
  if (argc != 3) return 2;
  base_url = argv[1];
  scratch = argv[2];
  if ((rc = step_get_me()) != 0) return rc;
  if ((rc = step_updates()) != 0) return rc;
  if ((rc = step_send_and_edit()) != 0) return rc;
  if ((rc = step_errors()) != 0) return rc;
  if ((rc = step_files()) != 0) return rc;
  if ((rc = step_cancel()) != 0) return rc;
  if ((rc = step_concurrency()) != 0) return rc;
  if ((rc = step_http_get()) != 0) return rc;
  fprintf(stderr, "telegram driver: all steps passed\n");
  return 0;
}
