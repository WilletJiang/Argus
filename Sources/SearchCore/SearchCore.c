#include "SearchCore.h"

#include "SearchTokenizer.h"
#include "SearchQuery.h"
#include <sqlite3.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_HITS 256
#define MAX_BODY_BYTES 32768

struct ArgusIndex {
    sqlite3 *db;
    sqlite3_stmt *upsert;
    sqlite3_stmt *set_content;
    sqlite3_stmt *metadata;
    char error[512];
};

static _Thread_local char open_error[512];

static void copy_utf8(char *out, size_t capacity, const char *text) {
    if (!capacity) return;
    if (!text) text = "";
    size_t bytes = strlen(text);
    if (bytes >= capacity) {
        bytes = capacity - 1;
        while (bytes && ((unsigned char)text[bytes] & 0xc0) == 0x80) --bytes;
    }
    memcpy(out, text, bytes);
    out[bytes] = 0;
}

static int fail(ArgusIndex *index, const char *message) {
    copy_utf8(index ? index->error : open_error, 512, message);
    return 0;
}

static int sql_error(ArgusIndex *index) {
    return fail(index, sqlite3_errmsg(index->db));
}

static int execute(ArgusIndex *index, const char *sql) {
    if (sqlite3_exec(index->db, sql, NULL, NULL, NULL) != SQLITE_OK) return sql_error(index);
    return 1;
}

static int register_tokenizers(ArgusIndex *index) {
    if (argus_register_tokenizers(index->db) == SQLITE_OK) return 1;
    return fail(index, "Could not register SQLite FTS5 tokenizers");
}

static const char *schema =
    "CREATE TABLE IF NOT EXISTS entries("
    "id INTEGER PRIMARY KEY,path TEXT NOT NULL UNIQUE,name TEXT NOT NULL,aliases TEXT NOT NULL,"
    "kind INTEGER NOT NULL,mtime REAL NOT NULL,size INTEGER NOT NULL,"
    "nname TEXT NOT NULL,npath TEXT NOT NULL,naliases TEXT NOT NULL,body TEXT,content_mtime REAL);"
    "CREATE INDEX IF NOT EXISTS entries_name ON entries(nname);"
    "CREATE INDEX IF NOT EXISTS entries_kind ON entries(kind);"
    "CREATE VIRTUAL TABLE IF NOT EXISTS meta USING fts5(nname,npath,naliases,"
    "content='entries',content_rowid='id',tokenize='argus_meta',detail=column,columnsize=0);"
    "CREATE VIRTUAL TABLE IF NOT EXISTS fulltext USING fts5(body,content='entries',"
    "content_rowid='id',tokenize='argus_body',columnsize=0);"
    "CREATE TABLE IF NOT EXISTS stats(id INTEGER PRIMARY KEY,content_bytes INTEGER NOT NULL);"
    "INSERT OR IGNORE INTO stats VALUES(1,0);"
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

static int prepare(ArgusIndex *index, const char *sql, sqlite3_stmt **out) {
    if (sqlite3_prepare_v2(index->db, sql, -1, out, NULL) != SQLITE_OK) return sql_error(index);
    return 1;
}

ArgusIndex *argus_index_open(const char *path) {
    open_error[0] = 0;
    if (!path || !*path) { fail(NULL, "Database path is empty"); return NULL; }
    ArgusIndex *index = calloc(1, sizeof(*index));
    if (!index) { fail(NULL, "Out of memory"); return NULL; }
    if (sqlite3_open_v2(path, &index->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                       SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
        sql_error(index); goto failed;
    }
    sqlite3_busy_timeout(index->db, 3000);
    sqlite3_limit(index->db, SQLITE_LIMIT_LENGTH, 1024 * 1024);
    if (!register_tokenizers(index) ||
        !execute(index, "PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;"
                        "PRAGMA cache_size=-2048;PRAGMA mmap_size=8388608;"
                        "PRAGMA temp_store=MEMORY;PRAGMA wal_autocheckpoint=512;") ||
        !execute(index, schema)) goto failed;
    if (!prepare(index,
        "INSERT INTO entries(path,name,aliases,kind,mtime,size,nname,npath,naliases)"
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9) ON CONFLICT(path) DO UPDATE SET "
        "name=excluded.name,aliases=excluded.aliases,kind=excluded.kind,"
        "nname=excluded.nname,npath=excluded.npath,naliases=excluded.naliases,"
        "body=CASE WHEN entries.mtime=excluded.mtime AND entries.size=excluded.size THEN entries.body END,"
        "content_mtime=CASE WHEN entries.mtime=excluded.mtime AND entries.size=excluded.size THEN entries.content_mtime END,"
        "mtime=excluded.mtime,size=excluded.size", &index->upsert) ||
        !prepare(index, "UPDATE entries SET body=?2,content_mtime=mtime WHERE path=?1", &index->set_content) ||
        !prepare(index, "SELECT mtime,size,content_mtime FROM entries WHERE path=?1", &index->metadata)) goto failed;
    return index;
failed:
    copy_utf8(open_error, sizeof(open_error), index->error);
    argus_index_close(index);
    return NULL;
}

void argus_index_close(ArgusIndex *index) {
    if (!index) return;
    sqlite3_finalize(index->upsert);
    sqlite3_finalize(index->set_content);
    sqlite3_finalize(index->metadata);
    if (index->db) sqlite3_close_v2(index->db);
    free(index);
}

const char *argus_index_error(ArgusIndex *index) { return index ? index->error : open_error; }
int argus_index_begin(ArgusIndex *index) {
    if (!index) return 0;
    index->error[0] = 0;
    return execute(index, "BEGIN IMMEDIATE");
}
int argus_index_commit(ArgusIndex *index) {
    if (!index) return 0;
    index->error[0] = 0;
    return execute(index, "COMMIT");
}
int argus_index_rollback(ArgusIndex *index) {
    if (!index) return 0;
    index->error[0] = 0;
    return execute(index, "ROLLBACK");
}

static int finish_mutation(ArgusIndex *index, sqlite3_stmt *statement) {
    int result = sqlite3_step(statement);
    int ok = result == SQLITE_DONE;
    if (!ok) sql_error(index);
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    return ok;
}

int argus_index_upsert(ArgusIndex *index, const char *path, const char *name,
                       const char *aliases, int kind, double mtime, int64_t size) {
    if (!index) return 0;
    index->error[0] = 0;
    if (!path || !*path || !name || kind < 0 || kind > 2 || !isfinite(mtime) || size < 0)
        return fail(index, "Invalid entry metadata");
    if (strlen(path) >= sizeof(((ArgusHit *)0)->path) || strlen(name) > 65536 ||
        (aliases && strlen(aliases) > 65536))
        return fail(index, "Entry metadata is too long");
    char *nname = argus_fold_text(name), *npath = argus_fold_text(path), *naliases = argus_fold_text(aliases);
    if (!nname || !npath || !naliases) {
        free(nname); free(npath); free(naliases);
        return fail(index, "Invalid UTF-8 or out of memory");
    }
    sqlite3_stmt *statement = index->upsert;
    sqlite3_bind_text(statement, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, aliases ? aliases : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 4, kind);
    sqlite3_bind_double(statement, 5, mtime);
    sqlite3_bind_int64(statement, 6, size);
    sqlite3_bind_text(statement, 7, nname, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 8, npath, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 9, naliases, -1, SQLITE_TRANSIENT);
    free(nname); free(npath); free(naliases);
    return finish_mutation(index, statement);
}

int argus_index_set_content(ArgusIndex *index, const char *path, const char *text) {
    if (!index) return 0;
    index->error[0] = 0;
    if (!path || !*path) return fail(index, "Content path is empty");
    if (!text) text = "";
    char bounded[MAX_BODY_BYTES + 1];
    if (strlen(text) > MAX_BODY_BYTES) {
        copy_utf8(bounded, sizeof(bounded), text);
        text = bounded;
    }
    char *valid = argus_fold_text(text);
    if (!valid) return fail(index, "Invalid UTF-8 or out of memory");
    free(valid);
    sqlite3_bind_text(index->set_content, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(index->set_content, 2, text, -1, SQLITE_TRANSIENT);
    if (!finish_mutation(index, index->set_content)) return 0;
    if (!sqlite3_changes(index->db)) return fail(index, "Content entry does not exist");
    return 1;
}

int argus_index_clear_content(ArgusIndex *index) {
    if (!index) return 0;
    index->error[0] = 0;
    return execute(index, "UPDATE entries SET body=NULL,content_mtime=NULL WHERE body IS NOT NULL");
}

int argus_index_compact(ArgusIndex *index) {
    if (!index) return 0;
    index->error[0] = 0;
    if (!sqlite3_get_autocommit(index->db)) return fail(index, "Compaction requires no open transaction");
    if (!execute(index, "INSERT INTO meta(meta) VALUES('optimize');"
                        "INSERT INTO fulltext(fulltext) VALUES('optimize');VACUUM;")) return 0;
    int result = sqlite3_wal_checkpoint_v2(index->db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
    if (result != SQLITE_OK) return sql_error(index);
    return 1;
}

int argus_index_remove(ArgusIndex *index, const char *path) {
    if (!index) return 0;
    index->error[0] = 0;
    if (!path) return fail(index, "Removal path is missing");
    sqlite3_stmt *statement;
    if (!prepare(index, "DELETE FROM entries WHERE path=?1", &statement)) return 0;
    sqlite3_bind_text(statement, 1, path, -1, SQLITE_TRANSIENT);
    int ok = finish_mutation(index, statement);
    sqlite3_finalize(statement);
    return ok;
}

int argus_index_remove_tree(ArgusIndex *index, const char *path) {
    if (!index) return 0;
    index->error[0] = 0;
    if (!path || !*path) return fail(index, "Removal path is empty");
    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/') --length;
    char *prefix = malloc(length + 2), *upper = malloc(length + 2);
    if (!prefix || !upper) { free(prefix); free(upper); return fail(index, "Out of memory"); }
    memcpy(prefix, path, length);
    prefix[length] = '/'; prefix[length + 1] = 0;
    if (length == 1 && path[0] == '/') prefix[1] = 0;
    strcpy(upper, prefix); upper[strlen(upper) - 1] = '0';
    sqlite3_stmt *statement;
    int ok = prepare(index, "DELETE FROM entries WHERE path=?1 OR (path>=?2 AND path<?3)", &statement);
    if (ok) {
        sqlite3_bind_text(statement, 1, path, (int)length, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, prefix, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, upper, -1, SQLITE_TRANSIENT);
        ok = finish_mutation(index, statement);
        sqlite3_finalize(statement);
    }
    free(prefix); free(upper);
    return ok;
}

static int prune_matching(ArgusIndex *index, ArgusPathPredicate predicate, void *context) {
    if (!index) return 0;
    index->error[0] = 0;
    if (!predicate) return fail(index, "Pruning predicate is missing");
    sqlite3_stmt *rows, *remove;
    if (!prepare(index, "SELECT id,path FROM entries", &rows)) return 0;
    if (!prepare(index, "DELETE FROM entries WHERE id=?1", &remove)) {
        sqlite3_finalize(rows); return 0;
    }
    int owned_transaction = sqlite3_get_autocommit(index->db);
    if (owned_transaction && !execute(index, "BEGIN IMMEDIATE")) {
        sqlite3_finalize(rows); sqlite3_finalize(remove); return 0;
    }
    int result, ok = 1;
    while ((result = sqlite3_step(rows)) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(rows, 1);
        if (!predicate(path, context)) continue;
        sqlite3_bind_int64(remove, 1, sqlite3_column_int64(rows, 0));
        if (!finish_mutation(index, remove)) { ok = 0; break; }
    }
    if (ok && result != SQLITE_DONE) { sql_error(index); ok = 0; }
    sqlite3_finalize(rows); sqlite3_finalize(remove);
    if (owned_transaction) {
        if (ok) ok = execute(index, "COMMIT");
        else sqlite3_exec(index->db, "ROLLBACK", NULL, NULL, NULL);
    }
    return ok;
}

static int path_is_missing(const char *path, void *context) {
    (void)context;
    struct stat status;
    return lstat(path, &status) != 0 && (errno == ENOENT || errno == ENOTDIR);
}

int argus_index_prune_missing(ArgusIndex *index) {
    return prune_matching(index, path_is_missing, NULL);
}

int argus_index_prune_excluded(ArgusIndex *index, ArgusPathPredicate predicate, void *context) {
    return prune_matching(index, predicate, context);
}

static int metadata_check(ArgusIndex *index, const char *path, double mtime, int64_t size, int content) {
    if (!index) return -1;
    index->error[0] = 0;
    if (!path || !isfinite(mtime)) { fail(index, "Invalid metadata check"); return -1; }
    sqlite3_bind_text(index->metadata, 1, path, -1, SQLITE_TRANSIENT);
    int result = sqlite3_step(index->metadata);
    int needed = 1;
    if (result == SQLITE_ROW) {
        needed = sqlite3_column_double(index->metadata, 0) != mtime;
        if (content) needed |= sqlite3_column_type(index->metadata, 2) == SQLITE_NULL ||
                              sqlite3_column_double(index->metadata, 2) != mtime;
        else needed |= sqlite3_column_int64(index->metadata, 1) != size;
    } else if (result != SQLITE_DONE) { sql_error(index); needed = -1; }
    sqlite3_reset(index->metadata);
    sqlite3_clear_bindings(index->metadata);
    return needed;
}

int argus_index_needs_update(ArgusIndex *index, const char *path, double mtime, int64_t size) {
    return metadata_check(index, path, mtime, size, 0);
}
int argus_index_content_needed(ArgusIndex *index, const char *path, double mtime) {
    return metadata_check(index, path, mtime, 0, 1);
}

static int64_t scalar(ArgusIndex *index, const char *sql) {
    if (!index) return -1;
    index->error[0] = 0;
    sqlite3_stmt *statement;
    if (!prepare(index, sql, &statement)) return -1;
    int result = sqlite3_step(statement);
    int64_t value = -1;
    if (result == SQLITE_ROW) value = sqlite3_column_int64(statement, 0);
    else sql_error(index);
    sqlite3_finalize(statement);
    return value;
}
int64_t argus_index_count(ArgusIndex *index) { return scalar(index, "SELECT count(*) FROM entries"); }
int64_t argus_index_content_bytes(ArgusIndex *index) {
    return scalar(index, "SELECT content_bytes FROM stats WHERE id=1");
}
void argus_index_interrupt(ArgusIndex *index) { if (index) sqlite3_interrupt(index->db); }

static const char *column_text(sqlite3_stmt *statement, int column) {
    const unsigned char *text = sqlite3_column_text(statement, column);
    return text ? (const char *)text : "";
}

static void insert_hit(ArgusHit *hits, int *count, int limit, sqlite3_stmt *statement,
                        double score, int content_match, const char *snippet) {
    const char *path = column_text(statement, 0);
    for (int i = 0; i < *count; ++i) if (!strcmp(hits[i].path, path)) {
        if (content_match) {
            hits[i].content_match = 1;
            copy_utf8(hits[i].snippet, sizeof(hits[i].snippet), snippet);
        }
        return;
    }
    int position = 0;
    while (position < *count && (hits[position].score > score ||
        (hits[position].score == score && strcmp(hits[position].path, path) < 0))) ++position;
    if (position >= limit) return;
    int new_count = *count < limit ? *count + 1 : *count;
    memmove(hits + position + 1, hits + position,
            (size_t)(new_count - position - 1) * sizeof(*hits));
    ArgusHit *hit = hits + position;
    memset(hit, 0, sizeof(*hit));
    copy_utf8(hit->path, sizeof(hit->path), path);
    copy_utf8(hit->name, sizeof(hit->name), column_text(statement, 1));
    copy_utf8(hit->snippet, sizeof(hit->snippet), snippet);
    hit->kind = sqlite3_column_int(statement, 2);
    hit->score = score;
    hit->content_match = content_match;
    *count = new_count;
}

static int search_candidates(ArgusIndex *index, SearchQuery *query, const char *expression,
                              int filter, int limit, ArgusHit *hits, int *count, int fuzzy) {
    sqlite3_stmt *statement;
    if (!prepare(index,
        "SELECT e.path,e.name,e.kind,e.nname,e.npath,e.naliases FROM meta JOIN entries e ON e.id=meta.rowid "
        "WHERE meta MATCH ?1 AND (?2=-1 OR (?2=0 AND e.kind<>2) OR e.kind=?2) LIMIT 4096",
        &statement)) return 0;
    sqlite3_bind_text(statement, 1, expression, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, filter);
    int result;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        double score = argus_query_score(query, column_text(statement, 3), column_text(statement, 4),
                                      column_text(statement, 5), fuzzy);
        if (score > 0) insert_hit(hits, count, limit, statement, score, 0, "");
    }
    if (result != SQLITE_DONE) sql_error(index);
    sqlite3_finalize(statement);
    return result == SQLITE_DONE;
}

static int search_name_prefix(ArgusIndex *index, SearchQuery *query, const char *text,
                              int filter, int limit, ArgusHit *hits, int *count) {
    char *prefix = argus_fold_text(text);
    if (!prefix) return fail(index, "Invalid UTF-8 or out of memory");
    size_t bytes = strlen(prefix);
    char *upper = malloc(bytes + 2);
    if (!upper) { free(prefix); return fail(index, "Out of memory"); }
    memcpy(upper, prefix, bytes);
    upper[bytes] = (char)0xff; upper[bytes + 1] = 0;
    sqlite3_stmt *statement;
    int ok = prepare(index,
        "SELECT path,name,kind,nname,npath,naliases FROM entries WHERE nname>=?1 AND nname<?2 "
        "AND (?3=-1 OR (?3=0 AND kind<>2) OR kind=?3) ORDER BY nname LIMIT 512", &statement);
    if (ok) {
        sqlite3_bind_text(statement, 1, prefix, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, upper, (int)bytes + 1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 3, filter);
        int result;
        while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
            double score = argus_query_score(query, column_text(statement, 3), column_text(statement, 4),
                                          column_text(statement, 5), 0);
            if (score > 0) insert_hit(hits, count, limit, statement, score, 0, "");
        }
        ok = result == SQLITE_DONE;
        if (!ok) sql_error(index);
        sqlite3_finalize(statement);
    }
    free(prefix); free(upper);
    return ok;
}

static int search_single_character(ArgusIndex *index, SearchQuery *query, int filter,
                                    int limit, ArgusHit *hits, int *count) {
    sqlite3_stmt *statement;
    if (!prepare(index,
        "SELECT path,name,kind,nname,npath,naliases FROM entries WHERE "
        "(?2=-1 OR (?2=0 AND kind<>2) OR kind=?2) AND "
        "(instr(nname,?1)>0 OR instr(naliases,?1)>0 OR instr(npath,?1)>0) LIMIT 4096",
        &statement)) return 0;
    sqlite3_bind_text(statement, 1, query->terms[0], -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, filter);
    int result;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        double score = argus_query_score(query, column_text(statement, 3), column_text(statement, 4),
                                      column_text(statement, 5), 0);
        if (score > 0) insert_hit(hits, count, limit, statement, score, 0, "");
    }
    if (result != SQLITE_DONE) sql_error(index);
    sqlite3_finalize(statement);
    return result == SQLITE_DONE;
}

static int search_index(ArgusIndex *index, const char *text, int filter, int limit,
                         ArgusHit *hits, int full_search) {
    if (!index) return -1;
    index->error[0] = 0;
    if (!text || !hits || limit <= 0) return 0;
    if (strlen(text) >= ARGUS_QUERY_BYTES) { fail(index, "Search query is too long"); return -1; }
    if (filter != -1 && filter != 0 && filter != 2) { fail(index, "Invalid search filter"); return -1; }
    if (limit > MAX_HITS) limit = MAX_HITS;
    SearchQuery query;
    if (!argus_query_parse(&query, text)) {
        fail(index, "Query cannot be normalized or has more than 24 terms"); return -1;
    }
    if (!query.count) return 0;
    int count = 0;
    if (!search_name_prefix(index, &query, text, filter, limit, hits, &count)) return -1;
    char *metadata = argus_query_match(&query, 0, 0);
    if (!metadata) { fail(index, "Out of memory"); return -1; }
    int width;
    uint32_t first = argus_codepoint(query.terms[0], (int)strlen(query.terms[0]), &width);
    int single_character = query.count == 1 && (int)strlen(query.terms[0]) == width && !argus_cjk_character(first);
    int ok = single_character ? search_single_character(index, &query, filter, limit, hits, &count) :
                               search_candidates(index, &query, metadata, filter, limit, hits, &count, 0);
    sqlite3_free(metadata);
    if (!ok) return -1;
    if (!full_search) return count;
    if (count < limit) {
        char *fuzzy = argus_query_match(&query, 0, 1);
        if (!fuzzy) { fail(index, "Out of memory"); return -1; }
        ok = search_candidates(index, &query, fuzzy, filter, limit, hits, &count, 1);
        sqlite3_free(fuzzy);
        if (!ok) return -1;
    }
    if (count == limit && hits[count - 1].score > 300) return count;
    char *body = argus_query_match(&query, 1, 0);
    if (!body) { fail(index, "Out of memory"); return -1; }
    sqlite3_stmt *statement;
    if (!prepare(index,
        "SELECT e.path,e.name,e.kind,argus_excerpt(fulltext),e.body FROM fulltext "
        "JOIN entries e ON e.id=fulltext.rowid WHERE fulltext MATCH ?1 "
        "AND (?2=-1 OR (?2=0 AND e.kind<>2) OR e.kind=?2) LIMIT 512", &statement)) {
        sqlite3_free(body); return -1;
    }
    sqlite3_bind_text(statement, 1, body, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, filter);
    sqlite3_free(body);
    int result;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        int body_matches = argus_query_body_matches(&query, column_text(statement, 4));
        if (body_matches < 0) {
            fail(index, "Cannot normalize document text");
            sqlite3_finalize(statement); return -1;
        }
        if (!body_matches) continue;
        insert_hit(hits, &count, limit, statement, 300, 1, column_text(statement, 3));
        if (count == limit && hits[count - 1].score >= 300) {
            result = SQLITE_DONE;
            break;
        }
    }
    if (result != SQLITE_DONE) sql_error(index);
    sqlite3_finalize(statement);
    return result == SQLITE_DONE ? count : -1;
}

int argus_index_search(ArgusIndex *index, const char *text, int filter, int limit, ArgusHit *hits) {
    return search_index(index, text, filter, limit, hits, 1);
}

int argus_index_search_metadata(ArgusIndex *index, const char *text, int filter,
                                int limit, ArgusHit *hits) {
    return search_index(index, text, filter, limit, hits, 0);
}
