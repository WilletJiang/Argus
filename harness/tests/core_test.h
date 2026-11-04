#ifndef AH_CORE_TEST_H
#define AH_CORE_TEST_H
#include "ah/ah.h"
#include "yyjson.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sqlite3.h>
#include <pthread.h>
#include <spawn.h>

typedef struct {
    const char *answers[8];
    unsigned calls;
    char *last_request;
    ah_engine *engine;
    bool cancel_model, cancel_tool, unknown, crash;
    unsigned executions, durable, provisional;
    uint64_t last_seq;
    bool started;
    const char *steering_session;
    bool steer;
    const char *fault_database;
} fixture;
extern const char done[], call[], ask[];
void ok(ah_code code, ah_error *error);
ah_engine *open_engine(fixture *f, const char *path, ah_config *custom);
void register_tool(ah_engine *engine, fixture *f, ah_effect effect, bool approval);
void create_input(ah_engine *engine, char id[AH_ID_CAP]);
void state_is(ah_engine *engine, const char *id, const char *state);
char *pending(ah_engine *engine, const char *id);
ah_code fixture_complete(void *context, const char *request, ah_delta_sink delta,
                         void *delta_context, ah_cancel_check cancelled, void *cancel_context,
                         char **response, ah_error *error);
void fixture_observe(void *context, const ah_event *event);
ah_code fixture_execute(void *context, const ah_tool_call *call_info, ah_tool_result *result,
                        ah_error *error);
#endif
