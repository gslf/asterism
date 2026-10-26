/* One exclusive query opens exact content, observes results, then closes it. */
#include "lsp_results.h"

static const char *arg(const xcdn_node_t *args, const char *key) {
  const xcdn_node_t *value = xcdn_object_get(args->value, key);
  return value && value->value && value->value->type == XCDN_VAL_STRING ? value->value->data.string
                                                                        : NULL;
}
static long long number(const xcdn_node_t *args, const char *key) {
  const xcdn_node_t *value = xcdn_object_get(args->value, key);
  return value && value->value && value->value->type == XCDN_VAL_INT ? value->value->data.integer
                                                                     : -1;
}
static asmodel_json_value *document(const astools_lsp_file *file) {
  asmodel_json_value *params = asmodel_json_object(), *doc = asmodel_json_object();
  int bad = !params || !doc;
  bad |= asmodel_json_object_set(doc, "uri", asmodel_json_string(file->uri));
  bad |= asmodel_json_object_set(params, "textDocument", doc);
  if (bad) {
    asmodel_json_free(params);
    return NULL;
  }
  return params;
}

static astools_err query(astools_ctx *c, astools_pproc *p, const char *command,
                         const xcdn_node_t *args, astools_lsp_file *file, int64_t deadline,
                         astools_task *cancel, asmodel_json_value **out) {
  const char *method = !strcmp(command, "symbols")      ? "textDocument/documentSymbol"
                       : !strcmp(command, "definition") ? "textDocument/definition"
                                                        : "textDocument/references";
  unsigned capability = !strcmp(command, "symbols")      ? LSP_SYMBOLS
                        : !strcmp(command, "definition") ? LSP_DEFINITION
                                                         : LSP_REFERENCES;
  int diagnostics = !strcmp(command, "diagnostics"),
      positional = !strcmp(command, "definition") || !strcmp(command, "references");
  if (!diagnostics && !(p->lsp_capabilities & capability)) return ASTOOLS_ERR_UNSUPPORTED;
  if (p->rpc_serial >= INT32_MAX - 1) return ASTOOLS_ERR_TOOL;
  const char *dot = strrchr(file->path, '.');
  if (!dot ||
      (strcmp(dot, ".c") && strcmp(dot, ".h") && strcmp(dot, ".cc") && strcmp(dot, ".cpp") &&
       strcmp(dot, ".cxx") && strcmp(dot, ".hpp") && strcmp(dot, ".hh") && strcmp(dot, ".hxx")))
    return ASTOOLS_ERR_UNSUPPORTED;
  long long line = number(args, "line"), column = number(args, "column");
  if (positional && (line < 1 || line > INT32_MAX || column < 1 || column > INT32_MAX))
    return ASTOOLS_ERR_INVALID;
  asmodel_json_value *params = document(file), *request = document(file);
  if (!params || !request) {
    asmodel_json_free(params);
    asmodel_json_free(request);
    return ASTOOLS_ERR_NOMEM;
  }
  asmodel_json_value *doc = asmodel_json_object_get(params, "textDocument");
  int version = ++p->rpc_serial;
  int bad = asmodel_json_object_set(doc, "version", asmodel_json_int(version));
  bad |= asmodel_json_object_set(doc, "languageId", asmodel_json_string(!strcmp(dot, ".c") ? "c" : "cpp"));
  bad |= asmodel_json_object_set(doc, "text", asmodel_json_string(file->text));
  if (positional) {
    asmodel_json_value *position = asmodel_json_object();
    bad |= asmodel_json_object_set(position, "line", asmodel_json_int(line - 1));
    bad |= asmodel_json_object_set(position, "character", asmodel_json_int(column - 1));
    size_t offset = 0;
    if (!astools_lsp_offset(file, position, &offset)) {
      asmodel_json_free(position);
      asmodel_json_free(params);
      asmodel_json_free(request);
      return ASTOOLS_ERR_INVALID;
    }
    bad |= asmodel_json_object_set(request, "position", position);
    if (!strcmp(command, "references")) {
      asmodel_json_value *context = asmodel_json_object();
      bad |= asmodel_json_object_set(context, "includeDeclaration", asmodel_json_bool(1));
      bad |= asmodel_json_object_set(request, "context", context);
    }
  }
  if (bad) {
    asmodel_json_free(params);
    asmodel_json_free(request);
    return ASTOOLS_ERR_NOMEM;
  }
  astools_err e = astools_lsp_send(c, p, 0, "textDocument/didOpen", params, deadline, cancel);
  if (e == ASTOOLS_OK) {
    if (diagnostics) e = astools_lsp_receive(c, p, 0, file->uri, version, deadline, cancel, out);
    else {
      e = astools_lsp_request(c, p, method, request, deadline, cancel, out);
      request = NULL;
    }
  }
  asmodel_json_free(request);
  if (e == ASTOOLS_OK)
    e = astools_lsp_send(c, p, 0, "textDocument/didClose", document(file), deadline, cancel);
  return e;
}

astools_err astools_lsp_invoke(astools_ctx *c, astools_pproc *p, const astools_cmd *cmd,
                               const xcdn_node_t *args, const astools_effective *grants,
                               int64_t deadline, astools_task *cancel, astools_result *result) {
  astools_lsp_view view = {0};
  p->rpc_error[0] = 0;
  asmodel_json_value *reply = NULL, *output = NULL;
  view.ctx = c;
  view.grants = grants;
  view.deadline = deadline;
  view.cancel = cancel;
  const char *path = arg(args, "path"), *expected = arg(args, "sha256");
  if (!path || !expected || strlen(expected) != 64 ||
      (strcmp(cmd->name, "symbols") && strcmp(cmd->name, "definition") &&
       strcmp(cmd->name, "references") && strcmp(cmd->name, "diagnostics")))
    return ASTOOLS_ERR_INVALID;
  if (strspn(expected, "0123456789abcdef") != 64) return ASTOOLS_ERR_INVALID;
  astools_err e = astools_lsp_file_read(c->workspace, path, grants, &view.files[0]);
  if (e != ASTOOLS_OK) return e;
  view.count = 1;
  if (strcmp(expected, view.files[0].sha256)) {
    e = ASTOOLS_ERR_BUSY;
    goto done;
  }
  e = query(c, p, cmd->name, args, &view.files[0], deadline, cancel, &reply);
  if (e == ASTOOLS_OK) e = astools_lsp_render(&view, cmd->name, reply);
  if (e == ASTOOLS_OK) e = astools_lsp_recheck(&view);
  if (e == ASTOOLS_OK && astools_task_cancelled(cancel)) e = ASTOOLS_ERR_CANCELLED;
  if (e == ASTOOLS_OK && astools_mono(c) >= deadline) e = ASTOOLS_ERR_TIMEOUT;
  if (e != ASTOOLS_OK) goto done;
  output = asmodel_json_object();
  int bad = !output;
  bad |= asmodel_json_object_set(output, "provider", asmodel_json_string(p->lsp_provider));
  bad |= asmodel_json_object_set(output, "position_encoding", asmodel_json_string("utf-8"));
  bad |= asmodel_json_object_set(output, "source_sha256", asmodel_json_string(view.files[0].sha256));
  bad |= asmodel_json_object_set(output, "coverage",
                       asmodel_json_string("server locations with current host file hashes; dependency "
                                 "freshness and index coverage are not certified"));
  bad |= asmodel_json_object_set(output, "truncated", asmodel_json_bool(view.truncated));
  bad |= asmodel_json_object_set(output, "excluded", asmodel_json_int((long long)view.excluded));
  bad |= asmodel_json_object_set(output, "items", view.items);
  view.items = NULL;
  if (bad || !(result->result_xcdn = asmodel_json_write(output, 0))) e = ASTOOLS_ERR_NOMEM;
  else if (c->cfg.max_output_bytes > 0 &&
           strlen(result->result_xcdn) > (size_t)c->cfg.max_output_bytes) {
    free(result->result_xcdn);
    result->result_xcdn = NULL;
    e = ASTOOLS_ERR_TOOL;
  } else result->ok = true;
done:
  asmodel_json_free(output);
  asmodel_json_free(reply);
  astools_lsp_view_free(&view);
  return e;
}
