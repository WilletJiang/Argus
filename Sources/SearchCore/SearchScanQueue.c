#include "SearchIndex.h"
#include <string.h>

static sqlite3_stmt *scan_statement(ArgusIndex *index, int slot, const char *sql) {
    sqlite3_stmt **statement = index->scan_statements + slot;
    if (!*statement && !argus_prepare(index, sql, statement)) return NULL;
    sqlite3_reset(*statement); sqlite3_clear_bindings(*statement);
    return *statement;
}

int argus_index_queue_scan(ArgusIndex *index, const char *path, int mode, int applications_only) {
    if (!index) return 0;
    index->error[0] = 0;
    if (!path || !*path || strlen(path) >= 4096 || (mode != 0 && mode != 1))
        return argus_fail(index, "Invalid scan job");
    sqlite3_stmt *statement = scan_statement(index, 0,
        "INSERT OR IGNORE INTO scan_jobs(mode,path,applications_only) VALUES(?1,?2,?3)");
    if (!statement) return 0;
    sqlite3_bind_int(statement, 1, mode); sqlite3_bind_text(statement, 2, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 3, applications_only != 0);
    int status = sqlite3_step(statement);
    if (status != SQLITE_DONE) argus_sql_error(index);
    sqlite3_reset(statement);
    return status == SQLITE_DONE;
}

int argus_index_next_scan(ArgusIndex *index, ArgusScanItem *item) {
    if (!index || !item) return -1;
    index->error[0] = 0;
    sqlite3_stmt *statement = scan_statement(index, 1,
        "SELECT id,path,mode,applications_only FROM scan_jobs ORDER BY id LIMIT 1");
    if (!statement) return -1;
    int status = sqlite3_step(statement);
    if (status != SQLITE_ROW) {
        if (status != SQLITE_DONE) argus_sql_error(index);
        sqlite3_reset(statement); return status == SQLITE_DONE ? 0 : -1;
    }
    memset(item, 0, sizeof(*item));
    item->id = sqlite3_column_int64(statement, 0);
    argus_copy_utf8(item->path, sizeof(item->path), argus_column_text(statement, 1));
    item->mode = sqlite3_column_int(statement, 2); item->applications_only = sqlite3_column_int(statement, 3);
    sqlite3_reset(statement);
    statement = scan_statement(index, 2, "DELETE FROM scan_jobs WHERE id=?1");
    if (!statement) return -1;
    sqlite3_bind_int64(statement, 1, item->id);
    status = sqlite3_step(statement);
    if (status != SQLITE_DONE) argus_sql_error(index);
    sqlite3_reset(statement);
    return status == SQLITE_DONE ? 1 : -1;
}

int argus_index_next_content(ArgusIndex *index, int64_t after_id, ArgusScanItem *item) {
    if (!index || !item) return -1;
    index->error[0] = 0;
    sqlite3_stmt *statement = scan_statement(index, 3,
        "SELECT id,path,size FROM entries WHERE id>?1 AND content_eligible=1 "
        "AND (content_mtime IS NULL OR content_mtime<>mtime) ORDER BY id LIMIT 1");
    if (!statement) return -1;
    sqlite3_bind_int64(statement, 1, after_id);
    int status = sqlite3_step(statement);
    if (status != SQLITE_ROW) {
        if (status != SQLITE_DONE) argus_sql_error(index);
        sqlite3_reset(statement); return status == SQLITE_DONE ? 0 : -1;
    }
    memset(item, 0, sizeof(*item));
    item->id = sqlite3_column_int64(statement, 0); item->size = sqlite3_column_int64(statement, 2);
    argus_copy_utf8(item->path, sizeof(item->path), argus_column_text(statement, 1));
    sqlite3_reset(statement);
    return 1;
}

int argus_index_clear_scan_jobs(ArgusIndex *index) {
    if (!index) return 0;
    index->error[0] = 0;
    return argus_execute(index, "DELETE FROM scan_jobs");
}
