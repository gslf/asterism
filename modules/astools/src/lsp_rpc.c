/* LSP-specific notifications use the common bounded JSON-RPC transport. */
#include "lsp.h"
#include "rpc.h"
typedef struct { const char *uri; int version; } observation;
static astools_err diagnostics(void *ud, const char *method, const asmodel_json_value *params, asmodel_json_value **out) {
  observation *want = ud;
  if (strcmp(method,"textDocument/publishDiagnostics")) return ASTOOLS_OK;
  const char *uri = astools_rpc_string(params,"uri");
  const asmodel_json_value *version = asmodel_json_object_get(params,"version"), *items = asmodel_json_object_get(params,"diagnostics");
  if (!uri || strcmp(uri,want->uri) || !asmodel_json_is_int(version) || asmodel_json_int_value(version) != want->version)
    return ASTOOLS_OK;
  if (asmodel_json_typeof(items) != ASMODEL_JSON_ARRAY) return ASTOOLS_ERR_PROTOCOL;
  *out = asmodel_json_clone(items); return *out ? ASTOOLS_OK : ASTOOLS_ERR_NOMEM;
}
astools_err astools_lsp_receive(astools_ctx *c, astools_pproc *p, int id, const char *uri,
    int version, int64_t deadline, astools_task *cancel, asmodel_json_value **out) {
  observation want = {uri,version};
  return astools_rpc_receive(c,p,id,id ? NULL : diagnostics,&want,deadline,cancel,out);
}
