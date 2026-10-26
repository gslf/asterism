/* Schema metadata can bind a reviewed contract, never grant a capability. */
#include "mcp_client.h"
#include "mcp_schema.h"
static const char *string(const xcdn_node_t *node, const char *key) {
  const xcdn_node_t *v = node && node->value ? xcdn_object_get(node->value, key) : NULL;
  return v && v->value && v->value->type == XCDN_VAL_STRING ? v->value->data.string : NULL;
}
static bool schema(const char *text, bool input) {
  asmodel_json_value *value = NULL;
  if (!text || strlen(text) > 65536 || asmodel_json_parse(text, strlen(text), &value)) return false;
  const char *type = astools_rpc_string(value, "type");
  bool valid = astools_mcp_schema_supported(value) && (!input || (type && !strcmp(type, "object")));
  asmodel_json_free(value);
  return valid;
}
bool astools_mcp_command_parse(const astools_manifest *m, astools_cmd *cmd,
                               const xcdn_node_t *binding) {
  if (m->protocol != ASTOOLS_PROTOCOL_MCP) return !binding;
  if (!binding || !binding->value || binding->value->type != XCDN_VAL_OBJECT || cmd->idempotent)
    return false;
  const char *name = string(binding, "name"), *input = string(binding, "input_schema"),
             *output = string(binding, "output_schema");
  if (!name || !*name || strlen(name) > 128 ||
      strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") !=
          strlen(name) ||
      !schema(input, true) ||
      (xcdn_object_get(binding->value, "output_schema") && !schema(output, false)))
    return false;
  if (binding->value->data.object.len != (output ? 3u : 2u)) return false;
  cmd->mcp_name = core_strdup(name);
  cmd->mcp_input_schema = core_strdup(input);
  if (output) cmd->mcp_output_schema = core_strdup(output);
  return cmd->mcp_name && cmd->mcp_input_schema && (!output || cmd->mcp_output_schema);
}

/* Object order is immaterial; arrays and every schema keyword retain meaning. */
bool astools_mcp_schema_equal(const asmodel_json_value *a, const asmodel_json_value *b) {
  if (!a || !b) return a == b;
  if (asmodel_json_typeof(a) != asmodel_json_typeof(b)) return false;
  switch (asmodel_json_typeof(a)) {
  case ASMODEL_JSON_NULL:
    return true;
  case ASMODEL_JSON_BOOL:
    return asmodel_json_bool_value(a) == asmodel_json_bool_value(b);
  case ASMODEL_JSON_NUMBER:
    if (asmodel_json_is_int(a) && asmodel_json_is_int(b)) return asmodel_json_int_value(a) == asmodel_json_int_value(b);
    if (asmodel_json_is_int(a) != asmodel_json_is_int(b)) {
      long long n = asmodel_json_int_value(asmodel_json_is_int(a) ? a : b);
      if (n < -9007199254740992LL || n > 9007199254740992LL) return false;
    }
    return asmodel_json_double_value(a) == asmodel_json_double_value(b);
  case ASMODEL_JSON_STRING:
    return asmodel_json_string_length(a) == asmodel_json_string_length(b) &&
           !memcmp(asmodel_json_string_value(a), asmodel_json_string_value(b), asmodel_json_string_length(a));
  case ASMODEL_JSON_ARRAY:
    if (asmodel_json_array_len(a) != asmodel_json_array_len(b)) return false;
    for (size_t i = 0; i < asmodel_json_array_len(a); i++)
      if (!astools_mcp_schema_equal(asmodel_json_array_at(a, i), asmodel_json_array_at(b, i))) return false;
    return true;
  case ASMODEL_JSON_OBJECT:
    if (asmodel_json_object_count(a) != asmodel_json_object_count(b)) return false;
    for (size_t i = 0; i < asmodel_json_object_count(a); i++)
      if (!astools_mcp_schema_equal(asmodel_json_object_value_at(a, i),
                                    asmodel_json_object_get(b, asmodel_json_object_key_at(a, i))))
        return false;
    return true;
  }
  return false;
}

/* JSON-only arguments: dates, tags, NaN and opaque xCDN extensions cannot leak
 * through a lossy serializer. Policy has already canonicalized path strings. */
static asmodel_json_value *arguments(const xcdn_node_t *node, unsigned depth) {
  if (!node || !node->value || node->tags_len || depth > 64) return NULL;
  const xcdn_value_t *v = node->value;
  switch (v->type) {
  case XCDN_VAL_NULL:
    return asmodel_json_null();
  case XCDN_VAL_BOOL:
    return asmodel_json_bool(v->data.boolean);
  case XCDN_VAL_INT:
    return asmodel_json_int(v->data.integer);
  case XCDN_VAL_FLOAT:
    return asmodel_json_double(v->data.floating);
  case XCDN_VAL_STRING:
    return asmodel_json_string(v->data.string);
  case XCDN_VAL_ARRAY: {
    asmodel_json_value *out = asmodel_json_array();
    for (size_t i = 0; out && i < xcdn_array_len(v); i++)
      if (asmodel_json_array_push(out, arguments(xcdn_array_get(v, i), depth + 1))) {
        asmodel_json_free(out);
        return NULL;
      }
    return out;
  }
  case XCDN_VAL_OBJECT: {
    asmodel_json_value *out = asmodel_json_object();
    for (size_t i = 0; out && i < v->data.object.len; i++)
      if (asmodel_json_object_set(out, v->data.object.entries[i].key,
                        arguments(v->data.object.entries[i].node, depth + 1))) {
        asmodel_json_free(out);
        return NULL;
      }
    return out;
  }
  default:
    return NULL;
  }
}
asmodel_json_value *astools_mcp_arguments(const xcdn_node_t *node) {
  return arguments(node, 0);
}

/* Shared admission makes selection/batch validation as strict as actual dispatch. */
astools_err astools_mcp_validate(astools_ctx *c, const astools_cmd *cmd, const xcdn_node_t *args) {
  if (!cmd->mcp_input_schema) return ASTOOLS_OK;
  asmodel_json_value *schema = NULL, *value = astools_mcp_arguments(args);
  bool valid = value && !asmodel_json_parse(cmd->mcp_input_schema,strlen(cmd->mcp_input_schema),&schema) &&
               astools_mcp_schema_validate(schema,value);
  asmodel_json_free(schema); asmodel_json_free(value);
  return valid ? ASTOOLS_OK : astools_seterr(c,ASTOOLS_ERR_INVALID,
      "Arguments violate the reviewed MCP input schema or its JSON/resource limits");
}
