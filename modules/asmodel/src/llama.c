/*
 * llama.c — embedded llama.cpp provider.
 *
 * One provider per model spec, serving generate() or embed() depending on
 * spec->embedding (plus token counting either way; the other function
 * pointer stays NULL). Generation applies the model's
 * embedded chat template (chatml fallback), attaches a per-call GBNF
 * grammar sampler when given, and streams token pieces through
 * token_cb. Embedding output is mean-pooled by llama.cpp
 * (LLAMA_POOLING_TYPE_MEAN) and L2-normalized here in double.
 *
 * Cancellation: *cancel is polled between prompt-eval chunks and at the
 * top of every decode iteration; in addition the per-call cancel
 * pointer is installed as ggml's abort callback on the llama context,
 * so long individual llama_decode calls abort early too (CPU backend
 * only, per llama.h).
 *
 * Prompt reuse (kv_cache): the cache holds up to MLL_SLOTS prompts, one
 * llama sequence each, so a caller that alternates prompts on one model
 * (one per role) keeps all of them. A call picks the sequence its prompt
 * continues (see mll_slot_pick), keeps the longest token prefix shared
 * with that sequence's last prompt and evaluates only the rest. Attention
 * caches drop the stale suffix in place. Recurrent and hybrid models
 * (Mamba, Qwen3.5, ...) hold a running state that cannot be cut back to a
 * token, so prompt evaluation snapshots that state at chunk boundaries and
 * where the next change is likely (see mll_ckpt_marks); the next call
 * restores the longest snapshot inside the shared prefix.
 *
 * Thread model: the manager serializes calls per provider; nothing here
 * assumes more than one call in flight. count_tokens touches only the
 * vocab and is thread-safe.
 */

#include "asmodel.h"
#include "core.h"

#include <stdio.h>
#include <string.h>

#ifdef ASTERISM_WITH_LLAMA

#include <limits.h>
#include <math.h>
#include <stdlib.h>

/* llama.h/ggml.h are not pedantic-C99-clean (anonymous unions, typedef
 * redefinitions); include them with those diagnostics off. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc11-extensions"
#pragma clang diagnostic ignored "-Wtypedef-redefinition"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4201) /* nameless struct/union */
#endif
#include "llama.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "llama_guard.h"

#if !defined(ASTERISM_NO_THREADS)
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif
#endif

/* ---- process-wide llama bootstrap --------------------------------------- */

/* llama.cpp logs to stderr by default. A library must never write to
 * stdout/stderr on its own initiative, so this process-wide callback drops
 * every record; failures surface through the provider's error returns. */
static void mll_log_silent(enum ggml_log_level level, const char *text,
                           void *ud) {
  (void)level;
  (void)text;
  (void)ud;
}

static void mll_boot(void) {
  llama_log_set(mll_log_silent, NULL);
  llama_backend_init();
}

#if defined(ASTERISM_NO_THREADS)

static void mll_backend_init(void) {
  static bool done = false;
  if (!done) {
    done = true;
    mll_boot();
  }
}

#elif defined(_WIN32)

static BOOL CALLBACK mll_boot_cb(PINIT_ONCE once, PVOID param, PVOID *ctx) {
  (void)once;
  (void)param;
  (void)ctx;
  mll_boot();
  return TRUE;
}

static void mll_backend_init(void) {
  static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
  InitOnceExecuteOnce(&once, mll_boot_cb, NULL, NULL);
}

#else /* POSIX threads */

static pthread_once_t mll_once = PTHREAD_ONCE_INIT;

static void mll_backend_init(void) {
  pthread_once(&mll_once, mll_boot);
}

#endif

/* ---- backend state ------------------------------------------------------ */

#define MLL_N_BATCH 512   /* prompt evaluation chunk size (generative)   */
#define MLL_PENALTY_LAST_N 64 /* repeat-penalty window (tokens)          */
#define MLL_EMBED_CTX 512 /* default embedding window when cfg->ctx <= 0 */
#define MLL_SLOTS 4       /* prompts kept per generative model           */
#define MLL_CKPT_MAX 8    /* recurrent-state snapshots kept per slot     */
#define MLL_CKPT_BYTES ((size_t)512 << 20) /* and their bytes across
                           * slots; the newest is always kept
                           * (Qwen3.5-4B: 50 MiB each)                   */
#define MLL_CKPT_MARGIN 4 /* tokens a snapshot stays before a predicted
                           * change: tokenizers merge across boundaries  */
#define MLL_RESUME_LEN 16 /* tokens that identify a resumed continuation */

/* Recurrent-state snapshot taken after the first n prompt tokens. */
typedef struct {
  int32_t n;
  uint8_t *data;
  size_t size;
} mll_ckpt;

/* One sequence of the shared cache (its index is the llama seq_id): the
 * last prompt it evaluated, the positions it still holds and, for
 * recurrent/hybrid memory, snapshots sorted by n, each a prefix of prompt
 * whose attention entries are still in the cache. */
typedef struct {
  llama_token *prompt; /* NULL: the sequence is empty                    */
  int32_t prompt_n;
  int32_t n_mem;       /* positions held: the prompt and decoded output  */
  uint64_t used;       /* call count at last use                         */
  mll_ckpt ckpt[MLL_CKPT_MAX];
  int n_ckpt;
} mll_slot;

typedef struct {
  char id[ASMODEL_ID_MAX];    /* tokenizer identity: the model itself     */
  struct llama_model *model;
  const struct llama_vocab *vocab;
  struct llama_context *lctx; /* one call at a time (slot mutex)         */
  const char *chat_template;  /* model-owned; NULL = chatml fallback     */
  int n_ctx;
  int n_batch;
  int dim;                    /* embedding models only                   */
  bool embedding;
  bool encoder_only;
  bool kv_cache;
  bool needs_ckpt;            /* recurrent/hybrid memory: reuse restores
                               * snapshots                               */
  mll_slot slot[MLL_SLOTS];   /* kv_cache only                           */
  int n_slots;
  uint64_t calls;
  struct llama_batch batch;   /* generative: n_batch tokens, one sequence
                               * each                                    */
  /* Per-call cancel pointer for the ggml abort callback: installed at
   * the top of generate() and cleared on exit, guarded by cancel_mu
   * because the ggml worker threads read it mid-decode. */
  os_mutex cancel_mu;
  volatile int *cancel_flag;
  int64_t deadline_at_ms;
} mll_ud;

static bool mll_abort_cb(void *data) {
  mll_ud *u = (mll_ud *)data;
  volatile int *flag;
  int64_t deadline_at;

  os_mutex_lock(&u->cancel_mu);
  flag = u->cancel_flag;
  deadline_at = u->deadline_at_ms;
  os_mutex_unlock(&u->cancel_mu);
  return (flag != NULL && *flag != 0) ||
         (deadline_at > 0 && os_monotonic_ms() >= deadline_at);
}

static void mll_set_cancel(mll_ud *u, volatile int *cancel,
                           int64_t deadline_at_ms) {
  os_mutex_lock(&u->cancel_mu);
  u->cancel_flag = cancel;
  u->deadline_at_ms = deadline_at_ms;
  os_mutex_unlock(&u->cancel_mu);
}

static bool mll_cancelled(volatile int *cancel) {
  return cancel != NULL && *cancel != 0;
}

static bool mll_expired(int64_t deadline_at_ms) {
  return deadline_at_ms > 0 && os_monotonic_ms() >= deadline_at_ms;
}

/* Tokenize text into a malloc'd array using the negative-return resize
 * convention (first call sizes, second call fills). *out_tok is NULL
 * when the text yields zero tokens. */
static asmodel_err mll_tokenize(const struct llama_vocab *vocab,
                              const char *text, bool add_special,
                              bool parse_special, llama_token **out_tok,
                              int32_t *out_n) {
  int32_t len, n, got;
  llama_token *tok;

  *out_tok = NULL;
  *out_n = 0;
  len = (int32_t)strlen(text);
  n = asmodel_llg_tokenize(vocab, text, len, NULL, 0, add_special,
                         parse_special);
  if (n == INT32_MIN) return ASMODEL_ERR_BACKEND;
  if (n < 0) n = -n;
  if (n == 0) return ASMODEL_OK;
  tok = (llama_token *)malloc((size_t)n * sizeof *tok);
  if (tok == NULL) return ASMODEL_ERR_NOMEM;
  got = asmodel_llg_tokenize(vocab, text, len, tok, n, add_special,
                           parse_special);
  if (got < 0) {
    free(tok);
    return ASMODEL_ERR_BACKEND;
  }
  *out_tok = tok;
  *out_n = got;
  return ASMODEL_OK;
}

/* Detokenize one token, append it to the answer buffer and stream it
 * through token_cb (NUL-terminated piece bytes). Pieces may split
 * multi-byte UTF-8 sequences across callback invocations — acceptable
 * for v1; consumers reassemble. Special tokens render as nothing
 * (special=false): the grammar never produces them and EOG stops
 * generation before this is reached. */
static asmodel_err mll_emit_piece(const struct llama_vocab *vocab,
                                llama_token tok, core_buf *b,
                                asmodel_token_fn token_cb, void *token_ud) {
  char small[160];
  char *big;
  int32_t n, got;
  asmodel_err e;

  n = llama_token_to_piece(vocab, tok, small, (int32_t)sizeof small - 1, 0,
                           false);
  if (n >= 0) {
    small[n] = '\0';
    e = core_buf_append(b, small, (size_t)n) == CORE_OK ? ASMODEL_OK : ASMODEL_ERR_NOMEM;
    if (e == ASMODEL_OK && token_cb != NULL && n > 0) token_cb(small, (size_t)n, token_ud);
    return e;
  }
  if (n == INT32_MIN) return ASMODEL_ERR_BACKEND;
  big = (char *)malloc((size_t)-n + 1);
  if (big == NULL) return ASMODEL_ERR_NOMEM;
  got = llama_token_to_piece(vocab, tok, big, -n, 0, false);
  if (got < 0) {
    free(big);
    return ASMODEL_ERR_BACKEND;
  }
  big[got] = '\0';
  e = core_buf_append(b, big, (size_t)got) == CORE_OK ? ASMODEL_OK : ASMODEL_ERR_NOMEM;
  if (e == ASMODEL_OK && token_cb != NULL && got > 0) token_cb(big, (size_t)got, token_ud);
  free(big);
  return e;
}

/* Template rendering preserves roles; this C backend accepts text blocks only. */
static asmodel_err mll_apply_template(const char *tmpl, const asmodel_input *input, char **out) {
  struct llama_chat_message messages[256];
  char *content[256] = {0};
  asmodel_err e = ASMODEL_ERR_UNSUPPORTED;
  *out = NULL;
  if (asmodel_input_validate(input) != ASMODEL_OK) return ASMODEL_ERR_INVALID;
  for (size_t i = 0; i < input->count; i++) {
    asmodel_err result = asmodel_message_text(&input->messages[i],&content[i]);
    if (result != ASMODEL_OK) { e = result == ASMODEL_ERR_NOMEM ? ASMODEL_ERR_NOMEM : ASMODEL_ERR_UNSUPPORTED; goto done; }
    messages[i].role = asmodel_role_name(input->messages[i].role);
    messages[i].content = content[i];
  }
  int32_t need = asmodel_llg_chat_apply_template(tmpl,messages,input->count,true,NULL,0);
  if (need < 0 || need == INT32_MAX) goto done;
  char *buf = malloc((size_t)need+1);
  if (!buf) { e = ASMODEL_ERR_NOMEM; goto done; }
  int32_t got = asmodel_llg_chat_apply_template(tmpl,messages,input->count,true,buf,need+1);
  if (got < 0 || got > need) { free(buf); goto done; }
  buf[got] = 0; *out = buf; e = ASMODEL_OK;
  /* Hybrid-thinking templates (Qwen3, Qwen3.5) render enable_thinking=false
   * as an empty think block after the assistant header; the C renderer has
   * no such switch, so emit what the model's own template would. This is the
   * model's documented default, not a reasoning-off guarantee: constrained
   * passes keep the grammar as their guarantee. */
  if (tmpl && strstr(tmpl, "enable_thinking")) {
    static const char empty_think[] = "<think>\n\n</think>\n\n";
    char *grown = realloc(buf, (size_t)got + sizeof empty_think);
    if (!grown) { free(buf); *out = NULL; e = ASMODEL_ERR_NOMEM; goto done; }
    memcpy(grown + got, empty_think, sizeof empty_think);
    *out = grown;
  }
done:
  for (size_t i = 0; i < input->count; i++) free(content[i]);
  return e;
}

/* ---- prompt reuse ------------------------------------------------------- */

static void mll_ckpt_drop_from(mll_slot *s, int first) {
  for (int k = first; k < s->n_ckpt; k++) free(s->ckpt[k].data);
  if (first < s->n_ckpt) s->n_ckpt = first;
}

static void mll_ckpt_drop_first(mll_slot *s) {
  free(s->ckpt[0].data);
  s->n_ckpt--;
  memmove(s->ckpt, s->ckpt + 1, (size_t)s->n_ckpt * sizeof s->ckpt[0]);
}

/* Empty sequence k: its cache entries, prompt record and snapshots. */
static void mll_slot_clear(mll_ud *u, int k) {
  mll_slot *s = &u->slot[k];
  llama_memory_seq_rm(llama_get_memory(u->lctx), k, -1, -1);
  free(s->prompt);
  s->prompt = NULL;
  s->prompt_n = 0;
  s->n_mem = 0;
  mll_ckpt_drop_from(s, 0);
}

static int32_t mll_common(const llama_token *a, int32_t na, const llama_token *b, int32_t nb) {
  int32_t n = 0;
  while (n < na && n < nb && a[n] == b[n]) n++;
  return n;
}

/* Tokens a prompt sharing the first n tokens of sequence s can start from:
 * all of them in an attention cache, the longest snapshot within them in
 * recurrent memory. */
static int32_t mll_slot_reusable(const mll_ud *u, const mll_slot *s, int32_t n) {
  int c = s->n_ckpt;
  if (!u->needs_ckpt) return n;
  while (c > 0 && s->ckpt[c - 1].n > n) c--;
  return c > 0 ? s->ckpt[c - 1].n : 0;
}

/* Plan a prompt's sequence. Its start comes from the sequence holding the
 * longest reusable prefix (*src, -1 if none; *common tokens shared). The
 * call continues there when it keeps at least half of that sequence's
 * prompt, as a prompt rebuilt step by step does; otherwise the prefix is
 * copied to an empty or the least recently used sequence (mll_slot_fork),
 * so a prompt that shares only a system prompt with another role's leaves
 * that role's sequence intact. With nothing reusable, a sequence the prompt
 * continues is overwritten before any other. Returns the sequence. */
static int mll_slot_pick(const mll_ud *u, const llama_token *tok, int32_t n_tok, int *src,
                         int32_t *common) {
  int cont = -1, empty = -1, lru = -1;
  int32_t cont_n = 0, best = 0;
  *src = -1;
  *common = 0;
  for (int k = 0; k < u->n_slots; k++) {
    const mll_slot *s = &u->slot[k];
    int32_t n, r;
    if (s->prompt == NULL) {
      if (empty < 0) empty = k;
      continue;
    }
    n = mll_common(tok, n_tok, s->prompt, s->prompt_n);
    /* at least the last token is evaluated again, for this prompt's logits */
    r = mll_slot_reusable(u, s, n < n_tok ? n : n_tok - 1);
    if (r > best) {
      *src = k;
      *common = n;
      best = r;
    }
    if (2 * n >= s->prompt_n && n > cont_n) {
      cont = k;
      cont_n = n;
    }
    if (lru < 0 || s->used < u->slot[lru].used) lru = k;
  }
  if (*src >= 0 && 2 * *common >= u->slot[*src].prompt_n) return *src;
  if (*src < 0 && cont >= 0) return cont;
  return empty >= 0 ? empty : lru;
}

/* The sequences share one cache of n_ctx positions. Make room for sequence
 * k to hold `need` of them, emptying the others least recently used first,
 * and return how many k may hold. Generation asks for one more position at
 * a time: output budgets are ceilings, rarely spent, and reserving them up
 * front would empty every other sequence. */
static int32_t mll_slot_room(mll_ud *u, int k, int32_t need) {
  for (;;) {
    int32_t others = 0;
    int lru = -1;
    for (int j = 0; j < u->n_slots; j++) {
      if (j == k || u->slot[j].n_mem == 0) continue;
      others += u->slot[j].n_mem;
      if (lru < 0 || u->slot[j].used < u->slot[lru].used) lru = j;
    }
    if (lru < 0 || others + need <= u->n_ctx) return u->n_ctx - others;
    mll_slot_clear(u, lru);
  }
}

/* Snapshot sequence k's recurrent state after its first n tokens.
 * Positions grow within a call, so appending keeps the slot sorted. Over
 * budget, the shortest prefix goes from the slot holding the most
 * snapshots (the least recently used among equals): later calls mostly
 * extend the long ones, and a slot without any can only start over. A
 * missing snapshot only costs reuse, so failures are silent. */
static void mll_ckpt_save(mll_ud *u, int k, int32_t n) {
  mll_slot *s = &u->slot[k];
  size_t size = llama_state_seq_get_size_ext(u->lctx, k, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
  size_t kept = size;
  uint8_t *data = size > 0 ? (uint8_t *)malloc(size) : NULL;
  if (data == NULL) return;
  if (asmodel_llg_state_seq_get(u->lctx, data, size, k, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY) !=
      size) {
    free(data);
    return;
  }
  if (s->n_ckpt == MLL_CKPT_MAX) mll_ckpt_drop_first(s);
  for (int j = 0; j < u->n_slots; j++)
    for (int c = 0; c < u->slot[j].n_ckpt; c++) kept += u->slot[j].ckpt[c].size;
  while (kept > MLL_CKPT_BYTES) {
    mll_slot *v = NULL;
    for (int j = 0; j < u->n_slots; j++) {
      mll_slot *o = &u->slot[j];
      if (o->n_ckpt > 0 && (v == NULL || o->n_ckpt > v->n_ckpt ||
                            (o->n_ckpt == v->n_ckpt && o->used < v->used)))
        v = o;
    }
    if (v == NULL) break;
    kept -= v->ckpt[0].size;
    mll_ckpt_drop_first(v);
  }
  s->ckpt[s->n_ckpt].n = n;
  s->ckpt[s->n_ckpt].data = data;
  s->ckpt[s->n_ckpt].size = size;
  s->n_ckpt++;
}

/* Positions where the next prompt will most likely diverge from this one,
 * judged from how this one diverged from the previous: callers that rebuild
 * a prompt insert new content at the old divergence point and keep what
 * followed, so the next insertion goes where that continuation resumes
 * (mark[0]); chats append after the ending both prompts share (mark[1]).
 * common is the raw shared prefix. A mark of 0 or less means none. */
static void mll_ckpt_marks(const llama_token *cur, int32_t n_cur, const llama_token *prev,
                           int32_t n_prev, int32_t common, int32_t mark[2]) {
  int32_t tail = 0;
  mark[0] = 0;
  if (n_prev - common >= MLL_RESUME_LEN)
    for (int32_t j = common + 1; j + MLL_RESUME_LEN <= n_cur; j++)
      if (!memcmp(cur + j, prev + common, MLL_RESUME_LEN * sizeof *cur)) {
        mark[0] = j - MLL_CKPT_MARGIN;
        break;
      }
  while (tail < n_cur && tail < n_prev && cur[n_cur - 1 - tail] == prev[n_prev - 1 - tail])
    tail++;
  mark[1] = n_cur - tail - MLL_CKPT_MARGIN;
}

/* Rewind sequence k to at most `common` tokens: restore its longest
 * snapshot inside the shared prefix, then drop the attention entries past
 * it. Returns the tokens kept; 0 means the sequence was emptied. */
static int32_t mll_ckpt_restore(mll_ud *u, int k, int32_t common) {
  mll_slot *s = &u->slot[k];
  llama_memory_t mem = llama_get_memory(u->lctx);
  int c = s->n_ckpt;
  while (c > 0 && s->ckpt[c - 1].n > common) c--;
  mll_ckpt_drop_from(s, c); /* past the shared prefix: stale */
  if (c > 0) {
    const mll_ckpt *p = &s->ckpt[c - 1];
    if (asmodel_llg_state_seq_set(u->lctx, p->data, p->size, k,
                                  LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY) == p->size &&
        llama_memory_seq_rm(mem, k, p->n, -1))
      return p->n;
  }
  mll_ckpt_drop_from(s, 0);
  llama_memory_seq_rm(mem, k, -1, -1);
  return 0;
}

/* Start empty sequence dst from the first n tokens of sequence src, n being
 * reusable there (mll_slot_reusable): share src's attention entries and
 * restore src's snapshot at n into dst, which also gives up the recurrent
 * state the copy shared with src. Returns the tokens dst starts from. */
static int32_t mll_slot_fork(mll_ud *u, int src, int dst, int32_t n) {
  const mll_slot *s = &u->slot[src];
  llama_memory_t mem = llama_get_memory(u->lctx);
  int c = 0;
  llama_memory_seq_cp(mem, src, dst, 0, n);
  if (!u->needs_ckpt) return n;
  while (c < s->n_ckpt && s->ckpt[c].n != n) c++;
  if (c < s->n_ckpt && asmodel_llg_state_seq_set(u->lctx, s->ckpt[c].data, s->ckpt[c].size, dst,
                                                 LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY) ==
                           s->ckpt[c].size)
    return n;
  llama_memory_seq_rm(mem, dst, -1, -1);
  return 0;
}

/* Decode tok[0..n) at positions pos.. of sequence seq, with logits for the
 * last token only when `last`. */
static int32_t mll_decode(mll_ud *u, const llama_token *tok, int32_t n, llama_pos pos,
                          llama_seq_id seq, bool last) {
  struct llama_batch *b = &u->batch;
  b->n_tokens = n;
  for (int32_t j = 0; j < n; j++) {
    b->token[j] = tok[j];
    b->pos[j] = pos + j;
    b->n_seq_id[j] = 1;
    b->seq_id[j][0] = seq;
    b->logits[j] = last && j == n - 1;
  }
  return asmodel_llg_decode(u->lctx, *b);
}

/* ---- generation --------------------------------------------------------- */

static int mll_generate(void *ud, const asmodel_input *input, const char *gbnf,
                        const asmodel_generate_params *p,
                        asmodel_token_fn token_cb, void *token_ud,
                        volatile int *cancel, char **out_text,
                        int *out_tokens_in, int *out_tokens_out) {
  mll_ud *u = (mll_ud *)ud;
  char *prompt = NULL;
  llama_token *tok = NULL;
  int32_t n_tok = 0;
  int32_t prompt_start = 0;
  int32_t mark[2] = {0, 0}; /* extra snapshot positions (mll_ckpt_marks) */
  int k = -1;               /* sequence this call evaluates in           */
  int32_t room = 0;         /* positions that sequence may hold          */
  struct llama_sampler *chain = NULL;
  core_buf outbuf;
  asmodel_err e;
  int32_t i, n_past;
  int produced = 0, limit;
  int64_t now = os_monotonic_ms();
  int64_t deadline_at = p->deadline_ms > 0 ?
      (p->deadline_ms > INT64_MAX-now ? INT64_MAX : now+p->deadline_ms) : 0;
  bool hit_eog = false, constrained = false;

  *out_text = NULL;
  *out_tokens_in = 0;
  *out_tokens_out = 0;
  core_buf_init(&outbuf);
  mll_set_cancel(u, cancel, deadline_at);

  if (mll_cancelled(cancel) || mll_expired(deadline_at)) {
    e = mll_expired(deadline_at) ? ASMODEL_ERR_TIMEOUT : ASMODEL_ERR_CANCELLED;
    goto out;
  }

  if (p->tools) { e = ASMODEL_ERR_UNSUPPORTED; goto out; }

  /* Embedded llama.cpp has no portable per-request thinking switch in its C
   * chat-template API.  A constrained micro-decision is still an absolute
   * reasoning-off contract because the grammar makes reasoning tokens
   * illegal.  Do not pretend that a model-specific prompt suffix can provide
   * the same guarantee for unconstrained output. */
  if ((p->require_constraint || p->reasoning == ASMODEL_REASONING_REQUIRED_OFF) && gbnf == NULL) {
    e = ASMODEL_ERR_UNSUPPORTED;
    goto out;
  }
  if (p->reasoning == ASMODEL_REASONING_REQUIRED_ON ||
      p->reasoning == ASMODEL_REASONING_BUDGETED) {
    e = ASMODEL_ERR_UNSUPPORTED;
    goto out;
  }

  e = mll_apply_template(u->chat_template,
                         input,
                         &prompt);
  if (e != ASMODEL_OK) goto out;

  /* add_special true so BOS-requiring vocabs (Llama-3-class instruct
   * models) get BOS — chat templates do not emit it themselves and
   * add_bos=false vocabs (e.g. Qwen) are unaffected; parse_special true
   * so role markers become their special tokens. Matches the upstream
   * convention of add_special on the first prompt segment. */
  e = mll_tokenize(u->vocab, prompt, true, true, &tok, &n_tok);
  if (e != ASMODEL_OK) goto out;
  if (n_tok == 0 || n_tok >= u->n_ctx) {
    e = ASMODEL_ERR_BACKEND; /* empty or context-overflowing prompt */
    goto out;
  }
  *out_tokens_in = (int)n_tok;
  limit = p->max_tokens > 0 ? p->max_tokens : INT_MAX;

  if (u->kv_cache) {
    llama_memory_t mem = llama_get_memory(u->lctx);
    int src;
    int32_t common;
    mll_slot *s, *prev;
    k = mll_slot_pick(u, tok, n_tok, &src, &common);
    s = &u->slot[k];
    s->used = ++u->calls;
    /* The next change is predicted from how this prompt departs from the
     * one it reuses, else from the one it replaces. */
    prev = src >= 0 ? &u->slot[src] : s;
    if (src < 0 && s->prompt != NULL) common = mll_common(tok, n_tok, s->prompt, s->prompt_n);
    if (u->needs_ckpt && prev->prompt != NULL)
      mll_ckpt_marks(tok, n_tok, prev->prompt, prev->prompt_n, common, mark);
    /* Re-evaluate at least the last prompt token so the logits used for
     * the first sample correspond to this prompt, not the previous
     * completion. */
    if (common >= n_tok) common = n_tok - 1;
    if (src == k) {
      if (u->needs_ckpt) prompt_start = mll_ckpt_restore(u, k, common);
      else if (llama_memory_seq_rm(mem, k, common, -1)) prompt_start = common;
      else llama_memory_seq_rm(mem, k, -1, -1);
    } else {
      if (s->prompt != NULL) mll_slot_clear(u, k);
      if (src >= 0)
        prompt_start = mll_slot_fork(u, src, k, mll_slot_reusable(u, &u->slot[src], common));
    }
    room = mll_slot_room(u, k, n_tok);
  } else {
    k = 0;
    asmodel_llg_memory_clear(llama_get_memory(u->lctx), true);
    room = u->n_ctx;
  }

  /* Fresh sampler chain per call: grammar state is per-generation, and
   * the chain shape follows the sampling params — grammar first (when
   * constrained), then greedy for temp <= 0, else top_p / temp / dist. */
  chain = llama_sampler_chain_init(llama_sampler_chain_default_params());
  if (chain == NULL) {
    e = ASMODEL_ERR_NOMEM;
    goto out;
  }
  if (gbnf != NULL) {
    struct llama_sampler *grammar =
        llama_sampler_init_grammar(u->vocab, gbnf, "root");
    if (grammar == NULL) {
      e = ASMODEL_ERR_BACKEND; /* GBNF failed to parse */
      goto out;
    }
    llama_sampler_chain_add(chain, grammar);
    constrained = true;
  }
  if (p->repeat_penalty > 1.0) {
    /* after the grammar mask so -inf survives the scaling: recent legal
     * tokens are penalized, which breaks greedy phrase loops */
    struct llama_sampler *pen = llama_sampler_init_penalties(
        llama_vocab_n_tokens(u->vocab), MLL_PENALTY_LAST_N,
        (float)p->repeat_penalty, 0.0f, 0.0f);
    if (pen == NULL) {
      e = ASMODEL_ERR_NOMEM;
      goto out;
    }
    llama_sampler_chain_add(chain, pen);
  }
  if (p->temperature <= 0.0) {
    struct llama_sampler *greedy = llama_sampler_init_greedy();
    if (greedy == NULL) {
      e = ASMODEL_ERR_NOMEM;
      goto out;
    }
    llama_sampler_chain_add(chain, greedy);
  } else {
    struct llama_sampler *sm;
    if (p->top_p > 0.0) {
      sm = llama_sampler_init_top_p((float)p->top_p, 1);
      if (sm == NULL) {
        e = ASMODEL_ERR_NOMEM;
        goto out;
      }
      llama_sampler_chain_add(chain, sm);
    }
    sm = llama_sampler_init_temp((float)p->temperature);
    if (sm == NULL) {
      e = ASMODEL_ERR_NOMEM;
      goto out;
    }
    llama_sampler_chain_add(chain, sm);
    sm = llama_sampler_init_dist(LLAMA_DEFAULT_SEED);
    if (sm == NULL) {
      e = ASMODEL_ERR_NOMEM;
      goto out;
    }
    llama_sampler_chain_add(chain, sm);
  }

  for (i = prompt_start; i < n_tok;) {
    int32_t end = n_tok - i < u->n_batch ? n_tok : i + u->n_batch;
    for (int m = 0; m < 2; m++)
      if (mark[m] > i && mark[m] < end) end = mark[m];
    if (mll_cancelled(cancel) || mll_expired(deadline_at)) {
      e = mll_expired(deadline_at) ? ASMODEL_ERR_TIMEOUT : ASMODEL_ERR_CANCELLED;
      goto out;
    }
    if (mll_decode(u, tok + i, end - i, i, k, end == n_tok) != 0) {
      e = mll_expired(deadline_at) ? ASMODEL_ERR_TIMEOUT
          : mll_cancelled(cancel) ? ASMODEL_ERR_CANCELLED : ASMODEL_ERR_BACKEND;
      goto out;
    }
    i = end;
    if (u->needs_ckpt && i < n_tok) mll_ckpt_save(u, k, i);
  }

  n_past = n_tok;
  produced = 0;
  while (produced < limit) {
    llama_token t;
    if (mll_cancelled(cancel) || mll_expired(deadline_at)) {
      e = mll_expired(deadline_at) ? ASMODEL_ERR_TIMEOUT : ASMODEL_ERR_CANCELLED;
      goto out;
    }
    if (asmodel_llg_sampler_sample(chain, u->lctx, -1, &t) != 0) {
      e = ASMODEL_ERR_BACKEND;
      goto out;
    }
    if (llama_vocab_is_eog(u->vocab, t)) {
      hit_eog = true;
      break;
    }
    e = mll_emit_piece(u->vocab, t, &outbuf, token_cb, token_ud);
    if (e != ASMODEL_OK) goto out;
    produced++;
    if (produced >= limit) break;
    if (n_past >= room && u->kv_cache) room = mll_slot_room(u, k, n_past + 1);
    if (n_past >= room) break;
    if (mll_decode(u, &t, 1, n_past, k, true) != 0) {
      e = mll_expired(deadline_at) ? ASMODEL_ERR_TIMEOUT
          : mll_cancelled(cancel) ? ASMODEL_ERR_CANCELLED : ASMODEL_ERR_BACKEND;
      goto out;
    }
    n_past++;
  }

  *out_tokens_out = produced;
  *out_text = core_buf_detach(&outbuf); /* strdup("") when empty */
  if (*out_text == NULL) {
    e = ASMODEL_ERR_NOMEM;
    goto out;
  }
  e = !hit_eog && (produced >= limit || n_past >= room) ? ASMODEL_ERR_LIMIT : ASMODEL_OK;
  if (u->kv_cache) {
    /* A length stop leaves the sequence as consistent as an end of
     * generation. Without a record of this prompt the sequence starts
     * clean; keeping the previous one would match tokens it no longer
     * holds. */
    mll_slot *s = &u->slot[k];
    llama_token *rec = (llama_token *)malloc((size_t)n_tok * sizeof *rec);
    if (rec != NULL) {
      memcpy(rec, tok, (size_t)n_tok * sizeof *rec);
      free(s->prompt);
      s->prompt = rec;
      s->prompt_n = n_tok;
      s->n_mem = n_past;
    } else {
      mll_slot_clear(u, k);
    }
  }

out:
  mll_set_cancel(u, NULL, 0);
  if (chain != NULL) llama_sampler_free(chain);
  free(tok);
  free(prompt);
  if (!*out_text && outbuf.len) *out_text = core_buf_detach(&outbuf);
  *out_tokens_out = produced;
  core_buf_free(&outbuf);
  if (p->result_info) {
    asmodel_generation_info *info = p->result_info;
    memset(info,0,sizeof *info);
    info->input_tokens = *out_tokens_in; info->output_tokens = produced;
    info->cached_input_tokens = prompt_start;
    info->usage_known = !n_tok || e == ASMODEL_OK || e == ASMODEL_ERR_LIMIT;
    info->finish_reason = e == ASMODEL_OK ? ASMODEL_FINISH_STOP : e == ASMODEL_ERR_LIMIT ?
        ASMODEL_FINISH_LENGTH : e == ASMODEL_ERR_CANCELLED ? ASMODEL_FINISH_CANCELLED : ASMODEL_FINISH_ERROR;
    if (constrained) info->applied |= ASMODEL_APPLIED_CONSTRAINT;
    if (e != ASMODEL_OK) snprintf(info->error,sizeof info->error,"%s",asmodel_err_name(e));
  }
  if (e != ASMODEL_OK && e != ASMODEL_ERR_LIMIT && u->kv_cache && k >= 0)
    mll_slot_clear(u, k);
  return e;
}

/* ---- token counting ----------------------------------------------------- */

static int mll_count_tokens(void *ud, const char *text) {
  mll_ud *u = (mll_ud *)ud;
  int32_t n;

  if (text == NULL) return 0;
  n = asmodel_llg_tokenize(u->vocab, text, (int32_t)strlen(text), NULL, 0,
                         false, false);
  if (n == INT32_MIN) return -1;
  return n < 0 ? (int)-n : (int)n;
}

static int mll_count_prompt_tokens(void *ud, const asmodel_input *input) {
  mll_ud *u = (mll_ud *)ud;
  char *prompt = NULL;
  llama_token *tok = NULL;
  int32_t n_tok = 0;
  asmodel_err e = mll_apply_template(
      u->chat_template,input,&prompt);
  if (e != ASMODEL_OK) return -1;
  e = mll_tokenize(u->vocab, prompt, true, true, &tok, &n_tok);
  free(tok);
  free(prompt);
  return e == ASMODEL_OK ? (int)n_tok : -1;
}

/* ---- embedding ---------------------------------------------------------- */

static asmodel_err mll_embed_one(void *ud, const char *text, int is_query, asmodel_embedding_info *info, float *out) {
  mll_ud *u = (mll_ud *)ud;
  llama_token *tok = NULL;
  int32_t n_tok = 0;
  const float *emb;
  (void)is_query; /* Input preprocessing belongs to the request owner. */
  double norm;
  asmodel_err e;
  int i, rc;

  if (text == NULL) text = "";
  e = mll_tokenize(u->vocab, text, true, false, &tok, &n_tok);
  if (e == ASMODEL_OK && n_tok == 0) {
    /* Some vocabs tokenize "" to nothing even with add_special; embed a
     * single space so the call still produces a vector. */
    e = mll_tokenize(u->vocab, " ", true, false, &tok, &n_tok);
    if (e == ASMODEL_OK && n_tok == 0) e = ASMODEL_ERR_BACKEND;
  }
  if (e != ASMODEL_OK) {
    free(tok);
    return e;
  }
  if (n_tok > u->n_ctx) { free(tok); return ASMODEL_ERR_LIMIT; }

  /* Reset any sequence state left by the previous call (NULL-safe for
   * memory-less encoder contexts). */
  asmodel_llg_memory_clear(llama_get_memory(u->lctx), true);

  /* llama_batch_get_one: seq 0, auto positions; with embeddings enabled
   * every token is an output, which mean pooling requires. */
  {
    info->usage_known = 0;
    struct llama_batch batch = llama_batch_get_one(tok, n_tok);
    rc = u->encoder_only ? asmodel_llg_encode(u->lctx, batch)
                         : asmodel_llg_decode(u->lctx, batch);
  }
  free(tok);
  if (rc != 0) return ASMODEL_ERR_BACKEND;

  info->input_tokens = n_tok; info->usage_known = 1;
  emb = llama_get_embeddings_seq(u->lctx, 0);
  if (emb == NULL) return ASMODEL_ERR_BACKEND;

  norm = 0.0;
  for (i = 0; i < u->dim; i++) norm += (double)emb[i] * (double)emb[i];
  norm = sqrt(norm);
  if (norm > 0.0 && isfinite(norm)) {
    for (i = 0; i < u->dim; i++) out[i] = (float)((double)emb[i] / norm);
  } else {
    return ASMODEL_ERR_BACKEND;
  }
  return ASMODEL_OK;
}

/* One context evaluates batch rows sequentially; ggml observes the same
 * cancellation/deadline contract as generation. Completed rows remain usable. */
static int mll_embed(void *ud, const char *const *texts, size_t count,
                     int is_query, const asmodel_embed_params *params, float *out) {
  mll_ud *u = ud;
  int64_t now = os_monotonic_ms();
  int64_t deadline = params->deadline_ms > 0 ?
      (params->deadline_ms > INT64_MAX-now ? INT64_MAX : now+params->deadline_ms) : 0;
  asmodel_err e = ASMODEL_OK;
  asmodel_embedding_info total = {0}; total.usage_known = 1;
  mll_set_cancel(u,params->cancel,deadline);
  for (size_t i = 0; i < count; i++) {
    if (mll_cancelled(params->cancel)) { e = ASMODEL_ERR_CANCELLED; break; }
    if (mll_expired(deadline)) { e = ASMODEL_ERR_TIMEOUT; break; }
    asmodel_embedding_info row = {0}; row.usage_known = 1;
    e = mll_embed_one(u,texts[i],is_query,&row,out+i*(size_t)u->dim);
    total.usage_known = total.usage_known && row.usage_known;
    if (row.input_tokens > INT32_MAX-total.input_tokens) total.usage_known = 0;
    else total.input_tokens += row.input_tokens;
    if (e != ASMODEL_OK) break;
    total.completed++;
  }
  if (mll_cancelled(params->cancel)) e = ASMODEL_ERR_CANCELLED;
  else if (mll_expired(deadline)) e = ASMODEL_ERR_TIMEOUT;
  if (params->result_info) *params->result_info = total;
  mll_set_cancel(u,NULL,0);
  return e;
}

/* ---- lifecycle ---------------------------------------------------------- */

static void mll_destroy(void *ud) {
  mll_ud *u = (mll_ud *)ud;

  if (u == NULL) return;
  if (u->lctx != NULL) llama_free(u->lctx);
  if (u->model != NULL) llama_model_free(u->model);
  llama_batch_free(u->batch); /* NULL-safe: zeroed for embedding models */
  for (int k = 0; k < MLL_SLOTS; k++) {
    free(u->slot[k].prompt);
    mll_ckpt_drop_from(&u->slot[k], 0);
  }
  os_mutex_destroy(&u->cancel_mu);
  free(u);
}

#define MLL_FAIL(code, ...) \
  do { snprintf(error, error_size, __VA_ARGS__); e = (code); goto fail; } while (0)

int asmodel_llama_provider_create(const asmodel_spec *spec,
                                  asmodel_provider *out, char *error,
                                  size_t error_size) {
  struct llama_model_params mparams;
  struct llama_context_params cparams;
  mll_ud *u = NULL;
  asmodel_err e;
  int hw, threads, n_ctx;

  memset(out, 0, sizeof *out);
  if (error_size > 0) error[0] = '\0';
  if (spec == NULL || spec->path == NULL || spec->path[0] == '\0') {
    snprintf(error, error_size, "model '%s': path is not set",
             spec != NULL && spec->id != NULL ? spec->id : "?");
    return ASMODEL_ERR_INVALID;
  }

  mll_backend_init();

  u = (mll_ud *)calloc(1, sizeof *u);
  if (u == NULL) {
    snprintf(error, error_size, "out of memory");
    return ASMODEL_ERR_NOMEM;
  }
  os_mutex_init(&u->cancel_mu);
  snprintf(u->id, sizeof u->id, "%s", spec->id ? spec->id : "");
  u->embedding = spec->embedding != 0;
  u->kv_cache = spec->kv_cache && !spec->embedding;

  mparams = llama_model_default_params();
  /* -1 = every layer in VRAM (llama.h: negative means all); 0 keeps the
   * model on the CPU. CPU-only builds ignore the value entirely. */
  mparams.n_gpu_layers = spec->gpu_layers;
  u->model = llama_model_load_from_file(spec->path, mparams);
  if (u->model == NULL)
    MLL_FAIL(ASMODEL_ERR_BACKEND, "model '%s': failed to load %s", spec->id, spec->path);
  u->vocab = llama_model_get_vocab(u->model);
  if (u->vocab == NULL)
    MLL_FAIL(ASMODEL_ERR_BACKEND, "model '%s': no vocab in %s", spec->id, spec->path);
  u->needs_ckpt = u->kv_cache && (llama_model_is_recurrent(u->model) ||
                                  llama_model_is_hybrid(u->model));

  hw = os_hardware_threads();
  threads = spec->threads > 0 ? spec->threads : 4;
  if (threads > hw) threads = hw;

  cparams = llama_context_default_params();
  cparams.n_threads = threads;
  cparams.n_threads_batch = threads;
  /* Cooperative cancellation inside long llama_decode calls: the installed
   * per-call cancel flag aborts the graph compute (CPU backend only; the
   * per-token checks in mll_generate cover the rest). */
  cparams.abort_callback = mll_abort_cb;
  cparams.abort_callback_data = u;

  if (u->embedding) {
    u->dim = (int)llama_model_n_embd_out(u->model);
    if (u->dim <= 0)
      MLL_FAIL(ASMODEL_ERR_BACKEND, "model '%s': no usable embedding output", spec->id);
    /* The declared dimension sizes every caller's output buffer; a
     * mismatch with the actual weights would corrupt memory. */
    if (spec->embedding_dim > 0 && spec->embedding_dim != u->dim)
      MLL_FAIL(ASMODEL_ERR_BACKEND,
               "model '%s': embedding dim %d does not match configured dim %d",
               spec->id, u->dim, spec->embedding_dim);
    u->encoder_only = llama_model_has_encoder(u->model) &&
                      !llama_model_has_decoder(u->model);
    n_ctx = spec->context_tokens > 0 ? spec->context_tokens : MLL_EMBED_CTX;
    cparams.n_ctx = (uint32_t)n_ctx;
    cparams.n_batch = (uint32_t)n_ctx;
    cparams.n_ubatch = (uint32_t)n_ctx; /* pooling: input in one ubatch */
    cparams.embeddings = true;
    cparams.pooling_type = LLAMA_POOLING_TYPE_MEAN;
  } else {
    u->chat_template = llama_model_chat_template(u->model, NULL);
    n_ctx = spec->context_tokens > 0 ? spec->context_tokens : 32768;
    cparams.n_ctx = (uint32_t)n_ctx;
    cparams.n_batch = MLL_N_BATCH;
    cparams.n_ubatch = MLL_N_BATCH;
    /* One sequence per kept prompt over one unified cache, so any prompt
     * may still use the whole context (mll_slot_room). Recurrent memory
     * holds one state per sequence. */
    u->n_slots = u->kv_cache ? MLL_SLOTS : 1;
    cparams.n_seq_max = (uint32_t)u->n_slots;
    cparams.kv_unified = true;
  }

  u->lctx = llama_init_from_model(u->model, cparams);
  if (u->lctx == NULL)
    MLL_FAIL(ASMODEL_ERR_BACKEND, "model '%s': failed to create context", spec->id);
  u->n_ctx = (int)llama_n_ctx(u->lctx);
  u->n_batch = (int)llama_n_batch(u->lctx);
  if (u->n_batch <= 0) u->n_batch = MLL_N_BATCH;
  /* Embedding truncation bound: the whole input must fit both the context
   * and one logical batch (llama may round n_ctx up past the request). */
  if (u->embedding && u->n_batch < u->n_ctx) u->n_ctx = u->n_batch;
  if (!u->embedding) {
    u->batch = llama_batch_init(u->n_batch, 0, 1);
    if (u->batch.token == NULL || u->batch.seq_id == NULL)
      MLL_FAIL(ASMODEL_ERR_NOMEM, "model '%s': out of memory", spec->id);
  }

  out->userdata = u;
  /* Counts include the chat template, so admission is exact. */
  out->token_quality = ASMODEL_TOKENS_EXACT;
  out->tokenizer_id = u->id;
  out->chat_template_id = u->chat_template != NULL ? "gguf" : "chatml";
  out->count_tokens = mll_count_tokens;
  out->count_prompt_tokens = mll_count_prompt_tokens;
  out->destroy = mll_destroy;
  if (u->embedding)
    out->embed = mll_embed;
  else
    out->generate = mll_generate;
  return ASMODEL_OK;

fail:
  mll_destroy(u);
  memset(out, 0, sizeof *out);
  return e;
}

int asmodel_llama_gpu(void) {
  mll_backend_init();
  return llama_supports_gpu_offload() ? 1 : 0;
}

#else /* !ASTERISM_WITH_LLAMA */

int asmodel_llama_gpu(void) { return -1; }

int asmodel_llama_provider_create(const asmodel_spec *spec,
                                  asmodel_provider *out, char *error,
                                  size_t error_size) {
  (void)spec;
  memset(out, 0, sizeof *out);
  snprintf(error, error_size, "built without embedded llama.cpp");
  return ASMODEL_ERR_UNSUPPORTED;
}

#endif /* ASTERISM_WITH_LLAMA */
