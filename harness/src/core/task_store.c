#include "core.h"
#include "ah/task.h"

ah_code core_task_recover(ah_engine *e, ah_error *error) {
    sqlite3_stmt *s = NULL;
    ah_code code =
        core_prepare(e, "SELECT 1 FROM tasks WHERE state='verifying' LIMIT 1", &s, error);
    if (code != AH_OK)
        return code;
    int step = sqlite3_step(s);
    sqlite3_finalize(s);
    if (step == SQLITE_DONE)
        return AH_OK;
    if (step != SQLITE_ROW)
        return ah_fail(error, AH_IO, "Cannot inspect interrupted verification");
    uint64_t after = 0;
    code = core_begin(e, &after, error);
    if (code == AH_OK)
        code = core_sql(e,
                        "INSERT INTO events(session_id,type,json) SELECT session_id,"
                        "'task.interrupted','{\"reason\":\"verifier interrupted; reconcile "
                        "effects\"}' FROM tasks WHERE state='verifying';"
                        "UPDATE tasks SET state='unknown' WHERE state='verifying';",
                        error);
    if (code == AH_OK)
        code = core_commit(e, after, error);
    else
        core_rollback(e);
    return code;
}

ah_code ah_task_json(ah_engine *e, const char *sid, char **json, ah_error *error) {
    if (json)
        *json = NULL;
    if (!e || !sid || !json)
        return ah_fail(error, AH_INVALID, "Engine, session and output required");
    sqlite3_stmt *s = NULL;
    ah_code code =
        core_prepare(e,
                     "SELECT revision,verifier_revision,model_limit,tool_limit,repair_limit,"
                     "deadline_ms,model_used,tool_used,repairs,state,last_failure,receipt,contract "
                     "FROM tasks WHERE session_id=?",
                     &s, error);
    if (code != AH_OK)
        return code;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    if (step != SQLITE_ROW) {
        sqlite3_finalize(s);
        return ah_fail(error, step == SQLITE_DONE ? AH_NOT_FOUND : AH_IO, "Task unavailable");
    }
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    const char *fields[] = {
        "revision",     "verifier_revision", "model_limit", "tool_limit", "repair_limit",
        "deadline_ms",  "model_used",        "tool_used",   "repairs",    "state",
        "last_failure", "receipt",           "contract"};
    for (int i = 0; i < 13; i++) {
        if (i >= 2 && i <= 8)
            yyjson_mut_obj_add_uint(doc, root, fields[i], (uint64_t)sqlite3_column_int64(s, i));
        else if (i >= 11) {
            const char *text = (const char *)sqlite3_column_text(s, i);
            yyjson_doc *parsed = text ? yyjson_read(text, strlen(text), 0) : NULL;
            if (parsed)
                yyjson_mut_obj_add_val(doc, root, fields[i],
                                       yyjson_val_mut_copy(doc, yyjson_doc_get_root(parsed)));
            else
                yyjson_mut_obj_add_null(doc, root, fields[i]);
            yyjson_doc_free(parsed);
        } else
            yyjson_mut_obj_add_strcpy(doc, root, fields[i],
                                      (const char *)sqlite3_column_text(s, i));
    }
    sqlite3_finalize(s);
    *json = ah_mut_write(doc);
    ah_mut_doc_free(doc);
    return *json ? AH_OK : ah_fail(error, AH_IO, "Cannot encode task");
}

ah_code ah_task_reconcile(ah_engine *e, const char *sid, const char *evidence, ah_error *error) {
    ah_code code = core_guard(e, error);
    if (code != AH_OK)
        return code;
    if (!sid || !evidence || strlen(evidence) > e->config.max_output_bytes ||
        !ah_json_valid(evidence))
        return ah_fail(error, AH_INVALID, "Bounded reconciliation evidence required");
    uint64_t after = 0;
    sqlite3_stmt *s = NULL;
    code = core_begin(e, &after, error);
    if (code == AH_OK)
        code = core_prepare(e,
                            "UPDATE tasks SET state='active' WHERE session_id=? AND state IN "
                            "('unknown','verifying')",
                            &s, error);
    if (code == AH_OK) {
        sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
        code = core_done(e, s, error);
        if (code == AH_OK && sqlite3_changes(e->db) != 1)
            code = ah_fail(error, AH_INVALID, "Task does not require reconciliation");
    }
    if (code == AH_OK)
        code = core_event(e, sid, NULL, "task.reconciled", evidence, error);
    if (code == AH_OK)
        code = core_commit(e, after, error);
    else
        core_rollback(e);
    return code;
}

ah_code ah_task_create(ah_engine *e, const char *sid, const ah_task_config *config,
                       ah_error *error) {
    ah_code code = core_guard(e, error);
    if (code != AH_OK)
        return code;
    if (!sid || !config || !config->contract_json ||
        strlen(config->contract_json) > e->config.max_context_bytes || !config->verifier_revision ||
        strlen(config->verifier_revision) != 64 || !config->max_model_calls ||
        !config->max_tool_calls || config->max_repairs > 100 || !config->timeout_ms ||
        config->timeout_ms > 30ULL * 24 * 60 * 60 * 1000)
        return ah_fail(error, AH_INVALID,
                       "Bounded task contract, verifier identity and positive budgets required");
    yyjson_doc *parsed = yyjson_read(config->contract_json, strlen(config->contract_json), 0);
    bool valid = parsed && yyjson_is_obj(yyjson_doc_get_root(parsed));
    yyjson_doc_free(parsed);
    if (!valid)
        return ah_fail(error, AH_INVALID, "Task contract must be a JSON object");
    sqlite3_stmt *s = NULL;
    code = core_prepare(
        e,
        "SELECT contract,verifier_revision,model_limit,tool_limit,repair_limit,timeout_ms FROM "
        "tasks WHERE session_id=?",
        &s, error);
    if (code != AH_OK)
        return code;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    if (step == SQLITE_ROW) {
        bool same = !strcmp((const char *)sqlite3_column_text(s, 0), config->contract_json) &&
                    !strcmp((const char *)sqlite3_column_text(s, 1), config->verifier_revision) &&
                    (uint64_t)sqlite3_column_int64(s, 2) == config->max_model_calls &&
                    (uint64_t)sqlite3_column_int64(s, 3) == config->max_tool_calls &&
                    (uint64_t)sqlite3_column_int64(s, 4) == config->max_repairs &&
                    (uint64_t)sqlite3_column_int64(s, 5) == config->timeout_ms;
        sqlite3_finalize(s);
        return same ? AH_OK
                    : ah_fail(error, AH_INVALID,
                              "Task contract/budgets are immutable; use a new session");
    }
    sqlite3_finalize(s);
    if (step != SQLITE_DONE)
        return ah_fail(error, AH_IO, "Cannot load task");
    core_session session;
    code = core_load(e, sid, &session, error);
    if (code != AH_OK)
        return code;
    valid = (session.state == AH_IDLE || session.state == AH_READY) && session.turn <= 1 &&
            !session.model_calls && !session.tool_calls;
    if (strlen(session.instructions) + strlen(config->contract_json) + 32 >
        e->config.max_context_bytes)
        valid = false;
    core_session_free(&session);
    if (!valid)
        return ah_fail(error, AH_INVALID, "Attach task before running a fresh session");
    char revision[65];
    ah_hash_bytes(config->contract_json, strlen(config->contract_json), revision);
    uint64_t after = 0;
    code = core_begin(e, &after, error);
    if (code == AH_OK)
        code = core_prepare(e,
                            "INSERT INTO "
                            "tasks(session_id,contract,revision,verifier_revision,model_limit,tool_"
                            "limit,repair_limit,timeout_ms,deadline_ms) VALUES(?,?,?,?,?,?,?,?,?)",
                            &s, error);
    if (code == AH_OK) {
        sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, config->contract_json, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, revision, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 4, config->verifier_revision, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 5, config->max_model_calls);
        sqlite3_bind_int64(s, 6, config->max_tool_calls);
        sqlite3_bind_int64(s, 7, config->max_repairs);
        sqlite3_bind_int64(s, 8, (sqlite3_int64)config->timeout_ms);
        sqlite3_bind_int64(s, 9, (sqlite3_int64)(core_wall_ms() + config->timeout_ms));
        code = core_done(e, s, error);
    }
    if (code == AH_OK)
        code = core_prepare(e,
                            "UPDATE sessions SET instructions=instructions||char(10)||'Task "
                            "acceptance contract:'||char(10)||? WHERE id=?",
                            &s, error);
    if (code == AH_OK) {
        sqlite3_bind_text(s, 1, config->contract_json, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, sid, -1, SQLITE_TRANSIENT);
        code = core_done(e, s, error);
    }
    if (code == AH_OK)
        code = core_event(e, sid, NULL, "task.created", config->contract_json, error);
    if (code == AH_OK)
        code = core_commit(e, after, error);
    else
        core_rollback(e);
    return code;
}
