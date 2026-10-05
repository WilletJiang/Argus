#include "SearchCore.h"
#include <assert.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static ArgusHit hits[32];
typedef int (*SearchFunction)(ArgusIndex *, const char *, int, int, ArgusHit *);

static void check_line(int value, ArgusIndex *index, int line) {
    if (!value) { fprintf(stderr, "Index failure at %d: %s\n", line, argus_index_error(index)); abort(); }
}
#define check(value, index) check_line(value, index, __LINE__)

static int search_with(ArgusIndex *index, const char *query, int filter, SearchFunction function) {
    int count = function(index, query, filter, 32, hits);
    if (count < 0) { fprintf(stderr, "Search '%s': %s\n", query, argus_index_error(index)); abort(); }
    return count;
}
static int search(ArgusIndex *index, const char *query, int filter) {
    return search_with(index, query, filter, argus_index_search);
}
static int search_metadata(ArgusIndex *index, const char *query, int filter) {
    return search_with(index, query, filter, argus_index_search_metadata);
}
static void verify_search_inputs(ArgusIndex *index, SearchFunction function) {
    assert(function(NULL, "safari", -1, 32, hits) == -1);
    assert(function(index, NULL, -1, 32, hits) == 0);
    assert(function(index, "safari", -1, 32, NULL) == 0);
    assert(function(index, "safari", -1, 0, hits) == 0);
    assert(function(index, "", -1, 32, hits) == 0);
    assert(function(index, "'\"%*-(){}", -1, 32, hits) == 0);
    assert(function(index, "safari", 1, 32, hits) == -1);
    assert(function(index, "\xc0\xaf", -1, 32, hits) == -1);
    assert(function(index, "a b c d e f g h i j k l m n o p q r s t u v w x y", -1, 32, hits) == -1);
    char oversized[2049];
    memset(oversized, 'a', sizeof(oversized) - 1); oversized[sizeof(oversized) - 1] = 0;
    assert(function(index, oversized, -1, 32, hits) == -1);
    assert(function(index, "", -1, 32, hits) == 0 && !argus_index_error(index)[0]);
}
static int found(int count, const char *path) {
    for (int i = 0; i < count; ++i) if (!strcmp(hits[i].path, path)) return i + 1;
    return 0;
}
static double now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_sec + time.tv_nsec / 1e9;
}
static int compare_double(const void *a, const void *b) {
    double left = *(const double *)a, right = *(const double *)b;
    return left < right ? -1 : left > right;
}
static void delete_database(const char *path) {
    char sidecar[1024];
    unlink(path);
    snprintf(sidecar, sizeof(sidecar), "%s-wal", path); unlink(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-shm", path); unlink(sidecar);
}

typedef struct { const char *prefix; int visited; } PrunePolicy;
static int excluded_prefix(const char *path, void *context) {
    PrunePolicy *policy = context;
    ++policy->visited;
    return !strncmp(path, policy->prefix, strlen(policy->prefix));
}

typedef struct {
    ArgusIndex *index;
    SearchFunction search;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int ready;
    int result;
} InterruptProbe;

static void *interrupt_search(void *context) {
    InterruptProbe *probe = context;
    ArgusHit output[8];
    pthread_mutex_lock(&probe->mutex);
    probe->ready = 1;
    pthread_cond_signal(&probe->condition);
    pthread_mutex_unlock(&probe->mutex);
    probe->result = probe->search(probe->index, "z", -1, 8, output);
    return NULL;
}

static void verify_interrupt(ArgusIndex *reader, SearchFunction function, const char *label) {
    int interrupted = 0;
    for (int attempt = 0; attempt < 3 && !interrupted; ++attempt) {
        InterruptProbe probe = {.index = reader, .search = function};
        pthread_mutex_init(&probe.mutex, NULL);
        pthread_cond_init(&probe.condition, NULL);
        pthread_t thread;
        assert(pthread_create(&thread, NULL, interrupt_search, &probe) == 0);
        pthread_mutex_lock(&probe.mutex);
        while (!probe.ready) pthread_cond_wait(&probe.condition, &probe.mutex);
        pthread_mutex_unlock(&probe.mutex);
        struct timespec pause = {0, 500000};
        nanosleep(&pause, NULL);
        argus_index_interrupt(reader);
        pthread_join(thread, NULL);
        interrupted = probe.result == -1;
        pthread_cond_destroy(&probe.condition);
        pthread_mutex_destroy(&probe.mutex);
    }
    assert(interrupted);
    assert(search(reader, "project123", -1) == 32);
    assert(search_metadata(reader, "project123", -1) == 32);
    printf("%s reader cancellation and subsequent queries passed.\n", label);
}

int main(void) {
    printf("SQLite %s.\n", sqlite3_libversion());
    char path[] = "/tmp/argus-core-tests-XXXXXX";
    int descriptor = mkstemp(path);
    assert(descriptor >= 0); close(descriptor); unlink(path);
    ArgusIndex *index = argus_index_open(path);
    check(index != NULL, index);
    assert(argus_index_count(index) == 0);
    verify_search_inputs(index, argus_index_search);
    verify_search_inputs(index, argus_index_search_metadata);
    assert(argus_index_upsert(index, "/bad", "\xc0\xaf", "", 0, 1, 0) == 0);
    assert(argus_index_needs_update(index, "/one", 1, 10) == 1);
    check(argus_index_begin(index), index);
    check(argus_index_upsert(index, "/Applications/Safari.app", "Safari.app", "safari sf", 2, 1, 10), index);
    check(argus_index_upsert(index, "/Users/test/项目/会议纪要.txt", "会议纪要.txt", "huiyijiyao hyjy hui yi ji yao", 0, 2, 30), index);
    check(argus_index_upsert(index, "/Users/test/Reports/Café résumé.txt", "Café résumé.txt", "", 0, 3, 100), index);
    check(argus_index_upsert(index, "/Users/test/Reports", "Reports", "", 1, 1, 0), index);
    check(argus_index_upsert(index, "/Users/test/Reports-old/keep.txt", "keep.txt", "", 0, 1, 0), index);
    check(argus_index_commit(index), index);
    assert(argus_index_count(index) == 5);
    ArgusIndex *reader = argus_index_open(path); check(reader != NULL, reader);
    check(argus_index_begin(index), index);
    check(argus_index_upsert(index, "/WAL/visible.txt", "visible.txt", "", 0, 1, 10), index);
    assert(argus_index_count(index) == 6 && argus_index_count(reader) == 5);
    assert(search(reader, "visible", -1) == 0);
    check(argus_index_commit(index), index);
    assert(argus_index_count(reader) == 6 && search(reader, "visible", -1) == 1);
    check(argus_index_begin(index), index);
    check(argus_index_remove(index, "/WAL/visible.txt"), index);
    check(argus_index_rollback(index), index);
    assert(search(reader, "visible", -1) == 1);
    check(argus_index_remove(index, "/WAL/visible.txt"), index);
    assert(argus_index_count(reader) == 5);
    argus_index_close(reader);
    check(argus_index_upsert(index, "/ExcludedCache/a.txt", "CacheMarker.txt", "", 0, 1, 10), index);
    check(argus_index_upsert(index, "/ExcludedCache/b.txt", "CacheMarker.txt", "", 0, 1, 10), index);
    check(argus_index_upsert(index, "/ExcludedCache2/keep.txt", "CacheMarker.txt", "", 0, 1, 10), index);
    check(argus_index_set_content(index, "/ExcludedCache/a.txt", "orphanword"), index);
    PrunePolicy policy = {"/ExcludedCache/", 0};
    check(argus_index_prune_excluded(index, excluded_prefix, &policy), index);
    assert(policy.visited == 8 && argus_index_count(index) == 6);
    assert(search(index, "CacheMarker", -1) == 1 && search(index, "orphanword", -1) == 0);
    assert(argus_index_content_bytes(index) == 0);
    check(argus_index_remove(index, "/ExcludedCache2/keep.txt"), index);
    assert(search(index, "safari", 2) == 1);
    assert(!strcmp(hits[0].name, "Safari.app"));
    assert(search_metadata(index, "Safari.app", 2) == 1 && !strcmp(hits[0].name, "Safari.app"));
    assert(search_metadata(index, "safari", 0) == 0);
    assert(found(search_metadata(index, "saf", 2), "/Applications/Safari.app"));
    assert(found(search_metadata(index, "fari", 2), "/Applications/Safari.app"));
    assert(search_metadata(index, "safrai", 2) == 0);
    assert(search_metadata(index, "xafari", 2) == 0);
    assert(search_metadata(index, "sfr", 2) == 0);
    assert(search(index, "safari", 0) == 0);
    assert(found(search(index, "fari", -1), "/Applications/Safari.app"));
    assert(found(search(index, "safrai", 2), "/Applications/Safari.app"));
    assert(found(search(index, "xafari", 2), "/Applications/Safari.app"));
    assert(found(search(index, "a", 2), "/Applications/Safari.app"));
    assert(found(search(index, "sfr", 2), "/Applications/Safari.app"));
    check(argus_index_upsert(index, "/Applications/Visual Studio Code.app", "Visual Studio Code.app", "", 2, 1, 10), index);
    assert(search_metadata(index, "vsc", 2) == 0);
    assert(found(search(index, "vsc", 2), "/Applications/Visual Studio Code.app"));
    check(argus_index_remove(index, "/Applications/Visual Studio Code.app"), index);
    assert(found(search(index, "CAFÉ RESUME", -1), "/Users/test/Reports/Café résumé.txt"));
    assert(found(search(index, "reports cafe", -1), "/Users/test/Reports/Café résumé.txt"));
    assert(found(search(index, "会议", -1), "/Users/test/项目/会议纪要.txt"));
    assert(found(search(index, "纪要", -1), "/Users/test/项目/会议纪要.txt"));
    assert(found(search(index, "议", -1), "/Users/test/项目/会议纪要.txt"));
    assert(found(search(index, "hyjy", -1), "/Users/test/项目/会议纪要.txt"));
    assert(found(search(index, "hui yi", -1), "/Users/test/项目/会议纪要.txt"));
    assert(found(search_metadata(index, "CAFÉ RESUME", -1), "/Users/test/Reports/Café résumé.txt"));
    assert(found(search_metadata(index, "reports cafe", 0), "/Users/test/Reports/Café résumé.txt"));
    assert(found(search_metadata(index, "会议", 0), "/Users/test/项目/会议纪要.txt"));
    assert(found(search_metadata(index, "hyjy", 0), "/Users/test/项目/会议纪要.txt"));
    assert(found(search_metadata(index, "hui yi", 0), "/Users/test/项目/会议纪要.txt"));
    assert(found(search_metadata(index, "项目", 0), "/Users/test/项目/会议纪要.txt"));
    assert(search(index, "\"; DROP TABLE entries; --", -1) == 0);
    assert(search_metadata(index, "\"; DROP TABLE entries; --", -1) == 0);
    assert(argus_index_count(index) == 5);
    assert(argus_index_content_needed(index, "/Users/test/Reports/Café résumé.txt", 3) == 1);
    const char *body = "The orbital alignment is remarkable. 中文全文搜索测试。Café and résumé. ﾃｽﾄ";
    check(argus_index_set_content(index, "/Users/test/Reports/Café résumé.txt", body), index);
    assert(argus_index_content_bytes(index) == (int64_t)strlen(body));
    assert(argus_index_content_needed(index, "/Users/test/Reports/Café résumé.txt", 3) == 0);
    assert(search_metadata(index, "orbital align", 0) == 0);
    assert(search_metadata(index, "全文搜索", 0) == 0);
    int metadata_name = found(search_metadata(index, "cafe", 0), "/Users/test/Reports/Café résumé.txt");
    assert(metadata_name && !hits[metadata_name - 1].content_match && !hits[metadata_name - 1].snippet[0]);
    int count = search(index, "orbital align", 0);
    assert(count == 1 && hits[0].content_match && strstr(hits[0].snippet, "orbital"));
    assert(found(search(index, "全文搜索", 0), "/Users/test/Reports/Café résumé.txt"));
    assert(found(search(index, "索", 0), "/Users/test/Reports/Café résumé.txt"));
    assert(found(search(index, "テス", 0), "/Users/test/Reports/Café résumé.txt"));
    check(argus_index_upsert(index, "/CJK-split.txt", "Split.txt", "", 0, 1, 10), index);
    check(argus_index_set_content(index, "/CJK-split.txt", "中文 文全 全文"), index);
    assert(!found(search(index, "中文全文", 0), "/CJK-split.txt"));
    check(argus_index_remove(index, "/CJK-split.txt"), index);
    char late_body[32769];
    late_body[0] = 0;
    for (int i = 0; i < 4000; ++i) strcat(late_body, "filler ");
    strcat(late_body, "RésumÉ orbitalmarker 中文。");
    check(argus_index_upsert(index, "/Late-Excerpt.txt", "Late-Excerpt.txt", "", 0, 1, 10), index);
    check(argus_index_set_content(index, "/Late-Excerpt.txt", late_body), index);
    int late_match = found(search(index, "resume", 0), "/Late-Excerpt.txt");
    assert(late_match && strstr(hits[late_match - 1].snippet, "RésumÉ"));
    late_match = found(search(index, "中文", 0), "/Late-Excerpt.txt");
    assert(late_match && strstr(hits[late_match - 1].snippet, "中文"));
    check(argus_index_remove(index, "/Late-Excerpt.txt"), index);
    count = search(index, "cafe", 0);
    int matching_name = found(count, "/Users/test/Reports/Café résumé.txt");
    assert(matching_name && hits[matching_name - 1].content_match && hits[matching_name - 1].snippet[0]);
    check(argus_index_upsert(index, "/Users/test/Reports/Café résumé.txt", "Café résumé.txt", "", 0, 3, 100), index);
    assert(argus_index_content_needed(index, "/Users/test/Reports/Café résumé.txt", 3) == 0);
    argus_index_close(index);
    index = argus_index_open(path); check(index != NULL, index);
    assert(search(index, "orbital", -1) == 1);
    assert(argus_index_content_bytes(index) == (int64_t)strlen(body));
    check(argus_index_upsert(index, "/Users/test/Reports/Café résumé.txt", "Café résumé.txt", "", 0, 4, 100), index);
    assert(argus_index_content_bytes(index) == 0);
    assert(search(index, "orbital", -1) == 0);
    assert(argus_index_content_needed(index, "/Users/test/Reports/Café résumé.txt", 4) == 1);
    check(argus_index_set_content(index, "/Users/test/Reports/Café résumé.txt", ""), index);
    assert(argus_index_content_needed(index, "/Users/test/Reports/Café résumé.txt", 4) == 0);
    char large_body[40000];
    for (int i = 0; i < 39999; i += 3) memcpy(large_body + i, "中", 3);
    large_body[39999] = 0;
    check(argus_index_set_content(index, "/Users/test/Reports/Café résumé.txt", large_body), index);
    assert(argus_index_content_bytes(index) == 32766);
    assert(search(index, "中", 0) == 1 && hits[0].content_match);
    assert(strlen(hits[0].snippet) % 3 == 0);
    check(argus_index_clear_content(index), index);
    assert(argus_index_content_bytes(index) == 0);
    assert(argus_index_content_needed(index, "/Users/test/Reports/Café résumé.txt", 4) == 1);
    assert(search(index, "中", 0) == 0);
    check(argus_index_remove_tree(index, "/Users/test/Reports"), index);
    assert(argus_index_count(index) == 3);
    assert(search(index, "keep", -1) == 1);
    check(argus_index_begin(index), index);
    check(argus_index_remove(index, "/Applications/Safari.app"), index);
    check(argus_index_rollback(index), index);
    assert(search(index, "safari", -1) == 1);
    check(argus_index_remove(index, "/Applications/Safari.app"), index);
    assert(search(index, "safari", -1) == 0);
    check(argus_index_remove_tree(index, "/"), index);
    assert(argus_index_count(index) == 0);
    assert(argus_index_content_bytes(index) == 0);
    printf("Correctness checks passed.\n");

    check(argus_index_begin(index), index);
    double start = now();
    for (int i = 0; i < 100000; ++i) {
        char item_path[256], name[128];
        snprintf(name, sizeof(name), "Project%03d-report-%06d.txt", i % 997, i);
        snprintf(item_path, sizeof(item_path), "/Users/benchmark/Documents/Group%03d/%s", i % 107, name);
        check(argus_index_upsert(index, item_path, name, "", 0, i, 1024), index);
    }
    check(argus_index_commit(index), index);
    printf("Indexed 100,000 rows in %.2fs.\n", now() - start);
    assert(argus_index_count(index) == 100000);
    const char *queries[] = {"project123", "report 001234", "group017 project", "porject123", "report", "1234", "zzzzzz", "p", "proj report", "project999"};
    double durations[200];
    for (int mode = 0; mode < 2; ++mode) {
        SearchFunction function = mode ? argus_index_search_metadata : argus_index_search;
        for (int i = 0; i < 200; ++i) {
            start = now();
            search_with(index, queries[i % 10], -1, function);
            durations[i] = (now() - start) * 1000;
        }
        qsort(durations, 200, sizeof(double), compare_double);
        printf("%s mixed-query P50 %.2fms, P95 %.2fms, max %.2fms.\n",
               mode ? "Metadata" : "Full search", durations[99], durations[189], durations[199]);
    }
    reader = argus_index_open(path); check(reader != NULL, reader);
    verify_interrupt(reader, argus_index_search, "Full search");
    verify_interrupt(reader, argus_index_search_metadata, "Metadata");
    argus_index_close(reader);
    char document[32769];
    document[0] = 0;
    const char *sentence = "The orbital alignment supports lunar navigation and deep space research. ";
    while (strlen(document) + strlen(sentence) < sizeof(document)) strcat(document, sentence);
    check(argus_index_begin(index), index);
    for (int i = 0; i < 1000; ++i) {
        char body_path[256], name[128];
        snprintf(name, sizeof(name), "Document%04d.txt", i);
        snprintf(body_path, sizeof(body_path), "/Nonexistent/ArgusFullText/%s", name);
        check(argus_index_upsert(index, body_path, name, "", 0, 1, 32768), index);
        check(argus_index_set_content(index, body_path, document), index);
    }
    check(argus_index_commit(index), index);
    for (int i = 0; i < 50; ++i) {
        start = now();
        assert(search(index, i % 2 ? "orbital align" : "lunar navigation", 0) == 32);
        durations[i] = (now() - start) * 1000;
    }
    qsort(durations, 50, sizeof(double), compare_double);
    printf("Fulltext 1000 x32KiB docs P50 %.2fms, P95 %.2fms.\n", durations[24], durations[47]);
    argus_index_close(index);
    struct stat status;
    assert(stat(path, &status) == 0);
    printf("Database %.2f MiB (100k metadata +1000 x32KiB text).\n", status.st_size / 1048576.0);
    index = argus_index_open(path); check(index != NULL, index);
    check(argus_index_prune_missing(index), index);
    assert(argus_index_count(index) == 0);
    check(argus_index_compact(index), index);
    argus_index_close(index);
    struct stat compacted;
    assert(stat(path, &compacted) == 0 && compacted.st_size < status.st_size / 10);
    printf("Empty index compacts to %.2f MiB.\n", compacted.st_size / 1048576.0);
    delete_database(path);
    return 0;
}
