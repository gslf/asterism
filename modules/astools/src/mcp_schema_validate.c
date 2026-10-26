/* Validate arguments before dispatch and structured results before accepting success. */
#include "mcp_client.h"
#include "mcp_schema.h"
#include <math.h>

static bool count_ok(const asmodel_json_value *schema, const char *min, const char *max, size_t n) {
  const asmodel_json_value *lo = asmodel_json_object_get(schema, min), *hi = asmodel_json_object_get(schema, max);
  return (!lo || (uint64_t)n >= (uint64_t)asmodel_json_int_value(lo)) &&
         (!hi || (uint64_t)n <= (uint64_t)asmodel_json_int_value(hi));
}
static bool number_ok(const asmodel_json_value *schema, const asmodel_json_value *value) {
  static const char *const keys[] = {"minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum"};
  for (size_t i = 0; i < 4; i++) {
    const asmodel_json_value *bound = asmodel_json_object_get(schema, keys[i]);
    if (!bound) continue;
    int order;
    if (asmodel_json_is_int(value) && asmodel_json_is_int(bound)) {
      long long a = asmodel_json_int_value(value), b = asmodel_json_int_value(bound);
      order = (a > b) - (a < b);
    } else if (asmodel_json_is_int(value) && asmodel_json_int_value(value) > 9007199254740992LL)
      order = 1;
    else if (asmodel_json_is_int(value) && asmodel_json_int_value(value) < -9007199254740992LL)
      order = -1;
    else {
      double a = asmodel_json_double_value(value), b = asmodel_json_double_value(bound);
      order = (a > b) - (a < b);
    }
    if ((i == 0 && order < 0) || (i == 1 && order > 0) || (i == 2 && order <= 0) ||
        (i == 3 && order >= 0))
      return false;
  }
  return true;
}
static bool validate(const asmodel_json_value *schema, const asmodel_json_value *value, unsigned depth,
                     unsigned *left) {
  if (!value || !*left || depth > 64) return false;
  --*left;
  if (!schema) return true;
  if (asmodel_json_typeof(schema) == ASMODEL_JSON_BOOL) return asmodel_json_bool_value(schema) != 0;
  static const char *const types[] = {"null", "boolean", "number", "string", "array", "object"};
  const char *type = astools_rpc_string(schema, "type");
  if (type && strcmp(type, types[asmodel_json_typeof(value)])) {
    double n = asmodel_json_double_value(value);
    if (strcmp(type, "integer") || asmodel_json_typeof(value) != ASMODEL_JSON_NUMBER || floor(n) != n) return false;
  }
  const asmodel_json_value *fixed = asmodel_json_object_get(schema, "const"), *choices = asmodel_json_object_get(schema, "enum");
  if (fixed && !astools_mcp_schema_equal(fixed, value)) return false;
  if (choices) {
    bool found = false;
    for (size_t i = 0; i < asmodel_json_array_len(choices); i++)
      if (astools_mcp_schema_equal(asmodel_json_array_at(choices, i), value)) {
        found = true;
        break;
      }
    if (!found) return false;
  }
  switch (asmodel_json_typeof(value)) {
  case ASMODEL_JSON_NUMBER:
    return number_ok(schema, value);
  case ASMODEL_JSON_STRING: {
    size_t n = 0;
    const unsigned char *bytes = (const unsigned char *)asmodel_json_string_value(value);
    for (size_t i = 0; i < asmodel_json_string_length(value); i++)
      if ((bytes[i] & 0xc0) != 0x80) n++;
    return count_ok(schema, "minLength", "maxLength", n);
  }
  case ASMODEL_JSON_ARRAY:
    if (!count_ok(schema, "minItems", "maxItems", asmodel_json_array_len(value))) return false;
    for (size_t i = 0; i < asmodel_json_array_len(value); i++)
      if (!validate(asmodel_json_object_get(schema, "items"), asmodel_json_array_at(value, i), depth + 1, left))
        return false;
    return true;
  case ASMODEL_JSON_OBJECT: {
    if (!count_ok(schema, "minProperties", "maxProperties", asmodel_json_object_count(value))) return false;
    const asmodel_json_value *required = asmodel_json_object_get(schema, "required"),
                   *props = asmodel_json_object_get(schema, "properties");
    for (size_t i = 0; i < asmodel_json_array_len(required); i++)
      if (!asmodel_json_object_get(value, asmodel_json_string_value(asmodel_json_array_at(required, i)))) return false;
    for (size_t i = 0; i < asmodel_json_object_count(value); i++) {
      const asmodel_json_value *child = asmodel_json_object_get(props, asmodel_json_object_key_at(value, i));
      if (!child) child = asmodel_json_object_get(schema, "additionalProperties");
      if (!validate(child, asmodel_json_object_value_at(value, i), depth + 1, left)) return false;
    }
    return true;
  }
  default:
    return true;
  }
}
bool astools_mcp_schema_validate(const asmodel_json_value *schema, const asmodel_json_value *value) {
  unsigned left = 32768;
  return validate(schema, value, 0, &left);
}
