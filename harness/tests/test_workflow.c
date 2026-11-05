#define _GNU_SOURCE 1
#include "workflow_fixture.h"
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>

static bool field(yyjson_val *root, const char *name, const char *value) {
    return yyjson_equals_str(yyjson_obj_get(root, name), value);
}
static void search_cases(workflow *w) {
    const char *text = "prefix\nneedle one\nneedle two\nsuffix\n\n\nneedle last\nend\n";
    wf_put(w, "a.txt", text);
    wf_put(w, "b.txt", "needle second file\n");
    wf_result r =
        wf_call(w, ah_local_find,
                "{\"path\":\".\",\"query\":\"needle\",\"context_lines\":1,\"max_results\":1}", 0);
    assert(r.code == AH_OK && !r.failed);
    assert(yyjson_get_uint(yyjson_obj_get(r.root, "total_excerpts")) == 3);
    yyjson_val *row = yyjson_arr_get(yyjson_obj_get(r.root, "results"), 0);
    assert(field(row, "path", "a.txt") &&
           field(row, "content", "prefix\nneedle one\nneedle two\nsuffix\n"));
    assert(
        field(row, "revision", "dd19234826ee0364deb0aa1cb5876cf052a015bf6d5ecf8980ec92c4082883c5"));
    assert(yyjson_get_uint(yyjson_obj_get(row, "start_line")) == 1 &&
           yyjson_get_uint(yyjson_obj_get(row, "end_line")) == 4);
    char snapshot[65];
    snprintf(snapshot, sizeof snapshot, "%s", yyjson_get_str(yyjson_obj_get(r.root, "snapshot")));
    wf_result_free(&r);
    char args[256];
    snprintf(args, sizeof args,
             "{\"path\":\".\",\"query\":\"needle\",\"context_lines\":1,\"max_results\":1,"
             "\"offset\":1,\"snapshot\":\"%s\"}",
             snapshot);
    r = wf_call(w, ah_local_find, args, 0);
    assert(!r.failed);
    row = yyjson_arr_get(yyjson_obj_get(r.root, "results"), 0);
    assert(field(row, "content", "\nneedle last\nend\n") &&
           yyjson_get_uint(yyjson_obj_get(row, "start_line")) == 6);
    wf_result_free(&r);
    char real_offset[256];
    snprintf(real_offset, sizeof real_offset,
             "{\"path\":\".\",\"query\":\"needle\",\"context_lines\":1,\"max_results\":1,"
             "\"offset\":1.0,\"snapshot\":\"%s\"}",
             snapshot);
    r = wf_call(w, ah_local_find, real_offset, 0);
    row = yyjson_arr_get(yyjson_obj_get(r.root, "results"), 0);
    if (!field(row, "content", "\nneedle last\nend\n"))
        fprintf(stderr, "BUG: schema-valid offset 1.0 did not select the same page as offset 1\n");
    assert(!r.failed && field(row, "content", "\nneedle last\nend\n"));
    wf_result_free(&r);
    wf_put(w, "b.txt", "different\n");
    r = wf_call(w, ah_local_find, args, 0);
    assert(r.failed && field(r.root, "error", "stale_snapshot"));
    assert(yyjson_arr_size(yyjson_obj_get(r.root, "results")) == 0);
    wf_result_free(&r);
    r = wf_call(w, ah_local_find, "{\"path\":\".\",\"query\":\"needle\",\"offset\":1}", 0);
    assert(r.failed);
    wf_result_free(&r);
    char link[PATH_MAX];
    wf_path(w, "link.txt", link);
    assert(symlink("a.txt", link) == 0);
    r = wf_call(w, ah_local_find, "{\"path\":\".\",\"query\":\"needle\",\"glob\":\"*.txt\"}", 0);
    assert(!r.failed && yyjson_get_uint(yyjson_obj_get(r.root, "skipped_files")) >= 1);
    size_t i, n;
    yyjson_val *item;
    yyjson_arr_foreach(yyjson_obj_get(r.root, "results"), i, n, item)
        assert(!field(item, "path", "link.txt"));
    wf_result_free(&r);
    r = wf_call(w, ah_local_find, "{\"path\":\"link.txt\",\"query\":\"needle\"}", 0);
    assert(r.failed);
    wf_result_free(&r);
    assert(unlink(link) == 0);
    wf_path(w, "linked_dir", link);
    assert(symlink(".", link) == 0);
    r = wf_call(w, ah_local_find, "{\"path\":\"linked_dir\",\"query\":\"needle\"}", 0);
    assert(r.failed);
    wf_result_free(&r);
    assert(unlink(link) == 0);
    wf_put(w, "a.txt", "abc");
    r = wf_call(w, ah_local_read_file, "{\"path\":\"a.txt\"}", 0);
    assert(field(r.root, "revision",
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    wf_result_free(&r);
    r = wf_call(
        w, ah_local_apply,
        "{\"files\":[{\"path\":\"a.txt\",\"edits\":[{\"old_text\":\"a\",\"new_text\":\"z\"}],"
        "\"base_revision\":\"0000000000000000000000000000000000000000000000000000000000000000\"}]}",
        0);
    assert(r.failed && field(yyjson_obj_get(r.root, "conflict"), "error", "conflict"));
    wf_result_free(&r);
    wf_expect_file(w, "a.txt", "abc");
}

static void bounded_excerpt(workflow *w) {
    char text[20001];
    memcpy(text, "needle ", 7);
    memset(text + 7, 'q', 19992);
    text[19999] = '\n';
    text[20000] = 0;
    wf_put(w, "long.txt", text);
    wf_result r = wf_call(w, ah_local_find, "{\"path\":\"long.txt\",\"query\":\"needle\"}", 4096);
    assert(!r.failed);
    yyjson_val *row = yyjson_arr_get(yyjson_obj_get(r.root, "results"), 0);
    const char *prefix = yyjson_get_str(yyjson_obj_get(row, "content"));
    size_t keep = strlen(prefix);
    assert(keep > 0 && keep < 20000 && !memcmp(prefix, text, keep));
    assert(yyjson_is_true(yyjson_obj_get(row, "truncated")));
    assert(yyjson_get_uint(yyjson_obj_get(row, "next_offset_bytes")) == keep);
    wf_result_free(&r);
    char args[128];
    snprintf(args, sizeof args, "{\"path\":\"long.txt\",\"offset_bytes\":%zu}", keep);
    r = wf_call(w, ah_local_read_file, args, 65536);
    assert(!r.failed);
    const char *tail = yyjson_get_str(yyjson_obj_get(r.root, "content"));
    assert(strlen(tail) == 20000 - keep && !memcmp(tail, text + keep, 20000 - keep));
    wf_result_free(&r);
}

static void reset_changes(workflow *w) {
    wf_put(w, "a.txt", WF_BASE);
    wf_put(w, "b.txt", WF_OTHER);
    w->phase_count = w->applied_count = 0;
    w->fail_phase = w->fail_target = NULL;
    w->mutate_phase = w->mutate_target = NULL;
}
static void apply_cases(workflow *w) {
    reset_changes(w);
    char *args = wf_apply_args(WF_OTHER_REV, false, NULL);
    wf_result r = wf_call(w, ah_local_apply, args, 0);
    assert(!r.failed && !r.unknown && field(r.root, "application", "complete"));
    assert(w->phase_count == 4 && w->applied_count == 2);
    wf_expect_file(w, "a.txt", "X\nB\nZ\nD\n");
    wf_expect_file(w, "b.txt", "LEFT\nRIGHT\n");
    char path[PATH_MAX];
    wf_path(w, "a.txt", path);
    struct stat st;
    assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0640);
    wf_result_free(&r);
    free(args);
    reset_changes(w);
    r = wf_call(w, ah_local_apply,
                "{\"files\":[{\"path\":\"a.txt\",\"base_revision\":\"" WF_REV
                "\",\"edits\":[{\"old_text\":\"A\",\"new_text\":\"A\"}]}]}",
                0);
    assert(!r.failed && !r.unknown && w->phase_count == 0);
    assert(field(yyjson_arr_get(yyjson_obj_get(r.root, "files"), 0), "status", "unchanged"));
    assert(yyjson_get_uint(yyjson_obj_get(r.root, "applied_files")) == 0);
    wf_expect_file(w, "a.txt", WF_BASE);
    wf_result_free(&r);
    reset_changes(w);
    args = wf_apply_args("0000000000000000000000000000000000000000000000000000000000000000", false,
                         NULL);
    r = wf_call(w, ah_local_apply, args, 0);
    assert(r.failed && !r.unknown && w->phase_count == 0);
    assert(field(r.root, "application", "not_applied") &&
           field(r.root, "checks_status", "not_run"));
    wf_expect_file(w, "a.txt", WF_BASE);
    wf_expect_file(w, "b.txt", WF_OTHER);
    wf_result_free(&r);
    free(args);
    args = wf_apply_args(NULL, true, NULL);
    r = wf_call(w, ah_local_apply, args, 0);
    assert(r.failed && !r.unknown && w->phase_count == 0);
    wf_expect_file(w, "a.txt", WF_BASE);
    wf_result_free(&r);
    free(args);
    args = wf_apply_args(NULL, false, "undefined_check");
    r = wf_call(w, ah_local_apply, args, 0);
    assert(r.failed && !r.unknown && w->phase_count == 0);
    wf_expect_file(w, "a.txt", WF_BASE);
    wf_result_free(&r);
    free(args);
}

static void checkpoint_cases(workflow *w) {
    const char *phases[] = {"file.started", "file.applied", "file.started"};
    const char *targets[] = {"a.txt", "a.txt", "b.txt"};
    const char *applications[] = {"not_applied", "unknown", "partial"};
    char *args = wf_apply_args(WF_OTHER_REV, false, NULL);
    for (unsigned i = 0; i < 3; i++) {
        reset_changes(w);
        w->fail_phase = phases[i];
        w->fail_target = targets[i];
        wf_result r = wf_call(w, ah_local_apply, args, 0);
        assert(r.failed && r.unknown == (i == 1));
        assert(r.code == (i == 1 ? AH_UNCERTAIN : AH_OK));
        assert(field(r.root, "application", applications[i]) &&
               field(r.root, "checks_status", "not_run"));
        assert(yyjson_get_uint(yyjson_obj_get(r.root, "applied_files")) == (i ? 1 : 0));
        wf_expect_file(w, "a.txt", i ? "X\nB\nZ\nD\n" : WF_BASE);
        wf_expect_file(w, "b.txt", WF_OTHER);
        wf_result_free(&r);
    }
    reset_changes(w);
    w->mutate_phase = "file.started";
    w->mutate_target = "b.txt";
    w->mutation_file = "b.txt";
    w->mutation_text = "foreign\n";
    wf_result r = wf_call(w, ah_local_apply, args, 0);
    assert(r.failed && !r.unknown && field(r.root, "application", "partial"));
    wf_expect_file(w, "a.txt", "X\nB\nZ\nD\n");
    wf_expect_file(w, "b.txt", "foreign\n");
    wf_result_free(&r);
    free(args);
    reset_changes(w);
}

static void check_cases(workflow *w) {
    wf_put(w, "input.txt", "good\n");
    wf_add_check(w, "quality", "pass", true, 2000);
    wf_add_check(w, "failure", "fail", false, 2000);
    wf_add_check(w, "mutation", "change", true, 2000);
    wf_add_check(w, "timeout", "hang", false, 150);
    wf_add_check(w, "checkpoint", "pass", true, 2000);
    unsigned before = wf_runs(w);
    wf_result r = wf_check(w, "quality");
    assert(!r.failed && field(r.root, "status", "pass") &&
           !yyjson_get_bool(yyjson_obj_get(r.root, "cached")));
    wf_result_free(&r);
    r = wf_check(w, "quality");
    assert(!r.failed && yyjson_get_bool(yyjson_obj_get(r.root, "cached")));
    wf_result_free(&r);
    assert(wf_runs(w) == before + 1);
    wf_put(w, "input.txt", "bad\n");
    r = wf_check(w, "quality");
    assert(r.failed && field(r.root, "status", "fail") && !r.unknown);
    wf_result_free(&r);
    assert(wf_runs(w) == before + 2);
    wf_put(w, "input.txt", "good\n");
    reset_changes(w);
    char *args = wf_apply_args(NULL, false, "failure");
    r = wf_call(w, ah_local_apply, args, 0);
    assert(r.failed && !r.unknown && field(r.root, "application", "complete"));
    wf_expect_file(w, "a.txt", "X\nB\nZ\nD\n");
    assert(yyjson_arr_size(yyjson_obj_get(r.root, "checks")) == 1);
    yyjson_val *failed_check = yyjson_arr_get(yyjson_obj_get(r.root, "checks"), 0);
    yyjson_val *excerpts = yyjson_obj_get(failed_check, "source_excerpts");
    assert(yyjson_arr_size(excerpts) == 1);
    assert(field(yyjson_arr_get(excerpts, 0), "path", "input.txt"));
    assert(field(yyjson_arr_get(excerpts, 0), "content", "good\n"));
    wf_result_free(&r);
    free(args);
    r = wf_check(w, "mutation");
    assert(r.failed && !r.unknown && field(r.root, "status", "stale"));
    wf_result_free(&r);
    wf_expect_file(w, "input.txt", "changed\n");
    wf_put(w, "input.txt", "good\n");
    r = wf_check(w, "timeout");
    assert(r.failed && r.unknown && r.code != AH_OK && field(r.root, "status", "unknown"));
    wf_result_free(&r);
    before = wf_runs(w);
    w->fail_phase = "check.started";
    w->fail_target = "checkpoint";
    r = wf_check(w, "checkpoint");
    assert(r.failed && !r.unknown && wf_runs(w) == before);
    wf_result_free(&r);
    w->fail_phase = "check.finished";
    r = wf_check(w, "checkpoint");
    assert(r.failed && r.unknown && field(r.root, "status", "unknown"));
    wf_result_free(&r);
    assert(wf_runs(w) == before + 1);
    w->fail_phase = NULL;
    r = wf_check(w, "checkpoint");
    if (yyjson_get_bool(yyjson_obj_get(r.root, "cached")))
        fprintf(stderr, "BUG: check.finished failure left a reusable cached pass\n");
    assert(!r.failed && !yyjson_get_bool(yyjson_obj_get(r.root, "cached")) &&
           wf_runs(w) == before + 2);
    wf_result_free(&r);
}

static void binary_scan_budget(const char *executable) {
    workflow w;
    wf_open(&w, executable);
    for (unsigned i = 0; i < 34; i++) {
        char leaf[32], path[PATH_MAX];
        snprintf(leaf, sizeof(leaf), "binary-%02u", i);
        wf_path(&w, leaf, path);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(fd >= 0);
        assert(ftruncate(fd, 1024 * 1024) == 0);
        assert(close(fd) == 0);
    }
    wf_result r = wf_call(&w, ah_local_find, "{\"path\":\".\",\"query\":\"needle\"}", 0);
    assert(r.code == AH_OK && !r.failed);
    assert(yyjson_is_true(yyjson_obj_get(r.root, "scan_truncated")));
    assert(yyjson_get_uint(yyjson_obj_get(r.root, "skipped_files")) == 32);
    wf_result_free(&r);
    wf_close(&w);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--check"))
        return wf_child(argc, argv);
    workflow w;
    wf_open(&w, argv[0]);
    search_cases(&w);
    bounded_excerpt(&w);
    apply_cases(&w);
    checkpoint_cases(&w);
    workflow engine;
    wf_open(&engine, argv[0]);
    wf_engine_case(&engine);
    wf_close(&engine);
    check_cases(&w);
    wf_close(&w);
    binary_scan_budget(argv[0]);
    puts("workflow: independent source, change, checkpoint, check and engine oracles passed");
    return 0;
}
