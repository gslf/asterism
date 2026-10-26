/* Supported contracts must retain their admission decision after cloning. */
#include "mcp_client.h"
#include "mcp_schema.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  asmodel_json_value *input = NULL;
  if (size > 16384 || asmodel_json_parse((const char *)data, size, &input))
    return 0;
  const asmodel_json_value *schema = asmodel_json_array_at(input, 0), *instance = asmodel_json_array_at(input, 1);
  if (!schema || !instance)
    goto done;
  asmodel_json_value *copy = asmodel_json_clone(schema), *value = asmodel_json_clone(instance);
  if (!copy || !value)
    goto release;
  if (!astools_mcp_schema_equal(schema, copy) || !astools_mcp_schema_equal(instance, value) ||
      astools_mcp_schema_equal(schema, instance) != astools_mcp_schema_equal(instance, schema))
    abort();
  bool accepted = astools_mcp_schema_supported(schema);
  if (accepted != astools_mcp_schema_supported(copy))
    abort();
  if (accepted &&
      astools_mcp_schema_validate(schema, instance) != astools_mcp_schema_validate(copy, value))
    abort();
release:
  asmodel_json_free(copy);
  asmodel_json_free(value);
done:
  asmodel_json_free(input);
  return 0;
}
