#include "core/core.h"
#include <assert.h>
#include <stdio.h>
#include <unistd.h>

static ah_code model(void *ctx, const char *request, ah_delta_sink delta, void *dc,
                     ah_cancel_check cancelled, void *cc, char **out, ah_error *err) {
    (void)ctx;
    (void)request;
    (void)delta;
    (void)dc;
    (void)cancelled;
    (void)cc;
    (void)out;
    (void)err;
    abort(); /* This test never performs inference. */
}
static void ok(ah_code code, const ah_error *error) {
    if (code != AH_OK) {
        fprintf(stderr, "%s: %s\n", ah_code_name(code), error->message);
        abort();
    }
}
static ah_engine *open_engine(const char *path) {
    ah_config config;
    ah_config_init(&config);
    config.database_path = path;
    config.provider.complete = model;
    ah_engine *engine = NULL;
    ah_error error = {0};
    ok(ah_open(&config, &engine, &error), &error);
    return engine;
}
static char *request(ah_engine *engine, const char *id) {
    core_session session;
    ah_error error = {0};
    ok(core_load(engine, id, &session, &error), &error);
    char *json = NULL;
    ok(core_request(engine, &session, &json, &error), &error);
    core_session_free(&session);
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    assert(doc);
    const char *key = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "prompt_cache_key"));
    assert(key && !strcmp(key, id) && strlen(key) == AH_ID_CAP - 1);
    yyjson_doc_free(doc);
    return json;
}
int main(void) {
    char path[] = "/tmp/ah-cache-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    ah_engine *engine = open_engine(path);
    ah_error error = {0};
    char id[AH_ID_CAP], fork[AH_ID_CAP], second[AH_ID_CAP];
    ok(ah_session_create(engine, "mock", "Stable instructions", id, &error), &error);
    char *before = request(engine, id);
    ah_close(engine);
    engine = open_engine(path);
    char *after = request(engine, id);
    assert(!strcmp(before, after));
    free(before);
    free(after);
    ok(ah_fork(engine, id, fork, &error), &error);
    assert(strcmp(fork, id));
    free(request(engine, fork));
    ok(ah_session_create(engine, "mock", "Stable instructions", second, &error), &error);
    assert(strcmp(second, id) && strcmp(second, fork));
    free(request(engine, second));
    ok(ah_submit(engine, id, "command", "Work", &error), &error);
    free(request(engine, id));
    uint64_t cursor;
    ok(core_begin(engine, &cursor, &error), &error);
    ok(core_state(engine, id, AH_STOPPED, &error), &error);
    ok(core_commit(engine, cursor, &error), &error);
    ok(ah_compact(engine, id, "Checkpoint", 0, &error), &error);
    free(request(engine, id));
    ok(ah_resume(engine, id, &error), &error);
    free(request(engine, id));
    ah_close(engine);
    assert(unlink(path) == 0);
    char lock_path[80];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    assert(unlink(lock_path) == 0);
    puts("cache keys survive reopen, checkpoint and resume; forks use distinct keys");
    return 0;
}
