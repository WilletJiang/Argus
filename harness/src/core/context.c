#include "core.h"

static ah_code checkpoint_allowed(ah_engine *e, const char *sid, core_session *session,
                                  ah_error *err) {
    ah_code rc = core_guard(e, err);
    if (rc != AH_OK)
        return rc;
    rc = core_load(e, sid, session, err);
    if (rc != AH_OK)
        return rc;
    if (session->state != AH_IDLE && session->state != AH_COMPLETED) {
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
    ah_code rc = checkpoint_allowed(e, sid, &session, err);
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
ah_code ah_compact(ah_engine *e, const char *sid, const char *summary, unsigned keep,
                   ah_error *err) {
    if (!e || !summary || strlen(summary) > e->config.max_context_bytes)
        return ah_fail(err, AH_INVALID, "bounded summary required");
    core_session session;
    ah_code rc = checkpoint_allowed(e, sid, &session, err);
    if (rc != AH_OK)
        return rc;
    unsigned cutoff = session.turn > keep ? session.turn - keep : 0;
    uint64_t after = 0;
    rc = core_begin(e, &after, err);
    sqlite3_stmt *s = NULL;
    if (rc == AH_OK)
        rc = core_prepare(e, "UPDATE items SET active=0 WHERE session_id=? AND turn<=?", &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, cutoff);
        rc = core_done(e, s, err);
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
    core_session_free(&session);
    return rc;
}
