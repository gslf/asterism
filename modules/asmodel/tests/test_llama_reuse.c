/* Prompt reuse on a real GGUF: set ASMODEL_TEST_GGUF to a chat model (exit 77,
 * skipped, otherwise). The prompts mimic the engine's: one user message rebuilt
 * each step with new evidence inserted before the turn's instruction, which
 * carries a step counter, and prompts of other roles interleaved on the same
 * model. A recurrent or hybrid model (Qwen3.5) exercises the state snapshots;
 * an attention-only model the in-place truncation. Either way, nearly the
 * whole prompt must be reused, other prompts must not evict it while the
 * provider has room for four, and the answer must match a context that
 * evaluated it from scratch. */
#include "asmodel.h"
#include "core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"failed at %d: %s\n",__LINE__,#x); return 1; } } while (0)

static const char *colours[] = {"red","green","blue","yellow","white","black","orange"};

typedef struct { char *text; int input, cached; int64_t ms; } reply;

static int ask_n(asmodel_provider *p, const char *system, int items, int max_tokens, reply *r) {
  core_buf user;
  asmodel_text_input in;
  asmodel_generation_info info;
  asmodel_generate_params gp;
  int tin = 0, tout = 0, rc;
  int64_t t0 = os_monotonic_ms();
  core_buf_init(&user);
  core_buf_printf(&user, "Inventory log, oldest first:\n");
  for (int i = 1; i <= items; i++)
    core_buf_printf(&user, "- item %d: a %s box delivered to shelf %d\n", i,
                    colours[i % 7], (i * 7) % 13);
  core_buf_printf(&user, "\n## This turn\nWhat colour is the last item? Answer with one word.\n"
                         "Remaining steps: %d. Stop at zero.\n", 100 - items);
  asmodel_input_pair(&in, system, user.data);
  memset(&info, 0, sizeof info);
  memset(&gp, 0, sizeof gp);
  gp.max_tokens = max_tokens;
  gp.result_info = &info;
  memset(r, 0, sizeof *r);
  rc = p->generate(p->userdata, &in.input, NULL, &gp, NULL, NULL, NULL, &r->text, &tin, &tout);
  r->input = info.input_tokens;
  r->cached = info.cached_input_tokens;
  r->ms = os_monotonic_ms() - t0;
  printf("%-5s %-14.14s items=%d prompt=%d reused=%d %lldms -> %s\n", p->tokenizer_id, system,
         items, r->input, r->cached, (long long)r->ms, r->text ? r->text : "(null)");
  core_buf_free(&user);
  return rc == ASMODEL_OK || rc == ASMODEL_ERR_LIMIT ? 0 : rc;
}

static int ask(asmodel_provider *p, const char *system, int items, reply *r) {
  return ask_n(p, system, items, 8, r);
}

/* The fresh context must give the same answer to the same prompt. */
static int same_as_fresh(asmodel_provider *fresh, const char *system, int items, const reply *r) {
  reply f;
  int ok;
  if (ask(fresh, system, items, &f)) return 0;
  ok = f.cached == 0 && f.input == r->input && f.text && r->text && !strcmp(f.text, r->text);
  free(f.text);
  return ok;
}

static int open_model(const char *path, int kv_cache, asmodel_provider *p) {
  char error[256];
  asmodel_spec spec;
  memset(&spec, 0, sizeof spec);
  spec.id = kv_cache ? "reuse" : "fresh";
  spec.path = path;
  spec.context_tokens = 4096;
  spec.threads = 4;
  spec.kv_cache = kv_cache;
  if (asmodel_llama_provider_create(&spec, p, error, sizeof error) != ASMODEL_OK) {
    fprintf(stderr, "%s\n", error);
    return 1;
  }
  return 0;
}

int main(void) {
  const char *path = getenv("ASMODEL_TEST_GGUF");
  const char *base = "You are a terse inventory assistant.";
  const char *other = "Answer questions about the warehouse log.";
  /* Short prompts of further roles: their first words differ from the
   * others', so they share nothing past the chat template. */
  const char *roles[] = {"Translate every reply into French.", "Count shelves, nothing else.",
                         "Keep answers under ten words."};
  asmodel_provider reuse, fresh;
  reply r;
  if (!path || !*path) {
    printf("ASMODEL_TEST_GGUF not set: skipped\n");
    return 77;
  }
  CHECK(!open_model(path, 1, &reuse));
  CHECK(!open_model(path, 0, &fresh));

  CHECK(!ask(&reuse, base, 80, &r));
  CHECK(r.cached == 0 && r.input > 1200);
  free(r.text);
  /* The first rebuild can only reuse up to a chunk boundary... */
  CHECK(!ask(&reuse, base, 81, &r));
  CHECK(r.cached >= 512);
  free(r.text);
  /* ...after which the previous insertion shows where new content goes:
   * only the new item and the instruction are evaluated. */
  CHECK(!ask(&reuse, base, 82, &r));
  CHECK(r.input - r.cached <= 80);
  CHECK(same_as_fresh(&fresh, base, 82, &r));
  free(r.text);

  /* A prompt sharing only the first items, less than half of that one,
   * starts from a copy of the shared prefix (one snapshot's worth on
   * recurrent memory) in a sequence of its own, leaving the longer
   * prompt's sequence as it was. */
  CHECK(!ask(&reuse, base, 36, &r));
  CHECK(r.cached >= 256);
  CHECK(same_as_fresh(&fresh, base, 36, &r));
  free(r.text);

  /* A different system prompt shares no usable prefix and takes a
   * sequence of its own... */
  CHECK(!ask(&reuse, other, 82, &r));
  CHECK(r.cached < 16);
  CHECK(same_as_fresh(&fresh, other, 82, &r));
  free(r.text);
  /* ...so the first one still continues where it was, and so does the
   * second one. */
  CHECK(!ask(&reuse, base, 83, &r));
  CHECK(r.input - r.cached <= 80);
  CHECK(same_as_fresh(&fresh, base, 83, &r));
  free(r.text);
  CHECK(!ask(&reuse, other, 83, &r));
  CHECK(r.cached >= 512);
  CHECK(same_as_fresh(&fresh, other, 83, &r));
  free(r.text);

  /* One more role fills the four sequences, with an output budget past the
   * context: it must not empty the others for room it never uses. The next
   * role evicts the least recently used, the copy; base is used again, so
   * the one after that evicts other. */
  CHECK(!ask_n(&reuse, roles[0], 3, 1 << 20, &r));
  free(r.text);
  CHECK(!ask(&reuse, roles[1], 3, &r));
  free(r.text);
  CHECK(!ask(&reuse, base, 84, &r));
  CHECK(r.input - r.cached <= 80);
  free(r.text);
  CHECK(!ask(&reuse, roles[2], 3, &r));
  CHECK(r.cached < 16);
  free(r.text);
  CHECK(!ask(&reuse, other, 84, &r));
  CHECK(r.cached < 16);
  free(r.text);
  CHECK(!ask(&reuse, base, 85, &r));
  CHECK(r.input - r.cached <= 80);
  free(r.text);

  reuse.destroy(reuse.userdata);
  fresh.destroy(fresh.userdata);
  return 0;
}
