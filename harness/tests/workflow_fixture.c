#define _GNU_SOURCE 1
#include "workflow_fixture.h"
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>

void wf_path(workflow *w, const char *name, char path[PATH_MAX]) {
    assert(snprintf(path, PATH_MAX, "%s/%s", w->workspace, name) < PATH_MAX);
}
void wf_put(workflow *w, const char *name, const char *text) {
    char path[PATH_MAX];
    wf_path(w, name, path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0640);
    assert(fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    assert(close(fd) == 0);
}
char *wf_read(workflow *w, const char *name) {
    char path[PATH_MAX];
    wf_path(w, name, path);
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    assert(fd >= 0);
    struct stat st;
    assert(fstat(fd, &st) == 0);
    char *text = calloc((size_t)st.st_size + 1, 1);
    assert(text);
    assert(read(fd, text, (size_t)st.st_size) == st.st_size);
    assert(close(fd) == 0);
    return text;
}
void wf_expect_file(workflow *w, const char *name, const char *text) {
    char *actual = wf_read(w, name);
    assert(!strcmp(actual, text));
    free(actual);
}
static void remove_tree(const char *path) {
    DIR *dir = opendir(path);
    assert(dir);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char child[PATH_MAX];
        assert(snprintf(child, sizeof child, "%s/%s", path, entry->d_name) < (int)sizeof child);
        struct stat st;
        assert(lstat(child, &st) == 0);
        if (S_ISDIR(st.st_mode))
            remove_tree(child);
        else
            assert(unlink(child) == 0);
    }
    closedir(dir);
    assert(rmdir(path) == 0);
}
void wf_open(workflow *w, const char *program) {
    memset(w, 0, sizeof *w);
    char path[] = "/tmp/ah-workflow-XXXXXX";
    assert(mkdtemp(path));
    assert(realpath(path, w->workspace) && realpath(program, w->executable));
    ah_local_tools_config config = {.workspace = w->workspace, .enable_workflows = true};
    ah_error error;
    assert(ah_local_tools_open(&config, &w->tools, &error) == AH_OK);
}
void wf_reset_changes(workflow *w) {
    wf_put(w, "a.txt", WF_BASE);
    wf_put(w, "b.txt", WF_OTHER);
    w->phase_count = w->applied_count = 0;
    w->fail_phase = w->fail_target = NULL;
    w->mutate_phase = w->mutate_target = NULL;
}
void wf_close(workflow *w) {
    ah_local_tools_close(w->tools);
    remove_tree(w->workspace);
}
ah_code wf_checkpoint(void *context, const char *json, ah_error *error) {
    workflow *w = context;
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    assert(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *phase = yyjson_get_str(yyjson_obj_get(root, "phase"));
    const char *target = yyjson_get_str(yyjson_obj_get(root, "target"));
    assert(phase && target);
    w->phase_count++;
    if (!strcmp(phase, "file.applied"))
        w->applied_count++;
    if (w->mutate_phase && !strcmp(phase, w->mutate_phase) &&
        (!w->mutate_target || !strcmp(target, w->mutate_target))) {
        wf_put(w, w->mutation_file, w->mutation_text);
        w->mutate_phase = NULL;
    }
    bool fail = w->fail_phase && !strcmp(phase, w->fail_phase) &&
                (!w->fail_target || !strcmp(target, w->fail_target));
    yyjson_doc_free(doc);
    return fail ? ah_fail(error, AH_IO, "injected checkpoint failure") : AH_OK;
}
static ah_tool_call call_for(workflow *w, const char *args, size_t view) {
    return (ah_tool_call){.session_id = "workflow",
                          .operation_id = "operation",
                          .arguments_json = args,
                          .deadline_ms = ah_now_ms() + 5000,
                          .output_limit = 65536,
                          .model_output_limit = view,
                          .checkpoint = wf_checkpoint,
                          .checkpoint_context = w};
}
static wf_result receipt(ah_code code, ah_tool_result *result) {
    assert(result->json);
    wf_result r = {.code = code, .failed = result->is_error, .unknown = result->outcome_unknown};
    r.doc = yyjson_read(result->json, strlen(result->json), 0);
    assert(r.doc);
    r.root = yyjson_doc_get_root(r.doc);
    free(result->json);
    return r;
}
wf_result wf_call(workflow *w, ah_execute_fn execute, const char *args, size_t view) {
    ah_tool_call call = call_for(w, args, view);
    ah_tool_result result = {0};
    ah_error error = {0};
    ah_code code = execute(w->tools, &call, &result, &error);
    return receipt(code, &result);
}
wf_result wf_check(workflow *w, const char *name) {
    ah_tool_call call = call_for(w, "{\"argv\":[\"not-host-approved\"]}", 0);
    ah_tool_result result = {0};
    ah_error error = {0};
    ah_local_check *check = ah_local_check_find(w->tools, name);
    assert(check);
    return receipt(ah_local_check_run(w->tools, check, &call, &result, &error), &result);
}
void wf_result_free(wf_result *r) {
    yyjson_doc_free(r->doc);
    memset(r, 0, sizeof *r);
}
void wf_add_check(workflow *w, const char *name, const char *mode, bool cache, uint64_t timeout) {
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *argv = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, argv);
    yyjson_mut_arr_add_str(doc, argv, w->executable);
    yyjson_mut_arr_add_str(doc, argv, "--check");
    yyjson_mut_arr_add_str(doc, argv, mode);
    char *json = ah_mut_write(doc);
    assert(json);
    ah_check_config check = {.name = name,
                             .description = "Independent fixture check",
                             .argv_json = json,
                             .inputs_json = "[\"input.txt\"]",
                             .environment_revision = "fixture-v1",
                             .timeout_ms = timeout,
                             .cacheable = cache};
    ah_error error;
    assert(ah_local_tools_add_check(w->tools, &check, &error) == AH_OK);
    free(json);
    ah_mut_doc_free(doc);
}
unsigned wf_runs(workflow *w) {
    char path[PATH_MAX];
    wf_path(w, "runs.log", path);
    if (access(path, F_OK))
        return 0;
    char *text = wf_read(w, "runs.log");
    unsigned n = 0;
    for (char *p = text; *p; p++)
        if (*p == '\n')
            n++;
    free(text);
    return n;
}
int wf_child(int argc, char **argv) {
    assert(argc == 3);
    int fd = open("runs.log", O_WRONLY | O_APPEND | O_CREAT, 0600);
    assert(fd >= 0);
    assert(write(fd, "run\n", 4) == 4);
    close(fd);
    if (!strcmp(argv[2], "hang")) {
        signal(SIGTERM, SIG_IGN);
        for (;;)
            pause();
    }
    if (!strcmp(argv[2], "change")) {
        fd = open("input.txt", O_WRONLY | O_TRUNC);
        assert(fd >= 0);
        assert(write(fd, "changed\n", 8) == 8);
        close(fd);
        return 0;
    }
    if (!strcmp(argv[2], "fail")) {
        fputs("input.txt:1: failing check\n", stderr);
        fputs("input.txt:1: failing check\n", stdout);
        return 7;
    }
    assert(!strcmp(argv[2], "pass"));
    char bytes[16] = {0};
    fd = open("input.txt", O_RDONLY);
    assert(fd >= 0);
    ssize_t n = read(fd, bytes, sizeof bytes);
    close(fd);
    return n == 5 && !memcmp(bytes, "good\n", 5) ? 0 : 9;
}
char *wf_apply_args(const char *second_revision, bool overlap, const char *check) {
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_val *files = yyjson_mut_obj_add_arr(doc, root, "files");
    const char *paths[] = {"a.txt", "b.txt"}, *revs[] = {WF_REV, second_revision};
    for (unsigned i = 0; i < (second_revision ? 2u : 1u); i++) {
        yyjson_mut_val *file = yyjson_mut_obj(doc);
        yyjson_mut_arr_append(files, file);
        yyjson_mut_obj_add_str(doc, file, "path", paths[i]);
        yyjson_mut_obj_add_str(doc, file, "base_revision", revs[i]);
        yyjson_mut_val *edits = yyjson_mut_obj_add_arr(doc, file, "edits");
        const char *old[] = {i ? "left" : "C", i ? "right" : "A"},
                   *replacement[] = {i ? "LEFT" : "Z", i ? "RIGHT" : "X"};
        if (overlap) {
            old[0] = "A\nB";
            old[1] = "B\nC";
        }
        for (unsigned j = 0; j < 2; j++) {
            yyjson_mut_val *h = yyjson_mut_obj(doc);
            yyjson_mut_arr_append(edits, h);
            yyjson_mut_obj_add_str(doc, h, "old_text", old[j]);
            yyjson_mut_obj_add_str(doc, h, "new_text", replacement[j]);
        }
    }
    if (check) {
        yyjson_mut_val *names = yyjson_mut_obj_add_arr(doc, root, "checks");
        yyjson_mut_arr_add_str(doc, names, check);
    }
    char *result = ah_mut_write(doc);
    ah_mut_doc_free(doc);
    assert(result);
    return result;
}

typedef struct {
    workflow *workspace;
    char database[PATH_MAX];
    char *arguments;
    unsigned calls, checkpoints, initial_runs;
} engine_case;

static ah_code engine_model(void *context, const char *request, ah_delta_sink delta,
                            void *delta_context, ah_cancel_check cancelled, void *cancel_context,
                            char **response, ah_error *error) {
    (void)delta;
    (void)delta_context;
    (void)error;
    engine_case *f = context;
    assert(!cancelled(cancel_context));
    if (f->calls++) {
        assert(f->calls == 2 && f->checkpoints == 4);
        yyjson_doc *doc = yyjson_read(request, strlen(request), 0);
        assert(doc);
        yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(doc), "input"), *item;
        size_t i, n, outputs = 0;
        yyjson_arr_foreach(input, i, n, item) {
            if (!yyjson_equals_str(yyjson_obj_get(item, "type"), "function_call_output"))
                continue;
            const char *json = yyjson_get_str(yyjson_obj_get(item, "output"));
            assert(json);
            yyjson_doc *receipt = yyjson_read(json, strlen(json), 0);
            assert(receipt);
            yyjson_val *envelope = yyjson_doc_get_root(receipt),
                       *result = yyjson_obj_get(envelope, "result");
            assert(yyjson_is_false(yyjson_obj_get(envelope, "is_error")));
            assert(yyjson_equals_str(yyjson_obj_get(result, "application"), "complete"));
            assert(yyjson_equals_str(
                yyjson_obj_get(yyjson_arr_get(yyjson_obj_get(result, "checks"), 0), "status"),
                "pass"));
            outputs++;
            yyjson_doc_free(receipt);
        }
        assert(outputs == 1);
        yyjson_doc_free(doc);
        *response = ah_strdup(
            "{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
            "\"content\":[{\"type\":\"output_text\",\"text\":\"verified\"}]}]}");
        return AH_OK;
    }
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "status", "completed");
    yyjson_mut_val *output = yyjson_mut_obj_add_arr(doc, root, "output"),
                   *call = yyjson_mut_obj(doc);
    yyjson_mut_arr_append(output, call);
    yyjson_mut_obj_add_str(doc, call, "type", "function_call");
    yyjson_mut_obj_add_str(doc, call, "namespace", "local");
    yyjson_mut_obj_add_str(doc, call, "name", "apply_changes");
    yyjson_mut_obj_add_str(doc, call, "call_id", "workflow-call");
    yyjson_mut_obj_add_str(doc, call, "arguments", f->arguments);
    *response = ah_mut_write(doc);
    ah_mut_doc_free(doc);
    assert(*response);
    return AH_OK;
}

static void engine_observe(void *context, const ah_event *event) {
    engine_case *f = context;
    if (strcmp(event->type, "tool.checkpoint"))
        return;
    assert(event->seq && event->operation_id);
    sqlite3 *db = NULL;
    sqlite3_stmt *row = NULL;
    assert(sqlite3_open_v2(f->database, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    assert(sqlite3_prepare_v2(db, "SELECT json FROM events WHERE seq=? AND type='tool.checkpoint'",
                              -1, &row, NULL) == SQLITE_OK);
    sqlite3_bind_int64(row, 1, (sqlite3_int64)event->seq);
    assert(sqlite3_step(row) == SQLITE_ROW);
    assert(!strcmp((const char *)sqlite3_column_text(row, 0), event->json));
    sqlite3_finalize(row);
    sqlite3_close(db);
    const char *expected[] = {"file.started", "file.applied", "check.started", "check.finished"};
    assert(f->checkpoints < 4);
    yyjson_doc *doc = yyjson_read(event->json, strlen(event->json), 0);
    assert(doc);
    assert(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "phase"),
                             expected[f->checkpoints]));
    if (f->checkpoints == 0)
        wf_expect_file(f->workspace, "a.txt", WF_BASE);
    else
        wf_expect_file(f->workspace, "a.txt", "X\nB\nZ\nD\n");
    if (f->checkpoints == 2)
        assert(wf_runs(f->workspace) == f->initial_runs);
    if (f->checkpoints == 3)
        assert(wf_runs(f->workspace) == f->initial_runs + 1);
    f->checkpoints++;
    yyjson_doc_free(doc);
}

void wf_engine_case(workflow *w) {
    wf_put(w, "a.txt", WF_BASE);
    wf_put(w, "input.txt", "good\n");
    wf_add_check(w, "engine_check", "pass", false, 2000);
    engine_case f = {.workspace = w,
                     .initial_runs = wf_runs(w),
                     .arguments = wf_apply_args(NULL, false, "engine_check")};
    wf_path(w, "engine.db", f.database);
    ah_config config;
    ah_config_init(&config);
    config.database_path = f.database;
    config.provider = (ah_provider){engine_model, &f};
    config.observer = engine_observe;
    config.observer_context = &f;
    ah_engine *engine = NULL;
    ah_error error;
    assert(ah_open(&config, &engine, &error) == AH_OK);
    assert(ah_local_tools_register(w->tools, engine, &error) == AH_OK);
    char session[AH_ID_CAP];
    assert(ah_session_create(engine, "offline-model", "Test workflow", session, &error) == AH_OK);
    assert(ah_submit(engine, session, "workflow-command", "Apply the fixed change and check",
                     &error) == AH_OK);
    ah_code code = ah_run(engine, session, &error);
    if (code != AH_OK)
        fprintf(stderr, "engine workflow: %s %s\n", ah_code_name(code), error.message);
    assert(code == AH_OK && f.calls == 2 && f.checkpoints == 4);
    char *json = NULL;
    assert(ah_session_json(engine, session, &json, &error) == AH_OK);
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    assert(doc);
    assert(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "state"), "completed"));
    yyjson_doc_free(doc);
    ah_free(json);
    ah_close(engine);
    free(f.arguments);
}
