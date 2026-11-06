#include "core.h"

#define MAX_PARALLEL_TOOLS 16

typedef struct {
    ah_engine *engine;
    core_operation operation;
    core_tool *registered;
    ah_tool_call call;
    ah_tool_result result;
    ah_error error;
    ah_code executed;
    atomic_bool *aborted;
    pthread_t thread;
    bool started, late;
    uint64_t began, ended;
} read_worker;

static bool worker_cancelled(void *context) {
    read_worker *worker = context;
    return atomic_load_explicit(&worker->engine->cancelled, memory_order_relaxed) ||
           atomic_load_explicit(worker->aborted, memory_order_relaxed) ||
           ah_now_ms() >= worker->call.deadline_ms;
}

static void *execute_read(void *context) {
    read_worker *worker = context;
    worker->began = ah_now_us();
    if (worker_cancelled(worker))
        worker->executed = ah_fail(&worker->error, AH_CANCELLED, "cancelled before execution");
    else
        worker->executed = worker->registered->tool.execute(
            worker->registered->tool.context, &worker->call, &worker->result, &worker->error);
    worker->late = worker_cancelled(worker);
    worker->ended = ah_now_us();
    return NULL;
}

static bool eligible(ah_engine *e, read_worker *worker) {
    core_operation *op = &worker->operation;
    core_tool *tool = core_find_tool(e, op->name);
    if (strcmp(op->status, "pending") || op->effect != AH_EFFECT_READ || op->approval ||
        core_builtin_schema(op->name) || !tool || !tool->tool.parallel_safe ||
        tool->tool.effect != op->effect || tool->tool.requires_approval != op->approval ||
        !op->schema || strcmp(tool->schema, op->schema))
        return false;
    ah_error validation = {0};
    if (ah_schema_validate(tool->schema, op->arguments, &validation) != AH_OK)
        return false;
    worker->registered = tool;
    return true;
}

static ah_code collect_reads(ah_engine *e, const char *sid, read_worker *workers, unsigned limit,
                             unsigned *count, ah_error *err) {
    sqlite3_stmt *stmt = NULL;
    ah_code rc =
        core_prepare(e,
                     "SELECT id FROM operations WHERE session_id=? AND status!='done' "
                     "AND seq>(SELECT seq FROM operations WHERE id=?) ORDER BY seq LIMIT ?",
                     &stmt, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, workers[0].operation.id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, (int)limit - 1);
    while (*count < limit) {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_DONE)
            break;
        if (step != SQLITE_ROW) {
            rc = ah_fail(err, AH_IO, "cannot read pending tool batch");
            break;
        }
        read_worker *worker = &workers[*count];
        rc = core_get_operation(e, sid, (const char *)sqlite3_column_text(stmt, 0),
                                &worker->operation, err);
        if (rc != AH_OK)
            break;
        if (!eligible(e, worker)) {
            core_operation_free(&worker->operation);
            break;
        }
        (*count)++;
    }
    sqlite3_finalize(stmt);
    return rc;
}

static ah_code commit_intents(ah_engine *e, core_session *session, read_worker *workers,
                              unsigned count, ah_error *err) {
    uint64_t after = 0;
    ah_code rc = core_begin(e, &after, err);
    sqlite3_stmt *stmt = NULL;
    if (rc == AH_OK)
        rc = core_prepare(e, "UPDATE sessions SET tool_calls=tool_calls+? WHERE id=?", &stmt, err);
    if (rc == AH_OK) {
        sqlite3_bind_int(stmt, 1, (int)count);
        sqlite3_bind_text(stmt, 2, session->id, -1, SQLITE_TRANSIENT);
        rc = core_done(e, stmt, err);
    }
    if (rc == AH_OK)
        rc = core_task_charge(e, session->id, false, count, err);
    for (unsigned i = 0; i < count && rc == AH_OK; i++) {
        core_operation *op = &workers[i].operation;
        rc = core_set_operation(e, session->id, op->id, "executing", err);
        if (rc == AH_OK)
            rc = core_event(e, session->id, op->id, "tool.started", op->arguments, err);
    }
    if (rc == AH_OK) {
        rc = core_commit(e, after, err);
        /* Intent can be committed even when observer replay fails. */
        if (rc != AH_OK)
            session->state = AH_OUTCOME_UNKNOWN;
    } else
        core_rollback(e);
    if (rc == AH_OK)
        session->tool_calls += count;
    return rc;
}

static ah_code record_results(ah_engine *e, core_session *session, read_worker *workers,
                              unsigned count, ah_error *err) {
    uint64_t after = 0;
    bool unknown = false;
    ah_code rc = core_begin(e, &after, err);
    for (unsigned i = 0; i < count && rc == AH_OK; i++) {
        read_worker *worker = &workers[i];
        core_operation *op = &worker->operation;
        ah_tool_result *result = &worker->result;
        bool valid = result->json && strlen(result->json) <= e->config.max_output_bytes &&
                     ah_json_valid(result->json);
        if (result->outcome_unknown) {
            unknown = true;
            rc = core_set_operation(e, session->id, op->id, "unknown", err);
            if (rc == AH_OK)
                rc = core_event(e, session->id, op->id, "tool.outcome_unknown",
                                valid ? result->json
                                      : "{\"reason\":\"tool reported an unknown outcome\"}",
                                err);
            continue;
        }
        if (!valid) {
            free(result->json);
            result->json = core_object_string(
                "error", worker->error.message[0] ? worker->error.message
                                                  : "tool returned no valid bounded JSON result");
            result->is_error = true;
        }
        if (!result->json) {
            rc = ah_fail(err, AH_IO, "out of memory recording tool failure");
            break;
        }
        /* Keep the returned data durable, but publish no later model output
         * until preceding unknown outcomes have been resolved. */
        rc = unknown ? core_save_result(e, session->id, op, result->json,
                                        result->is_error || worker->executed != AH_OK,
                                        worker->late ? "received_late" : "received",
                                        worker->late ? "tool.received_late" : "tool.received", err)
                     : core_receipt(e, session->id, op, result->json,
                                    result->is_error || worker->executed != AH_OK,
                                    worker->late ? "tool.late_result" : "tool.completed", err);
    }
    if (rc == AH_OK && unknown)
        rc = core_state(e, session->id, AH_OUTCOME_UNKNOWN, err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    if (rc != AH_OK || unknown)
        session->state = AH_OUTCOME_UNKNOWN;
    if (rc == AH_OK && unknown)
        return ah_fail(err, AH_UNCERTAIN, "tool outcome is unknown; explicit resolution required");
    return rc;
}

ah_code core_execute_parallel(ah_engine *e, core_session *session, const core_operation *first,
                              bool *handled, ah_error *err) {
    *handled = false;
    if (e->config.max_parallel_tools < 2 || session->tool_calls >= e->config.max_tool_calls)
        return AH_OK;
    /* Only new read callbacks consume batch budget. Replaying a saved receipt
     * must still work after the last permitted callback has finished. */
    if (first->effect != AH_EFFECT_READ || strcmp(first->status, "pending") || first->approval)
        return AH_OK;
    unsigned limit = e->config.max_parallel_tools;
    if (limit > MAX_PARALLEL_TOOLS)
        limit = MAX_PARALLEL_TOOLS;
    unsigned remaining = e->config.max_tool_calls - session->tool_calls;
    ah_code allowed = core_task_allow(e, session->id, false, 1, &remaining, err);
    if (allowed != AH_OK)
        return allowed;
    if (limit > remaining)
        limit = remaining;
    if (limit < 2)
        return AH_OK;

    read_worker workers[MAX_PARALLEL_TOOLS] = {0};
    workers[0].operation = *first;
    if (!eligible(e, &workers[0]))
        return AH_OK;
    unsigned count = 1;
    ah_code rc = collect_reads(e, session->id, workers, limit, &count, err);
    if (rc != AH_OK || count < 2)
        goto cleanup;
    *handled = true;
    rc = commit_intents(e, session, workers, count, err);
    if (rc != AH_OK)
        goto cleanup;

    atomic_bool aborted;
    atomic_init(&aborted, false);
    bool launch_failed = false;
    uint64_t now = ah_now_ms();
    uint64_t deadline =
        e->config.tool_timeout_ms > UINT64_MAX - now ? UINT64_MAX : now + e->config.tool_timeout_ms;
    if (e->deadline && deadline > e->deadline)
        deadline = e->deadline;
    for (unsigned i = 0; i < count; i++) {
        read_worker *worker = &workers[i];
        worker->engine = e;
        worker->aborted = &aborted;
        worker->call = (ah_tool_call){.session_id = session->id,
                                      .operation_id = worker->operation.id,
                                      .arguments_json = worker->operation.arguments,
                                      .cancelled = worker_cancelled,
                                      .cancel_context = worker,
                                      .deadline_ms = deadline,
                                      .output_limit = e->config.max_output_bytes,
                                      .model_output_limit = e->config.max_model_tool_bytes};
        if (!launch_failed && pthread_create(&worker->thread, NULL, execute_read, worker) == 0)
            worker->started = true;
        else {
            launch_failed = true;
            atomic_store_explicit(&aborted, true, memory_order_relaxed);
            worker->executed = ah_fail(&worker->error, AH_IO, "cannot start tool worker");
        }
    }
    bool late = false;
    for (unsigned i = 0; i < count; i++) {
        if (workers[i].started)
            pthread_join(workers[i].thread, NULL);
        if (workers[i].started)
            core_metric(e, session->id, workers[i].operation.id, "tool", workers[i].began,
                        workers[i].ended,
                        workers[i].result.json ? strlen(workers[i].result.json) : 0, 1,
                        workers[i].executed);
        late = late || workers[i].late;
    }
    /* Joining all workers keeps callbacks from outliving their call/context and
     * makes this owner the only thread that touches SQLite or emits events. */
    rc = record_results(e, session, workers, count, err);
    if (rc == AH_OK && launch_failed)
        rc = ah_fail(err, AH_IO, "cannot start tool worker; receipts recorded");
    if (rc == AH_OK && (late || core_cancelled(e)))
        rc = ah_fail(err,
                     atomic_load_explicit(&e->cancelled, memory_order_relaxed) ? AH_CANCELLED
                                                                               : AH_LIMIT,
                     "tool batch completed after cancellation or deadline; receipts recorded");
cleanup:
    for (unsigned i = 0; i < count; i++) {
        if (i)
            core_operation_free(&workers[i].operation);
        free(workers[i].result.json);
    }
    return rc;
}
