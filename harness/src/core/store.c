#include "core.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/file.h>

ah_code core_sql(ah_engine *e, const char *sql, ah_error *err) {
    char *message = NULL;
    int rc = sqlite3_exec(e->db, sql, NULL, NULL, &message);
    if (rc == SQLITE_OK)
        return AH_OK;
    ah_code code = ah_fail(err, rc == SQLITE_BUSY ? AH_BUSY : AH_IO, "sqlite: %s",
                           message ? message : sqlite3_errmsg(e->db));
    sqlite3_free(message);
    return code;
}
ah_code core_prepare(ah_engine *e, const char *sql, sqlite3_stmt **stmt, ah_error *err) {
    if (sqlite3_prepare_v2(e->db, sql, -1, stmt, NULL) == SQLITE_OK)
        return AH_OK;
    return ah_fail(err, AH_IO, "sqlite prepare: %s", sqlite3_errmsg(e->db));
}
ah_code core_done(ah_engine *e, sqlite3_stmt *stmt, ah_error *err) {
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE)
        return AH_OK;
    return ah_fail(err, rc == SQLITE_CONSTRAINT ? AH_INVALID : AH_IO, "sqlite write: %s",
                   sqlite3_errmsg(e->db));
}
ah_code core_begin(ah_engine *e, uint64_t *after, ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc = core_sql(e, "BEGIN IMMEDIATE", err);
    if (rc != AH_OK)
        return rc;
    rc = core_prepare(e, "SELECT COALESCE(MAX(seq),0) FROM events", &s, err);
    if (rc == AH_OK && sqlite3_step(s) == SQLITE_ROW)
        *after = (uint64_t)sqlite3_column_int64(s, 0);
    else if (rc == AH_OK)
        rc = ah_fail(err, AH_IO, "cannot read event sequence");
    sqlite3_finalize(s);
    if (rc != AH_OK)
        core_rollback(e);
    return rc;
}
ah_code core_commit(ah_engine *e, uint64_t after, ah_error *err) {
    ah_code rc = core_sql(e, "COMMIT", err);
    if (rc != AH_OK) {
        core_rollback(e);
        return rc;
    }
    if (e->config.observer)
        return ah_events(e, NULL, after, e->config.observer, e->config.observer_context, err);
    return AH_OK;
}
void core_rollback(ah_engine *e) {
    sqlite3_exec(e->db, "ROLLBACK", NULL, NULL, NULL);
}
/* Runs inside the schema transaction, only when upgrading from version 0/1. */
ah_code core_migrate_result_refs(ah_engine *e, ah_error *err) {
    ah_code rc =
        core_sql(e,
                 "INSERT OR IGNORE INTO result_refs(session_id,operation_id) SELECT session_id,id "
                 "FROM operations WHERE result IS NOT NULL",
                 err);
    sqlite3_stmt *events = NULL;
    if (rc == AH_OK)
        rc = core_prepare(e,
                          "SELECT seq,session_id,json FROM events WHERE type='session.forked' "
                          "ORDER BY seq",
                          &events, err);
    int step = SQLITE_DONE;
    while (rc == AH_OK && (step = sqlite3_step(events)) == SQLITE_ROW) {
        sqlite3_int64 seq = sqlite3_column_int64(events, 0);
        const char *child = (const char *)sqlite3_column_text(events, 1);
        const char *json = (const char *)sqlite3_column_text(events, 2);
        yyjson_doc *doc = yyjson_read(json, (size_t)sqlite3_column_bytes(events, 2), 0);
        yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
        const char *source = yyjson_get_str(yyjson_obj_get(root, "source_session"));
        ah_error validation = {0};
        if (!yyjson_is_obj(root) || ah_json_check(root, &validation) != AH_OK || !source ||
            !*source || strlen(source) >= AH_ID_CAP || !strcmp(source, child)) {
            yyjson_doc_free(doc);
            rc = ah_fail(err, AH_PROTOCOL, "invalid fork lineage during schema migration");
            break;
        }
        sqlite3_stmt *s = NULL;
        rc = core_prepare(e,
                          "SELECT EXISTS(SELECT 1 FROM sessions WHERE id=?) AND "
                          "EXISTS(SELECT 1 FROM events WHERE session_id=? AND seq<? AND "
                          "type IN('session.created','session.forked')) AND "
                          "NOT EXISTS(SELECT 1 FROM events WHERE session_id=? AND seq<? AND "
                          "type IN('session.created','session.forked'))",
                          &s, err);
        if (rc == AH_OK) {
            sqlite3_bind_text(s, 1, source, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, source, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 3, seq);
            sqlite3_bind_text(s, 4, child, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 5, seq);
            int found = sqlite3_step(s);
            if (found != SQLITE_ROW)
                rc = ah_fail(err, AH_IO, "cannot validate fork lineage");
            else if (!sqlite3_column_int(s, 0))
                rc = ah_fail(err, AH_PROTOCOL, "invalid fork ancestry during schema migration");
        }
        sqlite3_finalize(s);
        s = NULL;
        /* Owner results were seeded above, including results created after an
         * old fork. Only receipts already durable at that fork are inherited.
         * Processing fork events in order preserves transitive snapshots. */
        if (rc == AH_OK)
            rc = core_prepare(
                e,
                "INSERT OR IGNORE INTO result_refs(session_id,operation_id) "
                "SELECT ?,r.operation_id FROM result_refs r WHERE r.session_id=? "
                "AND EXISTS(SELECT 1 FROM events v WHERE v.operation_id=r.operation_id "
                "AND v.seq<? AND v.type IN('tool.completed','tool.late_result',"
                "'tool.denied','tool.resolved','input.received','tool.interrupted'))",
                &s, err);
        if (rc == AH_OK) {
            sqlite3_bind_text(s, 1, child, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, source, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 3, seq);
            rc = core_done(e, s, err);
        }
        yyjson_doc_free(doc);
    }
    if (rc == AH_OK && step != SQLITE_DONE)
        rc = ah_fail(err, AH_IO, "cannot read fork lineage");
    sqlite3_finalize(events);
    return rc;
}
ah_code core_event(ah_engine *e, const char *sid, const char *op, const char *type,
                   const char *json, ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(
        e, "INSERT INTO events(session_id,operation_id,type,json) VALUES(?,?,?,?)", &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    if (op)
        sqlite3_bind_text(s, 2, op, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 4, json ? json : "{}", -1, SQLITE_TRANSIENT);
    return core_done(e, s, err);
}
ah_code core_state(ah_engine *e, const char *sid, ah_state state, ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e, "UPDATE sessions SET state=? WHERE id=?", &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_int(s, 1, state);
    sqlite3_bind_text(s, 2, sid, -1, SQLITE_TRANSIENT);
    rc = core_done(e, s, err);
    if (rc != AH_OK)
        return rc;
    char json[80];
    snprintf(json, sizeof(json), "{\"state\":\"%s\"}", ah_state_name(state));
    return core_event(e, sid, NULL, "state", json, err);
}
ah_code core_load(ah_engine *e, const char *sid, core_session *out, ah_error *err) {
    memset(out, 0, sizeof(*out));
    if (!sid)
        return ah_fail(err, AH_INVALID, "session id required");
    sqlite3_stmt *s = NULL;
    ah_code rc =
        core_prepare(e,
                     "SELECT id,model,instructions,summary,state,turn,model_calls,tool_calls FROM "
                     "sessions WHERE id=?",
                     &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    if (step != SQLITE_ROW) {
        sqlite3_finalize(s);
        return ah_fail(err, step == SQLITE_DONE ? AH_NOT_FOUND : AH_IO, "session unavailable");
    }
    out->id = ah_strdup((const char *)sqlite3_column_text(s, 0));
    out->model = ah_strdup((const char *)sqlite3_column_text(s, 1));
    out->instructions = ah_strdup((const char *)sqlite3_column_text(s, 2));
    out->summary = ah_strdup((const char *)sqlite3_column_text(s, 3));
    out->state = (ah_state)sqlite3_column_int(s, 4);
    out->turn = (unsigned)sqlite3_column_int(s, 5);
    out->model_calls = (unsigned)sqlite3_column_int(s, 6);
    out->tool_calls = (unsigned)sqlite3_column_int(s, 7);
    sqlite3_finalize(s);
    if (!out->id || !out->model || !out->instructions || !out->summary) {
        core_session_free(out);
        return ah_fail(err, AH_IO, "out of memory");
    }
    return AH_OK;
}
void core_session_free(core_session *s) {
    free(s->id);
    free(s->model);
    free(s->instructions);
    free(s->summary);
    memset(s, 0, sizeof(*s));
}
ah_code core_id(char id[AH_ID_CAP], ah_error *err) {
    unsigned char bytes[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return ah_fail(err, AH_IO, "cannot open randomness source");
    size_t used = 0;
    while (used < sizeof(bytes)) {
        ssize_t n = read(fd, bytes + used, sizeof(bytes) - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            close(fd);
            return ah_fail(err, AH_IO, "randomness read failed");
        }
        used += (size_t)n;
    }
    close(fd);
    bytes[6] = (unsigned char)((bytes[6] & 15) | 64);
    bytes[8] = (unsigned char)((bytes[8] & 63) | 128);
    snprintf(id, AH_ID_CAP, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return AH_OK;
}
ah_code core_add_item(ah_engine *e, const char *sid, unsigned turn, const char *json,
                      ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc =
        core_prepare(e, "INSERT INTO items(session_id,turn,json,active) VALUES(?,?,?,1)", &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, turn);
    sqlite3_bind_text(s, 3, json, -1, SQLITE_TRANSIENT);
    return core_done(e, s, err);
}
ah_code ah_events(ah_engine *e, const char *sid, uint64_t after, ah_observer observer,
                  void *context, ah_error *err) {
    if (!e || !observer)
        return ah_fail(err, AH_INVALID, "engine and observer required");
    if (after > (uint64_t)INT64_MAX) {
        ah_error_clear(err);
        return AH_OK;
    }
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e,
                              "SELECT seq,session_id,operation_id,type,json FROM events WHERE "
                              "seq>? AND (? IS NULL OR session_id=?) ORDER BY seq",
                              &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_int64(s, 1, (sqlite3_int64)after);
    if (sid) {
        sqlite3_bind_text(s, 2, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, sid, -1, SQLITE_TRANSIENT);
    }
    int step;
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        ah_event event = {
            (uint64_t)sqlite3_column_int64(s, 0), (const char *)sqlite3_column_text(s, 1),
            (const char *)sqlite3_column_text(s, 2), (const char *)sqlite3_column_text(s, 3),
            (const char *)sqlite3_column_text(s, 4)};
        bool previous = e->observing;
        e->observing = true;
        observer(context, &event);
        e->observing = previous;
    }
    sqlite3_finalize(s);
    return step == SQLITE_DONE ? AH_OK : ah_fail(err, AH_IO, "event read failed");
}
char *core_object_string(const char *key, const char *value) {
    yyjson_mut_doc *d = ah_mut_doc_new();
    if (!d)
        return NULL;
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, o);
    yyjson_mut_obj_add_strcpy(d, o, key, value);
    char *json = ah_mut_write(d);
    ah_mut_doc_free(d);
    return json;
}
ah_code core_guard(ah_engine *e, ah_error *err) {
    if (!e)
        return ah_fail(err, AH_INVALID, "engine required");
    if (e->running || e->observing)
        return ah_fail(err, AH_BUSY, "engine is running");
    ah_error_clear(err);
    return AH_OK;
}
