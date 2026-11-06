#include "workflow_fixture.h"
#include <stdio.h>

static void apply_cases(workflow *w) {
    wf_reset_changes(w);
    char *args = wf_apply_args(WF_OTHER_REV, false, NULL);
    wf_result r = wf_call(w, ah_local_apply, args, 0);
    assert(!r.failed && !r.unknown && wf_field(r.root, "application", "complete"));
    assert(w->phase_count == 4 && w->applied_count == 2);
    wf_expect_file(w, "a.txt", "X\nB\nZ\nD\n");
    wf_expect_file(w, "b.txt", "LEFT\nRIGHT\n");
    char path[PATH_MAX];
    wf_path(w, "a.txt", path);
    struct stat st;
    assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0640);
    wf_result_free(&r);
    free(args);
    wf_reset_changes(w);
    r = wf_call(w, ah_local_apply,
                "{\"files\":[{\"path\":\"a.txt\",\"base_revision\":\"" WF_REV
                "\",\"edits\":[{\"old_text\":\"A\",\"new_text\":\"A\"}]}]}",
                0);
    assert(!r.failed && !r.unknown && w->phase_count == 0);
    assert(wf_field(yyjson_arr_get(yyjson_obj_get(r.root, "files"), 0), "status", "unchanged"));
    assert(yyjson_get_uint(yyjson_obj_get(r.root, "applied_files")) == 0);
    wf_expect_file(w, "a.txt", WF_BASE);
    wf_result_free(&r);
    wf_reset_changes(w);
    args = wf_apply_args("0000000000000000000000000000000000000000000000000000000000000000", false,
                         NULL);
    r = wf_call(w, ah_local_apply, args, 0);
    assert(r.failed && !r.unknown && w->phase_count == 0);
    assert(wf_field(r.root, "application", "not_applied") &&
           wf_field(r.root, "checks_status", "not_run"));
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
        wf_reset_changes(w);
        w->fail_phase = phases[i];
        w->fail_target = targets[i];
        wf_result r = wf_call(w, ah_local_apply, args, 0);
        assert(r.failed && r.unknown == (i == 1));
        assert(r.code == (i == 1 ? AH_UNCERTAIN : AH_OK));
        assert(wf_field(r.root, "application", applications[i]) &&
               wf_field(r.root, "checks_status", "not_run"));
        assert(yyjson_get_uint(yyjson_obj_get(r.root, "applied_files")) == (i ? 1 : 0));
        wf_expect_file(w, "a.txt", i ? "X\nB\nZ\nD\n" : WF_BASE);
        wf_expect_file(w, "b.txt", WF_OTHER);
        wf_result_free(&r);
    }
    wf_reset_changes(w);
    w->mutate_phase = "file.started";
    w->mutate_target = "b.txt";
    w->mutation_file = "b.txt";
    w->mutation_text = "foreign\n";
    wf_result r = wf_call(w, ah_local_apply, args, 0);
    assert(r.failed && !r.unknown && wf_field(r.root, "application", "partial"));
    wf_expect_file(w, "a.txt", "X\nB\nZ\nD\n");
    wf_expect_file(w, "b.txt", "foreign\n");
    wf_result_free(&r);
    free(args);
    wf_reset_changes(w);
}

/* Build the expected bytes at known offsets independently of the matcher. */
static void batch_matches(workflow *w) {
    enum { size = 65536, count = 8 };
    const char *keys[count] = {"α0", "β1", "γ2", "δ3", "λ4", "φ5", "π6", "雪"};
    char *source = malloc(size + 1), *expected = malloc(size + 1);
    assert(source && expected);
    for (unsigned mode = 0; mode < 4; ++mode) {
        memset(source, 'x', size);
        memset(expected, 'x', size);
        source[size] = expected[size] = 0;
        char args[1024] = "{\"files\":[{\"path\":\"batch\",\"edits\":[";
        size_t used = strlen(args);
        for (unsigned i = 0; i < count; ++i) {
            const char *needle = keys[i];
            size_t pos = (size - 3) * i / (count - 1);
            memcpy(source + pos, needle, 3);
            memcpy(expected + pos, "NEW", 3);
            if (i == count - 1) {
                if (mode == 1)
                    needle = "absent";
                if (mode == 2)
                    memcpy(source + 4, needle, 3);
                if (mode == 3) {
                    memcpy(source + pos - 1, "QQQQ", 4);
                    needle = "QQQ"; /* two overlapping occurrences */
                }
            }
            int n = snprintf(args + used, sizeof(args) - used,
                             "%s{\"old_text\":\"%s\",\"new_text\":\"NEW\"}", i ? "," : "", needle);
            assert(n > 0 && (size_t)n < sizeof(args) - used);
            used += (size_t)n;
        }
        strcpy(args + used, "]}]}");
        wf_put(w, "batch", source);
        wf_result r = wf_call(w, ah_local_apply, args, 0);
        assert(r.code == AH_OK && !r.unknown && r.failed == (mode != 0));
        wf_expect_file(w, "batch", mode ? source : expected);
        wf_result_free(&r);
    }
    free(expected);
    free(source);
}

int main(int argc, char **argv) {
    (void)argc;
    workflow w;
    wf_open(&w, argv[0]);
    apply_cases(&w);
    checkpoint_cases(&w);
    batch_matches(&w);
    wf_close(&w);
    return 0;
}
