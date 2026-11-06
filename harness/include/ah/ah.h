#ifndef AH_AH_H
#define AH_AH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AH_VERSION "0.4.0"
#define AH_ID_CAP 37
#define AH_ERROR_CAP 512

typedef struct ah_engine ah_engine;
typedef enum {
    AH_OK = 0,
    AH_INVALID,
    AH_IO,
    AH_PROTOCOL,
    AH_LIMIT,
    AH_CANCELLED,
    AH_BUSY,
    AH_NOT_FOUND,
    AH_UNCERTAIN,
    AH_AUTH,
    AH_NETWORK
} ah_code;
typedef struct {
    ah_code code;
    char message[AH_ERROR_CAP];
} ah_error;
typedef enum {
    AH_IDLE,
    AH_READY,
    AH_RUNNING,
    AH_WAITING_APPROVAL,
    AH_WAITING_INPUT,
    AH_COMPLETED,
    AH_STOPPED,
    AH_FAILED,
    AH_OUTCOME_UNKNOWN
} ah_state;

typedef struct {
    uint64_t seq; /* 0 means provisional; not a durable receipt. */
    const char *session_id;
    const char *operation_id;
    const char *type;
    const char *json; /* Valid JSON, borrowed for callback duration. */
} ah_event;
typedef void (*ah_observer)(void *context, const ah_event *event);
typedef bool (*ah_cancel_check)(void *context);
typedef void (*ah_delta_sink)(void *context, const char *text, size_t length);

/* Providers are synchronous on the owner thread; they MUST honor cancel/deadline.
 * request_json is a complete Responses request. response_json is malloc-owned,
 * must contain a successful terminal Responses object, and is freed by runtime.
 * Failed/partial streams must never return AH_OK. No library API is signal-safe. */
typedef ah_code (*ah_complete_fn)(void *context, const char *request_json, ah_delta_sink delta,
                                  void *delta_context, ah_cancel_check cancelled,
                                  void *cancel_context, char **response_json, ah_error *error);
typedef struct {
    ah_complete_fn complete;
    void *context;
} ah_provider;

typedef enum { AH_EFFECT_READ, AH_EFFECT_WRITE, AH_EFFECT_EXTERNAL } ah_effect;
/* Owner-thread effect callbacks may persist bounded phase receipts. A checkpoint
 * is evidence for reconciliation, not permission to repeat an uncertain effect.
 * NULL for parallel READ callbacks. Borrowed only during execute(). */
typedef ah_code (*ah_checkpoint_fn)(void *context, const char *json, ah_error *error);
typedef struct {
    const char *session_id;
    const char *operation_id; /* Stable across recovery; not provider call_id. */
    const char *arguments_json;
    ah_cancel_check cancelled;
    void *cancel_context;
    uint64_t deadline_ms; /* Monotonic absolute milliseconds. */
    size_t output_limit;
    size_t model_output_limit; /* 0 means no separate model-view bound. */
    ah_checkpoint_fn checkpoint;
    void *checkpoint_context;
} ah_tool_call;
typedef struct {
    char *json; /* malloc-owned valid JSON; runtime frees it. */
    bool is_error;
    bool outcome_unknown; /* May have committed a side effect. */
} ah_tool_result;
typedef ah_code (*ah_execute_fn)(void *context, const ah_tool_call *call, ah_tool_result *result,
                                 ah_error *error);
typedef struct {
    const char *name;
    const char *description;
    const char *parameters_json;
    ah_effect effect;
    bool requires_approval;
    ah_execute_fn execute;
    void *context;      /* Host-owned, must outlive engine. */
    bool parallel_safe; /* Opt-in: READ callback/context may run concurrently. */
} ah_tool;

typedef struct {
    const char *database_path;
    ah_provider provider;
    ah_observer observer;
    void *observer_context;
    size_t max_context_bytes;
    size_t max_output_bytes;
    size_t max_tool_arguments_bytes;
    unsigned max_model_calls;
    unsigned max_tool_calls;
    uint64_t run_timeout_ms;
    uint64_t tool_timeout_ms;
    unsigned max_parallel_tools; /* 1 serial; 0 also serial; maximum 16. */
    size_t max_model_tool_bytes; /* Per-receipt model view; 0 disables extra cap. */
} ah_config;

void ah_config_init(ah_config *config);
ah_code ah_open(const ah_config *config, ah_engine **out, ah_error *error);
void ah_close(ah_engine *engine);
ah_code ah_register_tool(ah_engine *engine, const ah_tool *tool, ah_error *error);
ah_code ah_session_create(ah_engine *engine, const char *model, const char *instructions,
                          char id[AH_ID_CAP], ah_error *error);
ah_code ah_submit(ah_engine *engine, const char *session_id, const char *command_id,
                  const char *text, ah_error *error);
ah_code ah_run(ah_engine *engine, const char *session_id, ah_error *error);
/* Cross-thread operations. Engine must remain alive until the owner joins. */
void ah_cancel(ah_engine *engine);
/* Queues a bounded, copied update for the next model boundary. AH_OK means
 * queued; input.steered is the durable acknowledgment. Retry unacknowledged
 * command IDs after a crash. Does not interrupt an in-flight tool. */
ah_code ah_steer(ah_engine *engine, const char *session_id, const char *command_id,
                 const char *text, ah_error *error);
ah_code ah_resume(ah_engine *engine, const char *session_id, ah_error *error);
ah_code ah_decide(ah_engine *engine, const char *session_id, const char *operation_id, bool allow,
                  ah_error *error);
ah_code ah_reply(ah_engine *engine, const char *session_id, const char *operation_id,
                 const char *reply_json, ah_error *error);
ah_code ah_resolve(ah_engine *engine, const char *session_id, const char *operation_id,
                   const char *result_json, bool is_error, ah_error *error);
ah_code ah_fork(ah_engine *engine, const char *session_id, char new_id[AH_ID_CAP], ah_error *error);
/* Explicit, atomic checkpoint. Keeps complete recent turns and original journal.
 * Also allowed for stopped/failed sessions with all tool exchanges settled.
 * keep=0 preserves current unfinished-turn user inputs. State/counters unchanged;
 * resume is explicit. summary is host-supplied, not verified. */
ah_code ah_compact(ah_engine *engine, const char *session_id, const char *summary,
                   unsigned keep_recent_turns, ah_error *error);
ah_code ah_session_json(ah_engine *engine, const char *session_id, char **json, ah_error *error);
ah_code ah_sessions_json(ah_engine *engine, char **json, ah_error *error);
ah_code ah_events(ah_engine *engine, const char *session_id, uint64_t after_seq,
                  ah_observer observer, void *context, ah_error *error);
void ah_free(void *memory);
const char *ah_code_name(ah_code code);
const char *ah_state_name(ah_state state);
uint64_t ah_now_ms(void);

#ifdef __cplusplus
}
#endif
#endif
