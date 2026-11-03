#ifndef AH_CORE_H
#define AH_CORE_H
#include "../internal.h"
#include "ah/tools.h"
#include <sqlite3.h>
#include <stdatomic.h>
#include <pthread.h>

typedef struct core_steering {
    char *session, *command, *text;
    struct core_steering *next;
} core_steering;
typedef struct {
    ah_tool tool;
    char *name, *description, *schema;
} core_tool;
struct ah_engine {
    ah_config config;
    sqlite3 *db;
    int lock_fd;
    core_tool *tools;
    size_t tool_count;
    atomic_bool cancelled;
    bool running;
    bool observing;
    uint64_t deadline;
    pthread_mutex_t mailbox_lock;
    bool mailbox_initialized;
    char active_session[AH_ID_CAP];
    core_steering *mailbox_head, *mailbox_tail;
    size_t mailbox_bytes;
};
typedef struct {
    char *id, *model, *instructions, *summary;
    ah_state state;
    unsigned turn, model_calls, tool_calls;
    bool completion_committed;
} core_session;
typedef struct {
    char *id, *call_id, *name, *arguments, *status, *result, *schema;
    int decision;
    bool is_error;
    ah_effect effect;
    bool approval;
} core_operation;

void core_mailbox_active(ah_engine *, const char *);
void core_mailbox_free(ah_engine *);
ah_code core_drain_steering(ah_engine *, core_session *, bool, ah_error *);
ah_code core_sql(ah_engine *, const char *, ah_error *);
ah_code core_prepare(ah_engine *, const char *, sqlite3_stmt **, ah_error *);
ah_code core_done(ah_engine *, sqlite3_stmt *, ah_error *);
ah_code core_begin(ah_engine *, uint64_t *, ah_error *);
ah_code core_commit(ah_engine *, uint64_t, ah_error *);
void core_rollback(ah_engine *);
ah_code core_event(ah_engine *, const char *, const char *, const char *, const char *, ah_error *);
ah_code core_state(ah_engine *, const char *, ah_state, ah_error *);
ah_code core_load(ah_engine *, const char *, core_session *, ah_error *);
void core_session_free(core_session *);
ah_code core_id(char[AH_ID_CAP], ah_error *);
ah_code core_add_item(ah_engine *, const char *, unsigned, const char *, ah_error *);
ah_code core_next_operation(ah_engine *, const char *, core_operation *, ah_error *);
ah_code core_get_operation(ah_engine *, const char *, const char *, core_operation *, ah_error *);
void core_operation_free(core_operation *);
ah_code core_receipt(ah_engine *, const char *, const core_operation *, const char *, bool,
                     const char *, ah_error *);
ah_code core_set_operation(ah_engine *, const char *, const char *, const char *, ah_error *);
core_tool *core_find_tool(ah_engine *, const char *);
bool core_cancelled(void *);
ah_code core_recover(ah_engine *, ah_error *);
const char *core_builtin_schema(const char *);
ah_code core_request(ah_engine *, const core_session *, char **, ah_error *);
ah_code core_accept_response(ah_engine *, core_session *, const char *, ah_error *);
ah_code core_execute_operation(ah_engine *, core_session *, core_operation *, ah_error *);
ah_code core_guard(ah_engine *, ah_error *);
char *core_object_string(const char *, const char *);
#endif
