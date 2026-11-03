/* Local tool scheduling benchmark. The provider is a mock; no network/inference. */
#include <ah/ah.h>
#include <ah/tools.h>
#include <yyjson.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CALLS 4
#define FILE_BYTES 4096
#define DELAY_MS 20
#define WARMUPS 2
#define MAX_RUNS 100

typedef struct {
    bool files, invalid;
    unsigned models, intents, receipts;
    char operations[CALLS][AH_ID_CAP];
    const char *content;
    char *calls;
} fixture;

static ah_code fail(ah_error *error, const char *message) {
    error->code = AH_PROTOCOL;
    snprintf(error->message, sizeof(error->message), "%s", message);
    return AH_PROTOCOL;
}
static double milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec * 1000 + (double)now.tv_nsec / 1000000;
}
static bool matching_text(yyjson_val *object, const char *key, const char *expected) {
    const char *actual = yyjson_get_str(yyjson_obj_get(object, key));
    return actual && !strcmp(actual, expected);
}
static bool verify_outputs(fixture *f, const char *request) {
    yyjson_doc *doc = yyjson_read(request, strlen(request), 0);
    if (!doc)
        return false;
    yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(doc), "input"), *item;
    size_t index, count;
    unsigned outputs = 0;
    bool valid = true;
    yyjson_arr_foreach(input, index, count, item) {
        if (!matching_text(item, "type", "function_call_output"))
            continue;
        char id[16];
        snprintf(id, sizeof(id), "call_%u", outputs);
        const char *text = yyjson_get_str(yyjson_obj_get(item, "output"));
        yyjson_doc *body = text ? yyjson_read(text, strlen(text), 0) : NULL;
        yyjson_val *root = body ? yyjson_doc_get_root(body) : NULL;
        yyjson_val *result = yyjson_obj_get(root, "result");
        valid = valid && outputs < CALLS && matching_text(item, "call_id", id) && root &&
                yyjson_is_false(yyjson_obj_get(root, "is_error"));
        if (f->files)
            valid = valid && matching_text(result, "content", f->content) &&
                    yyjson_get_uint(yyjson_obj_get(result, "bytes")) == FILE_BYTES &&
                    yyjson_is_false(yyjson_obj_get(result, "truncated"));
        else
            valid = valid && yyjson_is_uint(yyjson_obj_get(result, "index")) &&
                    yyjson_get_uint(yyjson_obj_get(result, "index")) == outputs;
        yyjson_doc_free(body);
        outputs++;
    }
    yyjson_doc_free(doc);
    return valid && outputs == (f->models ? CALLS : 0);
}
static ah_code model(void *context, const char *request, ah_delta_sink delta, void *delta_ctx,
                     ah_cancel_check cancelled, void *cancel_ctx, char **out, ah_error *error) {
    (void)delta;
    (void)delta_ctx;
    fixture *f = context;
    if (cancelled(cancel_ctx))
        return AH_CANCELLED;
    if (f->models > 1 || !verify_outputs(f, request))
        return fail(error, "incorrect tool contents, receipt order, or model-call count");
    static const char done[] =
        "{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
        "\"content\":[{\"type\":\"output_text\",\"text\":\"verified\"}]}]}";
    *out = strdup(f->models++ ? done : f->calls);
    return *out ? AH_OK : AH_IO;
}
static void observe(void *context, const ah_event *event) {
    fixture *f = context;
    if (!strcmp(event->type, "tool.intent")) {
        if (f->intents == CALLS || !event->operation_id ||
            strlen(event->operation_id) != AH_ID_CAP - 1)
            f->invalid = true;
        else
            memcpy(f->operations[f->intents++], event->operation_id, AH_ID_CAP);
    } else if (!strcmp(event->type, "tool.completed")) {
        if (f->receipts >= f->intents || !event->operation_id ||
            strcmp(f->operations[f->receipts], event->operation_id))
            f->invalid = true;
        else
            f->receipts++;
    }
}
static ah_code delayed_read(void *context, const ah_tool_call *call, ah_tool_result *result,
                            ah_error *error) {
    (void)context;
    struct timespec delay = {.tv_nsec = DELAY_MS * 1000000L};
    while (nanosleep(&delay, &delay) < 0)
        if (errno != EINTR)
            return AH_IO;
    if (call->cancelled(call->cancel_context))
        return AH_CANCELLED;
    yyjson_doc *doc = yyjson_read(call->arguments_json, strlen(call->arguments_json), 0);
    yyjson_val *index = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), "index") : NULL;
    if (!yyjson_is_uint(index) || yyjson_get_uint(index) >= CALLS) {
        yyjson_doc_free(doc);
        return fail(error, "invalid synthetic read index");
    }
    char output[32];
    snprintf(output, sizeof(output), "{\"index\":%u}", (unsigned)yyjson_get_uint(index));
    yyjson_doc_free(doc);
    result->json = strdup(output);
    return result->json ? AH_OK : AH_IO;
}
static char *model_calls(bool files) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc)
        return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc), *output = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "status", "completed");
    yyjson_mut_obj_add_val(doc, root, "output", output);
    for (unsigned i = 0; i < CALLS; i++) {
        char id[16], args[128];
        snprintf(id, sizeof(id), "call_%u", i);
        if (files)
            snprintf(args, sizeof(args),
                     "{\"path\":\"fixture.txt\",\"offset_line\":1,\"max_lines\":1000}");
        else
            snprintf(args, sizeof(args), "{\"index\":%u}", i);
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "type", "function_call");
        yyjson_mut_obj_add_str(doc, item, "namespace", "local");
        yyjson_mut_obj_add_str(doc, item, "name", files ? "read_file" : "delayed_read");
        yyjson_mut_obj_add_strcpy(doc, item, "call_id", id);
        yyjson_mut_obj_add_strcpy(doc, item, "arguments", args);
        yyjson_mut_arr_append(output, item);
    }
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}
static int compare(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static void report(const char *store, bool files, unsigned parallel, const double *samples,
                   unsigned count) {
    double sorted[MAX_RUNS];
    memcpy(sorted, samples, count * sizeof(*samples));
    qsort(sorted, count, sizeof(*sorted), compare);
    printf("{\"store\":\"%s\",\"workload\":\"%s\",\"max_parallel_tools\":%u,"
           "\"runs\":%u,\"warmups\":%u,\"calls_per_run\":%u,\"fixture_bytes\":%u,"
           "\"synthetic_delay_ms\":%u,\"timed\":\"submit+run\",\"fresh_session\":true,"
           "\"max_model_tool_bytes\":0,\"network\":false,\"model_inference\":false,"
           "\"correctness_checked\":true,\"ordered_receipts_checked\":%u,"
           "\"p50_ms\":%.3f,\"p95_ms\":%.3f,\"max_ms\":%.3f,\"raw_ms\":[",
           store, files ? "four_reads_same_4KiB_file_warm" : "four_synthetic_latency_reads",
           parallel, count, WARMUPS, CALLS, files ? FILE_BYTES : 0, files ? 0 : DELAY_MS,
           count * CALLS, sorted[(count + 1) / 2 - 1], sorted[(count * 95 + 99) / 100 - 1],
           sorted[count - 1]);
    for (unsigned i = 0; i < count; i++)
        printf("%s%.3f", i ? "," : "", samples[i]);
    puts("]}");
}
static int benchmark(const char *workspace, const char *database, const char *store,
                     const char *content, bool files, unsigned parallel) {
    fixture f = {.files = files, .content = content, .calls = model_calls(files)};
    if (!f.calls)
        return 1;
    ah_config config;
    ah_config_init(&config);
    config.database_path = database;
    config.provider = (ah_provider){model, &f};
    config.observer = observe;
    config.observer_context = &f;
    config.max_parallel_tools = parallel;
    config.max_model_tool_bytes = 0;
    ah_engine *engine = NULL;
    ah_local_tools *local = NULL;
    ah_error error = {0};
    ah_code rc = ah_open(&config, &engine, &error);
    if (rc == AH_OK && files) {
        ah_local_tools_config local_config = {.workspace = workspace};
        rc = ah_local_tools_open(&local_config, &local, &error);
        if (rc == AH_OK)
            rc = ah_local_tools_register(local, engine, &error);
    } else if (rc == AH_OK) {
        ah_tool tool = {.name = "delayed_read",
                        .description = "Synthetic 20ms read latency",
                        .parameters_json =
                            "{\"type\":\"object\",\"properties\":{\"index\":{"
                            "\"type\":\"integer\",\"minimum\":0,\"maximum\":3}},"
                            "\"required\":[\"index\"],\"additionalProperties\":false}",
                        .effect = AH_EFFECT_READ,
                        .execute = delayed_read,
                        .parallel_safe = true};
        rc = ah_register_tool(engine, &tool, &error);
    }
    unsigned count = files ? MAX_RUNS : 40;
    double samples[MAX_RUNS];
    for (unsigned i = 0; i < count + WARMUPS && rc == AH_OK; i++) {
        char session[AH_ID_CAP];
        f.models = f.intents = f.receipts = 0;
        f.invalid = false;
        rc = ah_session_create(engine, "mock", "", session, &error);
        double started = milliseconds();
        if (rc == AH_OK)
            rc = ah_submit(engine, session, "sample", "Run all four independent reads.", &error);
        if (rc == AH_OK)
            rc = ah_run(engine, session, &error);
        double elapsed = milliseconds() - started;
        if (rc == AH_OK &&
            (f.invalid || f.models != 2 || f.intents != CALLS || f.receipts != CALLS))
            rc = fail(&error, "incorrect durable receipt order or count");
        if (i >= WARMUPS)
            samples[i - WARMUPS] = elapsed;
    }
    ah_close(engine);
    ah_local_tools_close(local);
    free(f.calls);
    if (rc != AH_OK) {
        fprintf(stderr, "%s: %s\n", ah_code_name(rc), error.message);
        return 1;
    }
    report(store, files, parallel, samples, count);
    return 0;
}
int main(int argc, char **argv) {
    bool file_store = argc == 2 && !strcmp(argv[1], "--file-store");
    if (argc != 1 && !file_store) {
        fprintf(stderr, "usage: %s [--file-store]\n", argv[0]);
        return 1;
    }
    char root[] = "/tmp/ah-parallel-benchmark-XXXXXX";
    if (!mkdtemp(root))
        return 1;
    char file[256], database[256], lock[272], journal[272], content[FILE_BYTES + 1];
    snprintf(file, sizeof(file), "%s/fixture.txt", root);
    snprintf(database, sizeof(database), "%s/state.db", root);
    snprintf(lock, sizeof(lock), "%s.lock", database);
    snprintf(journal, sizeof(journal), "%s-journal", database);
    for (size_t i = 0; i < FILE_BYTES; i++)
        content[i] = i % 64 == 63 ? '\n' : 'x';
    content[FILE_BYTES] = 0;
    FILE *output = fopen(file, "wb");
    int rc = 1;
    if (output) {
        bool written = fwrite(content, 1, FILE_BYTES, output) == FILE_BYTES;
        int closed = fclose(output);
        if (written && !closed)
            rc = 0;
    }
    for (unsigned store = 0; store <= (unsigned)file_store && !rc; store++)
        for (unsigned workload = 0; workload < 2 && !rc; workload++)
            for (unsigned parallel = 1; parallel <= 4 && !rc; parallel += 3) {
                rc = benchmark(
                    root, store ? database : ":memory:", store ? "sqlite-delete-full" : "memory",
                    content, workload == 0, parallel);
                unlink(database);
                unlink(lock);
                unlink(journal);
            }
    unlink(file);
    unlink(database);
    unlink(lock);
    unlink(journal);
    if (rmdir(root))
        rc = 1;
    return rc;
}
