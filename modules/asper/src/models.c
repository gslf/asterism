/* Model bridge. The engine lends its model manager through
 * asper_open_params.models; asper never loads models itself. */
#include "asper_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  asmodel_manager *manager;
  char id[ASMODEL_ID_MAX];
  asper_ctx *ctx;
  int embedding_dim;
} model_ref;

static asper_err memory_error(asmodel_err e) {
  switch (e) {
    case ASMODEL_OK: return ASPER_OK;
    case ASMODEL_ERR_TIMEOUT: return ASPER_ERR_TIMEOUT;
    case ASMODEL_ERR_CANCELLED: return ASPER_ERR_CANCELLED;
    case ASMODEL_ERR_LIMIT: return ASPER_ERR_LIMIT;
    case ASMODEL_ERR_NOMEM: return ASPER_ERR_NOMEM;
    case ASMODEL_ERR_INVALID: return ASPER_ERR_INVALID;
    case ASMODEL_ERR_BUSY: return ASPER_ERR_BUSY;
    case ASMODEL_ERR_NOT_FOUND: return ASPER_ERR_NOT_FOUND;
    case ASMODEL_ERR_UNSUPPORTED: return ASPER_ERR_UNSUPPORTED;
    default: return ASPER_ERR_MODEL;
  }
}

static asper_err managed_embed(void *ud, const char *text, int is_query,
                               const asmodel_embed_params *control, float *out) {
  model_ref *r = (model_ref *)ud;
  asmodel_embed_params params = control ? *control : (asmodel_embed_params){0};
  if (!control) params.deadline_ms = r->ctx->cfg.recall_timeout_s > INT64_MAX/1000 ?
      INT64_MAX : r->ctx->cfg.recall_timeout_s*1000;
  asmodel_err e = asmodel_embed(r->manager,r->id,&text,1,is_query,&params,out,
                               (size_t)r->embedding_dim);
  return memory_error(e);
}

static int managed_count(void *ud, const char *text) {
  model_ref *r = (model_ref *)ud;
  return asmodel_count_tokens(r->manager, r->id, text);
}

static asper_err managed_generate(void *ud, const asmodel_input *input,
                                  const char *grammar, const asper_output_contract *contract,
                                  const asmodel_generate_params *params, volatile int *cancel, char **out) {
  model_ref *r = (model_ref *)ud;
  asmodel_generate_params p = *params;
  asmodel_generation_info local = {0};
  asmodel_generation_info *info = p.result_info ? p.result_info : &local;
  char *schema = contract ? asper_output_schema(contract) : NULL;
  if (contract && !schema) return ASPER_ERR_NOMEM;
  p.output_schema = schema; p.require_constraint = grammar != NULL || schema != NULL;
  p.result_info = info;
  asmodel_err e = asmodel_generate(r->manager, r->id, input, grammar, &p,
                                   NULL, NULL, cancel, out, NULL, NULL);
  free(schema);
  if (e == ASMODEL_OK) return info->json_output ? asper_output_decode(out) : ASPER_OK;
  return asper_seterr(r->ctx,memory_error(e),"model '%s': %s",r->id,info->error);

}

static void ref_destroy(void *ud) { free(ud); }

static model_ref *make_ref(asper_ctx *c, asmodel_manager *m, const char *id) {
  model_ref *r;
  if (!m || !id || !id[0]) return NULL;
  r = (model_ref *)calloc(1, sizeof *r);
  if (!r) return NULL;
  r->manager = m;
  r->ctx = c;
  snprintf(r->id, sizeof r->id, "%s", id);
  return r;
}

/* Missing remote revisions intentionally invalidate derived vectors on reopen. */
static asper_err pipeline_hash(asper_ctx *c, const char *id, uint8_t out[32]) {
  char *key = NULL;
  asmodel_err e = asmodel_manager_embedding_key(c->model_manager,id,&key);
  if (e == ASMODEL_OK) core_sha256(key,strlen(key),out);
  else if (e == ASMODEL_ERR_UNSUPPORTED) {
    char nonce[37]; core_uuid_v4(nonce); core_sha256(nonce,strlen(nonce),out);
    asper_log(c,ASPER_LOG_WARN,"model","embedding pipeline revision is unknown; persistent vectors will be rebuilt on reopen");
  } else return memory_error(e);
  free(key); return ASPER_OK;
}

asper_err asper_models_bind(asper_ctx *c, const asper_model_binding *binding,
                            asper_embedder *emb, asper_curator_iface *cur) {
  model_ref *er, *cr;
  memset(emb, 0, sizeof *emb); memset(cur, 0, sizeof *cur);
  if (!binding) return ASPER_OK;
  if (!binding->manager) return ASPER_ERR_INVALID;
  c->model_manager = binding->manager;
  er = make_ref(c, c->model_manager, binding->embedding_model_id);
  cr = make_ref(c, c->model_manager, binding->curator_model_id);
  if ((binding->embedding_model_id && binding->embedding_model_id[0] && !er) ||
      (binding->curator_model_id && binding->curator_model_id[0] && !cr)) {
    free(er); free(cr); return ASPER_ERR_NOMEM;
  }
  if (er) {
    emb->ud = er;
    emb->dim = binding->embedding_dim;
    er->embedding_dim = emb->dim;
    snprintf(emb->model_id, sizeof emb->model_id, "%s", er->id);
    asper_err e = pipeline_hash(c, er->id, emb->model_hash);
    if (e != ASPER_OK) { free(er); free(cr); memset(emb,0,sizeof *emb); return e; }
    emb->embed = managed_embed; emb->destroy = ref_destroy;
  }
  if (cr) {
    cur->ud = cr; cur->generate = managed_generate;
    cur->count_tokens = managed_count; cur->destroy = ref_destroy;
  }
  return ASPER_OK;
}

void asper_models_shutdown(asper_ctx *c) {
  if (c) c->model_manager = NULL; /* borrowed */
}
