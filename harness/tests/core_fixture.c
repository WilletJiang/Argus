#include "core_test.h"

const char done[] = "{\"id\":\"resp_done\",\"status\":\"completed\",\"output\":[{\"type\":"
                    "\"message\",\"role\":\"assistant\",\"status\":\"completed\","
                    "\"content\":[{\"type\":\"output_text\",\"text\":\"done\"}]}]}";
const char call[] =
    "{\"status\":\"completed\",\"output\":[{\"type\":\"reasoning\",\"encrypted_content\":"
    "\"ciphertext\",\"summary\":[]},{\"type\":\"function_call\",\"namespace\":\"local\",\"call_"
    "id\":\"call_1\",\"name\":\"test_tool\",\"arguments\":\"{\\\"value\\\":3}\"}]}";
const char ask[] =
    "{\"status\":\"completed\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"ask_1\","
    "\"name\":\"ask\",\"arguments\":\"{\\\"question\\\":\\\"Choose\\\",\\\"options\\\":["
    "\\\"a\\\",\\\"b\\\"]}\"}]}";
static const char schema[] = "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"integer\"}"
                             "},\"required\":[\"value\"],\"additionalProperties\":false}";
static char *copy(const char *text) {
    size_t n = strlen(text) + 1;
    char *result = malloc(n);
    assert(result);
    memcpy(result, text, n);
    return result;
}
void ok(ah_code code, ah_error *error) {
    if (code != AH_OK) {
        fprintf(stderr, "unexpected %s: %s\n", ah_code_name(code), error->message);
        abort();
    }
}
static void *steer_thread(void *context) {
    fixture *f = context;
    ah_error error = {0};
    ok(ah_steer(f->engine, f->steering_session, "steer_1", "steer after tools", &error), &error);
    ok(ah_steer(f->engine, f->steering_session, "steer_1", "steer after tools", &error), &error);
    assert(ah_steer(f->engine, f->steering_session, "steer_1", "collision", &error) == AH_INVALID);
    return NULL;
}
ah_code fixture_complete(void *context, const char *request, ah_delta_sink delta,
                         void *delta_context, ah_cancel_check cancelled, void *cancel_context,
                         char **response, ah_error *error) {
    (void)error;
    fixture *f = context;
    assert(!cancelled(cancel_context));
    free(f->last_request);
    f->last_request = copy(request);
    yyjson_doc *parsed = yyjson_read(request, strlen(request), 0);
    assert(parsed);
    yyjson_val *root = yyjson_doc_get_root(parsed);
    assert(!yyjson_get_bool(yyjson_obj_get(root, "store")));
    assert(yyjson_get_bool(yyjson_obj_get(root, "stream")));
    yyjson_val *space = yyjson_arr_get(yyjson_obj_get(root, "tools"), 0);
    assert(!strcmp(yyjson_get_str(yyjson_obj_get(space, "name")), "local"));
    yyjson_doc_free(parsed);
    delta(delta_context, "provisional", 11);
    const char *answer = f->answers[f->calls];
    assert(answer);
    f->calls++;
    if (f->steer && f->calls == 1) {
        pthread_t thread;
        assert(pthread_create(&thread, NULL, steer_thread, f) == 0);
        assert(pthread_join(thread, NULL) == 0);
    }
    *response = copy(answer);
    if (f->cancel_model)
        ah_cancel(f->engine);
    return AH_OK;
}
void fixture_observe(void *context, const ah_event *event) {
    fixture *f = context;
    assert(event->json);
    yyjson_doc *doc = yyjson_read(event->json, strlen(event->json), 0);
    assert(doc);
    yyjson_doc_free(doc);
    if (!event->seq) {
        f->provisional++;
        return;
    }
    assert(event->seq > f->last_seq);
    f->last_seq = event->seq;
    f->durable++;
    if (!strcmp(event->type, "tool.started"))
        f->started = true;
}
ah_code fixture_execute(void *context, const ah_tool_call *call_info, ah_tool_result *result,
                        ah_error *error) {
    (void)error;
    fixture *f = context;
    assert(f->started);
    assert(call_info->operation_id && strlen(call_info->operation_id) == 36);
    assert(!strcmp(call_info->arguments_json, "{\"value\":3}"));
    assert(call_info->deadline_ms >= ah_now_ms());
    f->executions++;
    if (f->crash)
        _exit(77);
    if (f->fault_database) {
        sqlite3 *db = NULL;
        assert(sqlite3_open(f->fault_database, &db) == SQLITE_OK);
        assert(sqlite3_exec(
                   db,
                   "CREATE TRIGGER fail_receipt BEFORE UPDATE ON operations WHEN NEW.status='done' "
                   "BEGIN SELECT RAISE(ABORT,'injected receipt failure'); END",
                   NULL, NULL, NULL) == SQLITE_OK);
        assert(sqlite3_close(db) == SQLITE_OK);
    }
    result->json = copy("{\"value\":6}");
    result->outcome_unknown = f->unknown;
    if (f->cancel_tool)
        ah_cancel(f->engine);
    return AH_OK;
}
ah_engine *open_engine(fixture *f, const char *path, ah_config *custom) {
    ah_config config;
    if (custom)
        config = *custom;
    else
        ah_config_init(&config);
    config.database_path = path;
    config.provider = (ah_provider){fixture_complete, f};
    config.observer = fixture_observe;
    config.observer_context = f;
    ah_engine *engine = NULL;
    ah_error error = {0};
    ok(ah_open(&config, &engine, &error), &error);
    f->engine = engine;
    return engine;
}
void register_tool(ah_engine *engine, fixture *f, ah_effect effect, bool approval) {
    ah_tool tool = {"test_tool", "Test doubling", schema, effect,
                    approval,    fixture_execute, f,      false};
    ah_error error = {0};
    ok(ah_register_tool(engine, &tool, &error), &error);
}
void create_input(ah_engine *engine, char id[AH_ID_CAP]) {
    ah_error error = {0};
    ok(ah_session_create(engine, "test-model", "Test instructions", id, &error), &error);
    ok(ah_submit(engine, id, "cmd_1", "first turn", &error), &error);
}
void state_is(ah_engine *engine, const char *id, const char *state) {
    ah_error error = {0};
    char *json = NULL;
    ok(ah_session_json(engine, id, &json, &error), &error);
    yyjson_doc *d = yyjson_read(json, strlen(json), 0);
    assert(d);
    assert(!strcmp(yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(d), "state")), state));
    yyjson_doc_free(d);
    ah_free(json);
}
char *pending(ah_engine *engine, const char *id) {
    ah_error error = {0};
    char *json = NULL;
    ok(ah_session_json(engine, id, &json, &error), &error);
    yyjson_doc *d = yyjson_read(json, strlen(json), 0);
    assert(d);
    const char *operation = yyjson_get_str(
        yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(d), "pending"), "operation_id"));
    assert(operation);
    char *result = copy(operation);
    yyjson_doc_free(d);
    ah_free(json);
    return result;
}
