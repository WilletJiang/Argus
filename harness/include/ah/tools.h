#ifndef AH_TOOLS_H
#define AH_TOOLS_H
#include "ah.h"
#include "task.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ah_local_tools ah_local_tools;
typedef struct {
    const char *workspace;
    bool enable_exec; /* Explicit broad local-process authority; NOT a sandbox. */
    bool approve_writes;
    bool enable_workflows; /* Opt-in search/apply_changes and host check presets. */
} ah_local_tools_config;
ah_code ah_local_tools_open(const ah_local_tools_config *config, ah_local_tools **out,
                            ah_error *error);
ah_code ah_local_tools_register(ah_local_tools *tools, ah_engine *engine, ah_error *error);
/* Host-defined commands; models select names and cannot change argv or inputs.
 * Configure before register(). argv_json is an array, inputs_json a nonempty
 * array of workspace-relative regular files (each <=1 MiB) or directories
 * (bounded complete scan, no symlinks; .git and in-flight write temps excluded). Their complete
 * dependency closure is the host's responsibility. Cache is opt-in and needs a
 * stable environment_revision covering toolchain/config/external dependencies.
 * Commands run with the same minimal environment as exec. */
typedef struct {
    const char *name;
    const char *description;
    const char *argv_json;
    const char *inputs_json;
    const char *environment_revision;
    uint64_t timeout_ms;
    bool cacheable;
    bool requires_approval;
} ah_check_config;
ah_code ah_local_tools_add_check(ah_local_tools *, const ah_check_config *, ah_error *);
/* Adapter for tasks whose acceptance is exactly the registered host checks.
 * Include documentation/examples/deliverables in those checks when required. */
ah_code ah_local_tools_verifier(ah_local_tools *, ah_task_verifier *, ah_error *);
void ah_local_tools_close(ah_local_tools *tools);
/* Validates the documented fail-closed JSON Schema subset. */
ah_code ah_schema_validate(const char *schema_json, const char *value_json, ah_error *error);
#ifdef __cplusplus
}
#endif
#endif
