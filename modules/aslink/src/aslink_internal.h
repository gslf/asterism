/*
 * aslink_internal.h — declarations shared by the aslink sources and tests.
 *
 * Layers, bottom up:
 *   api.c              errors, status messages and token redaction, context
 *   text.c             UTF-8 / UTF-16 counting, HTML escape, splitting
 *   telegram_decode.c  pure decoding of Bot API replies (no network)
 *   telegram_request.c pure building and validation of request bodies
 *   transport.c        the response rules shared by the backends below
 *   transport_*.c      the one HTTP seam, one backend chosen by CMake:
 *                      libcurl (curl), WinHTTP on Windows (winhttp) or the
 *                      UNSUPPORTED stub (none)
 *   telegram.c         Bot API methods over the transport
 *   http.c             the generic capped GET
 */

#ifndef ASLINK_INTERNAL_H
#define ASLINK_INTERNAL_H

#include <stdio.h>

#include "aslink.h"
#include "asmodel_json.h"
#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ASLINK_DEFAULT_BASE       "https://api.telegram.org"
#define ASLINK_MESSAGE_MAX        4096   /* sendMessage text, characters   */
#define ASLINK_CAPTION_MAX        1024   /* sendDocument caption           */
#define ASLINK_TOPIC_NAME_MAX     128    /* createForumTopic name          */
#define ASLINK_CALLBACK_TEXT_MAX  200    /* answerCallbackQuery text       */
#define ASLINK_CALLBACK_DATA_MAX  64     /* InlineKeyboardButton, bytes    */
#define ASLINK_BUTTONS_MAX        100
#define ASLINK_COMMANDS_MAX       100
#define ASLINK_COMMAND_DESC_MAX   256
#define ASLINK_RAW_TEXT_MAX       65536  /* bytes, any text with parse_mode */
#define ASLINK_RESPONSE_MAX       (16u * 1024u * 1024u)
#define ASLINK_DOWNLOAD_DEFAULT   (20u * 1024u * 1024u)
#define ASLINK_UPLOAD_MAX         (50u * 1000u * 1000u)
#define ASLINK_POLL_TIMEOUT_MAX   600

/* The cancel flag is the one field written after open, from any thread.
 * C99 has no atomics: GCC and Clang get their builtins, MSVC its volatile
 * semantics (acquire/release under the default /volatile:ms). */
#if defined(__GNUC__) || defined(__clang__)
#define ASLINK_FLAG_SET(p) __atomic_store_n((p), 1, __ATOMIC_RELEASE)
#define ASLINK_FLAG_GET(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#else
#define ASLINK_FLAG_SET(p) ((void)(*(p) = 1))
#define ASLINK_FLAG_GET(p) (*(p))
#endif

struct aslink_ctx {
  char *base;  /* API base without trailing slash */
  char *token;
  long  connect_timeout_ms, request_timeout_ms, transfer_timeout_ms;
  volatile int cancelled; /* ASLINK_FLAG_SET / ASLINK_FLAG_GET only */
};

aslink_err aslink_from_core(core_err e);

/* ── status ───────────────────────────────────────────────────────────── */

/* Zero a status (NULL is fine). */
void aslink_status_reset(aslink_status *st);
/* Format into st->message (NULL st is fine), then redact the token and any
 * "bot<digits>:<secret>" path segment. */
void aslink_status_msg(aslink_status *st, const char *token, const char *fmt, ...)
#if defined(__GNUC__)
  __attribute__((format(printf, 3, 4)))
#endif
  ;
/* Redact in place: every occurrence of token (when non-empty) and of
 * "bot<digits>:<[A-Za-z0-9_-]+>" becomes "<redacted>" / "bot<redacted>". */
void aslink_redact(char *msg, size_t cap, const char *token);

/* ── text ─────────────────────────────────────────────────────────────── */

/* UTF-16 code units of [s, s+len); 0 when the bytes are not strict UTF-8. */
int  aslink_utf16_units(const char *s, size_t len, size_t *out_units);
/* Byte length of the longest prefix of [s, s+len) that ends on a code point
 * boundary and fits cap - 1 bytes (room for the NUL). */
size_t aslink_utf8_prefix(const char *s, size_t len, size_t cap);
/* Copy a display string truncated on a code point boundary, with control
 * characters and bidi overrides neutralised; dst gets "" when src is not
 * strict UTF-8. cap includes the NUL. */
void aslink_copy_display(char *dst, size_t cap, const char *src, size_t len);

/* ── decoding (telegram_decode.c) ─────────────────────────────────────── */

/* Check the Bot API envelope. On ASLINK_OK *out_root owns the tree and
 * *out_result points into it. Otherwise st describes the failure (ok:false
 * description, error_code, parameters.retry_after) and nothing is owned.
 * http_status is the transport status (200 for a plain body). */
aslink_err aslink_decode_envelope(const char *body, size_t len, long http_status,
                                  const char *token, asmodel_json_value **out_root,
                                  const asmodel_json_value **out_result,
                                  aslink_status *st);
/* The whole getUpdates reply body (envelope included). */
aslink_err aslink_decode_updates(const char *json, size_t len, aslink_update **out,
                                 size_t *out_n, aslink_status *st);
/* The result array of a getUpdates reply. */
aslink_err aslink_decode_update_list(const asmodel_json_value *result, aslink_update **out,
                                     size_t *out_n, aslink_status *st);
/* Result helpers for the methods. */
aslink_err aslink_decode_message_id(const asmodel_json_value *result, int64_t *out);
aslink_err aslink_decode_topic(const asmodel_json_value *result, int64_t *out_thread_id);
aslink_err aslink_decode_file(const asmodel_json_value *result, char **out_path,
                              int64_t *out_size);
aslink_err aslink_decode_bot(const asmodel_json_value *result, aslink_bot_info *out);
aslink_err aslink_decode_true(const asmodel_json_value *result);

/* ── request bodies (telegram_request.c) ──────────────────────────────── */

/* Each builder validates its arguments (ASLINK_ERR_INVALID or
 * ASLINK_ERR_TOO_LARGE with st->message naming the field) and returns a
 * malloc'd compact JSON body. */
aslink_err aslink_req_get_updates(int64_t offset, int timeout_s, char **out,
                                  aslink_status *st);
aslink_err aslink_req_send_message(int64_t chat_id, int64_t thread_id, const char *text,
                                   const aslink_send_opts *opts, char **out,
                                   aslink_status *st);
aslink_err aslink_req_edit_message(int64_t chat_id, int64_t message_id, const char *text,
                                   const aslink_send_opts *opts, char **out,
                                   aslink_status *st);
aslink_err aslink_req_edit_keyboard(int64_t chat_id, int64_t message_id,
                                    const aslink_keyboard *keyboard, char **out,
                                    aslink_status *st);
aslink_err aslink_req_send_draft(int64_t chat_id, int64_t thread_id, int64_t draft_id,
                                 const char *text, const aslink_draft_opts *opts,
                                 char **out, aslink_status *st);
aslink_err aslink_req_chat_action(int64_t chat_id, int64_t thread_id, const char *action,
                                  char **out, aslink_status *st);
aslink_err aslink_req_answer_callback(const char *callback_id, const char *text,
                                      int alert, char **out, aslink_status *st);
aslink_err aslink_req_create_topic(int64_t chat_id, const char *name, char **out,
                                   aslink_status *st);
aslink_err aslink_req_set_commands(const aslink_command *cmds, size_t n, char **out,
                                   aslink_status *st);
aslink_err aslink_req_get_file(const char *file_id, char **out, aslink_status *st);
/* Shared field checks, also used by the multipart upload. */
aslink_err aslink_check_text(const char *field, const char *text, size_t max_units,
                             int allow_empty, aslink_status *st);
aslink_err aslink_check_ids(int64_t chat_id, int64_t thread_id, aslink_status *st);

/* ── transport (transport.c, transport_*.c) ───────────────────────────── */

/* A streamed download keeps a non-2xx body in memory up to this size. */
#define ASLINK_ERROR_BODY_MAX (64u * 1024u)

/* One multipart field: a value, or a file (path plus the name announced). */
typedef struct {
  const char *name;
  const char *value;
  const char *file_path;
  const char *file_name;
} aslink_form_field;

typedef struct {
  const char *url;              /* may embed the token; never reported     */
  const char *json_body;        /* POST application/json when non-NULL     */
  const aslink_form_field *form; /* POST multipart/form-data when n > 0    */
  size_t form_n;
  long connect_timeout_ms;
  long timeout_ms;              /* total; 0 = none                         */
  size_t max_body;              /* in-memory body cap                      */
  FILE *sink;                   /* stream a 2xx body here instead; other
                                   bodies stay in memory for the error    */
  uint64_t sink_max;            /* cap for the sink                        */
  uint64_t range_from;          /* > 0: "Range: bytes=N-" (see below)      */
  int follow_redirects;         /* 0 or the redirect limit                 */
  const volatile int *cancel;   /* NULL = not cancellable                  */
  const char *token;            /* redacted from messages                  */
  aslink_progress_fn progress;  /* sink bytes so far, plus range_from      */
  void *progress_ud;
} aslink_xfer;

/* Perform one exchange on a fresh handle. On ASLINK_OK a response arrived:
 * *out_status holds its HTTP status and *out_body / *out_len the
 * NUL-terminated body kept in memory (caller frees; "" when streamed to
 * the sink). Transport failures map
 * to IO, TIMEOUT, CANCELLED or TOO_LARGE with st->message set. Built
 * without a transport it returns ASLINK_ERR_UNSUPPORTED.
 *
 * With range_from, a 206 must start at range_from (else PROTOCOL) and is
 * streamed or kept like any 2xx. A 200 means the server ignored the
 * Range: with a sink nothing is read or written and the call returns
 * ASLINK_OK with status 200, so the caller restarts from byte 0; without
 * one the whole body is kept. */
aslink_err aslink_perform(const aslink_xfer *x, char **out_body, size_t *out_len,
                          long *out_status, aslink_status *st);
/* 1 when the library was built with a transport. */
int aslink_has_transport(void);

/* The body of the final response, as both backends handle it. */
typedef struct {
  const aslink_xfer *x;
  int routed;      /* aslink_body_route ran; the fields below are set     */
  int to_sink;     /* a 2xx streamed to x->sink                            */
  int skip;        /* a 200 to a ranged sink request: nothing is taken     */
  uint64_t cap;    /* bytes allowed for this body                          */
  uint64_t got;
  core_buf mem;    /* every body that is not streamed                      */
  int over, bad_range, nomem, write_failed; /* why the transfer stopped   */
} aslink_body;

void aslink_body_init(aslink_body *b, const aslink_xfer *x);
void aslink_body_free(aslink_body *b);
/* Route the final response once its headers are known. content_range is
 * its Content-Range value (NULL or "" when absent) and content_length its
 * announced length (-1 when unknown). Returns 0 and sets over or bad_range
 * when the transfer must stop. */
int  aslink_body_route(aslink_body *b, long status, const char *content_range,
                       int64_t content_length);
/* Take the next n bytes; 0 = stop the transfer (over, nomem or
 * write_failed is set, or skip). */
int  aslink_body_take(aslink_body *b, const void *p, size_t n);
/* The error of a body that stopped the transfer, st->message set;
 * ASLINK_OK when it did not stop it. */
aslink_err aslink_body_error(const aslink_body *b, const char *token, aslink_status *st);
/* NUL-terminate the in-memory body and hand it over ("" after a sink). */
aslink_err aslink_body_finish(aslink_body *b, char **out_body, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ASLINK_INTERNAL_H */
