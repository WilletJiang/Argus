#ifndef ARGUS_SEARCH_CORE_H
#define ARGUS_SEARCH_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ArgusIndex ArgusIndex;
typedef struct ArgusSearch ArgusSearch;
typedef int (*ArgusPathPredicate)(const char *path, void *context);

typedef struct {
    char path[4096];
    char name[512];
    char snippet[512];
    int kind;
    double score;
    int content_match;
} ArgusHit;

/* Presentation strings remain valid until the next read or search close. */
typedef struct {
    const char *path, *name, *snippet;
    int kind;
    double score;
    int content_match;
} ArgusResult;

/* Reuse one read snapshot and candidate/ranking state across progressive stages. */
ArgusSearch *argus_search_open(ArgusIndex *index, const char *query, int kind_filter, int limit);
int argus_search_metadata(ArgusSearch *search);
int argus_search_complete(ArgusSearch *search);
int argus_search_finished(ArgusSearch *search);
int argus_search_read(ArgusSearch *search, int ordinal, ArgusResult *result);
void argus_search_close(ArgusSearch *search);
/* Run on the writer/reader queues respectively; old body indexes remain searchable until commit. */
int argus_index_upgrade(ArgusIndex *index);
/* 1: ready or not needed, 0: unavailable (SQL remains usable), -1: interrupted.
   The optional metadata snapshot bounds its builder and file to 64 MiB each. */
int argus_index_prepare_snapshot(ArgusIndex *index);

typedef struct {
    char path[4096];
    int64_t id, size;
    int mode, applications_only;
} ArgusScanItem;
/* Filename jobs spill to SQLite; pending bodies are paged from their existing metadata. */
int argus_index_queue_scan(ArgusIndex *index, const char *path, int mode, int applications_only);
int argus_index_next_scan(ArgusIndex *index, ArgusScanItem *item);
int argus_index_next_content(ArgusIndex *index, int64_t after_id, ArgusScanItem *item);
int argus_index_clear_scan_jobs(ArgusIndex *index);

/* Use each handle on one serial queue. Separate reader/writer handles use WAL.
   Mutators return 1 on success and 0 on failure. Errors survive until next call.
   open(NULL) fails; error(NULL) describes the last open failure on this thread. */
ArgusIndex *argus_index_open(const char *database_path);
void argus_index_close(ArgusIndex *index);
const char *argus_index_error(ArgusIndex *index);
int argus_index_begin(ArgusIndex *index);
int argus_index_commit(ArgusIndex *index);
int argus_index_rollback(ArgusIndex *index);
int argus_index_upsert(ArgusIndex *index, const char *path, const char *name,
                       const char *aliases, int kind, double mtime, int64_t size);
int argus_index_upsert_scanned(ArgusIndex *index, const char *path, const char *name,
                              const char *aliases, int kind, double mtime, int64_t size,
                              int content_eligible);
int argus_index_set_content(ArgusIndex *index, const char *path, const char *text);
/* Content is truncated at 32 KiB without splitting UTF-8. */
int argus_index_clear_content(ArgusIndex *index);
/* Reclaims deleted text and index pages. Run outside a transaction while idle. */
int argus_index_compact(ArgusIndex *index);
int argus_index_remove(ArgusIndex *index, const char *path);
int argus_index_remove_tree(ArgusIndex *index, const char *path);
/* Removes absent filesystem entries; access errors preserve the entry. */
int argus_index_prune_missing(ArgusIndex *index);
/* Predicate runs synchronously. Its path is valid only for that callback. */
int argus_index_prune_excluded(ArgusIndex *index, ArgusPathPredicate predicate, void *context);
/* Query checks return -1 on error. Content is invalidated when metadata changes.
   set_content records the row's current mtime, including an empty extraction. */
int argus_index_needs_update(ArgusIndex *index, const char *path,
                             double mtime, int64_t size);
int argus_index_content_needed(ArgusIndex *index, const char *path, double mtime);
int64_t argus_index_count(ArgusIndex *index);
int64_t argus_index_content_bytes(ArgusIndex *index);
/* filter -1: all, 0: files/folders, 2: applications. At most 256 hits.
   Search returns hit count, or -1 on error. Empty/symbol-only queries return 0.
   Queries accept fewer than 2048 UTF-8 bytes and at most 24 terms.
   UTF-8 results are NUL-terminated without splitting a code point. */
int argus_index_search(ArgusIndex *index, const char *query, int kind_filter,
                       int limit, ArgusHit *out_hits);
/* Same query/filter/error rules; skips typo/subsequence expansion and document
   content. Metadata hits have content_match=0 and an empty snippet. */
int argus_index_search_metadata(ArgusIndex *index, const char *query, int kind_filter,
                                int limit, ArgusHit *out_hits);
/* Safe to call from another thread while a query is running. */
void argus_index_interrupt(ArgusIndex *index);

#ifdef __cplusplus
}
#endif
#endif
