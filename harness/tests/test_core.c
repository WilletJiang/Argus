#include "core_test.h"
extern char **environ;
static const char *test_executable;

static void normal_and_idempotence(void) {
    fixture f = {.answers = {call, done, done, done}};
    ah_engine *e = open_engine(&f, ":memory:", NULL);
    register_tool(e, &f, AH_EFFECT_READ, false);
    char id[AH_ID_CAP];
    create_input(e, id);
    ah_error error = {0};
    unsigned before = f.durable;
    ok(ah_events(e, id, UINT64_MAX, fixture_observe, &f, &error), &error);
    assert(f.durable == before);
    ok(ah_submit(e, id, "cmd_1", "first turn", &error), &error);
    assert(f.durable == before);
    assert(ah_submit(e, id, "cmd_1", "different", &error) == AH_INVALID);
    ok(ah_run(e, id, &error), &error);
    state_is(e, id, "completed");
    assert(f.calls == 2 && f.executions == 1 && f.provisional == 2);
    assert(strstr(f.last_request, "function_call_output"));
    assert(strstr(f.last_request, "ciphertext"));
    char fork_id[AH_ID_CAP];
    ok(ah_fork(e, id, fork_id, &error), &error);
    state_is(e, fork_id, "completed");
    ok(ah_submit(e, id, "cmd_2", "second turn", &error), &error);
    ok(ah_run(e, id, &error), &error);
    unsigned events = f.durable;
    ok(ah_compact(e, id, "Earlier work summary", 1, &error), &error);
    assert(f.durable > events);
    ok(ah_submit(e, id, "cmd_3", "third turn", &error), &error);
    ok(ah_run(e, id, &error), &error);
    assert(strstr(f.last_request, "Earlier work summary"));
    assert(strstr(f.last_request, "second turn"));
    assert(!strstr(f.last_request, "first turn"));
    char *list = NULL;
    ok(ah_sessions_json(e, &list, &error), &error);
    assert(strstr(list, fork_id));
    free(list);
    ah_close(e);
    free(f.last_request);
}
static void approval_and_input(void) {
    fixture f = {.answers = {call, ask, done}};
    ah_engine *e = open_engine(&f, ":memory:", NULL);
    register_tool(e, &f, AH_EFFECT_WRITE, true);
    char id[AH_ID_CAP];
    create_input(e, id);
    ah_error error = {0};
    ok(ah_run(e, id, &error), &error);
    state_is(e, id, "waiting_approval");
    assert(f.executions == 0);
    char *op = pending(e, id);
    assert(ah_submit(e, id, "steer", "change course", &error) == AH_BUSY);
    assert(ah_compact(e, id, "bad boundary", 0, &error) == AH_BUSY);
    ok(ah_decide(e, id, op, false, &error), &error);
    ok(ah_decide(e, id, op, false, &error), &error);
    assert(ah_decide(e, id, op, true, &error) == AH_INVALID);
    free(op);
    ok(ah_run(e, id, &error), &error);
    state_is(e, id, "waiting_input");
    assert(f.executions == 0);
    assert(strstr(f.last_request, "approval denied"));
    op = pending(e, id);
    ok(ah_reply(e, id, op, "{\"answer\":\"a\"}", &error), &error);
    ok(ah_reply(e, id, op, "{\"answer\":\"a\"}", &error), &error);
    assert(ah_reply(e, id, op, "null", &error) == AH_INVALID);
    ok(ah_run(e, id, &error), &error);
    state_is(e, id, "completed");
    free(op);
    ah_close(e);
    free(f.last_request);
}
static void cancellations_and_limits(void) {
    ah_error error = {0};
    char id[AH_ID_CAP];
    fixture f = {.answers = {call, done}, .cancel_model = true};
    ah_engine *e = open_engine(&f, ":memory:", NULL);
    register_tool(e, &f, AH_EFFECT_READ, false);
    create_input(e, id);
    assert(ah_run(e, id, &error) == AH_CANCELLED);
    state_is(e, id, "stopped");
    assert(f.executions == 0);
    f.cancel_model = false;
    ok(ah_resume(e, id, &error), &error);
    ok(ah_run(e, id, &error), &error);
    state_is(e, id, "completed");
    ah_close(e);
    free(f.last_request);
    memset(&f, 0, sizeof(f));
    f.answers[0] = call;
    f.answers[1] = done;
    f.cancel_tool = true;
    e = open_engine(&f, ":memory:", NULL);
    register_tool(e, &f, AH_EFFECT_WRITE, false);
    create_input(e, id);
    assert(ah_run(e, id, &error) == AH_CANCELLED);
    state_is(e, id, "stopped");
    assert(f.executions == 1 && f.calls == 1);
    f.cancel_tool = false;
    ok(ah_resume(e, id, &error), &error);
    ok(ah_run(e, id, &error), &error);
    assert(f.executions == 1 && strstr(f.last_request, "function_call_output"));
    ah_close(e);
    free(f.last_request);
    memset(&f, 0, sizeof(f));
    f.answers[0] = done;
    e = open_engine(&f, ":memory:", NULL);
    create_input(e, id);
    ah_cancel(e);
    assert(ah_run(e, id, &error) == AH_CANCELLED);
    assert(f.calls == 0);
    ah_close(e);
    memset(&f, 0, sizeof(f));
    f.answers[0] = call;
    ah_config config;
    ah_config_init(&config);
    config.max_model_calls = 1;
    e = open_engine(&f, ":memory:", &config);
    register_tool(e, &f, AH_EFFECT_READ, false);
    create_input(e, id);
    assert(ah_run(e, id, &error) == AH_LIMIT);
    state_is(e, id, "stopped");
    assert(f.executions == 1);
    ah_close(e);
    free(f.last_request);
}
static void protocol_and_unknown(void) {
    ah_error error = {0};
    char id[AH_ID_CAP];
    fixture f = {.answers = {"{\"status\":\"incomplete\",\"output\":[]}"}};
    ah_engine *e = open_engine(&f, ":memory:", NULL);
    create_input(e, id);
    assert(ah_run(e, id, &error) == AH_PROTOCOL);
    state_is(e, id, "failed");
    ah_close(e);
    free(f.last_request);
    memset(&f, 0, sizeof(f));
    f.answers[0] = call;
    f.answers[1] = done;
    f.unknown = true;
    e = open_engine(&f, ":memory:", NULL);
    register_tool(e, &f, AH_EFFECT_WRITE, false);
    create_input(e, id);
    assert(ah_run(e, id, &error) == AH_UNCERTAIN);
    state_is(e, id, "outcome_unknown");
    assert(ah_resume(e, id, &error) == AH_UNCERTAIN);
    char *op = pending(e, id);
    ok(ah_resolve(e, id, op, "{\"verified\":true}", false, &error), &error);
    ok(ah_run(e, id, &error), &error);
    assert(f.executions == 1);
    free(op);
    ah_close(e);
    free(f.last_request);
}
static void crash_recovery(ah_effect effect) {
    char path[] = "/tmp/ah-core-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    fixture f = {0};
    ah_engine *e = open_engine(&f, path, NULL);
    char id[AH_ID_CAP];
    create_input(e, id);
    ah_close(e);
    pid_t child;
    char effect_string[16];
    snprintf(effect_string, sizeof(effect_string), "%d", (int)effect);
    char *arguments[] = {(char *)test_executable, "--crash-child", path, id, effect_string, NULL};
    assert(posix_spawn(&child, test_executable, NULL, NULL, arguments, environ) == 0);
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 77)
        fprintf(stderr, "Crash child wait status=%d signal=%d exit=%d\n", status,
                WIFSIGNALED(status) ? WTERMSIG(status) : 0,
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 77);
    fixture recovered = {.answers = {done}};
    e = open_engine(&recovered, path, NULL);
    register_tool(e, &recovered, effect, false);
    ah_error error = {0};
    if (effect == AH_EFFECT_WRITE) {
        state_is(e, id, "outcome_unknown");
        assert(ah_run(e, id, &error) == AH_UNCERTAIN);
        char *op = pending(e, id);
        ok(ah_resolve(e, id, op, "{\"reconciled\":true}", false, &error), &error);
        free(op);
    } else {
        state_is(e, id, "stopped");
        ok(ah_resume(e, id, &error), &error);
    }
    ok(ah_run(e, id, &error), &error);
    assert(recovered.executions == 0 && strstr(recovered.last_request, "function_call_output"));
    ah_config config;
    ah_config_init(&config);
    config.database_path = path;
    config.provider = (ah_provider){fixture_complete, &recovered};
    ah_engine *second = NULL;
    assert(ah_open(&config, &second, &error) == AH_BUSY);
    ah_close(e);
    free(recovered.last_request);
    assert(unlink(path) == 0);
    char lock_path[80];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    assert(unlink(lock_path) == 0);
}
static void fork_keeps_call_identity(void) {
    fixture f = {.answers = {call, done, call}};
    ah_engine *e = open_engine(&f, ":memory:", NULL);
    register_tool(e, &f, AH_EFFECT_READ, false);
    char id[AH_ID_CAP], fork_id[AH_ID_CAP];
    create_input(e, id);
    ah_error error = {0};
    ok(ah_run(e, id, &error), &error);
    ok(ah_compact(e, id, "completed prior tool", 0, &error), &error);
    ok(ah_fork(e, id, fork_id, &error), &error);
    ok(ah_submit(e, fork_id, "new", "continue", &error), &error);
    assert(ah_run(e, fork_id, &error) == AH_PROTOCOL);
    assert(f.executions == 1);
    ah_close(e);
    free(f.last_request);
}
static void malformed_responses(void) {
    const char *responses[] = {
        "{\"status\":\"completed\",\"status\":\"failed\",\"output\":[]}",
        "{\"status\":\"completed\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"same\","
        "\"name\":\"test_tool\",\"arguments\":\"{}\"},{\"type\":\"function_call\",\"call_id\":"
        "\"same\",\"name\":\"test_tool\",\"arguments\":\"{}\"}]}",
        ("{\"status\":\"completed\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"nul\","
         "\"name\":\"test_tool\\u0000suffix\",\"arguments\":\"{}\"}]}")};
    const char *malformed_fields[] = {
        "{\"status\":\"completed\",\"output\":[{\"type\":\"function_call\",\"namespace\":17,"
        "\"status\":false,\"name\":\"test_tool\",\"call_id\":\"x\",\"arguments\":\"{}\"}]}",
        "{\"status\":\"completed\",\"id\":17,\"output\":[]}",
        "{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"user\",\"content\":"
        "17}]}",
        "{\"status\":\"completed\",\"output\":[{\"type\":\"reasoning\",\"encrypted_content\":{},"
        "\"summary\":false}]}",
        "{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
        "\"content\":[{\"type\":\"output_text\",\"text\":3}]}]}"};
    for (size_t i = 0; i < sizeof(malformed_fields) / sizeof(malformed_fields[0]); i++) {
        fixture f = {.answers = {malformed_fields[i]}};
        ah_engine *e = open_engine(&f, ":memory:", NULL);
        register_tool(e, &f, AH_EFFECT_WRITE, false);
        char id[AH_ID_CAP];
        create_input(e, id);
        ah_error error = {0};
        assert(ah_run(e, id, &error) == AH_PROTOCOL);
        assert(f.executions == 0);
        ah_close(e);
        free(f.last_request);
    }
    for (size_t i = 0; i < sizeof(responses) / sizeof(responses[0]); i++) {
        fixture f = {.answers = {responses[i]}};
        ah_engine *e = open_engine(&f, ":memory:", NULL);
        register_tool(e, &f, AH_EFFECT_WRITE, false);
        char id[AH_ID_CAP];
        create_input(e, id);
        ah_error error = {0};
        assert(ah_run(e, id, &error) == AH_PROTOCOL);
        assert(f.executions == 0);
        state_is(e, id, "failed");
        ah_close(e);
        free(f.last_request);
    }
    const char *nullable =
        "{\"status\":\"completed\",\"output\":[{\"type\":\"reasoning\",\"status\":null,\"encrypted_"
        "content\":null,\"content\":null,\"summary\":[]},{\"type\":\"function_call\",\"id\":null,"
        "\"status\":null,\"namespace\":null,\"name\":\"test_tool\",\"call_id\":\"nullable_call\","
        "\"arguments\":\"{\\\"value\\\":3}\"}]}";
    fixture valid = {.answers = {nullable, done}};
    ah_engine *valid_engine = open_engine(&valid, ":memory:", NULL);
    register_tool(valid_engine, &valid, AH_EFFECT_READ, false);
    char valid_id[AH_ID_CAP];
    create_input(valid_engine, valid_id);
    ah_error valid_error = {0};
    ok(ah_run(valid_engine, valid_id, &valid_error), &valid_error);
    assert(valid.executions == 1);
    ah_close(valid_engine);
    free(valid.last_request);
    fixture duplicate = {.answers = {call, call}};
    ah_engine *e = open_engine(&duplicate, ":memory:", NULL);
    register_tool(e, &duplicate, AH_EFFECT_READ, false);
    char id[AH_ID_CAP];
    create_input(e, id);
    ah_error error = {0};
    assert(ah_run(e, id, &error) == AH_PROTOCOL);
    assert(duplicate.executions == 1);
    ah_close(e);
    free(duplicate.last_request);
}
static void steering_boundaries(void) {
    for (unsigned with_tool = 0; with_tool < 2; with_tool++) {
        fixture f = {.answers = {with_tool ? call : done, done}, .steer = true};
        ah_engine *e = open_engine(&f, ":memory:", NULL);
        register_tool(e, &f, AH_EFFECT_READ, false);
        char id[AH_ID_CAP];
        create_input(e, id);
        f.steering_session = id;
        ah_error error = {0};
        ok(ah_run(e, id, &error), &error);
        state_is(e, id, "completed");
        assert(f.calls == 2);
        assert(strstr(f.last_request, "steer after tools"));
        if (with_tool)
            assert(strstr(f.last_request, "function_call_output") <
                   strstr(f.last_request, "steer after tools"));
        assert(ah_steer(e, id, "late", "too late", &error) == AH_BUSY);
        ah_close(e);
        free(f.last_request);
    }
}
static void approval_contract_change(void) {
    char path[] = "/tmp/ah-core-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    fixture f = {.answers = {call}};
    ah_engine *e = open_engine(&f, path, NULL);
    register_tool(e, &f, AH_EFFECT_WRITE, true);
    char id[AH_ID_CAP];
    create_input(e, id);
    ah_error error = {0};
    ok(ah_run(e, id, &error), &error);
    char *op = pending(e, id);
    ok(ah_decide(e, id, op, true, &error), &error);
    free(op);
    ah_close(e);
    free(f.last_request);
    fixture changed = {.answers = {done}};
    e = open_engine(&changed, path, NULL);
    ah_tool tool = {"test_tool",
                    "Changed contract",
                    "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"number\"}},"
                    "\"required\":[\"value\"],\"additionalProperties\":false}",
                    AH_EFFECT_WRITE,
                    true,
                    fixture_execute,
                    &changed,
                    false};
    ok(ah_register_tool(e, &tool, &error), &error);
    ok(ah_run(e, id, &error), &error);
    assert(changed.executions == 0);
    assert(strstr(changed.last_request, "tool policy changed"));
    ah_close(e);
    free(changed.last_request);
    assert(unlink(path) == 0);
    char lock_path[80];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    assert(unlink(lock_path) == 0);
}
static void receipt_failure_recovery(void) {
    char path[] = "/tmp/ah-core-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    fixture f = {.answers = {call}, .fault_database = path};
    ah_engine *e = open_engine(&f, path, NULL);
    register_tool(e, &f, AH_EFFECT_WRITE, false);
    char id[AH_ID_CAP];
    create_input(e, id);
    ah_error error = {0};
    assert(ah_run(e, id, &error) != AH_OK);
    assert(f.executions == 1);
    state_is(e, id, "running");
    ah_close(e);
    free(f.last_request);
    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    assert(sqlite3_exec(db, "DROP TRIGGER fail_receipt", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
    fixture recovered = {.answers = {done}};
    e = open_engine(&recovered, path, NULL);
    state_is(e, id, "outcome_unknown");
    char *op = pending(e, id);
    ok(ah_resolve(e, id, op, "{\"verified\":true}", false, &error), &error);
    free(op);
    ok(ah_run(e, id, &error), &error);
    assert(recovered.executions == 0);
    ah_close(e);
    free(recovered.last_request);
    assert(unlink(path) == 0);
    char lock_path[80];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    assert(unlink(lock_path) == 0);
}
int main(int argc, char **argv) {
    test_executable = argv[0];
    if (argc == 5 && !strcmp(argv[1], "--crash-child")) {
        fixture crashing = {.answers = {call}, .crash = true};
        ah_engine *engine = open_engine(&crashing, argv[2], NULL);
        register_tool(engine, &crashing, (ah_effect)atoi(argv[4]), false);
        ah_error error = {0};
        ah_code returned = ah_run(engine, argv[3], &error);
        fprintf(stderr, "Crash fixture unexpectedly returned %s: %s\n", ah_code_name(returned),
                error.message);
        ah_close(engine);
        return 88;
    }

    normal_and_idempotence();
    fork_keeps_call_identity();
    malformed_responses();
    steering_boundaries();
    approval_contract_change();
    receipt_failure_recovery();
    approval_and_input();
    cancellations_and_limits();
    protocol_and_unknown();
    crash_recovery(AH_EFFECT_WRITE);
    crash_recovery(AH_EFFECT_READ);
    puts("core tests passed");
    return 0;
}
