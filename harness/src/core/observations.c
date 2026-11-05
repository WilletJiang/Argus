#include "core.h"

/* Only complete model-visible read receipts can justify replacing another read
 * with a reference. The active-item join handles compaction; forks start empty. */
char *core_observation_view(ah_engine *e, const char *sid, const core_operation *op,
                            const char *json, bool is_error, char digest[65], ah_error *error) {
    digest[0] = 0;
    char *view = core_result_view(e, op->id, json, is_error);
    if (!view || is_error || strcmp(op->name, "read_file"))
        return view;
    yyjson_doc *args = yyjson_read(op->arguments, strlen(op->arguments), 0),
               *parsed = yyjson_read(view, strlen(view), 0);
    yyjson_val *result = parsed ? yyjson_obj_get(yyjson_doc_get_root(parsed), "result") : NULL;
    yyjson_val *revision = yyjson_obj_get(result, "revision"),
               *content = yyjson_obj_get(result, "content");
    bool eligible = args && yyjson_is_str(content) && yyjson_get_len(revision) == 64;
    bool force = args && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(args), "force"));
    if (eligible)
        ah_hash_bytes(json, strlen(json), digest);
    if (eligible && !force) {
        sqlite3_stmt *s = NULL;
        ah_code code = core_prepare(
            e,
            "SELECT o.operation_id FROM observations o JOIN items i ON i.seq=o.item_seq "
            "WHERE o.session_id=? AND o.digest=? AND i.active=1 ORDER BY i.seq LIMIT 1",
            &s, error);
        if (code == AH_OK) {
            sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, digest, -1, SQLITE_TRANSIENT);
            int step = sqlite3_step(s);
            if (step == SQLITE_ROW) {
                yyjson_mut_doc *d = ah_mut_doc_new();
                yyjson_mut_val *root = yyjson_mut_obj(d), *ref = yyjson_mut_obj(d);
                yyjson_mut_doc_set_root(d, root);
                yyjson_mut_obj_add_bool(d, root, "is_error", false);
                yyjson_mut_obj_add_val(d, root, "result", ref);
                yyjson_mut_obj_add_bool(d, ref, "reused", true);
                yyjson_mut_obj_add_str(d, ref, "retrieval_tool", "read_result");
                yyjson_mut_obj_add_strcpy(d, ref, "operation_id",
                                          (const char *)sqlite3_column_text(s, 0));
                const char *fields[] = {"path",  "revision",   "offset_bytes",
                                        "bytes", "start_line", "lines"};
                for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); i++) {
                    yyjson_val *value = yyjson_obj_get(result, fields[i]);
                    if (value)
                        yyjson_mut_obj_add_val(d, ref, fields[i], yyjson_val_mut_copy(d, value));
                }
                char *reused = ah_mut_write(d);
                ah_mut_doc_free(d);
                if (reused && (!e->config.max_model_tool_bytes ||
                               strlen(reused) <= e->config.max_model_tool_bytes)) {
                    uint64_t now = ah_now_us();
                    core_metric(e, sid, op->id, "read.reused", now, now, strlen(view), 1, AH_OK);
                    free(view);
                    view = reused;
                    digest[0] = 0;
                } else
                    free(reused);
            } else if (step != SQLITE_DONE) {
                free(view);
                view = NULL;
            }
        } else {
            free(view);
            view = NULL;
        }
        sqlite3_finalize(s);
    }
    yyjson_doc_free(args);
    yyjson_doc_free(parsed);
    return view;
}

ah_code core_observation_save(ah_engine *e, const char *sid, const char *op, const char *digest,
                              ah_error *error) {
    if (!digest[0])
        return AH_OK;
    sqlite3_int64 item = sqlite3_last_insert_rowid(e->db);
    sqlite3_stmt *s = NULL;
    ah_code code =
        core_prepare(e,
                     "INSERT OR REPLACE INTO observations(session_id,operation_id,digest,item_seq) "
                     "VALUES(?,?,?,?)",
                     &s, error);
    if (code == AH_OK) {
        sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, op, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, digest, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 4, item);
        code = core_done(e, s, error);
    }
    return code;
}
