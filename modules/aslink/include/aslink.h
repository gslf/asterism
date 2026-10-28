/*
 * aslink.h — ⁂ aslink — the channel layer of the always-on agent.
 *
 * Public C API of libaslink. The first channel is the Telegram Bot API,
 * reached only through outbound HTTPS: updates arrive by long polling
 * (getUpdates); there are no webhooks and no listening socket.
 *
 * Contract notes:
 *   - All strings are UTF-8; every text sent is validated before it leaves.
 *   - Every function returns aslink_err except destructors and helpers.
 *   - aslink_status is an optional out-parameter (NULL is fine). It is
 *     reset at the start of every call and describes the last HTTP
 *     exchange. Its message never contains the bot token.
 *   - Thread safety: the context is immutable after aslink_telegram_open
 *     except for its cancel flag. Any number of threads may call into the
 *     same context at once (one long-polls, others send or edit); each call
 *     uses its own transfer handle. aslink_close must not race other calls.
 *   - aslink_cancel is sticky: in-flight calls abort promptly (within about
 *     one second) and every later call on the context returns
 *     ASLINK_ERR_CANCELLED. It is the shutdown switch; open a new context to
 *     continue.
 *   - The HTTP transport is libcurl, or WinHTTP on Windows without it.
 *     Built with neither, every network call returns
 *     ASLINK_ERR_UNSUPPORTED after argument validation; the text helpers
 *     work everywhere.
 *   - Decoded updates are hostile input: identifiers are checked for type,
 *     range and charset; display strings (names, titles, file names) are
 *     truncated on a code point boundary, with control characters turned
 *     into spaces and bidi overrides into U+FFFD. Message text and callback
 *     data are verbatim. An update whose payload fails validation is still
 *     returned (kind ASLINK_UPDATE_OTHER, invalid = 1) so the poller can
 *     advance. document_name is never a safe path by itself.
 *
 * License: MIT — per aspera ad astra.
 */

#ifndef ASLINK_H
#define ASLINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aslink_ctx aslink_ctx;

typedef enum {
  ASLINK_OK = 0,
  ASLINK_ERR_INVALID,      /* invalid argument                                */
  ASLINK_ERR_NOMEM,        /* allocation failure                              */
  ASLINK_ERR_IO,           /* transport failure, HTTP 5xx, local file error   */
  ASLINK_ERR_PROTOCOL,     /* malformed response or ok:false                  */
  ASLINK_ERR_UNAUTHORIZED, /* HTTP/Telegram 401 or 403                        */
  ASLINK_ERR_RATE_LIMITED, /* HTTP/Telegram 429; see retry_after_s            */
  ASLINK_ERR_NOT_FOUND,    /* 404, "... not found", missing local file        */
  ASLINK_ERR_TOO_LARGE,    /* size cap exceeded, 413, "file is too big"       */
  ASLINK_ERR_TIMEOUT,      /* deadline expired                                */
  ASLINK_ERR_CANCELLED,    /* aslink_cancel                                   */
  ASLINK_ERR_UNSUPPORTED   /* built without an HTTP transport                 */
} aslink_err;

/* Stable name of an error code, e.g. "ASLINK_ERR_IO". */
const char *aslink_err_name(aslink_err e);

/* Outcome of the last HTTP exchange of a call. */
typedef struct {
  int  http_status;   /* 0 when no response arrived                        */
  int  retry_after_s; /* parameters.retry_after of a 429, else 0           */
  int  telegram_code; /* error_code of an ok:false reply, else 0           */
  char message[256];  /* description or transport error; token redacted   */
} aslink_status;

/* ── Telegram context ─────────────────────────────────────────────────── */

typedef struct {
  /* NULL = https://api.telegram.org. Must be https://, or http:// to a
   * loopback host (127.0.0.1, ::1, localhost) for tests and local servers.
   * A trailing slash is ignored. */
  const char *api_base;
  const char *token;            /* "<digits>:<[A-Za-z0-9_-]+>", required      */
  long connect_timeout_ms;      /* 0 = 10000                                  */
  long request_timeout_ms;      /* 0 = 30000; long polls add their timeout    */
  long transfer_timeout_ms;     /* 0 = 600000; file download and upload       */
} aslink_telegram_params;

aslink_err aslink_telegram_open(const aslink_telegram_params *p, aslink_ctx **out);
void       aslink_close(aslink_ctx *c); /* NULL is a no-op */
/* Abort in-flight calls promptly and fail every later call (sticky). Safe
 * from any thread, also while other calls run. */
void       aslink_cancel(aslink_ctx *c);

/* ── updates ──────────────────────────────────────────────────────────── */

typedef enum {
  ASLINK_UPDATE_MESSAGE = 0,  /* Update.message                              */
  ASLINK_UPDATE_CALLBACK,     /* Update.callback_query                       */
  ASLINK_UPDATE_STOP,         /* Update.stopped_message_generation (10.3):
                                 the user pressed stop on a draft sent with
                                 can_stop; chat_id, thread_id, draft_id     */
  ASLINK_UPDATE_OTHER         /* anything else, or an invalid payload        */
} aslink_update_kind;

typedef struct {
  int64_t update_id;
  aslink_update_kind kind;
  char    kind_name[32];      /* Update field, e.g. "message", "edited_message" */
  int     invalid;            /* 1: payload failed validation; kind is OTHER */
  /* chat and thread (message, callback message, stop) */
  int64_t chat_id;
  char    chat_type[16];      /* "private", "group", "supergroup", "channel" */
  int64_t thread_id;          /* message_thread_id; 0 = none                 */
  int     is_topic;           /* is_topic_message                            */
  int64_t message_id;
  int64_t date;               /* unix seconds; 0 when absent                 */
  /* sender (message, callback); 0 / "" when absent */
  int64_t from_id;
  char    from_username[65];
  char    from_name[129];     /* "first last", truncated                     */
  int     from_is_bot;
  /* content */
  char   *text;               /* text, else caption; owned; NULL if none     */
  int     forwarded;
  char    forward_origin[160]; /* "user:Name", "hidden_user:Name",
                                  "chat:Title", "channel:Title"              */
  int64_t reply_to_message_id; /* 0 = not a reply (topic roots excluded)     */
  char    voice_file_id[256];
  int     voice_duration_s;
  char    audio_file_id[256];
  int     audio_duration_s;
  char    audio_mime[128];
  char    photo_file_id[256]; /* largest size                                */
  char    document_file_id[256];
  char    document_name[256];
  char    document_mime[128];
  int64_t file_size;          /* of the attached voice/audio/photo/document  */
  /* callback query */
  char    callback_id[128];
  char   *callback_data;      /* owned; NULL when absent                     */
  int64_t callback_message_id;
  /* stop generation */
  int64_t draft_id;
} aslink_update;

/* Long poll. offset 0 = earliest unconfirmed update; pass the highest
 * update_id seen + 1 to confirm everything before it. timeout_s in
 * [0, 600]. Requests the update kinds above only (allowed_updates:
 * message, callback_query, stopped_message_generation). On success *out is
 * NULL when *out_n == 0. */
aslink_err aslink_get_updates(aslink_ctx *c, int64_t offset, int timeout_s,
                              aslink_update **out, size_t *out_n,
                              aslink_status *st);
void       aslink_updates_free(aslink_update *u, size_t n);

/* ── sending ──────────────────────────────────────────────────────────── */

/* Inline button: text plus exactly one of callback_data (1-64 bytes) or url
 * (http, https or tg). */
typedef struct {
  const char *text;
  const char *callback_data;
  const char *url;
} aslink_button;

typedef struct {
  const aslink_button *buttons;
  size_t n;        /* 1..100                                                */
  size_t per_row;  /* 0 = one row                                           */
} aslink_keyboard;

typedef struct {
  const char *parse_mode;          /* NULL, "HTML" or "MarkdownV2"          */
  const aslink_keyboard *keyboard; /* inline keyboard; NULL = none          */
  int64_t reply_to;                /* message id; sent with
                                      allow_sending_without_reply           */
  int silent;                      /* disable_notification                  */
  int no_preview;                  /* link_preview_options.is_disabled      */
} aslink_send_opts;

/* text: 1-4096 characters (checked locally when parse_mode is NULL; see
 * aslink_split_text). thread_id 0 = none; with topics enabled in a private
 * chat (Bot API 9.3) it selects the topic. opts may be NULL. */
aslink_err aslink_send_message(aslink_ctx *c, int64_t chat_id, int64_t thread_id,
                               const char *text, const aslink_send_opts *opts,
                               int64_t *out_message_id, aslink_status *st);
/* editMessageText; uses parse_mode, keyboard and no_preview of opts. */
aslink_err aslink_edit_message(aslink_ctx *c, int64_t chat_id, int64_t message_id,
                               const char *text, const aslink_send_opts *opts,
                               aslink_status *st);
/* editMessageReplyMarkup; keyboard NULL removes it. */
aslink_err aslink_edit_keyboard(aslink_ctx *c, int64_t chat_id, int64_t message_id,
                                const aslink_keyboard *keyboard, aslink_status *st);

typedef struct {
  const char *parse_mode; /* NULL, "HTML" or "MarkdownV2"                   */
  int can_stop;           /* show a stop button (Bot API 10.3); pressing it
                             yields an ASLINK_UPDATE_STOP with this draft_id */
  int keep_on_stop;       /* keep the draft visible after stop              */
} aslink_draft_opts;

/* sendMessageDraft (Bot API 9.3, all bots since 9.5): streams a temporary
 * preview (about 30 s) into a private chat; finish with
 * aslink_send_message. draft_id must be non-zero; reusing it animates the
 * change. text NULL or "" shows a "Thinking…" placeholder (10.0).
 * opts may be NULL. */
aslink_err aslink_send_draft(aslink_ctx *c, int64_t chat_id, int64_t thread_id,
                             int64_t draft_id, const char *text,
                             const aslink_draft_opts *opts, aslink_status *st);
/* action: typing, upload_photo, record_video, upload_video, record_voice,
 * upload_voice, upload_document, choose_sticker, find_location,
 * record_video_note, upload_video_note. */
aslink_err aslink_send_chat_action(aslink_ctx *c, int64_t chat_id, int64_t thread_id,
                                   const char *action, aslink_status *st);
/* text NULL = no notification, else 0-200 characters. */
aslink_err aslink_answer_callback(aslink_ctx *c, const char *callback_id,
                                  const char *text, int alert, aslink_status *st);
/* createForumTopic; private chats are allowed since Bot API 9.4 (the bot
 * needs topic mode, see aslink_bot_info). name: 1-128 characters. */
aslink_err aslink_create_topic(aslink_ctx *c, int64_t chat_id, const char *name,
                               int64_t *out_thread_id, aslink_status *st);

/* ── files ────────────────────────────────────────────────────────────── */

/* getFile, then GET <api_base>/file/bot<token>/<file_path>. Written to a
 * temporary sibling of dest_path and moved into place atomically; nothing
 * is left behind on failure. max_bytes 0 = 20 MiB (the Bot API cap).
 * ASLINK_ERR_TOO_LARGE when the announced or received size exceeds it. */
aslink_err aslink_download_file(aslink_ctx *c, const char *file_id,
                                const char *dest_path, uint64_t max_bytes,
                                aslink_status *st);
/* sendDocument as multipart/form-data upload (at most 50 MB). caption may
 * be NULL, else 0-1024 characters. */
aslink_err aslink_send_document(aslink_ctx *c, int64_t chat_id, int64_t thread_id,
                                const char *path, const char *caption,
                                aslink_status *st);

/* ── bot ──────────────────────────────────────────────────────────────── */

typedef struct {
  const char *command;     /* 1-32 of [a-z0-9_], without the slash         */
  const char *description; /* 1-256 characters                             */
} aslink_command;

/* setMyCommands, default scope; n in 1..100. */
aslink_err aslink_set_commands(aslink_ctx *c, const aslink_command *cmds, size_t n,
                               aslink_status *st);

typedef struct {
  int64_t id;
  char    username[65];
  int     has_topics_enabled;            /* topic mode in private chats (9.3) */
  int     allows_users_to_create_topics; /* (9.4)                             */
} aslink_bot_info;

/* getMe: checks the token and reports the bot's identity. */
aslink_err aslink_get_me(aslink_ctx *c, aslink_bot_info *out, aslink_status *st);

/* ── generic HTTP ─────────────────────────────────────────────────────── */

/* Capped GET for page-watch triggers: http/https only (also across
 * redirects), at most 5 redirects, body capped at max_bytes (TOO_LARGE
 * beyond), timeout_ms total (0 = 30000). Any completed HTTP response is
 * ASLINK_OK; *out_status holds the final status. *out_body is
 * NUL-terminated and owned by the caller. out_len / out_status may be NULL. */
aslink_err aslink_http_get(const char *url, size_t max_bytes, long timeout_ms,
                           char **out_body, size_t *out_len, long *out_status);

/* Resumable download for installers: http/https only (also across
 * redirects, at most 10), no total deadline (cancel stops it). part_path
 * is resumed from its current size with "Range: bytes=N-"; a server that
 * ignores the Range restarts it from zero. A part already max_bytes long is
 * left alone (*out_status 0); a longer one, or a body that would make it
 * longer, is TOO_LARGE. progress (may be NULL) sees the bytes in the part
 * file. ASLINK_OK means a 2xx; any other status is ASLINK_ERR_IO (4xx:
 * NOT_FOUND for 404, PROTOCOL for 416, else IO) with *out_status set. */
typedef void (*aslink_progress_fn)(void *ud, uint64_t have);
aslink_err aslink_http_download(const char *url, const char *part_path, uint64_t max_bytes,
                                const volatile int *cancel, aslink_progress_fn progress,
                                void *progress_ud, long *out_status, aslink_status *st);

/* ── text helpers (no network, always available) ──────────────────────── */

/* Escape & < > " for parse_mode HTML. NULL on NULL, invalid UTF-8 or
 * allocation failure; free() the result. */
char *aslink_html_escape(const char *utf8);

/* Split UTF-8 text into parts of at most limit_chars characters
 * (0 = 4096, Telegram's message limit; at least 2). Characters are counted
 * as UTF-16 code units, the unit of the Bot API's entity offsets: never
 * fewer than code points, so the parts fit under either reading. Breaks
 * prefer a paragraph, then a line, then a space in the second half of the
 * window, else the last such boundary, else a hard cut; a code point is
 * never split. The break whitespace is dropped and blank parts are
 * skipped, so "" yields zero parts. For parse_mode HTML, split the plain
 * text first and escape each part (the limit applies after parsing). */
aslink_err aslink_split_text(const char *utf8, size_t limit_chars,
                             char ***out_parts, size_t *out_n);
void       aslink_strings_free(char **s, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* ASLINK_H */
