/* Probe the supported revision before any tool effect; never downgrade implicitly. */
#include "mcp_client.h"
astools_err astools_mcp_initialize(astools_ctx *c, astools_pproc *p, int64_t deadline,
                                   astools_task *cancel) {
  p->rpc_lines = true;
  p->rpc_serial = 0;
  p->rpc_error[0] = 0;
  p->mcp_catalog_epoch = 0;
  asmodel_json_value *reply = NULL;
  astools_err e =
      astools_mcp_request(c, p, "server/discover", asmodel_json_object(), deadline, cancel, &reply);
  if (e != ASTOOLS_OK) return e;
  const asmodel_json_value *versions = asmodel_json_object_get(reply, "supportedVersions"),
                 *caps = asmodel_json_object_get(reply, "capabilities");
  bool supported = false;
  if (asmodel_json_typeof(versions) != ASMODEL_JSON_ARRAY || !asmodel_json_array_len(versions) || asmodel_json_array_len(versions) > 32)
    e = ASTOOLS_ERR_PROTOCOL;
  for (size_t i = 0; e == ASTOOLS_OK && i < asmodel_json_array_len(versions); i++) {
    const asmodel_json_value *v = asmodel_json_array_at(versions, i);
    const char *version = asmodel_json_string_value(v);
    if (!version || !*version || strlen(version) != asmodel_json_string_length(v) || strlen(version) > 64)
      e = ASTOOLS_ERR_PROTOCOL;
    else if (!strcmp(version, ASTOOLS_MCP_VERSION))
      supported = true;
  }
  if (e == ASTOOLS_OK && (!supported || asmodel_json_typeof(asmodel_json_object_get(caps, "tools")) != ASMODEL_JSON_OBJECT))
    e = ASTOOLS_ERR_UNSUPPORTED;
  if (e == ASTOOLS_ERR_UNSUPPORTED)
    snprintf(p->rpc_error, sizeof p->rpc_error, "Server must support MCP %s and tools",
             ASTOOLS_MCP_VERSION);
  asmodel_json_free(reply);
  return e;
}
