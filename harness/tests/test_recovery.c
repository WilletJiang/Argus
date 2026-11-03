#include "core/core.h"
#include <assert.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static bool crash_child;
static bool late_call;
static unsigned executions;

static ah_code model(void *context, const char *request, ah_delta_sink delta, void *delta_ctx,
                     ah_cancel_check cancelled, void *cancel_ctx, char **out, ah_error *error) {
    (void)context;
    (void)request;
    (void)delta;
    (void)delta_ctx;
    (void)cancelled;
    (void)cancel_ctx;
    (void)error;
    if (late_call) {
        late_call = false;
        *out =
            ah_strdup("{\"status\":\"completed\",\"output\":[{\"type\":\"function_call\","
                      "\"name\":\"migration_read\",\"call_id\":\"late\",\"arguments\":\"{}\"}]}");
        assert(*out);
        return AH_OK;
    }
    *out = ah_strdup(crash_child ? "{\"status\":\"completed\",\"output\":["
                                   "{\"type\":\"function_call\",\"name\":\"read\",\"call_id\":"
                                   "\"a\",\"arguments\":\"{}\"},"
                                   "{\"type\":\"function_call\",\"name\":\"read\",\"call_id\":"
                                   "\"b\",\"arguments\":\"{}\"},"
                                   "{\"type\":\"function_call\",\"name\":\"read\",\"call_id\":"
                                   "\"c\",\"arguments\":\"{}\"},"
                                   "{\"type\":\"function_call\",\"name\":\"read\",\"call_id\":"
                                   "\"d\",\"arguments\":\"{}\"}]}"
                                 : "{\"status\":\"completed\",\"output\":[]}");
    assert(*out);
    return AH_OK;
}
static ah_code execute(void *context, const ah_tool_call *call, ah_tool_result *result,
                       ah_error *error) {
    (void)context;
    (void)call;
    (void)result;
    (void)error;
    if (crash_child)
        _exit(77);
    executions++;
    abort(); /* Recovery must never silently retry a read. */
}
static void ok(ah_code code, ah_error *error) {
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
    ah_tool tool = {.name = "read",
                    .description = "Crash during a read batch",
                    .parameters_json = "{\"type\":\"object\"}",
                    .effect = AH_EFFECT_READ,
                    .execute = execute,
                    .parallel_safe = true};
    ok(ah_register_tool(engine, &tool, &error), &error);
    return engine;
}
static int scalar(ah_engine *engine, const char *sql) {
    sqlite3_stmt *s = NULL;
    assert(sqlite3_prepare_v2(engine->db, sql, -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    int value = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return value;
}
static ah_code migration_read(void *context, const ah_tool_call *call, ah_tool_result *result,
                              ah_error *error) {
    (void)context;
    (void)call;
    (void)error;
    result->json = ah_strdup("\"created after the first fork\"");
    assert(result->json);
    return AH_OK;
}
static void result_access(ah_engine *engine, const char *session, const char *call_id,
                          bool allowed) {
    sqlite3_stmt *s = NULL;
    assert(sqlite3_prepare_v2(engine->db, "SELECT id,result FROM operations WHERE call_id=?", -1,
                              &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, call_id, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(s) == SQLITE_ROW);
    char arguments[128];
    snprintf(arguments, sizeof(arguments), "{\"operation_id\":\"%s\"}", sqlite3_column_text(s, 0));
    char *expected = ah_strdup((const char *)sqlite3_column_text(s, 1));
    assert(expected);
    sqlite3_finalize(s);
    ah_tool_call call = {.session_id = session, .arguments_json = arguments, .output_limit = 65536};
    ah_tool_result result = {0};
    ah_error error = {0};
    ah_code rc = core_read_result(engine, &call, &result, &error);
    assert(rc == (allowed ? AH_OK : AH_NOT_FOUND));
    if (allowed) {
        yyjson_doc *doc = yyjson_read(result.json, strlen(result.json), 0);
        assert(doc);
        const char *content = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "content"));
        assert(content && !strcmp(content, expected));
        yyjson_doc_free(doc);
    }
    free(expected);
    free(result.json);
}
static void migration(ah_engine *engine, const char *path, const char *source) {
    ah_error error = {0};
    char child[AH_ID_CAP], grandchild[AH_ID_CAP], later_child[AH_ID_CAP];
    ok(ah_fork(engine, source, child, &error), &error);
    ok(ah_fork(engine, child, grandchild, &error), &error);
    ah_tool tool = {.name = "migration_read",
                    .description = "Produce a result after an earlier fork",
                    .parameters_json = "{\"type\":\"object\"}",
                    .effect = AH_EFFECT_READ,
                    .execute = migration_read};
    ok(ah_register_tool(engine, &tool, &error), &error);
    late_call = true;
    ok(ah_submit(engine, source, "late", "Another read.", &error), &error);
    ok(ah_run(engine, source, &error), &error);
    ok(ah_fork(engine, source, later_child, &error), &error);
    /* Recreate version 1, including forks that owned no executable operations. */
    ok(core_sql(engine,
                "DROP TABLE result_refs; DROP INDEX operations_session; PRAGMA user_version=1",
                &error),
       &error);
    ah_close(engine);
    engine = open_engine(path);
    assert(scalar(engine, "PRAGMA user_version") == 2);
    assert(scalar(engine, "SELECT count(*) FROM operations") == 5);
    assert(scalar(engine, "SELECT count(*) FROM result_refs") == 18);
    result_access(engine, source, "a", true);
    result_access(engine, child, "a", true);
    result_access(engine, grandchild, "a", true);
    result_access(engine, source, "late", true);
    result_access(engine, later_child, "late", true);
    result_access(engine, child, "late", false);
    result_access(engine, grandchild, "late", false);

    sqlite3_stmt *s = NULL;
    ok(core_prepare(engine,
                    "UPDATE events SET json='{}' WHERE type='session.forked' AND session_id=?", &s,
                    &error),
       &error);
    sqlite3_bind_text(s, 1, grandchild, -1, SQLITE_TRANSIENT);
    ok(core_done(engine, s, &error), &error);
    ah_close(engine);
    engine = open_engine(path); /* Version 2 must not replay old lineage. */
    result_access(engine, grandchild, "a", true);
    ok(core_sql(engine,
                "DROP TABLE result_refs; DROP INDEX operations_session; PRAGMA user_version=1",
                &error),
       &error);
    ah_close(engine);

    ah_config config;
    ah_config_init(&config);
    config.database_path = path;
    config.provider.complete = model;
    engine = NULL;
    assert(ah_open(&config, &engine, &error) == AH_PROTOCOL);
    assert(!engine && strstr(error.message, "lineage"));
    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    ah_engine view = {.db = db};
    assert(scalar(&view, "PRAGMA user_version") == 1);
    assert(scalar(&view, "SELECT count(*) FROM sqlite_master WHERE "
                         "name IN('result_refs','operations_session')") == 0);
    assert(scalar(&view, "SELECT count(*) FROM operations") == 5);
    assert(sqlite3_exec(db, "PRAGMA user_version=3", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
    assert(ah_open(&config, &engine, &error) == AH_PROTOCOL);
    assert(!engine && strstr(error.message, "newer"));
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    view.db = db;
    assert(scalar(&view, "PRAGMA user_version") == 3);
    assert(scalar(&view, "SELECT count(*) FROM sqlite_master WHERE name='result_refs'") == 0);
    assert(sqlite3_close(db) == SQLITE_OK);
}
int main(int argc, char **argv) {
    if (argc == 3) {
        crash_child = true;
        ah_engine *engine = open_engine(argv[1]);
        ah_error error = {0};
        ah_run(engine, argv[2], &error);
        return 88;
    }
    char path[] = "/tmp/ah-recovery-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    ah_engine *engine = open_engine(path);
    ah_error error = {0};
    char session[AH_ID_CAP];
    ok(ah_session_create(engine, "mock", "", session, &error), &error);
    ok(ah_submit(engine, session, "cmd", "Read four files.", &error), &error);
    ah_close(engine);
    pid_t child;
    char *arguments[] = {argv[0], path, session, NULL};
    assert(posix_spawn(&child, argv[0], NULL, NULL, arguments, environ) == 0);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 77);
    /* Older runtimes could label the session failed after committed intents
     * when observer replay failed. Durable executing intents still need recovery. */
    sqlite3 *interrupted = NULL;
    assert(sqlite3_open(path, &interrupted) == SQLITE_OK);
    char mark_failed[80];
    snprintf(mark_failed, sizeof(mark_failed), "UPDATE sessions SET state=%d", AH_FAILED);
    assert(sqlite3_exec(interrupted, mark_failed, NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(interrupted) == SQLITE_OK);
    engine = open_engine(path);
    assert(scalar(engine, "SELECT count(*) FROM operations WHERE status='done' AND is_error=1") ==
           4);
    assert(scalar(engine, "SELECT count(*) FROM events WHERE type='tool.interrupted'") == 4);
    assert(scalar(engine, "SELECT count(*) FROM result_refs") == 4);
    assert(scalar(engine, "SELECT tool_calls FROM sessions") == 4);
    assert(scalar(engine, "SELECT state FROM sessions") == AH_STOPPED);
    ok(ah_resume(engine, session, &error), &error);
    ok(ah_run(engine, session, &error), &error);
    assert(executions == 0);
    migration(engine, path, session);
    assert(unlink(path) == 0);
    char lock[128];
    snprintf(lock, sizeof(lock), "%s.lock", path);
    assert(unlink(lock) == 0);
    puts("batch recovery and migration tests passed");
    return 0;
}
