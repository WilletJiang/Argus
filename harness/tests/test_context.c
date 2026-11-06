#include "core/core.h"
#include <assert.h>
#include <stdio.h>

static const char call[] =
    "{\"status\":\"completed\",\"output\":[{\"type\":\"reasoning\",\"encrypted_content\":"
    "\"opaque\",\"summary\":[]},{\"type\":\"function_call\",\"name\":\"bulk\","
    "\"call_id\":\"call_bulk\",\"arguments\":\"{}\"}]}";
static const char done[] =
    "{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
    "\"content\":[{\"type\":\"output_text\",\"text\":\"done\"}]}]}";

typedef struct {
    ah_engine *engine;
    char session[AH_ID_CAP];
    size_t result_size;
    unsigned models, executions, fail_model;
    bool steer, cancel_intent, unknown;
    char *request;
} fixture;

static void ok(ah_code code, ah_error *error) {
    if (code != AH_OK) {
        fprintf(stderr, "%s: %s\n", ah_code_name(code), error->message);
        abort();
    }
}
static ah_code model(void *context, const char *request, ah_delta_sink delta, void *delta_ctx,
                     ah_cancel_check cancelled, void *cancel_ctx, char **out, ah_error *error) {
    (void)delta;
    (void)delta_ctx;
    (void)cancelled;
    (void)cancel_ctx;
    fixture *f = context;
    free(f->request);
    f->request = ah_strdup(request);
    assert(f->request);
    f->models++;
    if (f->models == f->fail_model)
        return ah_fail(error, AH_NETWORK, "injected model failure");
    if (f->models == 1 && f->steer)
        ok(ah_steer(f->engine, f->session, "steer", "Preserve exact counts.", error), error);
    *out = ah_strdup(f->models == 1 ? call : done);
    assert(*out);
    return AH_OK;
}
static ah_code execute(void *context, const ah_tool_call *tool_call, ah_tool_result *result,
                       ah_error *error) {
    (void)tool_call;
    (void)error;
    fixture *f = context;
    f->executions++;
    result->json = malloc(f->result_size + 3);
    assert(result->json);
    result->json[0] = '"';
    memset(result->json + 1, 'x', f->result_size);
    result->json[f->result_size + 1] = '"';
    result->json[f->result_size + 2] = 0;
    result->outcome_unknown = f->unknown;
    return AH_OK;
}
static void observe(void *context, const ah_event *event) {
    fixture *f = context;
    if (f->cancel_intent && !strcmp(event->type, "tool.intent"))
        ah_cancel(f->engine);
}
static void start(fixture *f, size_t context_limit, bool approval) {
    ah_config config;
    ah_config_init(&config);
    config.database_path = ":memory:";
    config.max_context_bytes = context_limit;
    config.provider = (ah_provider){model, f};
    config.observer = observe;
    config.observer_context = f;
    ah_error error = {0};
    ok(ah_open(&config, &f->engine, &error), &error);
    ah_tool tool = {.name = "bulk",
                    .description = "Return a bulk receipt",
                    .parameters_json = "{\"type\":\"object\"}",
                    .effect = f->unknown ? AH_EFFECT_WRITE : AH_EFFECT_READ,
                    .requires_approval = approval,
                    .execute = execute,
                    .context = f};
    ok(ah_register_tool(f->engine, &tool, &error), &error);
    ok(ah_session_create(f->engine, "mock", "Follow the task constraints.", f->session, &error),
       &error);
    ok(ah_submit(f->engine, f->session, "task", "Read bulk and report counts.", &error), &error);
    ok(ah_submit(f->engine, f->session, "constraint", "Never write files.", &error), &error);
}
static void close_fixture(fixture *f) {
    ah_close(f->engine);
    free(f->request);
}
static void state_is(fixture *f, ah_state expected) {
    core_session session;
    ah_error error = {0};
    ok(core_load(f->engine, f->session, &session, &error), &error);
    assert(session.state == expected);
    core_session_free(&session);
}
static int scalar(fixture *f, const char *sql) {
    sqlite3_stmt *s = NULL;
    assert(sqlite3_prepare_v2(f->engine->db, sql, -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    int result = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return result;
}
static void check_request(fixture *f, const char *summary, bool exchange, bool steering) {
    core_session session;
    ah_error error = {0};
    ok(core_load(f->engine, f->session, &session, &error), &error);
    char *request = NULL;
    ok(core_request(f->engine, &session, &request, &error), &error);
    core_session_free(&session);
    yyjson_doc *doc = yyjson_read(request, strlen(request), 0);
    assert(doc);
    yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(doc), "input"), *item;
    size_t index, count, users = 0, calls = 0, outputs = 0, reasoning = 0;
    yyjson_arr_foreach(input, index, count, item) {
        const char *role = yyjson_get_str(yyjson_obj_get(item, "role"));
        const char *type = yyjson_get_str(yyjson_obj_get(item, "type"));
        users += role && !strcmp(role, "user");
        calls += type && !strcmp(type, "function_call");
        outputs += type && !strcmp(type, "function_call_output");
        reasoning += type && !strcmp(type, "reasoning");
    }
    assert(users == (steering ? 4u : 3u)); /* Summary plus original user inputs. */
    assert(calls == (exchange ? 1u : 0u));
    assert(outputs == calls && reasoning == calls);
    assert(strstr(request, summary));
    assert(strstr(request, "Read bulk and report counts."));
    assert(strstr(request, "Never write files."));
    assert((strstr(request, "Preserve exact counts.") != NULL) == steering);
    yyjson_doc_free(doc);
    free(request);
}
static void oversized_receipt_recovery(void) {
    fixture f = {.result_size = 5000, .steer = true};
    start(&f, 4096, false);
    ah_error error = {0};
    assert(ah_run(f.engine, f.session, &error) == AH_LIMIT);
    assert(f.models == 1 && f.executions == 1);
    state_is(&f, AH_STOPPED);
    int original_items = scalar(&f, "SELECT COUNT(*) FROM items");
    char fork[AH_ID_CAP];
    assert(ah_fork(f.engine, f.session, fork, &error) == AH_BUSY);
    ok(ah_compact(f.engine, f.session, "Bulk receipt: 5000 characters.", 0, &error), &error);
    state_is(&f, AH_STOPPED);
    assert(ah_run(f.engine, f.session, &error) == AH_INVALID);
    check_request(&f, "Bulk receipt: 5000 characters.", false, true);
    assert(scalar(&f, "SELECT COUNT(*) FROM items") == original_items);
    assert(scalar(&f, "SELECT length(result) FROM operations") == 5002);
    assert(scalar(&f, "SELECT COUNT(*) FROM call_ids") == 1);
    ok(ah_compact(f.engine, f.session, "Bulk receipt: 5000 characters.", 0, &error), &error);
    check_request(&f, "Bulk receipt: 5000 characters.", false, true);
    ok(ah_resume(f.engine, f.session, &error), &error);
    ok(ah_run(f.engine, f.session, &error), &error);
    state_is(&f, AH_COMPLETED);
    assert(f.models == 2 && f.executions == 1);
    close_fixture(&f);
}
static void failed_model_recovery(void) {
    fixture f = {.result_size = 16, .fail_model = 2};
    start(&f, 4096, false);
    ah_error error = {0};
    assert(ah_run(f.engine, f.session, &error) == AH_NETWORK);
    state_is(&f, AH_FAILED);
    ok(ah_compact(f.engine, f.session, "Retain settled current turn.", 1, &error), &error);
    check_request(&f, "Retain settled current turn.", true, false);
    int active = scalar(&f, "SELECT COUNT(*) FROM items WHERE active=1");
    ok(core_sql(f.engine,
                "CREATE TRIGGER reject_summary BEFORE UPDATE OF summary ON sessions "
                "BEGIN SELECT RAISE(ABORT,'injected failure'); END",
                &error),
       &error);
    assert(ah_compact(f.engine, f.session, "Must roll back.", 0, &error) != AH_OK);
    assert(scalar(&f, "SELECT COUNT(*) FROM items WHERE active=1") == active);
    check_request(&f, "Retain settled current turn.", true, false);
    ok(core_sql(f.engine, "DROP TRIGGER reject_summary", &error), &error);
    ok(ah_compact(f.engine, f.session, "Bulk receipt: 16 characters.", 0, &error), &error);
    check_request(&f, "Bulk receipt: 16 characters.", false, false);
    state_is(&f, AH_FAILED);
    ok(ah_resume(f.engine, f.session, &error), &error);
    ok(ah_run(f.engine, f.session, &error), &error);
    assert(f.models == 3 && f.executions == 1);
    assert(scalar(&f, "SELECT model_calls FROM sessions") == 3);
    close_fixture(&f);
}
static void unfinished_exchanges_refused(void) {
    ah_error error = {0};
    for (unsigned mode = 0; mode < 3; mode++) {
        fixture f = {.result_size = 16, .cancel_intent = mode == 0, .unknown = mode == 2};
        start(&f, 4096, mode == 1);
        ah_code result = ah_run(f.engine, f.session, &error);
        assert(result == (mode == 0 ? AH_CANCELLED : mode == 1 ? AH_OK : AH_UNCERTAIN));
        state_is(&f, mode == 0 ? AH_STOPPED : mode == 1 ? AH_WAITING_APPROVAL : AH_OUTCOME_UNKNOWN);
        int active = scalar(&f, "SELECT COUNT(*) FROM items WHERE active=1");
        assert(ah_compact(f.engine, f.session, "Cannot drop unfinished exchange.", 0, &error) ==
               AH_BUSY);
        assert(scalar(&f, "SELECT COUNT(*) FROM items WHERE active=1") == active);
        close_fixture(&f);
    }
}
static void fork_receipt_references(void) {
    fixture f = {.result_size = 16};
    start(&f, 4096, false);
    ah_error error = {0};
    ok(ah_run(f.engine, f.session, &error), &error);
    char child[AH_ID_CAP], grandchild[AH_ID_CAP];
    ok(ah_fork(f.engine, f.session, child, &error), &error);
    ok(ah_fork(f.engine, child, grandchild, &error), &error);
    assert(scalar(&f, "SELECT COUNT(*) FROM operations") == 1);
    assert(scalar(&f, "SELECT COUNT(*) FROM result_refs") == 3);
    sqlite3_stmt *s = NULL;
    assert(sqlite3_prepare_v2(f.engine->db,
                              "SELECT length(o.result) FROM result_refs r "
                              "JOIN operations o ON o.id=r.operation_id WHERE r.session_id=?",
                              -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, grandchild, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) == 18);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
    ok(core_sql(f.engine,
                "CREATE TRIGGER reject_reference BEFORE INSERT ON result_refs "
                "BEGIN SELECT RAISE(ABORT,'injected failure'); END",
                &error),
       &error);
    assert(ah_fork(f.engine, child, grandchild, &error) != AH_OK);
    assert(scalar(&f, "SELECT COUNT(*) FROM sessions") == 3);
    assert(scalar(&f, "SELECT COUNT(*) FROM result_refs") == 3);
    close_fixture(&f);
}
int main(void) {
    oversized_receipt_recovery();
    failed_model_recovery();
    unfinished_exchanges_refused();
    fork_receipt_references();
    puts("context tests passed");
    return 0;
}
