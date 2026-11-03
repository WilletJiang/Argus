#include "core.h"
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>

void ah_config_init(ah_config *c) {
    if (!c)
        return;
    memset(c, 0, sizeof(*c));
    c->database_path = "harness.db";
    c->max_context_bytes = 1024 * 1024;
    c->max_output_bytes = 1024 * 1024;
    c->max_tool_arguments_bytes = 64 * 1024;
    c->max_model_calls = 32;
    c->max_tool_calls = 128;
    c->run_timeout_ms = 300000;
    c->tool_timeout_ms = 30000;
    c->max_parallel_tools = 4;
    c->max_model_tool_bytes = 16384;
}
ah_code ah_open(const ah_config *config, ah_engine **out, ah_error *err) {
    if (out)
        *out = NULL;
    if (!config || !out || !config->database_path || !config->provider.complete ||
        !config->max_context_bytes || !config->max_output_bytes ||
        !config->max_tool_arguments_bytes || !config->max_model_calls || !config->max_tool_calls ||
        !config->run_timeout_ms || !config->tool_timeout_ms || config->max_parallel_tools > 16 ||
        (config->max_model_tool_bytes && config->max_model_tool_bytes < 1024))
        return ah_fail(err, AH_INVALID,
                       "configuration requires provider, database and positive budgets");
    ah_engine *e = calloc(1, sizeof(*e));
    if (!e)
        return ah_fail(err, AH_IO, "out of memory");
    e->config = *config;
    e->lock_fd = -1;
    atomic_init(&e->cancelled, false);
    if (pthread_mutex_init(&e->mailbox_lock, NULL) != 0) {
        free(e);
        return ah_fail(err, AH_IO, "cannot initialize steering mailbox");
    }
    e->mailbox_initialized = true;
    if (strcmp(config->database_path, ":memory:") != 0) {
        size_t path_length = strlen(config->database_path);
        char *lock_path = malloc(path_length + 6);
        if (!lock_path) {
            ah_close(e);
            return ah_fail(err, AH_IO, "out of memory");
        }
        memcpy(lock_path, config->database_path, path_length);
        memcpy(lock_path + path_length, ".lock", 6);
        e->lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        free(lock_path);
        struct stat info;
        if (e->lock_fd < 0 || fstat(e->lock_fd, &info) < 0 || !S_ISREG(info.st_mode) ||
            info.st_nlink != 1 || info.st_uid != getuid() || (info.st_mode & 0077)) {
            ah_close(e);
            return ah_fail(err, AH_IO, "database lock must be an owner-only regular file");
        }
        if (flock(e->lock_fd, LOCK_EX | LOCK_NB) < 0) {
            ah_close(e);
            return ah_fail(err, AH_BUSY, "database is owned by another engine");
        }
        int db_fd = open(config->database_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        bool safe = db_fd >= 0 && fstat(db_fd, &info) == 0 && S_ISREG(info.st_mode) &&
                    info.st_nlink == 1 && info.st_uid == getuid() && !(info.st_mode & 0077);
        if (db_fd >= 0)
            close(db_fd);
        if (!safe) {
            ah_close(e);
            return ah_fail(err, AH_IO, "database must be an owner-only regular file");
        }
    }
    if (sqlite3_open_v2(config->database_path, &e->db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                        NULL) != SQLITE_OK) {
        ah_close(e);
        return ah_fail(err, AH_IO, "cannot open sqlite database");
    }
    sqlite3_busy_timeout(e->db, 1000);
    ah_code rc = core_sql(
        e, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON", err);
    sqlite3_stmt *s = NULL;
    int version = 0;
    if (rc == AH_OK)
        rc = core_prepare(e, "PRAGMA user_version", &s, err);
    if (rc == AH_OK) {
        if (sqlite3_step(s) != SQLITE_ROW)
            rc = ah_fail(err, AH_IO, "cannot read schema version");
        else {
            version = sqlite3_column_int(s, 0);
            if (version > 2)
                rc = ah_fail(err, AH_PROTOCOL, "database schema is newer than this runtime");
        }
    }
    sqlite3_finalize(s);
    if (rc == AH_OK)
        rc = core_sql(
            e,
            "BEGIN IMMEDIATE;"
            "CREATE TABLE IF NOT EXISTS sessions(id TEXT PRIMARY KEY,model TEXT NOT "
            "NULL,instructions TEXT NOT NULL,summary TEXT NOT NULL DEFAULT '',state INTEGER NOT "
            "NULL,turn INTEGER NOT NULL DEFAULT 0,model_calls INTEGER NOT NULL DEFAULT "
            "0,tool_calls INTEGER NOT NULL DEFAULT 0);"
            "CREATE TABLE IF NOT EXISTS events(seq INTEGER PRIMARY KEY AUTOINCREMENT,session_id "
            "TEXT NOT NULL REFERENCES sessions(id),operation_id TEXT,type TEXT NOT NULL,json TEXT "
            "NOT NULL);"
            "CREATE TABLE IF NOT EXISTS items(seq INTEGER PRIMARY KEY AUTOINCREMENT,session_id "
            "TEXT NOT NULL REFERENCES sessions(id),turn INTEGER NOT NULL,json TEXT NOT NULL,active "
            "INTEGER NOT NULL);"
            "CREATE INDEX IF NOT EXISTS items_session ON items(session_id,seq);"
            "CREATE TABLE IF NOT EXISTS commands(session_id TEXT NOT NULL REFERENCES "
            "sessions(id),id TEXT NOT NULL,text TEXT NOT NULL,PRIMARY KEY(session_id,id));"
            "CREATE TABLE IF NOT EXISTS operations(seq INTEGER PRIMARY KEY AUTOINCREMENT,id TEXT "
            "NOT NULL UNIQUE,session_id TEXT NOT NULL REFERENCES sessions(id),call_id TEXT NOT "
            "NULL,name TEXT NOT NULL,arguments TEXT NOT NULL,schema TEXT,effect INTEGER NOT "
            "NULL,approval INTEGER NOT NULL,status TEXT NOT NULL,result TEXT,is_error "
            "INTEGER,decision INTEGER NOT NULL DEFAULT -1,UNIQUE(session_id,call_id));"
            "CREATE TABLE IF NOT EXISTS call_ids(session_id TEXT NOT NULL REFERENCES "
            "sessions(id),call_id TEXT NOT NULL,PRIMARY KEY(session_id,call_id));"
            "INSERT OR IGNORE INTO call_ids(session_id,call_id) SELECT session_id,call_id FROM "
            "operations;"
            "CREATE INDEX IF NOT EXISTS operations_session ON operations(session_id,seq);"
            "CREATE TABLE IF NOT EXISTS result_refs(session_id TEXT NOT NULL REFERENCES "
            "sessions(id),operation_id TEXT NOT NULL REFERENCES operations(id),"
            "PRIMARY KEY(session_id,operation_id));",
            err);
    if (rc == AH_OK && version < 2)
        rc = core_migrate_result_refs(e, err);
    if (rc == AH_OK)
        rc = core_sql(e, "PRAGMA user_version=2; COMMIT", err);
    if (rc != AH_OK)
        core_rollback(e);
    if (rc == AH_OK)
        rc = core_recover(e, err);
    if (rc != AH_OK) {
        ah_close(e);
        return rc;
    }
    *out = e;
    ah_error_clear(err);
    return AH_OK;
}
void ah_close(ah_engine *e) {
    if (!e)
        return;
    core_mailbox_free(e);
    if (e->db)
        sqlite3_close(e->db);
    if (e->lock_fd >= 0) {
        flock(e->lock_fd, LOCK_UN);
        close(e->lock_fd);
    }
    for (size_t i = 0; i < e->tool_count; i++) {
        free(e->tools[i].name);
        free(e->tools[i].description);
        free(e->tools[i].schema);
    }
    free(e->tools);
    free(e);
}
ah_code ah_register_tool(ah_engine *e, const ah_tool *tool, ah_error *err) {
    ah_code rc = core_guard(e, err);
    if (rc != AH_OK)
        return rc;
    if (!tool || !tool->name || !*tool->name || strlen(tool->name) > 64 || !tool->description ||
        !tool->parameters_json || !tool->execute || tool->effect < AH_EFFECT_READ ||
        tool->effect > AH_EFFECT_EXTERNAL)
        return ah_fail(err, AH_INVALID, "invalid tool definition");
    for (const unsigned char *p = (const unsigned char *)tool->name; *p; p++)
        if (!isalnum(*p) && *p != '_')
            return ah_fail(err, AH_INVALID, "tool names must be alphanumeric or underscore");
    if (isdigit((unsigned char)tool->name[0]) || !strcmp(tool->name, "ask") ||
        !strcmp(tool->name, "wait") || !strcmp(tool->name, "read_result") ||
        !strcmp(tool->name, "harness_ask") || !strcmp(tool->name, "harness_wait") ||
        core_find_tool(e, tool->name))
        return ah_fail(err, AH_INVALID, "reserved or duplicate tool name");
    rc = ah_schema_check(tool->parameters_json, err);
    if (rc != AH_OK)
        return rc;
    yyjson_doc *d = yyjson_read(tool->parameters_json, strlen(tool->parameters_json), 0);
    bool valid = d && yyjson_is_obj(yyjson_doc_get_root(d));
    yyjson_doc_free(d);
    if (!valid)
        return ah_fail(err, AH_INVALID, "tool schema must be a JSON object");
    core_tool copied = {0};
    copied.name = ah_strdup(tool->name);
    copied.description = ah_strdup(tool->description);
    copied.schema = ah_strdup(tool->parameters_json);
    if (!copied.name || !copied.description || !copied.schema) {
        free(copied.name);
        free(copied.description);
        free(copied.schema);
        return ah_fail(err, AH_IO, "out of memory");
    }
    core_tool *resized = realloc(e->tools, (e->tool_count + 1) * sizeof(*resized));
    if (!resized) {
        free(copied.name);
        free(copied.description);
        free(copied.schema);
        return ah_fail(err, AH_IO, "out of memory");
    }
    e->tools = resized;
    copied.tool = *tool;
    copied.tool.name = copied.name;
    copied.tool.description = copied.description;
    copied.tool.parameters_json = copied.schema;
    e->tools[e->tool_count++] = copied;
    return AH_OK;
}
core_tool *core_find_tool(ah_engine *e, const char *name) {
    for (size_t i = 0; i < e->tool_count; i++)
        if (!strcmp(e->tools[i].name, name))
            return &e->tools[i];
    return NULL;
}
void ah_cancel(ah_engine *e) {
    if (e)
        atomic_store_explicit(&e->cancelled, true, memory_order_relaxed);
}
bool core_cancelled(void *context) {
    ah_engine *e = context;
    return atomic_load_explicit(&e->cancelled, memory_order_relaxed) ||
           (e->deadline && ah_now_ms() >= e->deadline);
}
