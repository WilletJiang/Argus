#include "core.h"

ah_code ah_session_create(ah_engine *e, const char *model, const char *instructions,
                          char id[AH_ID_CAP], ah_error *err) {
    ah_code rc = core_guard(e, err);
    if (rc != AH_OK)
        return rc;
    if (!model || !*model || !instructions || !id || strlen(model) > 256 ||
        strlen(instructions) > e->config.max_context_bytes)
        return ah_fail(err, AH_INVALID, "bounded model and instructions required");
    rc = core_id(id, err);
    if (rc != AH_OK)
        return rc;
    uint64_t after = 0;
    rc = core_begin(e, &after, err);
    sqlite3_stmt *s = NULL;
    if (rc == AH_OK)
        rc = core_prepare(e, "INSERT INTO sessions(id,model,instructions,state) VALUES(?,?,?,?)",
                          &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, model, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, instructions, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(s, 4, AH_IDLE);
        rc = core_done(e, s, err);
    }
    if (rc == AH_OK)
        rc = core_event(e, id, NULL, "session.created", "{}", err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    return rc;
}
ah_code ah_submit(ah_engine *e, const char *sid, const char *command, const char *text,
                  ah_error *err) {
    ah_code rc = core_guard(e, err);
    if (rc != AH_OK)
        return rc;
    if (!sid || !command || !*command || strlen(command) > 256 || !text || !*text ||
        strlen(text) > e->config.max_context_bytes)
        return ah_fail(err, AH_INVALID, "bounded nonempty command id and text required");
    sqlite3_stmt *s = NULL;
    rc = core_prepare(e, "SELECT text FROM commands WHERE session_id=? AND id=?", &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, command, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    if (step == SQLITE_ROW) {
        bool same = !strcmp((const char *)sqlite3_column_text(s, 0), text);
        sqlite3_finalize(s);
        return same ? AH_OK : ah_fail(err, AH_INVALID, "command id reused with different text");
    }
    sqlite3_finalize(s);
    if (step != SQLITE_DONE)
        return ah_fail(err, AH_IO, "command lookup failed");
    core_session session;
    rc = core_load(e, sid, &session, err);
    if (rc != AH_OK)
        return rc;
    core_operation op;
    ah_code pending = core_next_operation(e, sid, &op, err);
    if (pending == AH_OK) {
        core_operation_free(&op);
        core_session_free(&session);
        return ah_fail(err, AH_BUSY, "finish pending tool exchange before submitting");
    }
    if (pending != AH_NOT_FOUND) {
        core_session_free(&session);
        return pending;
    }
    if (session.state == AH_RUNNING || session.state == AH_OUTCOME_UNKNOWN) {
        core_session_free(&session);
        return ah_fail(err, AH_BUSY, "session cannot accept input in current state");
    }
    yyjson_mut_doc *d = ah_mut_doc_new();
    if (!d) {
        core_session_free(&session);
        return ah_fail(err, AH_IO, "out of memory");
    }
    yyjson_mut_val *item = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, item);
    yyjson_mut_obj_add_str(d, item, "role", "user");
    yyjson_mut_obj_add_strcpy(d, item, "content", text);
    char *json = ah_mut_write(d);
    ah_mut_doc_free(d);
    if (!json) {
        core_session_free(&session);
        return ah_fail(err, AH_IO, "out of memory");
    }
    atomic_store_explicit(&e->cancelled, false, memory_order_relaxed);
    uint64_t after = 0;
    rc = core_begin(e, &after, err);
    if (rc == AH_OK)
        rc = core_prepare(e, "INSERT INTO commands(session_id,id,text) VALUES(?,?,?)", &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, command, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, text, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    unsigned turn = session.turn + (session.state == AH_READY ? 0 : 1);
    if (rc == AH_OK)
        rc = core_add_item(e, sid, turn, json, err);
    if (rc == AH_OK)
        rc = core_prepare(e, "UPDATE sessions SET turn=?,model_calls=0,tool_calls=0 WHERE id=?", &s,
                          err);
    if (rc == AH_OK) {
        sqlite3_bind_int64(s, 1, turn);
        sqlite3_bind_text(s, 2, sid, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    if (rc == AH_OK)
        rc = core_event(e, sid, NULL, "input.submitted", json, err);
    if (rc == AH_OK)
        rc = core_state(e, sid, AH_READY, err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    free(json);
    core_session_free(&session);
    return rc;
}
ah_code ah_resume(ah_engine *e, const char *sid, ah_error *err) {
    ah_code rc = core_guard(e, err);
    if (rc != AH_OK)
        return rc;
    core_session session;
    rc = core_load(e, sid, &session, err);
    if (rc != AH_OK)
        return rc;
    ah_state state = session.state;
    core_session_free(&session);
    if (state == AH_READY) {
        atomic_store_explicit(&e->cancelled, false, memory_order_relaxed);
        return AH_OK;
    }
    if (state != AH_STOPPED && state != AH_FAILED)
        return ah_fail(err, state == AH_OUTCOME_UNKNOWN ? AH_UNCERTAIN : AH_INVALID,
                       "session cannot resume in current state");
    atomic_store_explicit(&e->cancelled, false, memory_order_relaxed);
    uint64_t after = 0;
    rc = core_begin(e, &after, err);
    if (rc == AH_OK)
        rc = core_state(e, sid, AH_READY, err);
    if (rc == AH_OK)
        rc = core_event(e, sid, NULL, "session.resumed", "{}", err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    return rc;
}
static yyjson_mut_val *session_value(yyjson_mut_doc *d, sqlite3_stmt *s) {
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, o, "id", (const char *)sqlite3_column_text(s, 0));
    yyjson_mut_obj_add_strcpy(d, o, "model", (const char *)sqlite3_column_text(s, 1));
    yyjson_mut_obj_add_str(d, o, "state", ah_state_name((ah_state)sqlite3_column_int(s, 2)));
    yyjson_mut_obj_add_uint(d, o, "turn", (uint64_t)sqlite3_column_int64(s, 3));
    yyjson_mut_obj_add_uint(d, o, "model_calls", (uint64_t)sqlite3_column_int64(s, 4));
    yyjson_mut_obj_add_uint(d, o, "tool_calls", (uint64_t)sqlite3_column_int64(s, 5));
    return o;
}
ah_code ah_sessions_json(ah_engine *e, char **json, ah_error *err) {
    if (json)
        *json = NULL;
    if (!e || !json)
        return ah_fail(err, AH_INVALID, "engine and output required");
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(
        e, "SELECT id,model,state,turn,model_calls,tool_calls FROM sessions ORDER BY rowid", &s,
        err);
    if (rc != AH_OK)
        return rc;
    yyjson_mut_doc *d = ah_mut_doc_new();
    if (!d) {
        sqlite3_finalize(s);
        return ah_fail(err, AH_IO, "out of memory");
    }
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, arr);
    int step;
    while ((step = sqlite3_step(s)) == SQLITE_ROW)
        yyjson_mut_arr_append(arr, session_value(d, s));
    sqlite3_finalize(s);
    if (step != SQLITE_DONE)
        rc = ah_fail(err, AH_IO, "session list failed");
    if (rc == AH_OK) {
        *json = ah_mut_write(d);
        if (!*json)
            rc = ah_fail(err, AH_IO, "out of memory");
    }
    ah_mut_doc_free(d);
    return rc;
}
static ah_code pending_operation(ah_engine *e, const char *sid, ah_state state, core_operation *op,
                                 ah_error *err) {
    if (state != AH_OUTCOME_UNKNOWN)
        return core_next_operation(e, sid, op, err);
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e,
                              "SELECT id FROM operations WHERE session_id=? AND status='unknown' "
                              "ORDER BY seq LIMIT 1",
                              &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
        int step = sqlite3_step(s);
        if (step == SQLITE_ROW)
            rc = core_get_operation(e, sid, (const char *)sqlite3_column_text(s, 0), op, err);
        else
            rc = step == SQLITE_DONE ? AH_NOT_FOUND : ah_fail(err, AH_IO, "pending lookup failed");
    }
    sqlite3_finalize(s);
    return rc;
}
ah_code ah_session_json(ah_engine *e, const char *sid, char **json, ah_error *err) {
    if (json)
        *json = NULL;
    if (!e || !sid || !json)
        return ah_fail(err, AH_INVALID, "engine, session and output required");
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(
        e, "SELECT id,model,state,turn,model_calls,tool_calls FROM sessions WHERE id=?", &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    if (step != SQLITE_ROW) {
        sqlite3_finalize(s);
        return ah_fail(err, step == SQLITE_DONE ? AH_NOT_FOUND : AH_IO, "session unavailable");
    }
    yyjson_mut_doc *d = ah_mut_doc_new();
    if (!d) {
        sqlite3_finalize(s);
        return ah_fail(err, AH_IO, "out of memory");
    }
    ah_state state = (ah_state)sqlite3_column_int(s, 2);
    yyjson_mut_val *o = session_value(d, s);
    yyjson_mut_doc_set_root(d, o);
    sqlite3_finalize(s);
    core_operation op;
    rc = pending_operation(e, sid, state, &op, err);
    if (rc == AH_OK) {
        yyjson_mut_val *p = yyjson_mut_obj(d);
        yyjson_mut_obj_add_val(d, o, "pending", p);
        yyjson_mut_obj_add_strcpy(d, p, "operation_id", op.id);
        yyjson_mut_obj_add_strcpy(d, p, "call_id", op.call_id);
        yyjson_mut_obj_add_strcpy(d, p, "name", op.name);
        yyjson_mut_obj_add_strcpy(d, p, "status", op.status);
        yyjson_doc *args = yyjson_read(op.arguments, strlen(op.arguments), 0);
        if (args)
            yyjson_mut_obj_add_val(d, p, "arguments",
                                   yyjson_val_mut_copy(d, yyjson_doc_get_root(args)));
        else
            yyjson_mut_obj_add_strcpy(d, p, "arguments", op.arguments);
        yyjson_doc_free(args);
        core_operation_free(&op);
    } else if (rc == AH_NOT_FOUND) {
        yyjson_mut_obj_add_null(d, o, "pending");
        rc = AH_OK;
    }
    if (rc == AH_OK) {
        *json = ah_mut_write(d);
        if (!*json)
            rc = ah_fail(err, AH_IO, "out of memory");
    }
    ah_mut_doc_free(d);
    return rc;
}
