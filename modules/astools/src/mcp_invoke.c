/* Preserve the MCP result envelope; references are data, never automatic reads. */
#include "mcp_client.h"
#include "mcp_schema.h"
static bool content_valid(const asmodel_json_value *item) {
  const char *type = astools_rpc_string(item, "type");
  if (!type) return false;
  if (!strcmp(type, "text")) return astools_rpc_string(item, "text") != NULL;
  if (!strcmp(type, "image") || !strcmp(type, "audio"))
    return astools_rpc_string(item, "data") && astools_rpc_string(item, "mimeType");
  if (!strcmp(type, "resource_link"))
    return astools_rpc_string(item, "uri") && astools_rpc_string(item, "name");
  if (!strcmp(type, "resource")) {
    const asmodel_json_value *resource = asmodel_json_object_get(item, "resource");
    return astools_rpc_string(resource, "uri") && ((astools_rpc_string(resource, "text") != NULL) !=
                                                   (astools_rpc_string(resource, "blob") != NULL));
  }
  return false;
}
astools_err astools_mcp_invoke(astools_ctx *c, astools_pproc *p, const astools_cmd *cmd,
                               const xcdn_node_t *args, int64_t deadline, astools_task *cancel,
                               astools_result *result) {
  p->rpc_error[0] = 0;
  asmodel_json_value *arguments = astools_mcp_arguments(args);
  if (!arguments) return ASTOOLS_ERR_INVALID;
  astools_err e = astools_mcp_check_tool(c, p, cmd, deadline, cancel);
  if (e != ASTOOLS_OK) {
    asmodel_json_free(arguments);
    return e;
  }
  asmodel_json_value *params = asmodel_json_object(), *reply = NULL;
  int bad = !params;
  bad |= asmodel_json_object_set(params, "name", asmodel_json_string(cmd->mcp_name));
  bad |= asmodel_json_object_set(params, "arguments", arguments);
  if (bad) {
    asmodel_json_free(params);
    return ASTOOLS_ERR_NOMEM;
  }
  e = astools_mcp_request(c, p, "tools/call", params, deadline, cancel, &reply);
  if (e != ASTOOLS_OK) return e;
  const char *type = astools_rpc_string(reply, "resultType");
  bool incomplete = type && !strcmp(type, "input_required");
  const asmodel_json_value *content = asmodel_json_object_get(reply, "content"),
                 *failed = asmodel_json_object_get(reply, "isError"),
                 *structured = asmodel_json_object_get(reply, "structuredContent");
  for (size_t i = 0; i < asmodel_json_object_count(reply); i++) {
    const char *key = asmodel_json_object_key_at(reply, i);
    bool common = !strcmp(key, "resultType") || !strcmp(key, "_meta");
    bool allowed = incomplete ? (!strcmp(key, "inputRequests") || !strcmp(key, "requestState"))
                              : (!strcmp(key, "content") || !strcmp(key, "isError") ||
                                 !strcmp(key, "structuredContent"));
    if (!common && !allowed) e = ASTOOLS_ERR_PROTOCOL;
  }
  if (incomplete) {
    const asmodel_json_value *requests = asmodel_json_object_get(reply, "inputRequests"),
                   *state = asmodel_json_object_get(reply, "requestState");
    /* No client capabilities were advertised. Preserve state without inspecting or replaying it. */
    if ((!requests && !state) ||
        (requests && (asmodel_json_typeof(requests) != ASMODEL_JSON_OBJECT || asmodel_json_object_count(requests))) ||
        (state && !astools_rpc_string(reply, "requestState")))
      e = ASTOOLS_ERR_PROTOCOL;
  } else {
    if (asmodel_json_typeof(content) != ASMODEL_JSON_ARRAY || asmodel_json_array_len(content) > 128 ||
        (failed && asmodel_json_typeof(failed) != ASMODEL_JSON_BOOL) ||
        (cmd->mcp_output_schema && !asmodel_json_bool_value(failed) && !structured))
      e = ASTOOLS_ERR_PROTOCOL;
    for (size_t i = 0; e == ASTOOLS_OK && i < asmodel_json_array_len(content); i++)
      if (!content_valid(asmodel_json_array_at(content, i))) e = ASTOOLS_ERR_PROTOCOL;
    if (e == ASTOOLS_OK && structured && cmd->mcp_output_schema) {
      asmodel_json_value *output = NULL;
      if (asmodel_json_parse(cmd->mcp_output_schema, strlen(cmd->mcp_output_schema), &output) ||
          !astools_mcp_schema_validate(output, structured)) {
        e = ASTOOLS_ERR_PROTOCOL;
        snprintf(p->rpc_error, sizeof p->rpc_error,
                 "Result violates the reviewed MCP output schema");
      }
      asmodel_json_free(output);
    }
  }
  if (e == ASTOOLS_OK && astools_task_cancelled(cancel)) e = ASTOOLS_ERR_CANCELLED;
  if (e == ASTOOLS_OK && astools_mono(c) >= deadline) e = ASTOOLS_ERR_TIMEOUT;
  if (e == ASTOOLS_OK) {
    result->result_xcdn = asmodel_json_write(reply, 0);
    if (!result->result_xcdn)
      e = ASTOOLS_ERR_NOMEM;
    else if (c->cfg.max_output_bytes > 0 &&
             strlen(result->result_xcdn) > (size_t)c->cfg.max_output_bytes)
      e = ASTOOLS_ERR_TOOL;
    else {
      result->ok = !incomplete && !asmodel_json_bool_value(failed);
      if (!result->ok) {
        result->error_code =
            core_strdup(incomplete ? "astools/mcp-input-required" : "astools/mcp-tool");
        result->error_message = core_strdup(
            incomplete ? "MCP request is incomplete; no automatic continuation was executed"
                       : "MCP tool reported an execution error; see the complete result");
        if (!result->error_code || !result->error_message) e = ASTOOLS_ERR_NOMEM;
      }
    }
  }
  asmodel_json_free(reply);
  return e;
}
