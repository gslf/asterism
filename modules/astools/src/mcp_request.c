/* Every request carries its own identity and capabilities; transport is not a session. */
#include "mcp_client.h"
astools_err astools_mcp_request(astools_ctx *c, astools_pproc *p, const char *method,
                                asmodel_json_value *params, int64_t deadline, astools_task *cancel,
                                asmodel_json_value **out) {
  *out = NULL;
  asmodel_json_value *meta = asmodel_json_object(), *info = asmodel_json_object();
  int bad = !params || !meta || !info;
  bad |= asmodel_json_object_set(info, "name", asmodel_json_string("astools"));
  bad |= asmodel_json_object_set(info, "version", asmodel_json_string(ASTERISM_VERSION));
  bad |= asmodel_json_object_set(meta, "io.modelcontextprotocol/clientInfo", info);
  bad |= asmodel_json_object_set(meta, "io.modelcontextprotocol/clientCapabilities", asmodel_json_object());
  bad |= asmodel_json_object_set(meta, "io.modelcontextprotocol/protocolVersion",
                       asmodel_json_string(ASTOOLS_MCP_VERSION));
  bad |= asmodel_json_object_set(params, "_meta", meta);
  if (bad) {
    asmodel_json_free(params);
    return ASTOOLS_ERR_NOMEM;
  }
  astools_err e = astools_rpc_request(c, p, method, params, deadline, cancel, out);
  if (e != ASTOOLS_OK) return e;
  const char *type = astools_rpc_string(*out, "resultType");
  const asmodel_json_value *metadata = asmodel_json_object_get(*out, "_meta");
  /* The base protocol defines omitted resultType as complete. No handshake fallback. */
  if (asmodel_json_typeof(*out) != ASMODEL_JSON_OBJECT || (metadata && asmodel_json_typeof(metadata) != ASMODEL_JSON_OBJECT) ||
      (asmodel_json_object_get(*out, "resultType") &&
       (!type || (strcmp(type, "complete") &&
                  (strcmp(type, "input_required") || strcmp(method, "tools/call")))))) {
    asmodel_json_free(*out);
    *out = NULL;
    return ASTOOLS_ERR_PROTOCOL;
  }
  return ASTOOLS_OK;
}
