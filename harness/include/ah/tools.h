#ifndef AH_TOOLS_H
#define AH_TOOLS_H
#include "ah.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ah_local_tools ah_local_tools;
typedef struct {
    const char *workspace;
    bool enable_exec; /* Explicit broad local-process authority; NOT a sandbox. */
    bool approve_writes;
} ah_local_tools_config;
ah_code ah_local_tools_open(const ah_local_tools_config *config, ah_local_tools **out,
                            ah_error *error);
ah_code ah_local_tools_register(ah_local_tools *tools, ah_engine *engine, ah_error *error);
void ah_local_tools_close(ah_local_tools *tools);
/* Validates the documented fail-closed JSON Schema subset. */
ah_code ah_schema_validate(const char *schema_json, const char *value_json, ah_error *error);
#ifdef __cplusplus
}
#endif
#endif
