#ifndef AH_WORKFLOW_FIXTURE_H
#define AH_WORKFLOW_FIXTURE_H
#include "../src/tools/local_internal.h"
#include <assert.h>
#include <limits.h>
#include <sqlite3.h>

#define WF_BASE "A\nB\nC\nD\n"
#define WF_REV "a7f5cf19fdb779272b12bed17134c60d18d464fddfda599b8a70fdc5cab185d1"
#define WF_OTHER "left\nright\n"
#define WF_OTHER_REV "e1722f3dcd04fc367b1c3e25ad6edf4d1183ec4fd2be697cad608588b19f275a"

typedef struct {
    char workspace[PATH_MAX], executable[PATH_MAX];
    ah_local_tools *tools;
    unsigned phase_count, applied_count;
    const char *fail_phase, *fail_target, *mutate_phase, *mutate_target;
    const char *mutation_file, *mutation_text;
} workflow;
typedef struct {
    ah_code code;
    bool failed, unknown;
    yyjson_doc *doc;
    yyjson_val *root;
} wf_result;
void wf_open(workflow *, const char *);
void wf_close(workflow *);
void wf_put(workflow *, const char *, const char *);
char *wf_read(workflow *, const char *);
void wf_expect_file(workflow *, const char *, const char *);
void wf_path(workflow *, const char *, char[PATH_MAX]);
ah_code wf_checkpoint(void *, const char *, ah_error *);
wf_result wf_call(workflow *, ah_execute_fn, const char *, size_t);
wf_result wf_check(workflow *, const char *);
void wf_result_free(wf_result *);
void wf_add_check(workflow *, const char *, const char *, bool, uint64_t);
unsigned wf_runs(workflow *);
int wf_child(int, char **);
char *wf_apply_args(const char *, bool, const char *);
void wf_engine_case(workflow *);
#endif
