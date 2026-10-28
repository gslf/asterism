/* One adapter from ⁂ asmodel providers (embedded llama.cpp or HTTP) to the
 * engine's model interface. */
#include "asngn_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { asmodel_provider provider; } provider_ud;
typedef struct { asngn_token_fn fn; void *ud; } token_bridge;

static void provider_token(const char *utf8, size_t len, void *ud) {
  token_bridge *b = (token_bridge *)ud;
  (void)len;
  if (b->fn) b->fn(utf8, b->ud);
}

static asngn_err provider_generate(void *ud, const asmodel_input *input,
                                 const char *grammar,
                                 const asngn_gen_params *params,
                                 asngn_token_fn token_fn, void *token_ud,
                                 volatile int *cancel, char **out,
                                 int *out_in, int *out_gen) {
  provider_ud *u = (provider_ud *)ud;
  asmodel_generate_params p;
  token_bridge bridge;
  int rc;
  memset(&p, 0, sizeof p);
  p.temperature = params->temp; p.top_p = params->top_p;
  p.repeat_penalty = params->repeat_penalty;
  p.max_tokens = params->max_tokens;
  p.reasoning = params->reasoning;
  p.reasoning_budget = params->reasoning_budget;
  p.output_schema = params->output_schema; p.tools = params->tools;
  p.require_constraint = params->require_constraint ? 1 : 0;
  p.deadline_ms = params->deadline_ms; p.result_info = params->result_info;
  bridge.fn = token_fn; bridge.ud = token_ud;
  rc = u->provider.generate(u->provider.userdata, input, grammar, &p,
                            token_fn ? provider_token : NULL, &bridge, cancel,
                            out, out_in, out_gen);
  return asngn_from_model_error((asmodel_err)rc);
}

static asngn_err provider_embed(void *ud, const char *const *texts, size_t count,
                               int is_query, const asmodel_embed_params *params, float *out) {
  provider_ud *u = ud;
  return u->provider.embed ? asngn_from_model_error((asmodel_err)
      u->provider.embed(u->provider.userdata,texts,count,is_query,params,out)) : ASNGN_ERR_UNSUPPORTED;
}

static int provider_count(void *ud, const char *text) {
  provider_ud *u = (provider_ud *)ud;
  return u->provider.count_tokens ?
      u->provider.count_tokens(u->provider.userdata, text) : -1;
}

static int provider_count_prompt(void *ud, const asmodel_input *input) {
  provider_ud *u = (provider_ud *)ud;
  return asmodel_provider_measure_prompt(&u->provider, input).admission_tokens;
}

static void provider_destroy(void *ud) {
  provider_ud *u = (provider_ud *)ud;
  if (!u) return;
  if (u->provider.destroy) u->provider.destroy(u->provider.userdata);
  free(u);
}

asngn_err asngn_model_provider_create(asngn_ctx *c,
                                      const asngn_pool_entry *e,
                                      asngn_model_iface *out) {
  asmodel_spec spec;
  provider_ud *u;
  char error[256] = {0}, *auth_file = NULL;
  int rc;
  memset(out, 0, sizeof *out);
  memset(&spec, 0, sizeof spec);
  /* ChatGPT credentials, like every engine-root artifact, resolve under the
   * engine root when relative (the process cwd is arbitrary). */
  if (e->auth_file != NULL) {
    auth_file = os_path_is_abs(e->auth_file) || c->root == NULL
                    ? core_strdup(e->auth_file) : os_path_join(c->root, e->auth_file);
    if (auth_file == NULL) return ASNGN_ERR_NOMEM;
  }
  spec.auth_file = auth_file;
  spec.id = e->id; spec.backend = e->backend; spec.path = e->path;
  spec.base_url = e->base_url; spec.remote_model = e->remote_model;
  spec.api_key_env = e->api_key_env;
  spec.remote_provider = e->remote_provider;
  spec.context_tokens = e->ctx; spec.threads = e->threads;
  spec.gpu_layers = e->gpu_layers; spec.kv_cache = e->kv_cache;
  spec.embedding = e->embedding; spec.embedding_dim = e->dim;
  u = (provider_ud *)calloc(1, sizeof *u);
  if (!u) { free(auth_file); return ASNGN_ERR_NOMEM; }
  rc = asmodel_provider_create(&spec, &u->provider, error, sizeof error);
  free(auth_file); /* the provider keeps its own copy */
  if (rc != ASMODEL_OK) {
    free(u);
    return asngn_seterr(c, rc == ASMODEL_ERR_NOMEM ? ASNGN_ERR_NOMEM : ASNGN_ERR_MODEL,
                        "model '%s': %s", e->id,
                        error[0] ? error : asmodel_err_name((asmodel_err)rc));
  }
  out->ud = u;
  out->generate = !e->embedding && u->provider.generate ? provider_generate : NULL;
  out->embed = e->embedding && u->provider.embed ? provider_embed : NULL;
  out->count_tokens = provider_count;
  out->count_prompt_tokens = provider_count_prompt;
  out->destroy = provider_destroy;
  if (e->backend == ASMODEL_BACKEND_OPENAI)
    asngn_log(c, ASNGN_LOG_INFO, "model", "configured '%s' -> %s (%s)",
              e->id, e->remote_model, e->base_url);
  else
    asngn_log(c, ASNGN_LOG_INFO, "model", "loaded '%s' from %s%s", e->id,
              e->path, e->embedding ? " (embedding)" : "");
  return ASNGN_OK;
}
