#include "core.h"

static ah_code count_call(ah_engine *, const char *, ah_error *);

static ah_code finish_error(ah_engine *e, core_session *session, core_operation *op,
                            const char *message, bool count, ah_error *err) {
    char *json = core_object_string("error", message);
    if (!json)
        return ah_fail(err, AH_IO, "out of memory");
    uint64_t after = 0;
    ah_code rc = core_begin(e, &after, err);
    if (rc == AH_OK && count)
        rc = count_call(e, session->id, err);
    if (rc == AH_OK)
        rc = core_receipt(e, session->id, op, json, true, "tool.completed", err);
    if (rc == AH_OK) {
        rc = core_commit(e, after, err);
        if (rc != AH_OK)
            session->state = AH_OUTCOME_UNKNOWN;
    } else
        core_rollback(e);
    if (rc == AH_OK && count)
        session->tool_calls++;
    free(json);
    return rc;
}
static ah_code set_pause(ah_engine *e, core_session *session, core_operation *op,
                         const char *status, ah_state state, const char *event, ah_error *err) {
    uint64_t after = 0;
    ah_code rc = core_begin(e, &after, err);
    if (rc == AH_OK)
        rc = core_set_operation(e, session->id, op->id, status, err);
    if (rc == AH_OK)
        rc = core_state(e, session->id, state, err);
    if (rc == AH_OK)
        rc = core_event(e, session->id, op->id, event, op->arguments, err);
    if (rc == AH_OK) {
        rc = core_commit(e, after, err);
        if (rc != AH_OK)
            session->state = AH_OUTCOME_UNKNOWN;
    } else
        core_rollback(e);
    if (rc == AH_OK)
        session->state = state;
    return rc;
}
static ah_code count_call(ah_engine *e, const char *sid, ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e, "UPDATE sessions SET tool_calls=tool_calls+1 WHERE id=?", &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    return core_done(e, s, err);
}
static ah_code publish_received(ah_engine *e, core_session *session, core_operation *op,
                                ah_error *err) {
    if (!op->result)
        return ah_fail(err, AH_IO, "stored tool receipt unavailable");
    uint64_t after = 0;
    ah_code rc = core_begin(e, &after, err);
    if (rc == AH_OK)
        rc = core_receipt(
            e, session->id, op, op->result, op->is_error,
            !strcmp(op->status, "received_late") ? "tool.late_result" : "tool.completed", err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    if (rc != AH_OK)
        session->state = AH_OUTCOME_UNKNOWN;
    return rc;
}
ah_code core_execute_operation(ah_engine *e, core_session *session, core_operation *op,
                               ah_error *err) {
    if (!strcmp(op->status, "approval")) {
        session->state = AH_WAITING_APPROVAL;
        return AH_OK;
    }
    if (!strcmp(op->status, "input")) {
        session->state = AH_WAITING_INPUT;
        return AH_OK;
    }
    if (!strcmp(op->status, "unknown")) {
        session->state = AH_OUTCOME_UNKNOWN;
        return ah_fail(err, AH_UNCERTAIN, "resolve uncertain tool outcome before continuing");
    }
    if (!strcmp(op->status, "received") || !strcmp(op->status, "received_late") ||
        !strcmp(op->status, "resolved"))
        return publish_received(e, session, op, err);
    if (strcmp(op->status, "pending") && strcmp(op->status, "approved"))
        return ah_fail(err, AH_PROTOCOL, "unexpected operation status");
    if (session->tool_calls >= e->config.max_tool_calls)
        return ah_fail(err, AH_LIMIT, "tool call budget exhausted");
    core_tool *registered = core_find_tool(e, op->name);
    const char *builtin = core_builtin_schema(op->name),
               *schema = builtin ? builtin : (registered ? registered->schema : NULL);
    if (!schema)
        return finish_error(e, session, op, "tool is unavailable", true, err);
    if (registered && (registered->tool.effect != op->effect ||
                       registered->tool.requires_approval != op->approval || !op->schema ||
                       strcmp(registered->schema, op->schema)))
        return finish_error(e, session, op, "tool policy changed since intent; execution refused",
                            true, err);
    ah_error validation = {0};
    ah_code valid = ah_schema_validate(schema, op->arguments, &validation);
    if (valid != AH_OK)
        return finish_error(e, session, op,
                            validation.message[0] ? validation.message
                                                  : "tool arguments failed schema validation",
                            true, err);
    if (op->approval && op->decision != 1)
        return set_pause(e, session, op, "approval", AH_WAITING_APPROVAL, "approval.requested",
                         err);
    if (builtin && strcmp(op->name, "read_result")) {
        uint64_t after = 0;
        ah_code rc = core_begin(e, &after, err);
        if (rc == AH_OK)
            rc = count_call(e, session->id, err);
        if (rc == AH_OK)
            rc = core_set_operation(e, session->id, op->id, "input", err);
        if (rc == AH_OK)
            rc = core_state(e, session->id, AH_WAITING_INPUT, err);
        if (rc == AH_OK)
            rc = core_event(e, session->id, op->id, "input.requested", op->arguments, err);
        if (rc == AH_OK) {
            rc = core_commit(e, after, err);
            if (rc != AH_OK)
                session->state = AH_OUTCOME_UNKNOWN;
        } else
            core_rollback(e);
        if (rc == AH_OK) {
            session->tool_calls++;
            session->state = AH_WAITING_INPUT;
        }
        return rc;
    }
    uint64_t after = 0;
    ah_code rc = core_begin(e, &after, err);
    if (rc == AH_OK)
        rc = count_call(e, session->id, err);
    if (rc == AH_OK)
        rc = core_set_operation(e, session->id, op->id, "executing", err);
    if (rc == AH_OK)
        rc = core_event(e, session->id, op->id, "tool.started", op->arguments, err);
    if (rc == AH_OK) {
        rc = core_commit(e, after, err);
        if (rc != AH_OK)
            session->state = AH_OUTCOME_UNKNOWN;
    } else
        core_rollback(e);
    if (rc != AH_OK)
        return rc;
    session->tool_calls++;
    if (core_cancelled(e)) {
        rc = finish_error(e, session, op, "cancelled before execution", false, err);
        if (rc != AH_OK)
            session->state = AH_OUTCOME_UNKNOWN;
        return rc == AH_OK ? ah_fail(err, AH_CANCELLED, "cancelled before tool execution") : rc;
    }
    uint64_t run_deadline = e->deadline, now = ah_now_ms(),
             tool_deadline = now + e->config.tool_timeout_ms;
    if (tool_deadline < now)
        tool_deadline = UINT64_MAX;
    if (run_deadline && tool_deadline > run_deadline)
        tool_deadline = run_deadline;
    e->deadline = tool_deadline;
    ah_tool_call call = {session->id,
                         op->id,
                         op->arguments,
                         core_cancelled,
                         e,
                         tool_deadline,
                         e->config.max_output_bytes};
    ah_tool_result result = {0};
    ah_error execution = {0};
    ah_code executed =
        builtin ? core_read_result(e, &call, &result, &execution)
                : registered->tool.execute(registered->tool.context, &call, &result, &execution);
    bool late = core_cancelled(e);
    e->deadline = run_deadline;
    bool valid_result = result.json && strlen(result.json) <= e->config.max_output_bytes &&
                        ah_json_valid(result.json);
    bool unknown = result.outcome_unknown ||
                   (op->effect != AH_EFFECT_READ && (executed != AH_OK || !valid_result));
    if (!valid_result && !unknown) {
        free(result.json);
        result.json = core_object_string(
            "error", execution.message[0] ? execution.message
                                          : "tool returned no valid bounded JSON result");
        result.is_error = true;
    }
    if (!result.json && !unknown) {
        session->state = AH_OUTCOME_UNKNOWN;
        return ah_fail(err, AH_IO, "out of memory recording tool failure");
    }
    rc = core_begin(e, &after, err);
    if (rc == AH_OK && unknown) {
        rc = core_set_operation(e, session->id, op->id, "unknown", err);
        if (rc == AH_OK)
            rc = core_event(
                e, session->id, op->id, "tool.outcome_unknown",
                valid_result
                    ? result.json
                    : "{\"reason\":\"tool failed without a trustworthy completion receipt\"}",
                err);
        if (rc == AH_OK)
            rc = core_state(e, session->id, AH_OUTCOME_UNKNOWN, err);
        session->state = AH_OUTCOME_UNKNOWN;
    } else if (rc == AH_OK)
        rc = core_receipt(e, session->id, op, result.json, result.is_error || executed != AH_OK,
                          late ? "tool.late_result" : "tool.completed", err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    free(result.json);
    if (rc != AH_OK) {
        session->state = AH_OUTCOME_UNKNOWN;
        return rc;
    }
    if (unknown)
        return ah_fail(err, AH_UNCERTAIN, "tool outcome is unknown; explicit resolution required");
    if (late)
        return ah_fail(err,
                       atomic_load_explicit(&e->cancelled, memory_order_relaxed) ? AH_CANCELLED
                                                                                 : AH_LIMIT,
                       "tool completed after cancellation or deadline; receipt recorded");
    return AH_OK;
}
