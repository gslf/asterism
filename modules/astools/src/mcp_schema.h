/* Deliberately bounded JSON Schema profile; unknown assertions fail admission. */
#ifndef ASTOOLS_MCP_SCHEMA_H
#define ASTOOLS_MCP_SCHEMA_H
#include "asmodel_json.h"
#include <stdbool.h>
bool astools_mcp_schema_supported(const asmodel_json_value *schema);
/* Call only with a supported schema. Limits also reject the instance. */
bool astools_mcp_schema_validate(const asmodel_json_value *schema, const asmodel_json_value *value);
#endif
