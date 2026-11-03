#include "core.h"

static ah_code read_operation(ah_engine *e, const char *sid, const char *id, core_operation *op,
                              ah_error *err) {
    memset(op, 0, sizeof(*op));
    sqlite3_stmt *s = NULL;
    const char *sql =
        id ? "SELECT "
             "id,call_id,name,arguments,status,effect,approval,result,decision,is_error,schema "
             "FROM operations WHERE session_id=? AND id=?"
           : "SELECT "
             "id,call_id,name,arguments,status,effect,approval,result,decision,is_error,schema "
             "FROM operations WHERE session_id=? AND status!='done' ORDER BY seq LIMIT 1";
    ah_code rc = core_prepare(e, sql, &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    if (id)
        sqlite3_bind_text(s, 2, id, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    if (step != SQLITE_ROW) {
        sqlite3_finalize(s);
        return step == SQLITE_DONE ? AH_NOT_FOUND : ah_fail(err, AH_IO, "cannot read operation");
    }
    op->id = ah_strdup((const char *)sqlite3_column_text(s, 0));
    op->call_id = ah_strdup((const char *)sqlite3_column_text(s, 1));
    op->name = ah_strdup((const char *)sqlite3_column_text(s, 2));
    op->arguments = ah_strdup((const char *)sqlite3_column_text(s, 3));
    op->status = ah_strdup((const char *)sqlite3_column_text(s, 4));
    op->effect = (ah_effect)sqlite3_column_int(s, 5);
    op->approval = sqlite3_column_int(s, 6) != 0;
    if (sqlite3_column_type(s, 7) != SQLITE_NULL)
        op->result = ah_strdup((const char *)sqlite3_column_text(s, 7));
    if (sqlite3_column_type(s, 10) != SQLITE_NULL)
        op->schema = ah_strdup((const char *)sqlite3_column_text(s, 10));
    op->decision = sqlite3_column_int(s, 8);
    op->is_error = sqlite3_column_int(s, 9) != 0;
    sqlite3_finalize(s);
    if (!op->id || !op->call_id || !op->name || !op->arguments || !op->status) {
        core_operation_free(op);
        return ah_fail(err, AH_IO, "out of memory");
    }
    return AH_OK;
}
ah_code core_next_operation(ah_engine *e, const char *sid, core_operation *op, ah_error *err) {
    return read_operation(e, sid, NULL, op, err);
}
ah_code core_get_operation(ah_engine *e, const char *sid, const char *id, core_operation *op,
                           ah_error *err) {
    return read_operation(e, sid, id, op, err);
}
void core_operation_free(core_operation *op) {
    free(op->id);
    free(op->call_id);
    free(op->name);
    free(op->arguments);
    free(op->status);
    free(op->result);
    free(op->schema);
    memset(op, 0, sizeof(*op));
}
ah_code core_set_operation(ah_engine *e, const char *sid, const char *id, const char *status,
                           ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc =
        core_prepare(e, "UPDATE operations SET status=? WHERE session_id=? AND id=?", &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, sid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, id, -1, SQLITE_TRANSIENT);
    return core_done(e, s, err);
}
ah_code core_receipt(ah_engine *e, const char *sid, const core_operation *op, const char *result,
                     bool is_error, const char *type, ah_error *err) {
    core_session session;
    ah_code rc = core_load(e, sid, &session, err);
    if (rc != AH_OK)
        return rc;
    yyjson_mut_doc *d = ah_mut_doc_new();
    if (!d) {
        core_session_free(&session);
        return ah_fail(err, AH_IO, "out of memory");
    }
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, o);
    yyjson_mut_obj_add_str(d, o, "type", "function_call_output");
    yyjson_mut_obj_add_strcpy(d, o, "call_id", op->call_id);
    yyjson_doc *parsed = yyjson_read(result, strlen(result), 0);
    yyjson_mut_doc *receipt = ah_mut_doc_new();
    if (!parsed || !receipt) {
        yyjson_doc_free(parsed);
        ah_mut_doc_free(receipt);
        ah_mut_doc_free(d);
        core_session_free(&session);
        return ah_fail(err, AH_IO, "cannot encode tool receipt");
    }
    yyjson_mut_val *body = yyjson_mut_obj(receipt);
    yyjson_mut_doc_set_root(receipt, body);
    yyjson_mut_obj_add_bool(receipt, body, "is_error", is_error);
    yyjson_mut_obj_add_val(receipt, body, "result",
                           yyjson_val_mut_copy(receipt, yyjson_doc_get_root(parsed)));
    char *encoded = ah_mut_write(receipt);
    yyjson_doc_free(parsed);
    ah_mut_doc_free(receipt);
    if (!encoded) {
        ah_mut_doc_free(d);
        core_session_free(&session);
        return ah_fail(err, AH_IO, "out of memory");
    }
    yyjson_mut_obj_add_strcpy(d, o, "output", encoded);
    free(encoded);
    char *json = ah_mut_write(d);
    ah_mut_doc_free(d);
    if (!json) {
        core_session_free(&session);
        return ah_fail(err, AH_IO, "out of memory");
    }
    rc = core_add_item(e, sid, session.turn, json, err);
    free(json);
    core_session_free(&session);
    sqlite3_stmt *s = NULL;
    if (rc == AH_OK)
        rc = core_prepare(
            e,
            "UPDATE operations SET status='done',result=?,is_error=? WHERE id=? AND session_id=?",
            &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, result, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(s, 2, is_error);
        sqlite3_bind_text(s, 3, op->id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 4, sid, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    if (rc == AH_OK)
        rc = core_event(e, sid, op->id, type, result, err);
    return rc;
}
ah_code ah_decide(ah_engine *e, const char *sid, const char *id, bool allow, ah_error *err) {
    ah_code rc = core_guard(e, err);
    if (rc != AH_OK)
        return rc;
    if (!sid || !id)
        return ah_fail(err, AH_INVALID, "session and operation required");
    core_operation op;
    rc = core_get_operation(e, sid, id, &op, err);
    if (rc != AH_OK)
        return ah_fail(err, rc, "operation unavailable");
    if (op.decision >= 0) {
        bool same = op.decision == (allow ? 1 : 0);
        core_operation_free(&op);
        return same
                   ? AH_OK
                   : ah_fail(err, AH_INVALID, "approval decision conflicts with recorded decision");
    }
    if (strcmp(op.status, "approval")) {
        core_operation_free(&op);
        return ah_fail(err, AH_INVALID, "operation is not awaiting approval");
    }
    atomic_store_explicit(&e->cancelled, false, memory_order_relaxed);
    uint64_t after = 0;
    rc = core_begin(e, &after, err);
    sqlite3_stmt *s = NULL;
    if (rc == AH_OK)
        rc = core_prepare(e, "UPDATE operations SET decision=? WHERE id=? AND session_id=?", &s,
                          err);
    if (rc == AH_OK) {
        sqlite3_bind_int(s, 1, allow ? 1 : 0);
        sqlite3_bind_text(s, 2, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, sid, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    if (rc == AH_OK)
        rc = allow ? core_set_operation(e, sid, id, "approved", err)
                   : core_receipt(e, sid, &op, "{\"error\":\"approval denied\"}", true,
                                  "tool.denied", err);
    if (rc == AH_OK)
        rc = core_event(e, sid, id, "approval.decided",
                        allow ? "{\"allow\":true}" : "{\"allow\":false}", err);
    if (rc == AH_OK)
        rc = core_state(e, sid, AH_READY, err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    core_operation_free(&op);
    return rc;
}
static ah_code provide_result(ah_engine *e, const char *sid, const char *id, const char *json,
                              bool is_error, bool resolve, ah_error *err) {
    ah_code rc = core_guard(e, err);
    if (rc != AH_OK)
        return rc;
    if (!sid || !id || !json || strlen(json) > e->config.max_output_bytes || !ah_json_valid(json))
        return ah_fail(err, AH_INVALID, "bounded valid JSON result required");
    core_operation op;
    rc = core_get_operation(e, sid, id, &op, err);
    if (rc != AH_OK)
        return ah_fail(err, rc, "operation unavailable");
    if (!strcmp(op.status, "done")) {
        bool same = op.result && !strcmp(op.result, json) && op.is_error == is_error;
        core_operation_free(&op);
        return same ? AH_OK : ah_fail(err, AH_INVALID, "result conflicts with recorded receipt");
    }
    if (strcmp(op.status, resolve ? "unknown" : "input")) {
        core_operation_free(&op);
        return ah_fail(err, AH_INVALID, "operation is not waiting for this result");
    }
    atomic_store_explicit(&e->cancelled, false, memory_order_relaxed);
    uint64_t after = 0;
    rc = core_begin(e, &after, err);
    if (rc == AH_OK)
        rc = core_receipt(e, sid, &op, json, is_error, resolve ? "tool.resolved" : "input.received",
                          err);
    if (rc == AH_OK)
        rc = core_state(e, sid, AH_READY, err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    core_operation_free(&op);
    return rc;
}
ah_code ah_reply(ah_engine *e, const char *sid, const char *op, const char *json, ah_error *err) {
    return provide_result(e, sid, op, json, false, false, err);
}
ah_code ah_resolve(ah_engine *e, const char *sid, const char *op, const char *json, bool is_error,
                   ah_error *err) {
    return provide_result(e, sid, op, json, is_error, true, err);
}
ah_code core_recover(ah_engine *e, ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e, "SELECT id FROM sessions WHERE state=?", &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_int(s, 1, AH_RUNNING);
    char **ids = NULL;
    size_t count = 0;
    int step;
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        char **next = realloc(ids, (count + 1) * sizeof(*ids));
        if (!next) {
            rc = ah_fail(err, AH_IO, "out of memory");
            break;
        }
        ids = next;
        ids[count] = ah_strdup((const char *)sqlite3_column_text(s, 0));
        if (!ids[count]) {
            rc = ah_fail(err, AH_IO, "out of memory");
            break;
        }
        count++;
    }
    sqlite3_finalize(s);
    if (rc == AH_OK && step != SQLITE_DONE)
        rc = ah_fail(err, AH_IO, "recovery scan failed");
    for (size_t i = 0; i < count && rc == AH_OK; i++) {
        core_operation op;
        ah_code found = core_next_operation(e, ids[i], &op, err);
        if (found != AH_OK && found != AH_NOT_FOUND) {
            rc = found;
            break;
        }
        uint64_t after = 0;
        rc = core_begin(e, &after, err);
        ah_state state = AH_STOPPED;
        if (rc == AH_OK && found == AH_OK && !strcmp(op.status, "executing")) {
            if (op.effect == AH_EFFECT_READ)
                rc =
                    core_receipt(e, ids[i], &op, "{\"error\":\"read interrupted by process exit\"}",
                                 true, "tool.interrupted", err);
            else {
                rc = core_set_operation(e, ids[i], op.id, "unknown", err);
                state = AH_OUTCOME_UNKNOWN;
                if (rc == AH_OK)
                    rc = core_event(e, ids[i], op.id, "tool.outcome_unknown",
                                    "{\"reason\":\"execution interrupted\"}", err);
            }
        }
        if (rc == AH_OK)
            rc = core_state(e, ids[i], state, err);
        if (rc == AH_OK)
            rc = core_event(e, ids[i], NULL, "session.recovered", "{}", err);
        if (rc == AH_OK)
            rc = core_commit(e, after, err);
        else
            core_rollback(e);
        if (found == AH_OK)
            core_operation_free(&op);
    }
    for (size_t i = 0; i < count; i++)
        free(ids[i]);
    free(ids);
    return rc;
}
