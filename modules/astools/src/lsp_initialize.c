/* Negotiate the capabilities actually used. No inferred UTF-16/UTF-8 offsets. */
#include "lsp.h"

#ifndef _WIN32
static int supported(const asmodel_json_value *caps, const char *name) {
  const asmodel_json_value *value = asmodel_json_object_get(caps, name);
  return (asmodel_json_typeof(value) == ASMODEL_JSON_BOOL && asmodel_json_bool_value(value)) || asmodel_json_typeof(value) == ASMODEL_JSON_OBJECT;
}
#endif

astools_err astools_lsp_initialize(astools_ctx *c, astools_pproc *p, int64_t deadline,
                                   astools_task *cancel) {
#ifdef _WIN32
  (void)c;
  (void)p;
  (void)deadline;
  (void)cancel;
  return ASTOOLS_ERR_UNSUPPORTED;
#else
  static const char capabilities[] =
      "{\"general\":{\"positionEncodings\":[\"utf-8\"]},"
      "\"workspace\":{\"applyEdit\":false},\"textDocument\":{"
      "\"documentSymbol\":{\"hierarchicalDocumentSymbolSupport\":true},"
      "\"definition\":{\"linkSupport\":true},\"publishDiagnostics\":{\"versionSupport\":true}}}";
  asmodel_json_value *params = asmodel_json_object(), *caps = NULL, *options = asmodel_json_object(), *reply = NULL;
  char *uri = astools_lsp_uri(c->workspace);
  p->rpc_serial = 0;
  p->rpc_lines = false;
  p->lsp_capabilities = 0;
  p->lsp_provider[0] = 0;
  p->rpc_error[0] = 0;
  int bad = !params || !options || !uri || asmodel_json_parse(capabilities, sizeof capabilities - 1, &caps);
  bad |= asmodel_json_object_set(params, "processId", asmodel_json_null());
  bad |= asmodel_json_object_set(params, "rootUri", asmodel_json_string(uri ? uri : ""));
  bad |= asmodel_json_object_set(params, "capabilities", caps);
  /* Clangd's explicit database path avoids ancestor-directory discovery. */
  bad |= asmodel_json_object_set(options, "compilationDatabasePath", asmodel_json_string(c->workspace));
  bad |= asmodel_json_object_set(params, "initializationOptions", options);
  free(uri);
  if (bad) {
    asmodel_json_free(params);
    return ASTOOLS_ERR_NOMEM;
  }
  astools_err e = astools_lsp_request(c, p, "initialize", params, deadline, cancel, &reply);
  if (e != ASTOOLS_OK) return e;
  const asmodel_json_value *reported = asmodel_json_object_get(reply, "capabilities"),
                 *sync = asmodel_json_object_get(reported, "textDocumentSync");
  const char *encoding = astools_lsp_string(reported, "positionEncoding");
  if (!encoding || strcmp(encoding, "utf-8") || !asmodel_json_bool_value(asmodel_json_object_get(sync, "openClose")))
    e = astools_seterr(
        c, ASTOOLS_ERR_UNSUPPORTED,
        "language server must negotiate UTF-8 positions and open/close synchronization");
  if (supported(reported, "documentSymbolProvider")) p->lsp_capabilities |= LSP_SYMBOLS;
  if (supported(reported, "definitionProvider")) p->lsp_capabilities |= LSP_DEFINITION;
  if (supported(reported, "referencesProvider")) p->lsp_capabilities |= LSP_REFERENCES;
  const asmodel_json_value *info = asmodel_json_object_get(reply, "serverInfo");
  const char *name = astools_lsp_string(info, "name"),
             *version = astools_lsp_string(info, "version");
  if (!name) name = "unknown";
  if (strlen(name) + (version ? strlen(version) + 2 : 0) >= sizeof p->lsp_provider)
    e = ASTOOLS_ERR_PROTOCOL;
  else
    snprintf(p->lsp_provider, sizeof p->lsp_provider, "%s%s%s", name, version ? ": " : "",
             version ? version : "");
  asmodel_json_free(reply);
  return e == ASTOOLS_OK ? astools_lsp_send(c, p, 0, "initialized", asmodel_json_object(), deadline, cancel)
                         : e;
#endif
}
