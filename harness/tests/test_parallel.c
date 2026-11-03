#include "parallel_test.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static const char schema[] = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"}},"
                             "\"required\":[\"id\"],\"additionalProperties\":false}";
static const char done[] =
    "{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
    "\"status\":\"completed\",\"content\":[{\"type\":\"output_text\",\"text\":\"done\"}]}]}";

void ok(ah_code code, const ah_error *error) {
    if (code != AH_OK) {
        fprintf(stderr, "%s: %s\n", ah_code_name(code), error->message);
        abort();
    }
}

static unsigned argument_id(const char *json) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    assert(doc);
    unsigned id = (unsigned)yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(doc), "id"));
    yyjson_doc_free(doc);
    assert(id && id <= CALLS);
    return id;
}

static void observe(void *context, const ah_event *event) {
    fixture *f = context;
    assert(pthread_equal(pthread_self(), f->owner));
    if (!strcmp(event->type, "tool.started")) {
        f->started++;
        if (f->started == 1 && f->expected_batch) {
            assert(sqlite3_get_autocommit(f->engine->db));
            assert(scalar(f, "SELECT COUNT(*) FROM operations WHERE status='executing'") ==
                   (int)f->expected_batch);
            assert(scalar(f, "SELECT tool_calls FROM sessions") == (int)f->expected_batch);
        }
        if (f->cancel_before_launch)
            ah_cancel(f->engine);
    }
    if (!strcmp(event->type, "tool.completed") || !strcmp(event->type, "tool.late_result")) {
        f->receipts++;
        if (!strcmp(event->type, "tool.late_result"))
            f->late_receipts++;
    }
    if (!f->interrupted && ((f->interrupt_intent && !strcmp(event->type, "tool.started")) ||
                            (f->interrupt_event && !strcmp(event->type, f->interrupt_event)))) {
        f->interrupted = true;
        sqlite3_interrupt(f->engine->db);
    }
}

static ah_code complete(void *context, const char *request, ah_delta_sink delta,
                        void *delta_context, ah_cancel_check cancelled, void *cancel_context,
                        char **response, ah_error *error) {
    (void)delta;
    (void)delta_context;
    (void)error;
    fixture *f = context;
    assert(pthread_equal(pthread_self(), f->owner));
    assert(!cancelled(cancel_context));
    free(f->last_request);
    f->last_request = ah_strdup(request);
    assert(f->last_request);
    if (f->provider_calls++) {
        *response = ah_strdup(done);
        return AH_OK;
    }
    char json[4096];
    size_t used = (size_t)snprintf(json, sizeof(json), "{\"status\":\"completed\",\"output\":[");
    for (unsigned i = 0; i < f->count; i++) {
        char arguments[80] = "{}";
        if (!strcmp(f->names[i], "harness_ask"))
            snprintf(arguments, sizeof(arguments), "{\\\"question\\\":\\\"choose\\\"}");
        else if (!strcmp(f->names[i], "harness_wait"))
            snprintf(arguments, sizeof(arguments), "{\\\"reason\\\":\\\"wait\\\"}");
        else if (f->invalid_id != i + 1)
            snprintf(arguments, sizeof(arguments), "{\\\"id\\\":%u}", i + 1);
        int written = snprintf(json + used, sizeof(json) - used,
                               "%s{\"type\":\"function_call\",\"call_id\":\"call_%u\","
                               "\"name\":\"%s\",\"arguments\":\"%s\"}",
                               i ? "," : "", i + 1, f->names[i], arguments);
        assert(written > 0 && (size_t)written < sizeof(json) - used);
        used += (size_t)written;
    }
    assert(snprintf(json + used, sizeof(json) - used, "]}") == 2);
    *response = ah_strdup(json);
    assert(*response);
    return AH_OK;
}

static void wait_until(fixture *f, bool *condition) {
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 5;
    while (!*condition) {
        int rc = pthread_cond_timedwait(&f->changed, &f->mutex, &deadline);
        assert(rc == 0 || rc == ETIMEDOUT);
        if (rc == ETIMEDOUT) {
            f->timed_out = true;
            break;
        }
    }
}

static ah_code execute(void *context, const ah_tool_call *call, ah_tool_result *result,
                       ah_error *error) {
    fixture *f = context;
    unsigned id = argument_id(call->arguments_json);
    assert(call->deadline_ms);
    assert(f->started >= f->expected_starts[id]);
    assert(f->receipts >= f->minimum_receipts[id]);
    if (f->expect_owner)
        assert(pthread_equal(pthread_self(), f->owner));
    pthread_mutex_lock(&f->mutex);
    assert(!f->entered[id]);
    f->entered[id] = true;
    f->executions++;
    f->active++;
    if (f->active > f->max_active)
        f->max_active = f->active;
    pthread_cond_broadcast(&f->changed);
    if (!strcmp(f->names[id - 1], "write")) {
        assert(f->active == 1);
        for (unsigned previous = 1; previous < id; previous++)
            assert(f->finished[previous]);
        for (unsigned next = id + 1; next <= f->count; next++)
            assert(!f->entered[next]);
    }
    for (unsigned other = 1; other <= f->barrier_count; other++)
        wait_until(f, &f->entered[other]);
    if (f->partner[id])
        wait_until(f, &f->entered[f->partner[id]]);
    if (f->wait_finished[id])
        wait_until(f, &f->finished[f->wait_finished[id]]);
    if (f->cancel_id == id)
        ah_cancel(f->engine);
    if (f->wait_deadline) {
        pthread_mutex_unlock(&f->mutex);
        while (!call->cancelled(call->cancel_context)) {
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
        pthread_mutex_lock(&f->mutex);
    }
    f->active--;
    f->finished[id] = true;
    f->finish_order[f->finished_count++] = id;
    pthread_cond_broadcast(&f->changed);
    pthread_mutex_unlock(&f->mutex);
    if (f->error_id == id)
        return ah_fail(error, AH_IO, "injected read error");
    char json[40];
    snprintf(json, sizeof(json), "{\"id\":%u}", id);
    result->json = ah_strdup(json);
    result->outcome_unknown = f->unknown[id];
    assert(result->json);
    return AH_OK;
}

static void register_tools(fixture *f) {
    ah_error error = {0};
    ah_tool tools[] = {
        {"read", "Concurrent read", schema, AH_EFFECT_READ, false, execute, f, true},
        {"serial", "Unmarked read", schema, AH_EFFECT_READ, false, execute, f, false},
        {"write", "Write barrier", schema, AH_EFFECT_WRITE, false, execute, f, false},
        {"approved", "Approval barrier", schema, AH_EFFECT_READ, true, execute, f, true},
    };
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++)
        ok(ah_register_tool(f->engine, &tools[i], &error), &error);
}

void open_fixture(fixture *f, unsigned parallel, unsigned budget, uint64_t timeout) {
    f->owner = pthread_self();
    assert(pthread_mutex_init(&f->mutex, NULL) == 0);
    assert(pthread_cond_init(&f->changed, NULL) == 0);
    ah_config config;
    ah_config_init(&config);
    config.database_path = f->database_path ? f->database_path : ":memory:";
    config.provider = (ah_provider){complete, f};
    config.observer = observe;
    config.observer_context = f;
    config.max_parallel_tools = parallel;
    config.max_tool_calls = budget;
    config.tool_timeout_ms = timeout;
    ah_error error = {0};
    ok(ah_open(&config, &f->engine, &error), &error);
    register_tools(f);
}

void reopen_fixture(fixture *f) {
    ah_config config = f->engine->config;
    ah_close(f->engine);
    f->engine = NULL;
    ah_error error = {0};
    ok(ah_open(&config, &f->engine, &error), &error);
    register_tools(f);
}

void close_fixture(fixture *f) {
    assert(!f->active && !f->timed_out);
    ah_close(f->engine);
    free(f->last_request);
    assert(pthread_cond_destroy(&f->changed) == 0);
    assert(pthread_mutex_destroy(&f->mutex) == 0);
}

ah_code run_fixture(fixture *f, char sid[AH_ID_CAP]) {
    ah_error error = {0};
    ok(ah_session_create(f->engine, "mock", "test", sid, &error), &error);
    ok(ah_submit(f->engine, sid, "command", "run tools", &error), &error);
    return ah_run(f->engine, sid, &error);
}

int scalar(fixture *f, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(f->engine->db, sql, -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int value = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

void assert_receipt_order(fixture *f, unsigned count) {
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(f->engine->db,
                              "SELECT json FROM items WHERE json LIKE '%function_call_output%' "
                              "ORDER BY seq",
                              -1, &stmt, NULL) == SQLITE_OK);
    for (unsigned i = 1; i <= count; i++) {
        assert(sqlite3_step(stmt) == SQLITE_ROW);
        const char *json = (const char *)sqlite3_column_text(stmt, 0);
        yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
        assert(doc);
        char expected[32];
        snprintf(expected, sizeof(expected), "call_%u", i);
        assert(
            !strcmp(yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "call_id")), expected));
        yyjson_doc_free(doc);
    }
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    assert(sqlite3_prepare_v2(f->engine->db,
                              "SELECT operations.call_id FROM events JOIN operations "
                              "ON operations.id=events.operation_id "
                              "WHERE type IN ('tool.completed','tool.late_result',"
                              "'tool.interrupted') "
                              "ORDER BY events.seq",
                              -1, &stmt, NULL) == SQLITE_OK);
    for (unsigned i = 1; i <= count; i++) {
        assert(sqlite3_step(stmt) == SQLITE_ROW);
        char expected[32];
        snprintf(expected, sizeof(expected), "call_%u", i);
        assert(!strcmp((const char *)sqlite3_column_text(stmt, 0), expected));
    }
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    if (f->provider_calls > 1) {
        yyjson_doc *doc = yyjson_read(f->last_request, strlen(f->last_request), 0);
        assert(doc);
        yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(doc), "input"), *item;
        size_t index, size;
        unsigned seen = 0;
        yyjson_arr_foreach(input, index, size, item) {
            const char *type = yyjson_get_str(yyjson_obj_get(item, "type"));
            if (!type || strcmp(type, "function_call_output"))
                continue;
            char expected[32];
            snprintf(expected, sizeof(expected), "call_%u", ++seen);
            assert(!strcmp(yyjson_get_str(yyjson_obj_get(item, "call_id")), expected));
        }
        assert(seen == count);
        yyjson_doc_free(doc);
    }
}

static void concurrent_order_and_errors(void) {
    fixture f = {.names = {"read", "read", "read"},
                 .count = 3,
                 .error_id = 2,
                 .barrier_count = 3,
                 .expected_batch = 3};
    f.partner[1] = 3;
    f.partner[2] = 3;
    f.partner[3] = 1;
    f.wait_finished[1] = 2;
    f.wait_finished[2] = 3;
    for (unsigned id = 1; id <= 3; id++)
        f.expected_starts[id] = 3;
    open_fixture(&f, 3, 16, 10000);
    char sid[AH_ID_CAP];
    assert(run_fixture(&f, sid) == AH_OK);
    assert(f.max_active == 3 && f.executions == 3 && f.provider_calls == 2);
    assert(f.finish_order[0] == 3 && f.finish_order[1] == 2 && f.finish_order[2] == 1);
    assert(f.receipts == 3 && !f.late_receipts);
    assert(scalar(&f, "SELECT COUNT(*) FROM operations WHERE status='done' AND is_error=1") == 1);
    assert_receipt_order(&f, 3);
    close_fixture(&f);
}

static void write_barrier(void) {
    fixture f = {.names = {"read", "read", "write", "read", "read"}, .count = 5};
    f.partner[1] = 2;
    f.partner[2] = 1;
    f.partner[4] = 5;
    f.partner[5] = 4;
    f.expected_starts[1] = f.expected_starts[2] = 2;
    f.expected_starts[4] = f.expected_starts[5] = 5;
    open_fixture(&f, 4, 16, 10000);
    char sid[AH_ID_CAP];
    assert(run_fixture(&f, sid) == AH_OK);
    assert(f.max_active == 2 && f.executions == 5 && f.provider_calls == 2);
    assert_receipt_order(&f, 5);
    close_fixture(&f);
}

static void opt_out(void) {
    for (unsigned mode = 0; mode < 3; mode++) {
        fixture f = {.names = {"read", "read"}, .count = 2, .expect_owner = true};
        if (mode == 1)
            f.names[0] = f.names[1] = "serial";
        open_fixture(&f, mode == 2 ? 0 : (mode ? 4 : 1), 16, 10000);
        char sid[AH_ID_CAP];
        assert(run_fixture(&f, sid) == AH_OK);
        assert(f.max_active == 1 && f.executions == 2);
        close_fixture(&f);
    }
}

static void invalid_and_unavailable_barriers(void) {
    for (unsigned invalid = 0; invalid < 2; invalid++) {
        fixture f = {.names = {"read", "read", "missing", "read", "read"}, .count = 5};
        if (invalid) {
            f.names[2] = "read";
            f.invalid_id = 3;
        }
        f.partner[1] = 2;
        f.partner[2] = 1;
        f.partner[4] = 5;
        f.partner[5] = 4;
        f.minimum_receipts[4] = f.minimum_receipts[5] = 3;
        open_fixture(&f, 4, 16, 10000);
        char sid[AH_ID_CAP];
        assert(run_fixture(&f, sid) == AH_OK);
        assert(f.executions == 4 && !f.entered[3] && f.provider_calls == 2);
        assert(scalar(&f, "SELECT COUNT(*) FROM operations WHERE is_error=1") == 1);
        assert_receipt_order(&f, 5);
        close_fixture(&f);
    }
}

static void cancellation_and_deadline(void) {
    for (unsigned deadline = 0; deadline < 2; deadline++) {
        fixture f = {.names = {"read", "read", "read"}, .count = 3};
        f.partner[1] = f.partner[2] = 3;
        f.partner[3] = 1;
        f.wait_finished[1] = f.wait_finished[2] = 3;
        f.cancel_id = deadline ? 0 : 3;
        f.wait_deadline = deadline != 0;
        open_fixture(&f, 3, 16, deadline ? 50 : 10000);
        char sid[AH_ID_CAP];
        assert(run_fixture(&f, sid) == (deadline ? AH_LIMIT : AH_CANCELLED));
        assert(f.executions == 3 && f.provider_calls == 1 && f.receipts == 3);
        assert(f.late_receipts == 3);
        assert(scalar(&f, "SELECT COUNT(*) FROM operations WHERE status='executing'") == 0);
        assert(scalar(&f, "SELECT state FROM sessions") == AH_STOPPED);
        assert_receipt_order(&f, 3);
        close_fixture(&f);
    }
}

static void cancelled_before_launch(void) {
    fixture f = {.names = {"read", "read"}, .count = 2, .cancel_before_launch = true};
    open_fixture(&f, 2, 16, 10000);
    char sid[AH_ID_CAP];
    assert(run_fixture(&f, sid) == AH_CANCELLED);
    assert(!f.executions && f.provider_calls == 1 && f.receipts == 2 && f.late_receipts == 2);
    assert_receipt_order(&f, 2);
    close_fixture(&f);
}

static void budget_boundary(void) {
    fixture f = {.names = {"read", "read", "read"}, .count = 3};
    f.partner[1] = 2;
    f.partner[2] = 1;
    open_fixture(&f, 4, 2, 10000);
    char sid[AH_ID_CAP];
    assert(run_fixture(&f, sid) == AH_LIMIT);
    assert(f.executions == 2 && !f.entered[3] && f.provider_calls == 1);
    assert(scalar(&f, "SELECT tool_calls FROM sessions") == 2);
    assert(scalar(&f, "SELECT COUNT(*) FROM operations WHERE status='pending'") == 1);
    assert_receipt_order(&f, 2);
    close_fixture(&f);
}

static void approval_and_input_barriers(void) {
    const char *barriers[] = {"approved", "harness_ask", "harness_wait"};
    for (unsigned kind = 0; kind < 3; kind++) {
        fixture f = {.names = {"read", "read", barriers[kind], "read"}, .count = 4};
        f.partner[1] = 2;
        f.partner[2] = 1;
        open_fixture(&f, 4, 16, 10000);
        char sid[AH_ID_CAP];
        assert(run_fixture(&f, sid) == AH_OK);
        assert(f.executions == 2 && !f.entered[3] && !f.entered[4] && f.provider_calls == 1);
        assert(scalar(&f, "SELECT state FROM sessions") ==
               (kind ? AH_WAITING_INPUT : AH_WAITING_APPROVAL));
        close_fixture(&f);
    }
}

int main(void) {
    concurrent_order_and_errors();
    write_barrier();
    opt_out();
    invalid_and_unavailable_barriers();
    cancellation_and_deadline();
    cancelled_before_launch();
    budget_boundary();
    approval_and_input_barriers();
    parallel_recovery_cases();
    puts("parallel tests passed");
    return 0;
}
