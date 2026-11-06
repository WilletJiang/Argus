#include "core/core.h"
#include <assert.h>
#include <stdio.h>

static const char done[] =
    "{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
    "\"status\":\"completed\",\"content\":[{\"type\":\"output_text\",\"text\":\"done\"}]}]}";
static const char schema[] =
    "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}";

typedef struct {
    ah_engine *engine;
    const char *pause_name;
    char *payload, *last_request, *reconstructed;
    char operation[AH_ID_CAP];
    unsigned model_calls, pages;
    size_t offset;
    bool retrieve;
} fixture;

static void ok(ah_code code, const ah_error *error) {
    if (code != AH_OK) {
        fprintf(stderr, "%s: %s\n", ah_code_name(code), error->message);
        abort();
    }
}

static yyjson_doc *parse(const char *json) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    assert(doc);
    return doc;
}

static char *payload(size_t repetitions) {
    const char text[] = "地球🌍\n\"\\\t";
    size_t length = repetitions * (sizeof(text) - 1);
    char *repeated = malloc(length + 1);
    assert(repeated);
    for (size_t i = 0; i < repetitions; i++)
        memcpy(repeated + i * (sizeof(text) - 1), text, sizeof(text) - 1);
    repeated[length] = 0;
    yyjson_mut_doc *doc = ah_mut_doc_new();
    assert(doc);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "text", repeated);
    char *json = ah_mut_write(doc);
    assert(json);
    ah_mut_doc_free(doc);
    free(repeated);
    return json;
}

static void check_advertisements(yyjson_val *root) {
    yyjson_val *space = yyjson_arr_get(yyjson_obj_get(root, "tools"), 0);
    yyjson_val *functions = yyjson_obj_get(space, "tools"), *tool;
    bool ask = false, wait = false, read = false;
    size_t index, size;
    yyjson_arr_foreach(functions, index, size, tool) {
        const char *name = yyjson_get_str(yyjson_obj_get(tool, "name"));
        assert(name && strcmp(name, "harness_ask") && strcmp(name, "harness_wait"));
        ask = ask || !strcmp(name, "ask");
        wait = wait || !strcmp(name, "wait");
        read = read || !strcmp(name, "read_result");
    }
    assert(ask && wait && read);
}

static char *function_response(const char *name, const char *id, const char *arguments) {
    yyjson_mut_doc *doc = ah_mut_doc_new();
    assert(doc);
    yyjson_mut_val *root = yyjson_mut_obj(doc), *output = yyjson_mut_arr(doc),
                   *call = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "status", "completed");
    yyjson_mut_obj_add_val(doc, root, "output", output);
    yyjson_mut_arr_append(output, call);
    yyjson_mut_obj_add_str(doc, call, "type", "function_call");
    yyjson_mut_obj_add_str(doc, call, "name", name);
    yyjson_mut_obj_add_str(doc, call, "call_id", id);
    yyjson_mut_obj_add_str(doc, call, "arguments", arguments);
    char *json = ah_mut_write(doc);
    assert(json);
    ah_mut_doc_free(doc);
    return json;
}

static yyjson_val *last_output(yyjson_val *root) {
    yyjson_val *input = yyjson_obj_get(root, "input"), *item, *last = NULL;
    size_t index, size;
    yyjson_arr_foreach(input, index, size, item) {
        const char *type = yyjson_get_str(yyjson_obj_get(item, "type"));
        if (type && !strcmp(type, "function_call_output"))
            last = yyjson_obj_get(item, "output");
    }
    assert(yyjson_is_str(last));
    return last;
}

static ah_code complete(void *context, const char *request, ah_delta_sink delta,
                        void *delta_context, ah_cancel_check cancelled, void *cancel_context,
                        char **response, ah_error *error) {
    (void)delta;
    (void)delta_context;
    (void)error;
    fixture *f = context;
    assert(!cancelled(cancel_context));
    free(f->last_request);
    f->last_request = ah_strdup(request);
    assert(f->last_request);
    yyjson_doc *doc = parse(request);
    yyjson_val *root = yyjson_doc_get_root(doc);
    check_advertisements(root);
    unsigned turn = f->model_calls++;
    if (!turn) {
        const char *name = f->pause_name ? f->pause_name : "produce";
        const char *arguments = !f->pause_name        ? "{}"
                                : strstr(name, "ask") ? "{\"question\":\"choose\"}"
                                                      : "{\"reason\":\"wait\"}";
        *response = function_response(name, "source", arguments);
    } else if (f->pause_name)
        *response = ah_strdup(done);
    else {
        yyjson_doc *receipt = parse(yyjson_get_str(last_output(root)));
        yyjson_val *body = yyjson_doc_get_root(receipt), *result = yyjson_obj_get(body, "result");
        assert(!yyjson_get_bool(yyjson_obj_get(body, "is_error")));
        assert(strlen(yyjson_get_str(last_output(root))) <= f->engine->config.max_model_tool_bytes);
        if (turn == 1) {
            assert(yyjson_get_bool(yyjson_obj_get(result, "truncated")));
            const char *id = yyjson_get_str(yyjson_obj_get(result, "operation_id"));
            assert(id && strlen(id) == AH_ID_CAP - 1);
            memcpy(f->operation, id, AH_ID_CAP);
        } else {
            const char *content = yyjson_get_str(yyjson_obj_get(result, "content"));
            size_t length = yyjson_get_len(yyjson_obj_get(result, "content"));
            assert(content && yyjson_get_uint(yyjson_obj_get(result, "offset")) == f->offset);
            assert(length &&
                   yyjson_get_uint(yyjson_obj_get(result, "next_offset")) == f->offset + length);
            assert(f->offset + length <= strlen(f->payload));
            memcpy(f->reconstructed + f->offset, content, length);
            f->offset += length;
            f->pages++;
            assert(yyjson_get_bool(yyjson_obj_get(result, "truncated")) ==
                   (f->offset < strlen(f->payload)));
        }
        if (f->retrieve && f->offset < strlen(f->payload)) {
            char arguments[160], id[32];
            snprintf(arguments, sizeof(arguments),
                     "{\"operation_id\":\"%s\",\"offset\":%zu,\"limit\":4096}", f->operation,
                     f->offset);
            snprintf(id, sizeof(id), "page_%u", f->pages);
            *response = function_response("read_result", id, arguments);
        } else
            *response = ah_strdup(done);
        yyjson_doc_free(receipt);
    }
    yyjson_doc_free(doc);
    assert(*response);
    return AH_OK;
}

static ah_code execute(void *context, const ah_tool_call *call, ah_tool_result *result,
                       ah_error *error) {
    (void)call;
    (void)error;
    fixture *f = context;
    result->json = ah_strdup(f->payload);
    assert(result->json);
    return AH_OK;
}

static void open_fixture(fixture *f, size_t budget) {
    ah_config config;
    ah_config_init(&config);
    config.database_path = ":memory:";
    config.provider = (ah_provider){complete, f};
    config.max_model_tool_bytes = budget;
    ah_error error = {0};
    ok(ah_open(&config, &f->engine, &error), &error);
    ah_tool tool = {"produce", "Produce result", schema, AH_EFFECT_READ, false, execute, f, false};
    ok(ah_register_tool(f->engine, &tool, &error), &error);
}

static void close_fixture(fixture *f) {
    ah_close(f->engine);
    free(f->payload);
    free(f->last_request);
    free(f->reconstructed);
}

static void start(fixture *f, char sid[AH_ID_CAP]) {
    ah_error error = {0};
    ok(ah_session_create(f->engine, "mock", "test", sid, &error), &error);
    ok(ah_submit(f->engine, sid, "command", "run", &error), &error);
    ok(ah_run(f->engine, sid, &error), &error);
}

static void bounded_views(void) {
    fixture f = {0};
    open_fixture(&f, 1024);
    char *large = payload(4096);
    const size_t budgets[] = {16384, 1024, 0};
    ah_config defaults;
    ah_config_init(&defaults);
    assert(defaults.max_model_tool_bytes == budgets[0]);
    for (size_t i = 0; i < sizeof(budgets) / sizeof(budgets[0]); i++) {
        f.engine->config.max_model_tool_bytes = budgets[i];
        char *view = core_result_view(f.engine, "operation", large, true);
        assert(view && (!budgets[i] || strlen(view) <= budgets[i]));
        yyjson_doc *doc = parse(view);
        yyjson_val *root = yyjson_doc_get_root(doc), *result = yyjson_obj_get(root, "result");
        assert(yyjson_get_bool(yyjson_obj_get(root, "is_error")));
        if (budgets[i]) {
            assert(yyjson_get_bool(yyjson_obj_get(result, "truncated")));
            assert(yyjson_get_uint(yyjson_obj_get(result, "total_bytes")) == strlen(large));
            assert(
                !strcmp(yyjson_get_str(yyjson_obj_get(result, "retrieval_tool")), "read_result"));
            size_t length = yyjson_get_len(yyjson_obj_get(result, "preview"));
            assert(length &&
                   !memcmp(yyjson_get_str(yyjson_obj_get(result, "preview")), large, length));
        } else {
            char *full = ah_json_write(result);
            assert(full && !strcmp(full, large));
            free(full);
        }
        yyjson_doc_free(doc);
        free(view);
        view = core_result_view(f.engine, "operation", "{\"value\":7}", false);
        assert(view && !strcmp(view, "{\"is_error\":false,\"result\":{\"value\":7}}"));
        free(view);
    }
    free(large);
    close_fixture(&f);
}

static ah_code read_page(fixture *f, const char *sid, size_t offset, size_t limit,
                         size_t output_limit, ah_tool_result *result) {
    char arguments[160];
    snprintf(arguments, sizeof(arguments), "{\"operation_id\":\"%s\",\"offset\":%zu,\"limit\":%zu}",
             f->operation, offset, limit);
    ah_tool_call call = {
        .session_id = sid, .arguments_json = arguments, .output_limit = output_limit};
    ah_error error = {0};
    return core_read_result(f->engine, &call, result, &error);
}

static void reconstruction_and_access(void) {
    fixture f = {.payload = payload(512), .retrieve = true};
    f.reconstructed = calloc(strlen(f.payload) + 1, 1);
    assert(f.reconstructed);
    open_fixture(&f, 1024);
    char sid[AH_ID_CAP];
    start(&f, sid);
    assert(f.pages > 1 && !strcmp(f.reconstructed, f.payload));
    core_operation op;
    ah_error error = {0};
    ok(core_get_operation(f.engine, sid, f.operation, &op, &error), &error);
    assert(!strcmp(op.result, f.payload));
    core_operation_free(&op);

    size_t total = strlen(f.payload), leading = 0;
    while (((unsigned char)f.payload[leading] & 0xc0) != 0xc0)
        leading++;
    ah_tool_result result = {0};
    assert(read_page(&f, sid, total + 1, 4096, 65536, &result) == AH_INVALID);
    assert(read_page(&f, sid, leading + 1, 4096, 65536, &result) == AH_INVALID);
    assert(read_page(&f, sid, leading, 1, 65536, &result) == AH_INVALID);
    assert(read_page(&f, sid, 0, 4096, 32, &result) == AH_LIMIT);
    assert(!result.json);
    /* The first Chinese character uses three bytes. A page ending inside
     * the following character must stop at the exact same boundary. */
    for (size_t limit = 3; limit <= 5; ++limit) {
        ok(read_page(&f, sid, leading, limit, 65536, &result), &error);
        yyjson_doc *doc = parse(result.json);
        yyjson_val *body = yyjson_doc_get_root(doc);
        assert(yyjson_equals_str(yyjson_obj_get(body, "content"), "地"));
        assert(yyjson_get_uint(yyjson_obj_get(body, "next_offset")) == leading + 3);
        yyjson_doc_free(doc);
        free(result.json);
        result.json = NULL;
    }
    ok(read_page(&f, sid, total, 4096, 65536, &result), &error);
    yyjson_doc *page = parse(result.json);
    yyjson_val *root = yyjson_doc_get_root(page);
    assert(yyjson_get_uint(yyjson_obj_get(root, "next_offset")) == total);
    assert(yyjson_get_len(yyjson_obj_get(root, "content")) == 0);
    assert(!yyjson_get_bool(yyjson_obj_get(root, "truncated")));
    yyjson_doc_free(page);
    free(result.json);
    result.json = NULL;

    char unrelated[AH_ID_CAP], fork[AH_ID_CAP];
    ok(ah_session_create(f.engine, "mock", "other", unrelated, &error), &error);
    assert(read_page(&f, unrelated, 0, 4096, 65536, &result) == AH_NOT_FOUND);
    ok(ah_fork(f.engine, sid, fork, &error), &error);
    ok(read_page(&f, fork, 0, 4096, 65536, &result), &error);
    page = parse(result.json);
    root = yyjson_doc_get_root(page);
    size_t length = yyjson_get_len(yyjson_obj_get(root, "content"));
    assert(length && !memcmp(yyjson_get_str(yyjson_obj_get(root, "content")), f.payload, length));
    yyjson_doc_free(page);
    free(result.json);

    core_session session;
    ok(core_load(f.engine, sid, &session, &error), &error);
    char *before = NULL, *after = NULL;
    ok(core_request(f.engine, &session, &before, &error), &error);
    f.engine->config.max_model_tool_bytes = 0;
    ok(core_request(f.engine, &session, &after, &error), &error);
    assert(!strcmp(before, after));
    free(before);
    free(after);
    core_session_free(&session);
    close_fixture(&f);
}

static void pause_aliases(void) {
    const char *names[] = {"ask", "wait", "harness_ask", "harness_wait"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        fixture f = {.pause_name = names[i]};
        open_fixture(&f, 1024);
        char sid[AH_ID_CAP];
        start(&f, sid);
        ah_error error = {0};
        core_session session;
        ok(core_load(f.engine, sid, &session, &error), &error);
        assert(session.state == AH_WAITING_INPUT && f.model_calls == 1);
        core_session_free(&session);
        core_operation op;
        ok(core_next_operation(f.engine, sid, &op, &error), &error);
        ok(ah_reply(f.engine, sid, op.id, "{\"answer\":\"continue\"}", &error), &error);
        core_operation_free(&op);
        ok(ah_run(f.engine, sid, &error), &error);
        ok(core_load(f.engine, sid, &session, &error), &error);
        assert(session.state == AH_COMPLETED && f.model_calls == 2);
        core_session_free(&session);
        close_fixture(&f);
    }
}

int main(void) {
    bounded_views();
    reconstruction_and_access();
    pause_aliases();
    puts("result tests passed");
    return 0;
}
