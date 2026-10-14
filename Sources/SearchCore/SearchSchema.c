#include "SearchIndex.h"
#include <stdio.h>
#include <string.h>

static const char *schema =
    "CREATE TABLE IF NOT EXISTS entries("
    "id INTEGER PRIMARY KEY,path TEXT NOT NULL UNIQUE,name TEXT NOT NULL,aliases TEXT NOT NULL,"
    "kind INTEGER NOT NULL,mtime REAL NOT NULL,size INTEGER NOT NULL,"
    "nname TEXT NOT NULL,npath TEXT NOT NULL,naliases TEXT NOT NULL,body TEXT,content_mtime REAL,body_offsets BLOB,content_eligible INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS scan_jobs(id INTEGER PRIMARY KEY,mode INTEGER NOT NULL,path TEXT NOT NULL,"
    "applications_only INTEGER NOT NULL,UNIQUE(mode,path));"
    "CREATE INDEX IF NOT EXISTS entries_name ON entries(nname);"
    "CREATE INDEX IF NOT EXISTS entries_kind ON entries(kind);"
    "CREATE VIRTUAL TABLE IF NOT EXISTS meta USING fts5(nname,npath,naliases,"
    "content='entries',content_rowid='id',tokenize='argus_meta',detail=column,columnsize=0);"
    "CREATE VIRTUAL TABLE IF NOT EXISTS fulltext USING fts5(body,content='entries',"
    "content_rowid='id',tokenize='argus_body_v2',columnsize=0);"
    "CREATE TABLE IF NOT EXISTS stats(id INTEGER PRIMARY KEY,content_bytes INTEGER NOT NULL);"
    "INSERT OR IGNORE INTO stats(id,content_bytes) VALUES(1,0);"
    "CREATE TRIGGER IF NOT EXISTS entries_insert AFTER INSERT ON entries BEGIN "
    "INSERT INTO meta(rowid,nname,npath,naliases) VALUES(new.id,new.nname,new.npath,new.naliases); END;"
    "CREATE TRIGGER IF NOT EXISTS entries_delete AFTER DELETE ON entries BEGIN "
    "INSERT INTO meta(meta,rowid,nname,npath,naliases) VALUES('delete',old.id,old.nname,old.npath,old.naliases);"
    "INSERT INTO fulltext(fulltext,rowid,body) SELECT 'delete',old.id,old.body WHERE old.body IS NOT NULL;"
    "UPDATE stats SET content_bytes=content_bytes-length(cast(coalesce(old.body,'') AS blob)) WHERE id=1; END;"
    "CREATE TRIGGER IF NOT EXISTS entries_meta_update AFTER UPDATE OF nname,npath,naliases ON entries "
    "WHEN old.nname<>new.nname OR old.npath<>new.npath OR old.naliases<>new.naliases BEGIN "
    "INSERT INTO meta(meta,rowid,nname,npath,naliases) VALUES('delete',old.id,old.nname,old.npath,old.naliases);"
    "INSERT INTO meta(rowid,nname,npath,naliases) VALUES(new.id,new.nname,new.npath,new.naliases); END;"
    "CREATE TRIGGER IF NOT EXISTS entries_body_update AFTER UPDATE OF body ON entries "
    "WHEN old.body IS NOT new.body BEGIN "
    "INSERT INTO fulltext(fulltext,rowid,body) SELECT 'delete',old.id,old.body WHERE old.body IS NOT NULL;"
    "INSERT INTO fulltext(rowid,body) SELECT new.id,new.body WHERE new.body IS NOT NULL;"
    "UPDATE stats SET content_bytes=content_bytes+length(cast(coalesce(new.body,'') AS blob))"
    "-length(cast(coalesce(old.body,'') AS blob)) WHERE id=1; END;";

static int add_column(ArgusIndex *index, const char *table, const char *name, const char *definition, int *added) {
    char sql[256];
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
    sqlite3_stmt *statement;
    if (!argus_prepare(index, sql, &statement)) return 0;
    int found = 0;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const char *column = (const char *)sqlite3_column_text(statement, 1);
        if (column && !strcmp(column, name)) found = 1;
    }
    sqlite3_finalize(statement);
    if (added) *added = !found;
    if (found) return 1;
    snprintf(sql, sizeof(sql), "ALTER TABLE %s ADD COLUMN %s %s", table, name, definition);
    return argus_execute(index, sql);
}

static int extend_schema(ArgusIndex *index) {
    int added_count, added_eligibility;
    if (!add_column(index, "entries", "body_offsets", "BLOB", NULL) ||
        !add_column(index, "entries", "content_eligible", "INTEGER NOT NULL DEFAULT 0", &added_eligibility) ||
        !add_column(index, "stats", "row_count", "INTEGER NOT NULL DEFAULT 0", &added_count) ||
        !add_column(index, "stats", "metadata_generation", "INTEGER NOT NULL DEFAULT 0", NULL) ||
        !add_column(index, "stats", "metadata_id", "BLOB", NULL)) return 0;
    if (added_eligibility && !argus_execute(index, "UPDATE entries SET content_eligible=1 WHERE body IS NOT NULL")) return 0;
    if (added_count && !argus_execute(index, "UPDATE stats SET row_count=(SELECT count(*) FROM entries) WHERE id=1")) return 0;
    return argus_execute(index,
        "UPDATE stats SET metadata_id=randomblob(16) WHERE id=1 AND metadata_id IS NULL;"
        "CREATE TRIGGER IF NOT EXISTS entries_generation_insert AFTER INSERT ON entries BEGIN "
        "UPDATE stats SET row_count=row_count+1,metadata_generation=metadata_generation+1 WHERE id=1; END;"
        "CREATE TRIGGER IF NOT EXISTS entries_generation_delete AFTER DELETE ON entries BEGIN "
        "UPDATE stats SET row_count=row_count-1,metadata_generation=metadata_generation+1 WHERE id=1; END;"
        "CREATE TRIGGER IF NOT EXISTS entries_generation_update AFTER UPDATE OF nname,npath,naliases,kind ON entries "
        "WHEN old.nname<>new.nname OR old.npath<>new.npath OR old.naliases<>new.naliases OR old.kind<>new.kind BEGIN "
        "UPDATE stats SET metadata_generation=metadata_generation+1 WHERE id=1; END;");
}

int argus_index_upgrade(ArgusIndex *index) {
    if (!index) return 0;
    index->error[0] = 0;
    sqlite3_stmt *statement;
    if (!argus_prepare(index, "SELECT instr(sql,'argus_body_v2') FROM sqlite_master WHERE name='fulltext'", &statement)) return 0;
    int result = sqlite3_step(statement);
    int current = result == SQLITE_ROW && sqlite3_column_int(statement, 0) != 0;
    sqlite3_finalize(statement);
    if (result != SQLITE_ROW) return argus_sql_error(index);
    if (current) return 1;
    if (!sqlite3_get_autocommit(index->db)) return argus_fail(index, "Index upgrade requires no open transaction");
    if (!argus_execute(index, "BEGIN IMMEDIATE")) return 0;
    int ok = argus_execute(index,
        "DROP TABLE fulltext;"
        "CREATE VIRTUAL TABLE fulltext USING fts5(body,content='entries',content_rowid='id',"
        "tokenize='argus_body_v2',columnsize=0);"
        "INSERT INTO fulltext(fulltext) VALUES('rebuild');"
        "UPDATE entries SET body_offsets=argus_offsets(body) WHERE body IS NOT NULL;"
        "COMMIT;");
    if (!ok) sqlite3_exec(index->db, "ROLLBACK", NULL, NULL, NULL);
    return ok;
}

int argus_schema_open(ArgusIndex *index) {
    if (!argus_execute(index, "BEGIN IMMEDIATE")) return 0;
    int ok = argus_execute(index, schema) && extend_schema(index) && argus_execute(index, "COMMIT");
    if (!ok) sqlite3_exec(index->db, "ROLLBACK", NULL, NULL, NULL);
    return ok;
}
