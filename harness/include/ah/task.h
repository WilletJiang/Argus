#ifndef AH_TASK_H
#define AH_TASK_H
#include "ah.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Optional task lifecycle above a session. One immutable task per fresh session.
 * Wall budget includes downtime and waiting; model/tool budgets include attempts.
 * A verifier invocation counts as one tool attempt, including its fixed recipe. */
typedef struct {
    const char *contract_json;
    const char *verifier_revision; /* 64-character host policy/content digest. */
    unsigned max_model_calls, max_tool_calls, max_repairs;
    uint64_t timeout_ms;
} ah_task_config;
typedef ah_code (*ah_revision_fn)(void *, const ah_tool_call *, char[65], ah_error *);
typedef struct {
    const char *definition_revision;
    ah_revision_fn revision;
    /* Host-trusted verifier; return {"ok":true,...} only for actual acceptance.
     * is_error, failed/unknown callbacks or a changed revision never accept.
     * Both callbacks must honor cancellation/deadline; owner API reentry forbidden. */
    ah_execute_fn verify;
    void *context;
} ah_task_verifier;
ah_code ah_task_create(ah_engine *, const char *session_id, const ah_task_config *, ah_error *);
/* Runs through bounded repair turns; returns on ask/approval/uncertain effects.
 * Accepted is recorded separately; AH_COMPLETED still only means a model turn. */
ah_code ah_task_run(ah_engine *, const char *session_id, const ah_task_verifier *, ah_error *);
ah_code ah_task_json(ah_engine *, const char *session_id, char **json, ah_error *);
/* After the host has inspected an interrupted verifier's effects, explicitly
 * reconcile it. Never marks accepted; the next run must verify again. */
ah_code ah_task_reconcile(ah_engine *, const char *session_id, const char *evidence_json,
                          ah_error *);
#ifdef __cplusplus
}
#endif
#endif
