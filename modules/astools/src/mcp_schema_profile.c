/* This profile cannot silently erase a keyword from an operator-reviewed schema. */
#include "mcp_client.h"
#include "mcp_schema.h"
#include <math.h>

static bool named(const char *name, const char *const *names, size_t count) {
  for (size_t i = 0; i < count; i++)
    if (!strcmp(name, names[i])) return true;
  return false;
}
static bool strings(const asmodel_json_value *array) {
  if (asmodel_json_typeof(array) != ASMODEL_JSON_ARRAY || asmodel_json_array_len(array) > 256) return false;
  for (size_t i = 0; i < asmodel_json_array_len(array); i++) {
    const asmodel_json_value *v = asmodel_json_array_at(array, i);
    const char *s = asmodel_json_string_value(v);
    if (!s || strlen(s) != asmodel_json_string_length(v)) return false;
    for (size_t j = 0; j < i; j++)
      if (!strcmp(s, asmodel_json_string_value(asmodel_json_array_at(array, j)))) return false;
  }
  return true;
}
static bool supported(const asmodel_json_value *schema, unsigned depth, unsigned *left) {
  if (!schema || depth > 32 || !*left) return false;
  --*left;
  if (asmodel_json_typeof(schema) == ASMODEL_JSON_BOOL) return true;
  if (asmodel_json_typeof(schema) != ASMODEL_JSON_OBJECT) return false;
  static const char *const types[] = {"null",   "boolean", "number", "integer",
                                      "string", "array",   "object"};
  static const char *const counts[] = {"minLength", "maxLength",     "minItems",
                                       "maxItems",  "minProperties", "maxProperties"};
  static const char *const bounds[] = {"minimum", "maximum", "exclusiveMinimum",
                                       "exclusiveMaximum"};
  static const char *const text[] = {"title", "description", "$comment"};
  static const char *const flags[] = {"readOnly", "writeOnly", "deprecated"};
  for (size_t i = 0; i < asmodel_json_object_count(schema); i++) {
    const char *key = asmodel_json_object_key_at(schema, i);
    const asmodel_json_value *v = asmodel_json_object_value_at(schema, i);
    if (!strcmp(key, "$schema")) {
      const char *uri = astools_rpc_string(schema, key);
      if (!uri || strcmp(uri, "https://json-schema.org/draft/2020-12/schema")) return false;
    } else if (!strcmp(key, "type")) {
      const char *type = astools_rpc_string(schema, key);
      if (!type || !named(type, types, 7)) return false;
    } else if (!strcmp(key, "properties")) {
      if (asmodel_json_typeof(v) != ASMODEL_JSON_OBJECT || asmodel_json_object_count(v) > 256) return false;
      for (size_t j = 0; j < asmodel_json_object_count(v); j++)
        if (!supported(asmodel_json_object_value_at(v, j), depth + 1, left)) return false;
    } else if (!strcmp(key, "items") || !strcmp(key, "additionalProperties")) {
      if (!supported(v, depth + 1, left)) return false;
    } else if (!strcmp(key, "required")) {
      if (!strings(v)) return false;
    } else if (named(key, counts, 6)) {
      if (!asmodel_json_is_int(v) || asmodel_json_int_value(v) < 0 || asmodel_json_int_value(v) > 9007199254740992LL)
        return false;
    } else if (named(key, bounds, 4)) {
      /* Bounds inside the exact binary64 integer range keep mixed comparisons portable. */
      if (asmodel_json_typeof(v) != ASMODEL_JSON_NUMBER || fabs(asmodel_json_double_value(v)) > 9007199254740992.0 ||
          (asmodel_json_is_int(v) &&
           (asmodel_json_int_value(v) < -9007199254740992LL || asmodel_json_int_value(v) > 9007199254740992LL)))
        return false;
    } else if (!strcmp(key, "enum")) {
      if (asmodel_json_typeof(v) != ASMODEL_JSON_ARRAY || !asmodel_json_array_len(v) || asmodel_json_array_len(v) > 256) return false;
      for (size_t j = 0; j < asmodel_json_array_len(v); j++)
        for (size_t k = 0; k < j; k++)
          if (astools_mcp_schema_equal(asmodel_json_array_at(v, j), asmodel_json_array_at(v, k))) return false;
    } else if (named(key, text, 3)) {
      if (asmodel_json_typeof(v) != ASMODEL_JSON_STRING) return false;
    } else if (named(key, flags, 3)) {
      if (asmodel_json_typeof(v) != ASMODEL_JSON_BOOL) return false;
    } else if (!strcmp(key, "examples")) {
      if (asmodel_json_typeof(v) != ASMODEL_JSON_ARRAY) return false;
    } else if (strcmp(key, "default") && strcmp(key, "const"))
      return false;
  }
  return true;
}
bool astools_mcp_schema_supported(const asmodel_json_value *schema) {
  unsigned left = 1024;
  return supported(schema, 0, &left);
}
