#include "parallel_test.h"
#include <assert.h>
#include <stdio.h>
#include <unistd.h>

static void remove_database(const char *path) {
    assert(unlink(path) == 0);
    char lock[128];
    snprintf(lock, sizeof(lock), "%s.lock", path);
    assert(unlink(lock) == 0);
}

static void pending_is(fixture *f, const char *sid, unsigned call) {
    char *json = NULL;
    ah_error error = {0};
    ok(ah_session_json(f->engine, sid, &json, &error), &error);
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    assert(doc);
    yyjson_val *pending = yyjson_obj_get(yyjson_doc_get_root(doc), "pending");
    char expected[32];
    snprintf(expected, sizeof(expected), "call_%u", call);
    assert(!strcmp(yyjson_get_str(yyjson_obj_get(pending, "call_id")), expected));
    assert(!strcmp(yyjson_get_str(yyjson_obj_get(pending, "status")), "unknown"));
    yyjson_doc_free(doc);
    free(json);
}

static void resolve_call(fixture *f, const char *sid, unsigned call) {
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(f->engine->db, "SELECT id FROM operations WHERE call_id=?", -1, &stmt,
                              NULL) == SQLITE_OK);
    char call_id[32], id[AH_ID_CAP];
    snprintf(call_id, sizeof(call_id), "call_%u", call);
    sqlite3_bind_text(stmt, 1, call_id, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    snprintf(id, sizeof(id), "%s", (const char *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    ah_error error = {0};
    ok(ah_resolve(f->engine, sid, id, "{}", false, &error), &error);
    ok(ah_resolve(f->engine, sid, id, "{}", false, &error), &error);
    assert(ah_resolve(f->engine, sid, id, "{\"conflict\":true}", false, &error) == AH_INVALID);
    assert(ah_resolve(f->engine, sid, id, "{}", true, &error) == AH_INVALID);
}

static void unknown_results_survive_restart(void) {
    for (unsigned mode = 0; mode < 4; mode++) {
        char path[] = "/tmp/ah-parallel-unknown-XXXXXX";
        int fd = mkstemp(path);
        assert(fd >= 0);
        close(fd);
        fixture f = {.names = {"read", "read", "read", "read"},
                     .count = 4,
                     .database_path = path,
                     .barrier_count = 4};
        f.unknown[2] = true;
        f.unknown[3] = mode >= 2;
        if (mode == 1) {
            f.cancel_id = 2;
            f.wait_finished[3] = 2;
            f.wait_finished[4] = 3;
        }
        open_fixture(&f, 4, 4, 10000);
        char sid[AH_ID_CAP];
        assert(run_fixture(&f, sid) == AH_UNCERTAIN);
        assert(f.executions == 4 && f.provider_calls == 1 && f.receipts == 1);
        assert(scalar(&f, "SELECT tool_calls FROM sessions") == 4);
        assert(scalar(&f, "SELECT COUNT(*) FROM operations WHERE status LIKE 'received%'") ==
               (mode >= 2 ? 1 : 2));
        reopen_fixture(&f);
        pending_is(&f, sid, 2);
        unsigned first = mode == 3 ? 3 : 2;
        resolve_call(&f, sid, first);
        if (mode >= 2) {
            unsigned second = first == 2 ? 3 : 2;
            assert(scalar(&f, "SELECT state FROM sessions") == AH_OUTCOME_UNKNOWN);
            pending_is(&f, sid, second);
            reopen_fixture(&f);
            pending_is(&f, sid, second);
            resolve_call(&f, sid, second);
        }
        assert(f.receipts == 1);
        reopen_fixture(&f);
        ah_error error = {0};
        ok(ah_run(f.engine, sid, &error), &error);
        assert(f.executions == 4 && f.provider_calls == 2 && f.receipts == 4);
        assert(scalar(&f, "SELECT tool_calls FROM sessions") == 4);
        assert(scalar(&f, "SELECT COUNT(*) FROM operations WHERE status='done'") == 4);
        assert_receipt_order(&f, 4);
        if (mode == 1)
            assert(f.late_receipts >= 2);
        resolve_call(&f, sid, first);
        assert(f.receipts == 4);
        close_fixture(&f);
        remove_database(path);
    }
}

static void failed_intent_observation_recovers(void) {
    for (unsigned parallel = 1; parallel <= 2; parallel++) {
        char path[] = "/tmp/ah-parallel-observer-XXXXXX";
        int fd = mkstemp(path);
        assert(fd >= 0);
        close(fd);
        fixture f = {
            .names = {"read", "read"}, .count = 2, .database_path = path, .interrupt_intent = true};
        open_fixture(&f, parallel, 2, 10000);
        char sid[AH_ID_CAP];
        assert(run_fixture(&f, sid) == AH_IO);
        assert(!f.executions && f.provider_calls == 1);
        assert(scalar(&f, "SELECT state FROM sessions") == AH_RUNNING);
        assert(scalar(&f, "SELECT COUNT(*) FROM operations WHERE status='executing'") ==
               (int)parallel);
        reopen_fixture(&f);
        assert(scalar(&f, "SELECT state FROM sessions") == AH_STOPPED);
        assert(scalar(&f, "SELECT COUNT(*) FROM operations WHERE status='executing'") == 0);
        ah_error error = {0};
        ok(ah_resume(f.engine, sid, &error), &error);
        ok(ah_run(f.engine, sid, &error), &error);
        assert(f.executions == 2 - parallel && !f.entered[1] && f.provider_calls == 2);
        assert(scalar(&f, "SELECT tool_calls FROM sessions") == 2);
        assert_receipt_order(&f, 2);
        close_fixture(&f);
        remove_database(path);
    }
}

static void failed_pause_observation_preserves_wait(void) {
    const char *names[] = {"approved", "harness_ask", "harness_wait"};
    for (unsigned kind = 0; kind < 3; kind++) {
        fixture f = {.names = {names[kind]},
                     .count = 1,
                     .interrupt_event = kind ? "input.requested" : "approval.requested"};
        open_fixture(&f, 1, 16, 10000);
        char sid[AH_ID_CAP];
        assert(run_fixture(&f, sid) == AH_IO);
        assert(scalar(&f, "SELECT state FROM sessions") ==
               (kind ? AH_WAITING_INPUT : AH_WAITING_APPROVAL));
        ah_error error = {0};
        ok(ah_run(f.engine, sid, &error), &error);
        assert(f.provider_calls == 1 && !f.executions);
        core_operation op;
        ok(core_next_operation(f.engine, sid, &op, &error), &error);
        if (kind)
            ok(ah_reply(f.engine, sid, op.id, "{}", &error), &error);
        else
            ok(ah_decide(f.engine, sid, op.id, true, &error), &error);
        core_operation_free(&op);
        ok(ah_run(f.engine, sid, &error), &error);
        assert(f.provider_calls == 2 && f.executions == (kind ? 0U : 1U));
        assert(scalar(&f, "SELECT state FROM sessions") == AH_COMPLETED);
        close_fixture(&f);
    }
}

static void failed_cancellation_receipt_recovers(void) {
    char path[] = "/tmp/ah-parallel-cancel-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    fixture f = {
        .names = {"read"}, .count = 1, .database_path = path, .cancel_before_launch = true};
    open_fixture(&f, 1, 1, 10000);
    ah_error error = {0};
    ok(core_sql(f.engine,
                "CREATE TRIGGER fail_receipt BEFORE UPDATE ON operations "
                "WHEN NEW.status='done' BEGIN SELECT RAISE(ABORT,'receipt failure'); END",
                &error),
       &error);
    char sid[AH_ID_CAP];
    assert(run_fixture(&f, sid) == AH_INVALID);
    assert(!f.executions && scalar(&f, "SELECT state FROM sessions") == AH_RUNNING);
    assert(scalar(&f, "SELECT COUNT(*) FROM operations WHERE status='executing'") == 1);
    ok(core_sql(f.engine, "DROP TRIGGER fail_receipt", &error), &error);
    reopen_fixture(&f);
    assert(scalar(&f, "SELECT state FROM sessions") == AH_STOPPED);
    ok(ah_resume(f.engine, sid, &error), &error);
    ok(ah_run(f.engine, sid, &error), &error);
    assert(!f.executions && f.provider_calls == 2);
    assert_receipt_order(&f, 1);
    close_fixture(&f);
    remove_database(path);
}

void parallel_recovery_cases(void) {
    unknown_results_survive_restart();
    failed_intent_observation_recovers();
    failed_pause_observation_preserves_wait();
    failed_cancellation_receipt_recovers();
}
