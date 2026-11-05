#include "core/core.h"
#include "ah/task.h"
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

typedef struct {
    unsigned models, checks, version;
    bool repair_changes, stale_pass, unknown;
    ah_engine *engine;
    const char *sid;
    const char *crash_event;
} fixture;
static const char policy[] = "1111111111111111111111111111111111111111111111111111111111111111";
static void observe(void *context, const ah_event *event) {
    fixture *f = context;
    if (f->crash_event && !strcmp(f->crash_event, event->type))
        _exit(0);
}

static ah_code model(void *context, const char *request, ah_delta_sink delta, void *dc,
                     ah_cancel_check cancelled, void *cc, char **response, ah_error *error) {
    (void)request;
    (void)delta;
    (void)dc;
    (void)cancelled;
    (void)cc;
    (void)error;
    fixture *f = context;
    f->models++;
    if (f->models > 1 && f->repair_changes)
        f->version++;
    *response =
        ah_strdup("{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":"
                  "\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"done\"}]}]}");
    return AH_OK;
}
static ah_code revision(void *context, const ah_tool_call *call, char out[65], ah_error *error) {
    (void)call;
    (void)error;
    fixture *f = context;
    ah_hash_bytes(&f->version, sizeof(f->version), out);
    return AH_OK;
}
static ah_code verify(void *context, const ah_tool_call *call, ah_tool_result *result,
                      ah_error *error) {
    fixture *f = context;
    f->checks++;
    assert(ah_submit(f->engine, f->sid, "reenter", "bad", error) == AH_BUSY);
    assert(call->checkpoint(call->checkpoint_context, "{\"phase\":\"checked\"}", error) == AH_OK);
    if (f->stale_pass)
        f->version++;
    result->json =
        ah_strdup(f->version || f->stale_pass ? "{\"ok\":true}"
                                              : "{\"ok\":false,\"missing\":\"implementation\"}");
    if (f->unknown) {
        result->outcome_unknown = true;
        return AH_UNCERTAIN;
    }
    return AH_OK;
}
static ah_engine *open_engine(fixture *f, const char *db) {
    ah_config config;
    ah_config_init(&config);
    config.database_path = db;
    config.provider = (ah_provider){model, f};
    config.observer = observe;
    config.observer_context = f;
    ah_engine *engine = NULL;
    ah_error error = {0};
    assert(ah_open(&config, &engine, &error) == AH_OK);
    f->engine = engine;
    return engine;
}
static void setup(ah_engine *e, fixture *f, char sid[AH_ID_CAP], unsigned models,
                  unsigned repairs) {
    ah_error error = {0};
    assert(ah_session_create(e, "mock", "complete the task", sid, &error) == AH_OK);
    f->sid = sid;
    ah_task_config config = {
        "{\"required\":\"implementation\"}", policy, models, 8, repairs, 60000};
    assert(ah_task_create(e, sid, &config, &error) == AH_OK);
    assert(ah_task_create(e, sid, &config, &error) == AH_OK);
    config.max_model_calls++;
    assert(ah_task_create(e, sid, &config, &error) == AH_INVALID);
    assert(ah_submit(e, sid, "start", "Implement it", &error) == AH_OK);
}
static yyjson_doc *status(ah_engine *e, const char *sid) {
    char *json = NULL;
    ah_error error = {0};
    assert(ah_task_json(e, sid, &json, &error) == AH_OK);
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    free(json);
    assert(doc);
    return doc;
}
static void expect_state(ah_engine *e, const char *sid, const char *state, unsigned models,
                         unsigned checks) {
    yyjson_doc *doc = status(e, sid);
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(yyjson_equals_str(yyjson_obj_get(root, "state"), state));
    assert(yyjson_get_uint(yyjson_obj_get(root, "model_used")) == models);
    assert(yyjson_get_uint(yyjson_obj_get(root, "tool_used")) == checks);
    yyjson_doc_free(doc);
}
static void accepted_and_stale(void) {
    fixture f = {.repair_changes = true};
    ah_engine *e = open_engine(&f, ":memory:");
    char sid[AH_ID_CAP];
    setup(e, &f, sid, 4, 2);
    ah_task_verifier v = {policy, revision, verify, &f};
    ah_error error = {0};
    assert(ah_task_run(e, sid, &v, &error) == AH_OK);
    assert(f.models == 2 && f.checks == 2);
    expect_state(e, sid, "accepted", 2, 2);
    assert(ah_task_run(e, sid, &v, &error) == AH_OK && f.checks == 2);
    f.version++;
    assert(ah_task_run(e, sid, &v, &error) == AH_INVALID);
    ah_close(e);
}
static void no_progress_and_stale_pass(void) {
    for (unsigned stale = 0; stale < 2; stale++) {
        fixture f = {.stale_pass = stale != 0};
        ah_engine *e = open_engine(&f, ":memory:");
        char sid[AH_ID_CAP];
        setup(e, &f, sid, 8, stale ? 0 : 3);
        ah_task_verifier v = {policy, revision, verify, &f};
        ah_error error = {0};
        assert(ah_task_run(e, sid, &v, &error) == AH_LIMIT);
        assert(f.models == (stale ? 1u : 2u));
        expect_state(e, sid, "active", f.models, f.checks);
        yyjson_doc *doc = status(e, sid);
        assert(!yyjson_equals_str(
            yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(doc), "receipt"), "status"), "pass"));
        yyjson_doc_free(doc);
        ah_close(e);
    }
}
static void persisted_budget_and_unknown(void) {
    char path[] = "/tmp/ah-task-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    fixture f = {0};
    ah_engine *e = open_engine(&f, path);
    char sid[AH_ID_CAP];
    setup(e, &f, sid, 1, 2);
    ah_task_verifier v = {policy, revision, verify, &f};
    ah_error error = {0};
    assert(ah_task_run(e, sid, &v, &error) == AH_LIMIT);
    assert(f.models == 1);
    ah_close(e);
    e = open_engine(&f, path);
    assert(ah_submit(e, sid, "try-reset", "Do it again", &error) == AH_OK);
    assert(ah_task_run(e, sid, &v, &error) == AH_LIMIT && f.models == 1);
    ah_close(e);
    unlink(path);
    char lock[sizeof(path) + 6];
    snprintf(lock, sizeof(lock), "%s.lock", path);
    unlink(lock);

    f = (fixture){.version = 1, .unknown = true};
    e = open_engine(&f, ":memory:");
    setup(e, &f, sid, 3, 2);
    v.context = &f;
    assert(ah_task_run(e, sid, &v, &error) == AH_UNCERTAIN);
    expect_state(e, sid, "unknown", 1, 1);
    f.unknown = false;
    assert(ah_task_run(e, sid, &v, &error) == AH_UNCERTAIN && f.checks == 1);
    assert(ah_task_reconcile(e, sid, "{\"inspected\":true}", &error) == AH_OK);
    assert(ah_task_run(e, sid, &v, &error) == AH_OK);
    expect_state(e, sid, "accepted", 1, 2);
    ah_close(e);
}
static void expired_and_changed_policy(void) {
    fixture f = {.version = 1};
    ah_engine *e = open_engine(&f, ":memory:");
    char sid[AH_ID_CAP];
    setup(e, &f, sid, 3, 2);
    ah_error error = {0};
    ah_task_verifier v = {"changed", revision, verify, &f};
    assert(ah_task_run(e, sid, &v, &error) == AH_INVALID && !f.models);
    v.definition_revision = policy;
    assert(core_sql(e, "UPDATE tasks SET deadline_ms=1", &error) == AH_OK);
    assert(ah_task_run(e, sid, &v, &error) == AH_LIMIT && !f.models);
    ah_close(e);
}
static void recovery_boundaries(void) {
    const char *boundaries[] = {"task.verification.started", "task.repair_requested"};
    for (unsigned which = 0; which < 2; which++) {
        char path[] = "/tmp/ah-task-crash-XXXXXX", sid[AH_ID_CAP];
        int fd = mkstemp(path);
        assert(fd >= 0);
        close(fd);
        fixture f = {0};
        ah_engine *e = open_engine(&f, path);
        setup(e, &f, sid, 4, 2);
        ah_close(e);
        pid_t pid = fork();
        assert(pid >= 0);
        if (!pid) {
            f.crash_event = boundaries[which];
            e = open_engine(&f, path);
            ah_task_verifier v = {policy, revision, verify, &f};
            ah_error error = {0};
            ah_task_run(e, sid, &v, &error);
            _exit(7);
        }
        int child_status = 0;
        assert(waitpid(pid, &child_status, 0) == pid && WIFEXITED(child_status) &&
               !WEXITSTATUS(child_status));
        f.models = 1;
        f.repair_changes = true;
        e = open_engine(&f, path);
        ah_task_verifier v = {policy, revision, verify, &f};
        ah_error error = {0};
        if (!which) {
            assert(ah_task_run(e, sid, &v, &error) == AH_UNCERTAIN && !f.checks);
            assert(ah_task_reconcile(e, sid, "{\"reconciled\":true}", &error) == AH_OK);
            f.version = 1;
        }
        assert(ah_task_run(e, sid, &v, &error) == AH_OK);
        expect_state(e, sid, "accepted", which ? 2 : 1, 2);
        yyjson_doc *doc = status(e, sid);
        assert(yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(doc), "repairs")) == which);
        yyjson_doc_free(doc);
        ah_close(e);
        unlink(path);
        char lock[sizeof(path) + 6];
        snprintf(lock, sizeof(lock), "%s.lock", path);
        unlink(lock);
    }
}
static void local_verifier(void) {
    char root[] = "/tmp/ah-task-local-XXXXXX", file[128], sid[AH_ID_CAP];
    assert(mkdtemp(root));
    snprintf(file, sizeof(file), "%s/source.txt", root);
    FILE *out = fopen(file, "w");
    assert(out);
    fputs("source", out);
    fclose(out);
    fixture f = {0};
    ah_engine *e = open_engine(&f, ":memory:");
    ah_error error = {0};
    ah_local_tools *tools = NULL;
    ah_local_tools_config config = {.workspace = root, .enable_workflows = true};
    assert(ah_local_tools_open(&config, &tools, &error) == AH_OK);
    ah_check_config check = {.name = "presence",
                             .description = "required file exists",
                             .argv_json = "[\"test\",\"-f\",\"source.txt\"]",
                             .inputs_json = "[\".\"]",
                             .environment_revision = "fixed-test-environment",
                             .timeout_ms = 1000,
                             .cacheable = true};
    assert(ah_local_tools_add_check(tools, &check, &error) == AH_OK);
    assert(ah_local_tools_register(tools, e, &error) == AH_OK);
    ah_task_verifier verifier;
    assert(ah_local_tools_verifier(tools, &verifier, &error) == AH_OK);
    assert(ah_session_create(e, "mock", "", sid, &error) == AH_OK);
    f.sid = sid;
    ah_task_config task = {
        "{\"required\":\"source.txt exists\"}", verifier.definition_revision, 3, 8, 1, 10000};
    assert(ah_task_create(e, sid, &task, &error) == AH_OK);
    assert(ah_submit(e, sid, "start", "verify file", &error) == AH_OK);
    assert(ah_task_run(e, sid, &verifier, &error) == AH_OK);
    expect_state(e, sid, "accepted", 1, 1);
    assert(chmod(file, 0700) == 0);
    assert(ah_task_run(e, sid, &verifier, &error) == AH_INVALID);
    out = fopen(file, "a");
    assert(out);
    fputs("changed", out);
    fclose(out);
    assert(ah_task_run(e, sid, &verifier, &error) == AH_INVALID);
    unlink(file);
    rmdir(root);
    ah_close(e);
    ah_local_tools_close(tools);
}

static void check_authority(void) {
    for (unsigned partial = 0; partial < 2; partial++) {
        fixture f = {0};
        ah_engine *e = open_engine(&f, ":memory:");
        ah_error error = {0};
        ah_local_tools *tools = NULL;
        ah_local_tools_config config = {.workspace = "/tmp", .enable_workflows = true};
        assert(ah_local_tools_open(&config, &tools, &error) == AH_OK);
        ah_check_config check = {.name = "gated",
                                 .description = "host policy",
                                 .argv_json = "[\"true\"]",
                                 .inputs_json = "[\".\"]",
                                 .timeout_ms = 1000,
                                 .requires_approval = true};
        if (partial) {
            ah_tool conflict = {
                "read_file", "fixture", "{\"type\":\"object\"}", AH_EFFECT_READ, false, verify,
                &f,          false};
            assert(ah_register_tool(e, &conflict, &error) == AH_OK);
            assert(ah_local_tools_register(tools, e, &error) == AH_INVALID);
            assert(ah_local_tools_add_check(tools, &check, &error) == AH_INVALID);
        } else {
            assert(ah_local_tools_add_check(tools, &check, &error) == AH_OK);
            assert(ah_local_tools_register(tools, e, &error) == AH_OK);
            ah_task_verifier v;
            assert(ah_local_tools_verifier(tools, &v, &error) == AH_AUTH);
        }
        ah_close(e);
        ah_local_tools_close(tools);
    }
}
int main(void) {
    accepted_and_stale();
    no_progress_and_stale_pass();
    persisted_budget_and_unknown();
    expired_and_changed_policy();
    recovery_boundaries();
    local_verifier();
    check_authority();
    puts("task tests passed");
    return 0;
}
