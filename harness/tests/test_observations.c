#include "core/core.h"
#include <assert.h>
#include <stdio.h>

typedef struct {
    unsigned step, reads, full, reused;
    bool force, change, preview;
} fixture;
static ah_code read_tool(void *context, const ah_tool_call *call, ah_tool_result *result,
                         ah_error *error) {
    (void)call;
    (void)error;
    fixture *f = context;
    f->reads++;
    char content[4097];
    memset(content, f->change && f->reads % 2 == 0 ? 'B' : 'A', sizeof(content) - 1);
    content[4096] = 0;
    char revision[65];
    ah_hash_bytes(content, 4096, revision);
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_bool(doc, root, "ok", true);
    yyjson_mut_obj_add_str(doc, root, "path", "source.txt");
    yyjson_mut_obj_add_strcpy(doc, root, "revision", revision);
    yyjson_mut_obj_add_strcpy(doc, root, "content", content);
    yyjson_mut_obj_add_uint(doc, root, "bytes", 4096);
    result->json = ah_mut_write(doc);
    ah_mut_doc_free(doc);
    return AH_OK;
}
static ah_code model(void *context, const char *request, ah_delta_sink delta, void *dc,
                     ah_cancel_check cancelled, void *cc, char **response, ah_error *error) {
    (void)delta;
    (void)dc;
    (void)cancelled;
    (void)cc;
    (void)error;
    fixture *f = context;
    unsigned step = f->step++ % 3;
    if (step) {
        yyjson_doc *doc = yyjson_read(request, strlen(request), 0);
        yyjson_val *input = yyjson_obj_get(yyjson_doc_get_root(doc), "input");
        yyjson_val *last = yyjson_arr_get(input, yyjson_arr_size(input) - 1);
        const char *output = yyjson_get_str(yyjson_obj_get(last, "output"));
        assert(output);
        yyjson_doc *out = yyjson_read(output, strlen(output), 0);
        yyjson_val *result = yyjson_obj_get(yyjson_doc_get_root(out), "result");
        if (f->preview)
            assert(yyjson_is_true(yyjson_obj_get(result, "truncated")));
        else if (step == 1 || f->force || f->change) {
            assert(yyjson_get_len(yyjson_obj_get(result, "content")) == 4096);
            f->full++;
        } else {
            assert(yyjson_is_true(yyjson_obj_get(result, "reused")));
            assert(!yyjson_obj_get(result, "content"));
            f->reused++;
        }
        yyjson_doc_free(out);
        yyjson_doc_free(doc);
    }
    if (step == 2) {
        *response = ah_strdup(
            "{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
            "\"content\":[{\"type\":\"output_text\",\"text\":\"done\"}]}]}");
        return AH_OK;
    }
    char id[32];
    snprintf(id, sizeof(id), "read-%u", f->step);
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "status", "completed");
    yyjson_mut_val *array = yyjson_mut_obj_add_arr(doc, root, "output"),
                   *call = yyjson_mut_obj(doc);
    yyjson_mut_arr_append(array, call);
    yyjson_mut_obj_add_str(doc, call, "type", "function_call");
    yyjson_mut_obj_add_strcpy(doc, call, "call_id", id);
    yyjson_mut_obj_add_str(doc, call, "name", "read_file");
    yyjson_mut_obj_add_str(doc, call, "arguments",
                           step == 1 && f->force ? "{\"force\":true}" : "{}");
    *response = ah_mut_write(doc);
    ah_mut_doc_free(doc);
    return AH_OK;
}
static void run_mode(bool force, bool change, bool preview) {
    fixture f = {.force = force, .change = change, .preview = preview};
    ah_config config;
    ah_config_init(&config);
    config.database_path = ":memory:";
    config.provider = (ah_provider){model, &f};
    if (preview)
        config.max_model_tool_bytes = 1024;
    ah_error error = {0};
    ah_engine *e = NULL;
    assert(ah_open(&config, &e, &error) == AH_OK);
    ah_tool tool = {"read_file",
                    "test source",
                    "{\"type\":\"object\",\"properties\":{\"force\":{\"type\":\"boolean\"}},"
                    "\"additionalProperties\":false}",
                    AH_EFFECT_READ,
                    false,
                    read_tool,
                    &f,
                    false};
    assert(ah_register_tool(e, &tool, &error) == AH_OK);
    char sid[AH_ID_CAP], forked[AH_ID_CAP];
    assert(ah_session_create(e, "mock", "", sid, &error) == AH_OK);
    for (unsigned i = 0; i < 3; i++) {
        const char *id = i == 2 ? forked : sid;
        if (i == 1)
            assert(ah_compact(e, sid, "content no longer in context", 0, &error) == AH_OK);
        if (i == 2)
            assert(ah_fork(e, sid, forked, &error) == AH_OK);
        assert(ah_submit(e, id, i == 1 ? "second" : "fresh", "read twice", &error) == AH_OK);
        assert(ah_run(e, id, &error) == AH_OK);
    }
    assert(f.reads == 6);
    if (!force && !change && !preview)
        assert(f.full == 3 && f.reused == 3);
    if (force || change)
        assert(f.full == 6 && !f.reused);
    ah_close(e);
}
int main(void) {
    char digest[65];
    ah_hash_bytes("", 0, digest);
    assert(!strcmp(digest, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    ah_hash hash;
    ah_hash_init(&hash);
    ah_hash_update(&hash, "a", 1);
    ah_hash_update(&hash, "bc", 2);
    ah_hash_finish(&hash, digest);
    assert(!strcmp(digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    ah_hash_init(&hash);
    for (unsigned i = 0; i < 1000000; i++)
        ah_hash_update(&hash, "a", 1);
    ah_hash_finish(&hash, digest);
    assert(!strcmp(digest, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    run_mode(false, false, false);
    run_mode(true, false, false);
    run_mode(false, true, false);
    run_mode(false, false, true);
    puts("observation tests passed");
    return 0;
}
