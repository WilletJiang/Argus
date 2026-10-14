#include "SearchIndex.h"
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static ArgusHit expected[48], actual[48];
static void ok(int value, ArgusIndex *index) {
    if (!value) { fprintf(stderr, "%s\n", argus_index_error(index)); abort(); }
}
static void sql(ArgusIndex *index, const char *text) { ok(argus_execute(index, text), index); }
static void put(ArgusIndex *index, const char *path, const char *body) {
    ok(argus_index_upsert(index, path, "Document.txt", "", 0, 1, 100), index);
    ok(argus_index_set_content(index, path, body), index);
}
static void cleanup(const char *path) {
    char sidecar[1024];
    unlink(path);
    const char *suffixes[] = {"-wal", "-shm", ".search"};
    for (int i = 0; i < 3; ++i) { snprintf(sidecar, sizeof(sidecar), "%s%s", path, suffixes[i]); unlink(sidecar); }
}

static void migration(ArgusIndex *writer, const char *path) {
    put(writer, "/body", "orbital Café 中文全文 中a a国 项目。");
    // Recreate the exact legacy tokenizer format; opening must leave it readable.
    sql(writer, "DROP TABLE fulltext; CREATE VIRTUAL TABLE fulltext USING fts5(body,content='entries',"
                "content_rowid='id',tokenize='argus_body',columnsize=0);"
                "INSERT INTO fulltext(fulltext) VALUES('rebuild'); UPDATE entries SET body_offsets=NULL;");
    ArgusIndex *reader = argus_index_open(path); ok(reader != NULL, reader);
    ArgusSearch *search = argus_search_open(reader, "中文全文", 0, 48);
    assert(search && argus_search_metadata(search) == 0);
    assert(!search->positional);
    // Inject a failure halfway through migration and prove rollback preserves v1.
    sql(writer, "CREATE TRIGGER fail_offsets BEFORE UPDATE OF body_offsets ON entries BEGIN "
                "SELECT RAISE(ABORT,'migration rollback test'); END;");
    assert(!argus_index_upgrade(writer));
    sql(writer, "DROP TRIGGER fail_offsets;");
    ok(argus_index_upgrade(writer), writer);
    assert(argus_search_complete(search) == 1);
    ArgusResult view;
    assert(argus_search_read(search, 0, &view) && strstr(view.snippet, "中文全文"));
    argus_search_close(search);
    search = argus_search_open(reader, "中文全文", 0, 48);
    assert(search && search->positional && argus_search_complete(search) == 1);
    argus_search_close(search);
    assert(argus_index_search(reader, "中a国", 0, 48, actual) == 0);
    // v1's unordered gram shortlist hid a later exact phrase behind 512 false candidates.
    ok(argus_index_begin(writer), writer);
    for (int i = 0; i < 700; ++i) {
        char item[80]; snprintf(item, sizeof(item), "/false/%04d", i);
        put(writer, item, "中文 文全 全文");
    }
    put(writer, "/last-phrase", "中文全文");
    ok(argus_index_commit(writer), writer);
    assert(argus_index_search(reader, "中文全文", 0, 48, actual) == 2);
    argus_index_close(reader);
    ok(argus_index_remove_tree(writer, "/"), writer);
}

static unsigned random_state = 83;
static unsigned next_random(void) { random_state = random_state * 1664525U + 1013904223U; return random_state >> 16; }
static void phrases(ArgusIndex *index) {
    const char *alphabet[] = {"中", "文", "全", "国", "a", "b", " ", "。"};
    for (int trial = 0; trial < 1800; ++trial) {
        char body[256] = "", query[48] = "";
        for (int j = 0; j < 32; ++j) strcat(body, alphabet[next_random() % 8]);
        strcat(query, alphabet[next_random() % 4]);
        for (int j = 0, n = next_random() % 6; j < n; ++j) strcat(query, alphabet[next_random() % 6]);
        put(index, "/0", body);
        int count = argus_index_search(index, query, 0, 48, actual);
        assert(count >= 0);
        // The old body predicate is the normalization/substring oracle.
        SearchQuery parsed; assert(argus_query_parse(&parsed, query));
        int oracle = argus_query_body_matches(&parsed, body);
        if ((count > 0) != oracle) {
            fprintf(stderr, "Phrase mismatch: '%s' in '%s' (%d versus %d)\n", query, body, count, oracle); abort();
        }
    }
    char late[32768] = "";
    for (int i = 0; i < 3800; ++i) strcat(late, "filler ");
    strcat(late, "Café résumé ﾃｽﾄ中文全文结束");
    put(index, "/0", late);
    const char *queries[] = {"cafe", "resume", "テス", "中文全文", "结束"};
    for (int i = 0; i < 5; ++i) {
        assert(argus_index_search(index, queries[i], 0, 48, actual) == 1);
        assert(strstr(actual[0].snippet, "Café résumé ﾃｽﾄ中文全文结束"));
    }
    ok(argus_index_remove_tree(index, "/"), index);
}

static void equivalent(ArgusIndex *index, const char *query, int filter) {
    // A caller-owned read transaction forces the SQL path as an independent reference.
    sql(index, "BEGIN");
    int count = argus_index_search(index, query, filter, 48, expected);
    ok(count >= 0, index);
    ok(argus_index_commit(index), index);
    int result = argus_index_search(index, query, filter, 48, actual);
    if (result != count || memcmp(expected, actual, (size_t)count * sizeof(*actual))) {
        fprintf(stderr, "Snapshot mismatch: '%s', filter %d (%d versus %d)\n", query, filter, count, result); abort();
    }
}

typedef struct { ArgusIndex *index; atomic_int ready; int result; } BuildProbe;
static void *build_interrupted(void *context) {
    BuildProbe *probe = context;
    atomic_store(&probe->ready, 1);
    probe->result = argus_index_prepare_snapshot(probe->index);
    return NULL;
}

static void snapshot(ArgusIndex *index, const char *path) {
    ok(argus_index_begin(index), index);
    for (int i = 0; i < 6000; ++i) {
        char item[256], name[100], aliases[100];
        snprintf(name, sizeof(name), "Project%03d-report-%06d.txt", i % 97, i);
        snprintf(item, sizeof(item), "/Users/test/Group%03d/%s", i % 107, name);
        snprintf(aliases, sizeof(aliases), "hyjy hui yi ji yao %s", i % 7 ? "safari sf" : "a b");
        ok(argus_index_upsert(index, item, name, aliases, i % 3, i, 100), index);
    }
    ok(argus_index_upsert(index, "/full／width/中文.app", "Ｃａｆｅ́项目.app", "pinyin xm", 2, 1, 10), index);
    ok(argus_index_commit(index), index);
    ok(argus_index_prepare_snapshot(index) == 1, index);
    assert(index->snapshot);
    const char *queries[] = {"project023", "group017 project", "report", "proj report", "porject023",
        "1234", "p", "a b", "s f", "hyjy", "hui yi", "项目", "cafe", "width", "zzzzzz", "a"};
    for (int filter = -1; filter <= 2; ++filter) if (filter != 1)
        for (int i = 0; i < 16; ++i) equivalent(index, queries[i], filter);
    for (int trial = 0; trial < 600; ++trial) {
        char query[80]; snprintf(query, sizeof(query), trial % 2 ? "group%03u project%03u" : "report %06u",
                                next_random() % 107, next_random() % 97);
        equivalent(index, query, -1);
    }
    ArgusSearch *search = argus_search_open(index, "report", -1, 48);
    assert(search);
    argus_index_interrupt(index);
    assert(argus_search_metadata(search) == -1);
    argus_search_close(search);
    equivalent(index, "report", -1);
    ok(argus_index_begin(index), index);
    ok(argus_index_upsert(index, "/new.app", "NewSearchMarker.app", "", 2, 1, 10), index);
    assert(argus_index_search(index, "newsearchmarker", 2, 48, actual) == 1);
    ok(argus_index_rollback(index), index);
    equivalent(index, "newsearchmarker", 2);
    ok(argus_index_upsert(index, "/new.app", "NewSearchMarker.app", "", 2, 1, 10), index);
    equivalent(index, "newsearchmarker", 2); // Stale generation must fall back.
    ok(argus_index_prepare_snapshot(index), index);
    argus_index_close(index);
    // Corrupt the persisted arena; digest validation must reject it without losing SQL results.
    char cache[1024]; snprintf(cache, sizeof(cache), "%s.search", path);
    int fd = open(cache, O_RDWR); assert(fd >= 0);
    unsigned char bad = 255; assert(pwrite(fd, &bad, 1, 400) == 1); close(fd);
    index = argus_index_open(path); ok(index != NULL, index);
    equivalent(index, "project023", -1);
    assert(!index->snapshot);
    ok(argus_index_prepare_snapshot(index), index);
    assert(index->snapshot);
    sql(index, "UPDATE stats SET metadata_id=randomblob(16) WHERE id=1");
    equivalent(index, "project023", -1); // A replaced database must not reuse a matching generation.
    assert(!index->snapshot);
    BuildProbe probe = {.index = index}; pthread_t thread;
    assert(pthread_create(&thread, NULL, build_interrupted, &probe) == 0);
    while (!atomic_load(&probe.ready)) usleep(50);
    usleep(500); argus_index_interrupt(index);
    pthread_join(thread, NULL);
    assert(probe.result == -1);
    ok(argus_index_prepare_snapshot(index), index);
    equivalent(index, "project023", -1);
    ok(argus_index_remove_tree(index, "/"), index);
    ok(argus_index_prepare_snapshot(index), index);
    assert(!index->snapshot); // Shrinking a corpus must release its old mapping even while idle.
    argus_index_close(index);
}

static void scan_queue(ArgusIndex *index) {
    ArgusScanItem item;
    ok(argus_index_queue_scan(index, "/one", 0, 1), index);
    ok(argus_index_queue_scan(index, "/one", 0, 1), index);
    ok(argus_index_queue_scan(index, "/two", 1, 0), index);
    assert(argus_index_next_scan(index, &item) == 1 && !strcmp(item.path, "/one") && item.applications_only);
    assert(argus_index_next_scan(index, &item) == 1 && !strcmp(item.path, "/two") && item.mode == 1);
    assert(argus_index_next_scan(index, &item) == 0);
    ok(argus_index_upsert_scanned(index, "/pending", "pending.txt", "", 0, 1, 100, 1), index);
    assert(argus_index_next_content(index, 0, &item) == 1);
    assert(argus_index_next_content(index, item.id, &item) == 0);
    ok(argus_index_set_content(index, "/pending", ""), index);
    assert(argus_index_next_content(index, 0, &item) == 0);
    ok(argus_index_upsert_scanned(index, "/pending", "pending.txt", "", 0, 2, 101, 1), index);
    assert(argus_index_next_content(index, 0, &item) == 1 && item.size == 101);
    ok(argus_index_remove_tree(index, "/"), index);
}

int main(void) {
    char path[] = "/tmp/argus-optimization-tests-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); close(fd); unlink(path);
    ArgusIndex *index = argus_index_open(path); ok(index != NULL, index);
    scan_queue(index); migration(index, path); phrases(index); snapshot(index, path);
    cleanup(path);
    puts("Optimization checks passed: migration rollback/concurrent reader, 1800 phrase oracles, late UTF-8 snippets, 648 snapshot equivalence cases, generation/identity invalidation, corruption, search/build cancellation, scan spool.");
    return 0;
}
