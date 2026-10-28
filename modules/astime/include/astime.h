/*
 * astime.h — ⁂ astime — routines for the always-on agent.
 *
 * A routine is one xCDN file, <engine-root>/routines/<name>.xcdn: a trigger
 * (cron, interval or one-shot time), an optional watch (a page or a file
 * whose content change fires the routine), a goal, the mode, profile, tools
 * and providers the turn may use, the delivery channel and quiet hours.
 * Routines are readable (canonical xCDN), inspectable (dry-run, lint) and
 * deterministic: every decision takes the time as a parameter.
 *
 * Runtime state lives apart from the definition, one file per routine in
 * <dir>/.state/<name>.xcdn, written with atomic replace. A corrupt state
 * file is reported and reset, never fatal.
 *
 * The daemon (`asterism serve`) opens one store, calls astime_due about
 * every 30 seconds, runs each due routine as an agent turn and reports back
 * with astime_mark_started / astime_mark_finished.
 *
 * Contract notes:
 *   - All strings are UTF-8. Times are unix seconds (UTC).
 *   - Fallible calls return astime_err. Error text goes to a caller buffer
 *     of ASTIME_ERR_SIZE bytes (may be NULL): "file:line: key.path: what".
 *   - Text out-parameters are malloc'd: release with astime_free.
 *   - Time zones are UTC or the process local zone (TZ, tzset).
 *   - The store is thread-safe; everything else is reentrant.
 *   - No network and no inference: watches are fetched by the caller, which
 *     passes the content hash back.
 *
 * MIT License — see LICENSE.
 */

#ifndef ASTIME_H
#define ASTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  ASTIME_OK = 0,
  ASTIME_ERR_IO,        /* filesystem failure                                 */
  ASTIME_ERR_PARSE,     /* malformed xCDN                                     */
  ASTIME_ERR_INVALID,   /* invalid value, argument or phrasing                */
  ASTIME_ERR_NOT_FOUND, /* unknown routine / no fire time within the horizon  */
  ASTIME_ERR_EXISTS,    /* a routine with that name already exists            */
  ASTIME_ERR_BUSY,      /* routine running / store locked by another process  */
  ASTIME_ERR_NOMEM      /* allocation failure                                 */
} astime_err;

/* Stable name of an error code, e.g. "ASTIME_ERR_INVALID". */
const char *astime_err_name(astime_err e);

#define ASTIME_ERR_SIZE        512 /* error message buffers                 */
#define ASTIME_NAME_MAX        65  /* slug (64) + NUL                        */
#define ASTIME_CRON_TEXT_MAX   128 /* cron expression as written + NUL       */
#define ASTIME_HASH_MAX        65  /* lowercase hex digest (<= 64) + NUL     */
#define ASTIME_SHORT_ERROR_MAX 200 /* last_error, truncated on a UTF-8 edge  */
#define ASTIME_TIME_TEXT_SIZE  32  /* "YYYY-MM-DD HH:MM" (+ " UTC")          */
#define ASTIME_SIG_MAX         96  /* trigger/watch signature in the state   */

#define ASTIME_CATCH_UP_DEFAULT  (12 * 3600) /* missed-run grace, seconds    */
#define ASTIME_DEFER_RETRY       600         /* retry after a deferred run   */
#define ASTIME_MIN_INTERVAL      60          /* shortest every / watch.every */
#define ASTIME_CRON_HORIZON_DAYS 2930        /* ~8 years: covers 29 Feb      */

/* Release text returned by astime calls. */
void astime_free(void *p);

/* A list of malloc'd strings. */
typedef struct {
  char **items;
  size_t n;
} astime_strings;

void astime_strings_free(astime_strings *l);

typedef enum { ASTIME_TZ_LOCAL = 0, ASTIME_TZ_UTC } astime_tz;

/* "local" | "utc" */
const char *astime_tz_name(astime_tz tz);

/* ── time helpers ─────────────────────────────────────────────────────── */

/* Offset of the zone from UTC at instant t, seconds east (CET = 3600). */
int64_t astime_utc_offset(int64_t t, astime_tz tz);

/* "YYYY-MM-DD HH:MM" in the zone; UTC adds " UTC". */
void astime_format_time(int64_t t, astime_tz tz, char out[ASTIME_TIME_TEXT_SIZE]);

/* ── cron ─────────────────────────────────────────────────────────────── */

/* Five fields: minute hour day-of-month month day-of-week. Lists, ranges,
 * steps ("a/n" means a-max/n), "*", month and day names (jan..dec,
 * sun..sat, any case), Sunday as 0 or 7, ranges ending on Sunday
 * ("fri-sun"), and @hourly @daily @midnight @weekly @monthly @yearly
 * @annually. Day matching follows Vixie cron: when either day field starts
 * with "*" both must match, otherwise either may. Treat as opaque. */
typedef struct {
  uint64_t minute; /* bits 0..59                       */
  uint32_t hour;   /* bits 0..23                       */
  uint32_t dom;    /* bits 1..31                       */
  uint16_t month;  /* bits 1..12                       */
  uint8_t  dow;    /* bits 0..6, Sunday = 0            */
  uint8_t  flags;  /* ASTIME_CRON_DOM_STAR | _DOW_STAR */
} astime_cron;

#define ASTIME_CRON_DOM_STAR 0x01
#define ASTIME_CRON_DOW_STAR 0x02

astime_err astime_cron_parse(const char *expr, astime_cron *out,
                             char err[ASTIME_ERR_SIZE]);

/* First fire time strictly after `after`. Wall times inside a DST gap are
 * skipped; a repeated wall time fires once, at its first occurrence.
 * ASTIME_ERR_NOT_FOUND when nothing fires within ASTIME_CRON_HORIZON_DAYS
 * (e.g. "0 0 30 2 *"). */
astime_err astime_cron_next(const astime_cron *cron, int64_t after,
                            astime_tz tz, int64_t *out_next);

/* Latest fire time at or before `at_or_before`, same rules. */
astime_err astime_cron_prev(const astime_cron *cron, int64_t at_or_before,
                            astime_tz tz, int64_t *out_prev);

/* ── routine definition ───────────────────────────────────────────────── */

typedef enum {
  ASTIME_TRIGGER_NONE = 0, /* watch-only routine */
  ASTIME_TRIGGER_CRON,
  ASTIME_TRIGGER_EVERY,
  ASTIME_TRIGGER_AT        /* one-shot           */
} astime_trigger_kind;

typedef struct {
  astime_trigger_kind kind;
  astime_tz tz;                         /* also the zone of quiet hours and
                                           of the daily run count          */
  char cron_text[ASTIME_CRON_TEXT_MAX]; /* CRON: as written                */
  astime_cron cron;                     /* CRON: parsed from cron_text     */
  int64_t every_s;                      /* EVERY: interval in seconds      */
  int64_t at;                           /* AT: instant                     */
  int32_t at_offset_s;                  /* AT: UTC offset it is written in */
} astime_trigger;

/* Fill a CRON trigger (text and parsed form). */
astime_err astime_trigger_set_cron(astime_trigger *t, const char *expr,
                                   astime_tz tz, char err[ASTIME_ERR_SIZE]);

typedef enum { ASTIME_WATCH_NONE = 0, ASTIME_WATCH_URL, ASTIME_WATCH_FILE } astime_watch_kind;
typedef enum { ASTIME_MODE_CHAT = 0, ASTIME_MODE_CODING, ASTIME_MODE_AUTOMATE } astime_mode;
typedef enum {
  ASTIME_PROFILE_CHAT = 0,
  ASTIME_PROFILE_CODING_READONLY,
  ASTIME_PROFILE_CODING_SANDBOXED,
  ASTIME_PROFILE_AUTOMATION_CI
} astime_profile;
typedef enum { ASTIME_NOTIFY_ALWAYS = 0, ASTIME_NOTIFY_ON_CHANGE, ASTIME_NOTIFY_NEVER } astime_notify;

/* Provider allowlist bits. Everything but LOCAL is paid inference
 * (subscription quota). */
#define ASTIME_PROVIDER_LOCAL       0x01u
#define ASTIME_PROVIDER_CHATGPT     0x02u
#define ASTIME_PROVIDER_CODEX       0x04u
#define ASTIME_PROVIDER_CLAUDE_CODE 0x08u
#define ASTIME_PROVIDER_PAID        0x0Eu

const char *astime_mode_name(astime_mode m);       /* "chat" | "coding" | "automate" */
const char *astime_profile_name(astime_profile p); /* "chat" | "coding-readonly" | ... */
const char *astime_notify_name(astime_notify n);   /* "always" | "on_change" | "never" */
const char *astime_provider_name(unsigned bit);    /* "local" | "chatgpt" | ... */

typedef struct {
  char *name;               /* slug, equals the file name                 */
  bool enabled;             /* default false                              */
  astime_trigger trigger;   /* optional when a watch is set               */
  astime_watch_kind watch_kind;
  char *watch_target;       /* URL (http/https) or absolute path          */
  int64_t watch_every_s;    /* poll interval                              */
  char *goal;
  astime_mode mode;         /* default chat                               */
  astime_profile profile;   /* default follows mode, as in the engine     */
  bool tools_set;           /* false: no allowlist (profile decides)      */
  char **tools;             /* tool package ids                           */
  size_t tools_n;
  unsigned providers;       /* ASTIME_PROVIDER_*; default LOCAL           */
  astime_notify notify;     /* default always                             */
  bool quiet_set;
  int quiet_start_min;      /* minutes from midnight, 0..1439             */
  int quiet_end_min;        /* 0..1440; start == end is an empty window   */
  char *deliver_channel;    /* NULL: the owner's default channel          */
  char *deliver_star;       /* NULL: the default star                     */
  int max_runs_per_day;     /* 0 = unlimited                              */
} astime_routine;

/* Defaults: disabled, no trigger, mode/profile chat, providers local,
 * notify always. Free with astime_routine_free. */
void       astime_routine_init(astime_routine *r);
void       astime_routine_free(astime_routine *r);
astime_err astime_routine_copy(const astime_routine *src, astime_routine *dst);

/* Parse one #routine document. `source` names it in messages (file path);
 * `expected_name`, when not NULL, must equal the name key. Unknown keys,
 * wrong types and invalid values fail with ASTIME_ERR_INVALID; xCDN syntax
 * errors with ASTIME_ERR_PARSE. */
astime_err astime_routine_parse(const char *text, const char *source,
                                const char *expected_name, astime_routine *out,
                                char err[ASTIME_ERR_SIZE]);

/* Read and parse <path>; the name must equal the file name sans ".xcdn". */
astime_err astime_routine_load(const char *path, astime_routine *out,
                               char err[ASTIME_ERR_SIZE]);

/* Semantic checks shared by parse and save (also for routines built in code). */
astime_err astime_routine_validate(const astime_routine *r, char err[ASTIME_ERR_SIZE]);

/* Canonical xCDN text of a valid routine (stable key order, trailing newline). */
astime_err astime_routine_serialize(const astime_routine *r, char **out_text);

/* Validate and write <dir>/<name>.xcdn atomically (dir is created). */
astime_err astime_routine_save(const char *dir, const astime_routine *r,
                               char err[ASTIME_ERR_SIZE]);

/* Every <dir>/<name>.xcdn in name order. Invalid files are skipped and reported in
 * `issues` (may be NULL), one "file:line: message" each; only a failure to
 * list the directory or to allocate fails the call. A missing dir is empty. */
astime_err astime_load_dir(const char *dir, astime_routine **out, size_t *out_n,
                           astime_strings *issues);
void       astime_routines_free(astime_routine *list, size_t n);

/* ── state ────────────────────────────────────────────────────────────── */

typedef enum {
  ASTIME_STATUS_NONE = 0, /* never finished */
  ASTIME_STATUS_OK,
  ASTIME_STATUS_FAILED,
  ASTIME_STATUS_SKIPPED,  /* ran the check, nothing to do (e.g. watch unchanged) */
  ASTIME_STATUS_DEFERRED  /* could not run now: the slot is retried later        */
} astime_status;

const char *astime_status_name(astime_status s); /* "none" | "ok" | "failed" | ... */

typedef struct {
  int64_t armed_at;       /* first seen enabled with this trigger; slots
                             before it never fire                            */
  char trigger_sig[ASTIME_SIG_MAX]; /* re-arms when the trigger changes      */
  char watch_sig[ASTIME_SIG_MAX];   /* resets the watch when the target changes */
  int64_t last_due_at;    /* last scheduled slot consumed                    */
  int64_t last_run_at;    /* last start                                      */
  int64_t running_since;  /* 0 = idle                                        */
  astime_status last_status;
  char last_error[ASTIME_SHORT_ERROR_MAX];
  int runs_today;
  char runs_day[11];      /* "YYYY-MM-DD" in the trigger zone, "" = none     */
  int consecutive_failures;
  char last_watch_hash[ASTIME_HASH_MAX];
  int64_t last_watch_at;
  int64_t next_due;       /* cached by the store, 0 = none                   */
  int64_t defer_until;    /* deferred: not due before this                   */
} astime_state;

void astime_state_init(astime_state *st);

/* <dir>/.state/<name>.xcdn. Load: ASTIME_ERR_NOT_FOUND when missing,
 * ASTIME_ERR_PARSE when corrupt (the store resets such files). */
astime_err astime_state_load(const char *dir, const char *name, astime_state *out,
                             char err[ASTIME_ERR_SIZE]);
astime_err astime_state_save(const char *dir, const char *name, const astime_state *st);

/* ── scheduling (pure) ────────────────────────────────────────────────── */

/* Earliest time >= now at which the routine is due given its state (NULL =
 * fresh), with the default catch-up grace and without running/cap changes
 * in between. ASTIME_ERR_NOT_FOUND when it never will be (disabled, one-shot
 * consumed, nothing within the horizon). */
astime_err astime_next_due(const astime_routine *r, const astime_state *st,
                           int64_t now, int64_t *out);

/* Up to `want` trigger fire times strictly after now (the watch is not a
 * schedule). EVERY continues from the state's last slot, or starts now. */
astime_err astime_next_fires(const astime_routine *r, const astime_state *st,
                             int64_t now, int64_t *out, size_t want, size_t *got);

/* True when t falls in the routine's quiet hours; *out_end (may be NULL)
 * receives the instant the window ends. */
bool astime_in_quiet_hours(const astime_routine *r, int64_t t, int64_t *out_end);

/* ── store ────────────────────────────────────────────────────────────── */

typedef struct astime_store astime_store;

typedef enum {
  ASTIME_DUE_SCHEDULE = 0, /* a trigger slot, on time                       */
  ASTIME_DUE_CATCH_UP,     /* a slot missed (e.g. machine off), run once now */
  ASTIME_DUE_WATCH         /* watch poll: run only if the content hash changed */
} astime_due_reason;

const char *astime_due_reason_name(astime_due_reason r);

typedef struct {
  char name[ASTIME_NAME_MAX];
  astime_due_reason reason;
  int64_t due_at;          /* the slot (or poll time) being served          */
  int64_t deliver_after;   /* 0 = deliver now; else end of quiet hours       */
  astime_watch_kind watch; /* NONE, or fetch the target and pass its hash    */
  char last_watch_hash[ASTIME_HASH_MAX]; /* "" = first check (baseline)     */
} astime_due_item;

typedef struct {
  astime_due_item *items;
  size_t n;
} astime_due_list;

void astime_due_list_free(astime_due_list *l);

/* Open the routines directory (created if missing) and its state, holding
 * an exclusive lock (<dir>/.state/lock): ASTIME_ERR_BUSY when another
 * process owns it. Invalid definitions and corrupt state are reported via
 * astime_store_issues, never fatal. A run interrupted by a crash is closed
 * as failed. */
astime_err astime_store_open(const char *dir, astime_store **out);
void       astime_store_close(astime_store *s);

/* Re-read the definitions; state of known routines is kept. */
astime_err astime_store_reload(astime_store *s);

/* Problems found by the last open/reload plus later state/write failures. */
astime_err astime_store_issues(astime_store *s, astime_strings *out);

/* Missed-run grace (default ASTIME_CATCH_UP_DEFAULT; 0 = never catch up). */
void       astime_store_set_catch_up(astime_store *s, int64_t grace_s);

astime_err astime_store_list(astime_store *s, astime_strings *out_names); /* sorted */
/* Deep copy of a routine and/or its state; either out may be NULL. */
astime_err astime_store_get(astime_store *s, const char *name,
                            astime_routine *out_routine, astime_state *out_state);
/* Validate, write the definition atomically and load it. ASTIME_ERR_EXISTS
 * when the name is taken and !replace. */
astime_err astime_store_put(astime_store *s, const astime_routine *r, bool replace,
                            char err[ASTIME_ERR_SIZE]);
astime_err astime_store_set_enabled(astime_store *s, const char *name, bool enabled);
/* Delete the definition and its state (ASTIME_ERR_BUSY while running). */
astime_err astime_store_remove(astime_store *s, const char *name);
/* `base` slugified, then "-2", "-3"... until no routine has that name. */
astime_err astime_store_unique_name(astime_store *s, const char *base,
                                    char out[ASTIME_NAME_MAX]);

/* Routines due at `now`: enabled, not running, under max_runs_per_day,
 * past any deferral, with an unconsumed slot no older than the catch-up
 * grace (only the latest missed slot runs: never a backlog) or a watch
 * poll due. Slots before the routine was armed (first seen enabled with
 * this trigger) never fire. Inside quiet hours deliver_after is set to the
 * end of the window. Sorted by name. */
astime_err astime_due(astime_store *s, int64_t now, astime_due_list *out);

/* Earliest next_due over all routines (ASTIME_ERR_NOT_FOUND if none). */
astime_err astime_store_next_wakeup(astime_store *s, int64_t now, int64_t *out);

/* The run starts: consumes the pending slot and watch poll at `now`.
 * ASTIME_ERR_BUSY if already running. */
astime_err astime_mark_started(astime_store *s, const char *name, int64_t now);

/* The run ended with status OK | FAILED | SKIPPED | DEFERRED. `error` (may
 * be NULL) is kept short; `watch_hash` (may be NULL) is the hex digest of
 * the watched content just fetched. OK and FAILED count toward
 * max_runs_per_day; DEFERRED gives the slot back and retries after
 * ASTIME_DEFER_RETRY. A one-shot routine that succeeded is disabled in its
 * definition file. */
astime_err astime_mark_finished(astime_store *s, const char *name, int64_t now,
                                astime_status status, const char *error,
                                const char *watch_hash);

/* SHA-256 (lowercase hex) of a watched file, or of fetched bytes. */
astime_err astime_hash_file(const char *path, char out[ASTIME_HASH_MAX]);
void       astime_hash_bytes(const void *data, size_t len, char out[ASTIME_HASH_MAX]);

/* ── dry-run and lint ─────────────────────────────────────────────────── */

/* The trigger in words, e.g. "on weekdays at 08:00". */
astime_err astime_describe_trigger(const astime_trigger *t, char **out_text);

/* Plain-text preview for a chat message: trigger in words and the next 3
 * fire times, goal, mode/profile, tools, providers and paid inference,
 * watch, delivery, quiet hours, cost note, last run (state may be NULL)
 * and the lint warnings. */
astime_err astime_dry_run(const astime_routine *r, const astime_state *st,
                          int64_t now, char **out_text);

/* Static policy warnings, one sentence each. */
astime_err astime_lint(const astime_routine *r, int64_t now, astime_strings *out);

/* ── natural schedule parsing (deterministic, no model) ───────────────── */

/* English phrasings and raw cron: "every day at 8am", "weekdays at 9",
 * "every monday and thursday at 10", "every 15 minutes", "in 2 hours",
 * "tomorrow at 3pm", "on november 3rd at 9", "on 2026-11-03 at 09:00",
 * "0 8 * * 1-5", "@daily".
 * One-shot times resolve against now in tz and must be in the future.
 * Unknown phrasing: ASTIME_ERR_INVALID with examples. */
astime_err astime_schedule_parse(const char *text, int64_t now, astime_tz tz,
                                 astime_trigger *out, char err[ASTIME_ERR_SIZE]);

/* "<schedule> | <goal>" into a routine with safe defaults: disabled until
 * confirmed, mode and profile chat, providers local, notify always, name
 * slugified from the goal. Free with astime_routine_free. */
astime_err astime_routine_from_chat(const char *text, int64_t now, astime_tz tz,
                                    astime_routine *out, char err[ASTIME_ERR_SIZE]);

/* Lowercase ASCII slug of free text (accents folded), "routine" if empty. */
void astime_slug_from_text(const char *text, char out[ASTIME_NAME_MAX]);

#ifdef __cplusplus
}
#endif

#endif /* ASTIME_H */
