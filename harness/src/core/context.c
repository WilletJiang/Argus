#include "core.h"

static ah_code checkpoint_allowed(ah_engine *e, const char *sid, core_session *session,
                                  bool recover, ah_error *err) {
    ah_code rc = core_guard(e, err);
    if (rc != AH_OK)
        return rc;
    rc = core_load(e, sid, session, err);
    if (rc != AH_OK)
        return rc;
    bool stopped = session->state == AH_STOPPED || session->state == AH_FAILED;
    if (session->state != AH_IDLE && session->state != AH_COMPLETED && !(recover && stopped)) {
        core_session_free(session);
        return ah_fail(err, AH_BUSY, "checkpoint requires a complete turn boundary");
    }
    core_operation op;
    rc = core_next_operation(e, sid, &op, err);
    if (rc == AH_OK) {
        core_operation_free(&op);
        core_session_free(session);
        return ah_fail(err, AH_BUSY, "pending exchange prevents checkpoint");
    }
    if (rc == AH_NOT_FOUND)
        return AH_OK;
    core_session_free(session);
    return rc;
}
ah_code ah_fork(ah_engine *e, const char *sid, char new_id[AH_ID_CAP], ah_error *err) {
    if (!new_id)
        return ah_fail(err, AH_INVALID, "output id required");
    core_session session;
    ah_code rc = checkpoint_allowed(e, sid, &session, false, err);
    if (rc != AH_OK)
        return rc;
    rc = core_id(new_id, err);
    uint64_t after = 0;
    if (rc == AH_OK)
        rc = core_begin(e, &after, err);
    sqlite3_stmt *s = NULL;
    if (rc == AH_OK)
        rc = core_prepare(e,
                          "INSERT INTO sessions(id,model,instructions,summary,state,turn) SELECT "
                          "?,model,instructions,summary,state,turn FROM sessions WHERE id=?",
                          &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, new_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, sid, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    if (rc == AH_OK)
        rc = core_prepare(e,
                          "INSERT INTO items(session_id,turn,json,active) SELECT "
                          "?,turn,json,active FROM items WHERE session_id=? ORDER BY seq",
                          &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, new_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, sid, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    if (rc == AH_OK)
        rc = core_prepare(e,
                          "INSERT INTO call_ids(session_id,call_id) SELECT ?,call_id FROM call_ids "
                          "WHERE session_id=?",
                          &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, new_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, sid, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    if (rc == AH_OK)
        rc = core_prepare(e,
                          "INSERT INTO result_refs(session_id,operation_id) SELECT "
                          "?,operation_id FROM result_refs WHERE session_id=?",
                          &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, new_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, sid, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    char *source = core_object_string("source_session", sid);
    if (rc == AH_OK && !source)
        rc = ah_fail(err, AH_IO, "out of memory");
    if (rc == AH_OK)
        rc = core_event(e, new_id, NULL, "session.forked", source, err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    free(source);
    core_session_free(&session);
    return rc;
}
/* A stopped turn may have settled tools but still need its final model response.
 * Retain its user inputs when the host summarizes that turn with keep=0. */
static ah_code current_inputs(ah_engine *e, const core_session *session, sqlite3_int64 **ids,
                              size_t *count, ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(
        e, "SELECT seq,json FROM items WHERE session_id=? AND turn=? AND active=1 ORDER BY seq", &s,
        err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, session->turn);
    int step;
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        const char *json = (const char *)sqlite3_column_text(s, 1);
        yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
        if (!doc) {
            rc = ah_fail(err, AH_IO, "corrupt stored context");
            break;
        }
        const char *role = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "role"));
        bool user = role && !strcmp(role, "user");
        yyjson_doc_free(doc);
        if (!user)
            continue;
        sqlite3_int64 *next = realloc(*ids, (*count + 1) * sizeof(**ids));
        if (!next) {
            rc = ah_fail(err, AH_IO, "out of memory");
            break;
        }
        *ids = next;
        (*ids)[(*count)++] = sqlite3_column_int64(s, 0);
    }
    if (rc == AH_OK && step != SQLITE_DONE)
        rc = ah_fail(err, AH_IO, "context read failed");
    sqlite3_finalize(s);
    return rc;
}
ah_code ah_compact(ah_engine *e, const char *sid, const char *summary, unsigned keep,
                   ah_error *err) {
    if (!e || !summary || strlen(summary) > e->config.max_context_bytes)
        return ah_fail(err, AH_INVALID, "bounded summary required");
    core_session session;
    ah_code rc = checkpoint_allowed(e, sid, &session, true, err);
    if (rc != AH_OK)
        return rc;
    unsigned cutoff = session.turn > keep ? session.turn - keep : 0;
    uint64_t after = 0;
    rc = core_begin(e, &after, err);
    sqlite3_int64 *inputs = NULL;
    size_t input_count = 0;
    if (rc == AH_OK && keep == 0 && (session.state == AH_STOPPED || session.state == AH_FAILED))
        rc = current_inputs(e, &session, &inputs, &input_count, err);
    sqlite3_stmt *s = NULL;
    if (rc == AH_OK)
        rc = core_prepare(e, "UPDATE items SET active=0 WHERE session_id=? AND turn<=?", &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, cutoff);
        rc = core_done(e, s, err);
    }
    for (size_t i = 0; i < input_count && rc == AH_OK; i++) {
        rc = core_prepare(e, "UPDATE items SET active=1 WHERE seq=? AND session_id=?", &s, err);
        if (rc == AH_OK) {
            sqlite3_bind_int64(s, 1, inputs[i]);
            sqlite3_bind_text(s, 2, sid, -1, SQLITE_TRANSIENT);
            rc = core_done(e, s, err);
        }
    }
    if (rc == AH_OK)
        rc = core_prepare(e, "UPDATE sessions SET summary=? WHERE id=?", &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, summary, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, sid, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    char *json = core_object_string("summary", summary);
    if (rc == AH_OK && !json)
        rc = ah_fail(err, AH_IO, "out of memory");
    if (rc == AH_OK)
        rc = core_event(e, sid, NULL, "context.compacted", json, err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    free(json);
    free(inputs);
    core_session_free(&session);
    return rc;
}
