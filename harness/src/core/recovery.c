#include "core.h"

ah_code core_recover(ah_engine *e, ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e,
                              "SELECT id FROM sessions WHERE state=? OR EXISTS "
                              "(SELECT 1 FROM operations WHERE session_id=sessions.id "
                              "AND status='executing')",
                              &s, err);
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
        uint64_t after = 0;
        rc = core_begin(e, &after, err);
        ah_state state = AH_STOPPED;
        while (rc == AH_OK) {
            sqlite3_stmt *pending = NULL;
            rc = core_prepare(e,
                              "SELECT id FROM operations WHERE session_id=? AND status='executing' "
                              "ORDER BY seq LIMIT 1",
                              &pending, err);
            if (rc != AH_OK)
                break;
            sqlite3_bind_text(pending, 1, ids[i], -1, SQLITE_TRANSIENT);
            int found = sqlite3_step(pending);
            core_operation op = {0};
            if (found == SQLITE_ROW)
                rc = core_get_operation(e, ids[i], (const char *)sqlite3_column_text(pending, 0),
                                        &op, err);
            else if (found != SQLITE_DONE)
                rc = ah_fail(err, AH_IO, "cannot recover operation");
            sqlite3_finalize(pending);
            if (rc != AH_OK || found == SQLITE_DONE)
                break;
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
            core_operation_free(&op);
        }
        if (rc == AH_OK)
            rc = core_state(e, ids[i], state, err);
        if (rc == AH_OK)
            rc = core_event(e, ids[i], NULL, "session.recovered", "{}", err);
        if (rc == AH_OK)
            rc = core_commit(e, after, err);
        else
            core_rollback(e);
    }
    for (size_t i = 0; i < count; i++)
        free(ids[i]);
    free(ids);
    return rc;
}
