#ifndef AH_LOCAL_INTERNAL_H
#define AH_LOCAL_INTERNAL_H
#include "ah/tools.h"
#include "../internal.h"
#include <unistd.h>

#define AH_LOCAL_MAX_BYTES (1024 * 1024)
struct ah_local_tools {
    int workspace_fd;
    bool enable_exec;
    bool approve_writes;
};
size_t ah_local_limit(const ah_tool_call *call);
ah_code ah_local_stopped(const ah_tool_call *call);
ah_code ah_local_error(const ah_tool_call *call, ah_tool_result *result, ah_error *error,
                       ah_code code, const char *message);
ah_code ah_local_finish(const ah_tool_call *call, ah_tool_result *result, yyjson_mut_doc *doc,
                        ah_error *error);
bool ah_local_utf8(const char *text, size_t length, size_t *complete);
ah_code ah_local_exec(void *context, const ah_tool_call *call, ah_tool_result *result,
                      ah_error *error);
extern const char ah_exec_schema[];
#endif
