#ifndef AH_LOCAL_INTERNAL_H
#define AH_LOCAL_INTERNAL_H
#include "ah/tools.h"
#include "../internal.h"
#include "../hash.h"
#include <unistd.h>
#include <sys/stat.h>

#define AH_LOCAL_MAX_BYTES (1024 * 1024)
typedef struct {
    char *name, *description, *arguments, *inputs, *environment;
    char definition[65], cached_input[65];
    char *cached_result;
    uint64_t timeout_ms;
    bool cacheable, approval;
} ah_local_check;
struct ah_local_tools {
    int workspace_fd;
    bool enable_exec;
    bool approve_writes;
    bool enable_workflows;
    bool registered;
    ah_local_check checks[16];
    size_t check_count;
    char check_revision[65];
};
size_t ah_local_limit(const ah_tool_call *call);
size_t ah_local_view_limit(const ah_tool_call *call);
bool ah_local_same_file(const struct stat *a, const struct stat *b);
ah_code ah_local_source(int parent, const char *leaf, const ah_tool_call *call,
                        struct stat *original, char **text, size_t *length);
ah_code ah_local_revision(int fd, const ah_tool_call *call, char revision[65]);
ah_code ah_local_replace(int parent, const char *leaf, const char *content, size_t length,
                         const struct stat *original, const ah_tool_call *call,
                         ah_tool_result *result, ah_error *error);
ah_code ah_local_conflict(const ah_tool_call *, ah_tool_result *, ah_error *, const char *,
                          const char *, size_t, const char *);
ah_code ah_local_stopped(const ah_tool_call *call);
ah_code ah_local_error(const ah_tool_call *call, ah_tool_result *result, ah_error *error,
                       ah_code code, const char *message);
ah_code ah_local_finish(const ah_tool_call *call, ah_tool_result *result, yyjson_mut_doc *doc,
                        ah_error *error);
bool ah_local_utf8(const char *text, size_t length, size_t *complete);
ah_code ah_local_exec(void *context, const ah_tool_call *call, ah_tool_result *result,
                      ah_error *error);
int ah_local_parent_fd(ah_local_tools *tools, yyjson_val *path, char leaf[4097]);
int ah_local_parent_path(ah_local_tools *tools, const char *path, size_t length, char leaf[4097]);
typedef struct {
    char *items[2048];
    size_t count, visited, skipped;
    bool truncated;
    ah_hash topology;
} ah_paths;
ah_code ah_local_paths(ah_local_tools *, yyjson_val *, const ah_tool_call *, ah_paths *);
void ah_local_paths_free(ah_paths *);
ah_code ah_local_input_revision(ah_local_tools *, yyjson_val *, const ah_tool_call *, char[65]);
void ah_local_stat_hash(ah_hash *, const struct stat *);
ah_code ah_local_find(void *, const ah_tool_call *, ah_tool_result *, ah_error *);
extern const char ah_find_schema[];
ah_code ah_local_check_run(ah_local_tools *, ah_local_check *, const ah_tool_call *,
                           ah_tool_result *, ah_error *);
ah_local_check *ah_local_check_find(ah_local_tools *, const char *);
ah_code ah_local_checks_register(ah_local_tools *, ah_engine *, ah_error *);
void ah_local_checks_free(ah_local_tools *);
ah_code ah_local_check_names(ah_local_tools *, yyjson_val *, const ah_tool_call *, yyjson_mut_doc *,
                             yyjson_mut_val *, bool *, bool *, ah_error *);
ah_code ah_local_phase(const ah_tool_call *, const char *, const char *, const char *, const char *,
                       ah_error *);
ah_code ah_local_apply(void *, const ah_tool_call *, ah_tool_result *, ah_error *);
extern const char ah_apply_schema[];
extern const char ah_checks_schema[];
ah_code ah_local_run_checks(void *, const ah_tool_call *, ah_tool_result *, ah_error *);
void ah_local_diagnostics(ah_local_tools *, ah_local_check *, const ah_tool_call *, yyjson_val *,
                          yyjson_mut_doc *, yyjson_mut_val *);
yyjson_doc *ah_local_arguments(const ah_tool_call *call, const char *schema, ah_tool_result *result,
                               ah_error *error);
ah_code ah_local_read_file(void *context, const ah_tool_call *call, ah_tool_result *result,
                           ah_error *error);
ah_code ah_local_write_file(void *context, const ah_tool_call *call, ah_tool_result *result,
                            ah_error *error);
extern const char ah_read_schema[], ah_write_schema[], ah_exec_schema[];
#endif
