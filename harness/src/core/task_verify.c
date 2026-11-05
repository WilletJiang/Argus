#include "core.h"
#include "ah/task.h"

typedef struct {
    ah_engine *engine;
    const char *session, *id;
    unsigned count;
    bool failed;
} verification_context;

bool core_task_valid_revision(char revision[65]) {
    revision[64] = 0;
    for (size_t i = 0; i < 64; i++)
        if (!((revision[i] >= '0' && revision[i] <= '9') ||
              (revision[i] >= 'a' && revision[i] <= 'f')))
            return false;
    return true;
}

static ah_code verification_checkpoint(void *context, const char *json, ah_error *error) {
    verification_context *c = context;
    if (!json || strlen(json) > 16384 || !ah_json_valid(json) || c->count++ >= 128 ||
        c->engine->observing) {
        c->failed = true;
        return ah_fail(error, AH_INVALID, "Invalid verification checkpoint");
    }
    uint64_t after = 0;
    ah_code code = core_begin(c->engine, &after, error);
    if (code == AH_OK)
        code = core_event(c->engine, c->session, c->id, "task.checkpoint", json, error);
    if (code == AH_OK)
        code = core_commit(c->engine, after, error);
    else
        core_rollback(c->engine);
    c->failed |= code != AH_OK;
    return code;
}

static ah_code save(ah_engine *e, const char *sid, const char *id, const char *state,
                    const char *receipt, ah_error *error) {
    uint64_t after = 0;
    ah_code code = core_begin(e, &after, error);
    sqlite3_stmt *s = NULL;
    if (code == AH_OK)
        code = core_prepare(e, "UPDATE tasks SET state=?,receipt=? WHERE session_id=?", &s, error);
    if (code == AH_OK) {
        sqlite3_bind_text(s, 1, state, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, receipt, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, sid, -1, SQLITE_TRANSIENT);
        code = core_done(e, s, error);
    }
    if (code == AH_OK)
        code =
            core_event(e, sid, id, !strcmp(state, "accepted") ? "task.accepted" : "task.verified",
                       receipt, error);
    if (code == AH_OK)
        code = core_commit(e, after, error);
    else
        core_rollback(e);
    return code;
}

ah_code core_task_verify(ah_engine *e, const char *sid, const ah_task_verifier *verifier,
                         const char *contract_revision, ah_error *error) {
    ah_code code = core_task_allow(e, sid, false, 1, NULL, error);
    if (code != AH_OK)
        return code;
    char id[AH_ID_CAP];
    code = core_id(id, error);
    if (code != AH_OK)
        return code;
    uint64_t after = 0;
    code = core_begin(e, &after, error);
    sqlite3_stmt *s = NULL;
    if (code == AH_OK)
        code = core_task_charge(e, sid, false, 1, error);
    if (code == AH_OK)
        code = core_prepare(e, "UPDATE tasks SET state='verifying' WHERE session_id=?", &s, error);
    if (code == AH_OK) {
        sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
        code = core_done(e, s, error);
    }
    if (code == AH_OK)
        code = core_event(e, sid, id, "task.verification.started", "{}", error);
    if (code == AH_OK)
        code = core_commit(e, after, error);
    else
        core_rollback(e);
    if (code != AH_OK)
        return code;

    e->running = true;
    core_mailbox_active(e, sid);
    uint64_t now = ah_now_ms();
    e->deadline = now + e->config.tool_timeout_ms;
    if (e->deadline < now)
        e->deadline = UINT64_MAX;
    code = core_task_deadline(e, sid, &e->deadline, error);
    verification_context context = {e, sid, id, 0, false};
    ah_tool_call call = {.session_id = sid,
                         .operation_id = id,
                         .arguments_json = "{}",
                         .cancelled = core_cancelled,
                         .cancel_context = e,
                         .deadline_ms = e->deadline,
                         .output_limit = e->config.max_output_bytes,
                         .model_output_limit = e->config.max_model_tool_bytes,
                         .checkpoint = verification_checkpoint,
                         .checkpoint_context = &context};
    char before[65] = {0}, current[65] = {0};
    uint64_t started = ah_now_us();
    if (code == AH_OK)
        code = verifier->revision(verifier->context, &call, before, error);
    bool fingerprinted = code == AH_OK && core_task_valid_revision(before);
    ah_tool_result result = {0};
    bool invoked = false;
    if (fingerprinted && !core_cancelled(e)) {
        invoked = true;
        code = verifier->verify(verifier->context, &call, &result, error);
    }
    bool late = core_cancelled(e);
    ah_error check_error = {0};
    ah_code checked = fingerprinted
                          ? verifier->revision(verifier->context, &call, current, &check_error)
                          : AH_INVALID;
    bool stable = fingerprinted && checked == AH_OK && core_task_valid_revision(current) &&
                  !strcmp(before, current);
    bool valid = result.json && strlen(result.json) <= e->config.max_output_bytes &&
                 ah_json_valid(result.json);
    yyjson_doc *parsed = valid ? yyjson_read(result.json, strlen(result.json), 0) : NULL;
    bool unknown =
        context.failed || result.outcome_unknown || (invoked && (code != AH_OK || !valid));
    bool passed = invoked && code == AH_OK && !result.is_error && parsed &&
                  yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(parsed), "ok"));
    bool superseded = false;
    core_session session;
    ah_code drained = core_load(e, sid, &session, &check_error);
    if (drained == AH_OK) {
        drained = core_drain_steering(e, &session, true, &check_error);
        superseded = session.state != AH_COMPLETED;
        core_session_free(&session);
    }
    if (drained != AH_OK)
        unknown = true;
    const char *status = unknown          ? "unknown"
                         : late           ? "expired"
                         : superseded     ? "superseded"
                         : !fingerprinted ? "error"
                         : !stable        ? "stale"
                         : passed         ? "pass"
                                          : "fail";
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "status", status);
    yyjson_mut_obj_add_strcpy(doc, root, "contract_revision", contract_revision);
    yyjson_mut_obj_add_strcpy(doc, root, "definition_revision", verifier->definition_revision);
    yyjson_mut_obj_add_strcpy(doc, root, "input_revision", before);
    yyjson_mut_obj_add_strcpy(doc, root, "observed_revision", current);
    yyjson_mut_obj_add_uint(doc, root, "duration_us", ah_now_us() - started);
    yyjson_mut_obj_add_uint(doc, root, "verified_at_ms", core_wall_ms());
    if (parsed)
        yyjson_mut_obj_add_val(doc, root, "result",
                               yyjson_val_mut_copy(doc, yyjson_doc_get_root(parsed)));
    if (!fingerprinted)
        yyjson_mut_obj_add_strcpy(doc, root, "error",
                                  error && *error->message ? error->message
                                                           : "Required inputs unavailable");
    char *receipt = ah_mut_write(doc);
    ah_mut_doc_free(doc);
    yyjson_doc_free(parsed);
    free(result.json);
    bool accepted = !strcmp(status, "pass");
    core_metric(e, sid, id, "verification", started, ah_now_us(), receipt ? strlen(receipt) : 0, 1,
                code);
    ah_code saved =
        receipt ? save(e, sid, id,
                       unknown    ? "unknown"
                       : accepted ? "accepted"
                                  : "active",
                       receipt, error)
                : ah_fail(error, AH_IO, "Cannot record verification; reconcile before reuse");
    free(receipt);
    core_mailbox_active(e, NULL);
    e->deadline = 0;
    e->running = false;
    if (saved != AH_OK)
        return saved; /* verifying survives if commit did not happen */
    if (unknown)
        return ah_fail(error, AH_UNCERTAIN, "Verifier outcome unknown; reconcile before retry");
    if (late)
        return ah_fail(error, atomic_load(&e->cancelled) ? AH_CANCELLED : AH_LIMIT,
                       "Verification exceeded its deadline or was cancelled");
    return AH_OK;
}
