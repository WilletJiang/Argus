#include "core.h"

typedef struct {
    ah_engine *engine;
    const char *session;
} delta_context;
static void on_delta(void *context, const char *text, size_t length) {
    delta_context *sink = context;
    if (!sink->engine->config.observer || !text || core_cancelled(sink->engine))
        return;
    yyjson_mut_doc *d = ah_mut_doc_new();
    if (!d)
        return;
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, o);
    yyjson_mut_obj_add_strncpy(d, o, "text", text, length);
    char *json = ah_mut_write(d);
    if (json) {
        ah_event event = {0, sink->session, NULL, "model.delta", json};
        sink->engine->config.observer(sink->engine->config.observer_context, &event);
        free(json);
    }
    ah_mut_doc_free(d);
}
static ah_code transition(ah_engine *e, const char *sid, ah_state state, const char *event,
                          const char *json, ah_error *err) {
    uint64_t after = 0;
    ah_code rc = core_begin(e, &after, err);
    if (rc == AH_OK)
        rc = core_state(e, sid, state, err);
    if (rc == AH_OK && event)
        rc = core_event(e, sid, NULL, event, json, err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    return rc;
}
static ah_code model_intent(ah_engine *e, core_session *session, ah_error *err) {
    uint64_t after = 0;
    ah_code rc = core_begin(e, &after, err);
    sqlite3_stmt *s = NULL;
    if (rc == AH_OK)
        rc = core_prepare(e, "UPDATE sessions SET model_calls=model_calls+1 WHERE id=?", &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    if (rc == AH_OK)
        rc = core_event(e, session->id, NULL, "model.started", "{}", err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    if (rc == AH_OK)
        session->model_calls++;
    return rc;
}
ah_code ah_run(ah_engine *e, const char *sid, ah_error *err) {
    ah_code rc = core_guard(e, err);
    if (rc != AH_OK)
        return rc;
    core_session session;
    rc = core_load(e, sid, &session, err);
    if (rc != AH_OK)
        return rc;
    if (session.state == AH_COMPLETED || session.state == AH_WAITING_APPROVAL ||
        session.state == AH_WAITING_INPUT) {
        core_session_free(&session);
        return AH_OK;
    }
    if (session.state != AH_READY) {
        ah_state state = session.state;
        core_session_free(&session);
        return ah_fail(err, state == AH_OUTCOME_UNKNOWN ? AH_UNCERTAIN : AH_INVALID,
                       "session must be ready; submit or resume explicitly");
    }
    e->running = true;
    core_mailbox_active(e, sid);
    uint64_t now = ah_now_ms();
    e->deadline = now + e->config.run_timeout_ms;
    if (e->deadline < now)
        e->deadline = UINT64_MAX;
    rc = transition(e, sid, AH_RUNNING, "run.started", "{}", err);
    session.state = AH_RUNNING;
    while (rc == AH_OK && session.state == AH_RUNNING) {
        if (core_cancelled(e)) {
            rc = ah_fail(err,
                         atomic_load_explicit(&e->cancelled, memory_order_relaxed) ? AH_CANCELLED
                                                                                   : AH_LIMIT,
                         "run cancelled or deadline exceeded");
            break;
        }
        core_operation op;
        ah_code found = core_next_operation(e, sid, &op, err);
        if (found == AH_OK) {
            rc = core_execute_operation(e, &session, &op, err);
            core_operation_free(&op);
            continue;
        }
        if (found != AH_NOT_FOUND) {
            rc = found;
            break;
        }
        rc = core_drain_steering(e, &session, false, err);
        if (rc != AH_OK)
            break;
        if (session.model_calls >= e->config.max_model_calls) {
            rc = ah_fail(err, AH_LIMIT, "model call budget exhausted");
            break;
        }
        char *request = NULL;
        rc = core_request(e, &session, &request, err);
        if (rc != AH_OK)
            break;
        rc = model_intent(e, &session, err);
        if (rc != AH_OK) {
            free(request);
            break;
        }
        if (core_cancelled(e)) {
            free(request);
            rc = ah_fail(err, AH_CANCELLED, "cancelled before model call");
            break;
        }
        char *response = NULL;
        delta_context sink = {e, sid};
        rc = e->config.provider.complete(e->config.provider.context, request, on_delta, &sink,
                                         core_cancelled, e, &response, err);
        free(request);
        if (core_cancelled(e)) {
            uint64_t after = 0;
            ah_code saved = core_begin(e, &after, err);
            const char *late = response && strlen(response) <= e->config.max_output_bytes &&
                                       ah_json_valid(response)
                                   ? response
                                   : "{\"discarded\":true}";
            if (saved == AH_OK)
                saved = core_event(e, sid, NULL, "model.late_result", late, err);
            if (saved == AH_OK)
                saved = core_commit(e, after, err);
            else
                core_rollback(e);
            free(response);
            rc = saved == AH_OK ? ah_fail(err,
                                          atomic_load_explicit(&e->cancelled, memory_order_relaxed)
                                              ? AH_CANCELLED
                                              : AH_LIMIT,
                                          "model returned after cancellation or deadline")
                                : saved;
            break;
        }
        if (rc == AH_OK)
            rc = core_accept_response(e, &session, response, err);
        free(response);
        if (rc == AH_OK && session.state == AH_COMPLETED) {
            rc = core_drain_steering(e, &session, true, err);
            if (rc == AH_OK && session.state == AH_COMPLETED && !session.completion_committed)
                rc = transition(e, sid, AH_COMPLETED, NULL, NULL, err);
        }
    }
    if (rc != AH_OK && session.state != AH_OUTCOME_UNKNOWN) {
        ah_error original = {0};
        if (err)
            original = *err;
        ah_state failed = (rc == AH_CANCELLED || rc == AH_LIMIT) ? AH_STOPPED : AH_FAILED;
        char *json =
            core_object_string("error", err && err->message[0] ? err->message : ah_code_name(rc));
        ah_error save_error = {0};
        ah_code saved = transition(e, sid, failed, "run.stopped", json ? json : "{}", &save_error);
        free(json);
        if (saved != AH_OK) {
            rc = saved;
            if (err)
                *err = save_error;
        } else if (err)
            *err = original;
    }
    core_mailbox_active(e, NULL);
    e->deadline = 0;
    e->running = false;
    core_session_free(&session);
    return rc;
}
