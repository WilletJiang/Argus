#ifndef ARGUS_SEARCH_INDEX_H
#define ARGUS_SEARCH_INDEX_H
#include "SearchCore.h"
#include "SearchQuery.h"
#include <sqlite3.h>
#include <stdatomic.h>
#include <stddef.h>
#define ARGUS_MAX_HITS 256

typedef struct ArgusSnapshot ArgusSnapshot;
typedef struct {
    sqlite3_int64 id;
    double score;
    char *path;
    int content_match;
    int position;
} ArgusRank;

struct ArgusIndex {
    sqlite3 *db;
    sqlite3_stmt *upsert, *set_content, *metadata;
    sqlite3_stmt *search_statements[9];
    sqlite3_stmt *scan_statements[4];
    uint64_t checked_generation;
    unsigned char checked_identity[16];
    int snapshot_checked;
    ArgusSnapshot *snapshot;
    atomic_uint_fast64_t interrupt_epoch;
    char error[512];
};

struct ArgusSearch {
    ArgusIndex *index;
    SearchQuery query;
    ArgusRank ranks[ARGUS_MAX_HITS];
    uint64_t epoch;
    int filter, limit, count, owns_transaction, positional, metadata_done, complete;
    char name[512], snippet[512];
};

void argus_copy_utf8(char *out, size_t capacity, const char *text);
const char *argus_column_text(sqlite3_stmt *statement, int column);
int argus_fail(ArgusIndex *index, const char *message);
int argus_sql_error(ArgusIndex *index);
int argus_schema_open(ArgusIndex *index);
int argus_execute(ArgusIndex *index, const char *sql);
int argus_prepare(ArgusIndex *index, const char *sql, sqlite3_stmt **out);
int argus_search_cancelled(ArgusSearch *search);
int argus_search_consider(ArgusSearch *search, sqlite3_int64 id, const char *path,
                         double score, int content_match, int position);
void argus_snapshot_close(ArgusSnapshot *snapshot);
int argus_snapshot_prepare(ArgusIndex *index);
int argus_snapshot_search(ArgusSearch *search);
#endif
