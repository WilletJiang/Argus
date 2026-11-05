#include "local_internal.h"

const char ah_checks_schema[] =
    "{\"type\":\"object\",\"properties\":{\"checks\":{\"type\":\"array\",\"minItems\":1,"
    "\"maxItems\":8,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":64}}},"
    "\"required\":[\"checks\"],\"additionalProperties\":false}";

ah_code ah_local_phase(const ah_tool_call *call, const char *phase, const char *target,
                       const char *before, const char *after, ah_error *error) {
    if (!call->checkpoint)
        return ah_fail(error, AH_INVALID, "Durable phase checkpoints required");
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "phase", phase);
    yyjson_mut_obj_add_strcpy(doc, root, "target", target);
    if (before)
        yyjson_mut_obj_add_strcpy(doc, root, "before", before);
    if (after)
        yyjson_mut_obj_add_strcpy(doc, root, "after", after);
    char *json = ah_mut_write(doc);
    ah_mut_doc_free(doc);
    ah_code code = json ? call->checkpoint(call->checkpoint_context, json, error) : AH_IO;
    free(json);
    return code;
}

/* Hash complete declared inputs both before and after execution. This is not a
 * filesystem snapshot; a host needing isolation must supply a pinned workspace. */
static ah_code inputs_revision(ah_local_tools *tools, ah_local_check *check,
                               const ah_tool_call *call, char revision[65]) {
    yyjson_doc *doc = yyjson_read(check->inputs, strlen(check->inputs), 0);
    if (!doc)
        return AH_IO;
    ah_hash hash;
    ah_hash_init(&hash);
    ah_hash_update(&hash, check->definition, 64);
    size_t i, n;
    yyjson_val *path;
    ah_code code = AH_OK;
    yyjson_arr_foreach(yyjson_doc_get_root(doc), i, n, path) {
        char content_revision[65];
        code = ah_local_input_revision(tools, path, call, content_revision);
        if (code != AH_OK)
            break;
        ah_hash_update(&hash, yyjson_get_str(path), yyjson_get_len(path) + 1);
        ah_hash_update(&hash, content_revision, 64);
    }
    yyjson_doc_free(doc);
    if (code == AH_OK)
        ah_hash_finish(&hash, revision);
    return code;
}

ah_code ah_local_check_run(ah_local_tools *tools, ah_local_check *check, const ah_tool_call *call,
                           ah_tool_result *result, ah_error *error) {
    memset(result, 0, sizeof(*result));
    char before[65], after[65];
    ah_code code = inputs_revision(tools, check, call, before);
    if (code != AH_OK)
        return ah_local_error(call, result, error, code,
                              "Check input unavailable, changed or exceeds 1 MiB");
    bool cached = check->cacheable && check->cached_result && !strcmp(before, check->cached_input);
    uint64_t started = ah_now_us();
    ah_tool_result execution = {0};
    if (cached)
        execution.json = ah_strdup(check->cached_result);
    else {
        code = ah_local_phase(call, "check.started", check->name, before, NULL, error);
        if (code != AH_OK)
            return ah_local_error(call, result, error, code, "Cannot record check intent");
        ah_tool_call inner = *call;
        inner.arguments_json = check->arguments;
        uint64_t deadline = ah_now_ms() + check->timeout_ms;
        if (!inner.deadline_ms || deadline < inner.deadline_ms)
            inner.deadline_ms = deadline;
        size_t share = ah_local_limit(call) / (tools->check_count ? tools->check_count : 1);
        if (share > 16384)
            share = 16384;
        if (!inner.output_limit || inner.output_limit > share)
            inner.output_limit = share;
        ah_local_tools runner = *tools;
        runner.enable_exec = true; /* Only host-fixed argv, no additional model authority. */
        code = ah_local_exec(&runner, &inner, &execution, error);
    }
    yyjson_doc *parsed =
        execution.json ? yyjson_read(execution.json, strlen(execution.json), 0) : NULL;
    ah_code observed = inputs_revision(tools, check, call, after);
    bool stable = observed == AH_OK && !strcmp(before, after);
    bool passed = code == AH_OK && parsed &&
                  yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(parsed), "ok"));
    const char *status = execution.outcome_unknown  ? "unknown"
                         : !stable                  ? "stale"
                         : code != AH_OK || !parsed ? "error"
                         : passed                   ? "pass"
                                                    : "fail";
    if (!cached) {
        ah_code saved = ah_local_phase(call, "check.finished", check->name, before, status, error);
        if (saved != AH_OK) {
            execution.outcome_unknown = true;
            status = "unknown";
            code = saved;
        }
    }
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_bool(doc, root, "ok", !strcmp(status, "pass"));
    yyjson_mut_obj_add_strcpy(doc, root, "check", check->name);
    yyjson_mut_obj_add_str(doc, root, "status", status);
    yyjson_mut_obj_add_bool(doc, root, "cached", cached);
    yyjson_mut_obj_add_strcpy(doc, root, "definition_revision", check->definition);
    yyjson_mut_obj_add_strcpy(doc, root, "input_revision", before);
    yyjson_mut_obj_add_strcpy(doc, root, "environment_revision", check->environment);
    yyjson_mut_obj_add_uint(doc, root, "duration_us", ah_now_us() - started);
    if (parsed)
        yyjson_mut_obj_add_val(doc, root, "process",
                               yyjson_val_mut_copy(doc, yyjson_doc_get_root(parsed)));
    if (parsed && strcmp(status, "pass"))
        ah_local_diagnostics(tools, check, call, yyjson_doc_get_root(parsed), doc, root);
    result->is_error = strcmp(status, "pass") != 0;
    bool unknown = execution.outcome_unknown;
    yyjson_doc_free(parsed);
    result->outcome_unknown = true;
    ah_code finished = ah_local_finish(call, result, doc, error);
    if (finished == AH_OK)
        result->outcome_unknown = unknown;
    if (unknown || finished != AH_OK) {
        free(check->cached_result);
        check->cached_result = NULL;
        check->cached_input[0] = 0;
    } else if (!cached && stable && passed && check->cacheable) {
        free(check->cached_result);
        check->cached_result = ah_strdup(execution.json);
        memcpy(check->cached_input, before, 65);
    }
    free(execution.json);
    return unknown ? ah_fail(error, code == AH_OK ? AH_UNCERTAIN : code,
                             "Check effects need reconciliation")
                   : finished;
}

ah_code ah_local_check_names(ah_local_tools *tools, yyjson_val *names, const ah_tool_call *call,
                             yyjson_mut_doc *doc, yyjson_mut_val *receipts, bool *failed,
                             bool *unknown, ah_error *error) {
    size_t i, n;
    yyjson_val *name;
    yyjson_arr_foreach(names, i, n, name) {
        ah_local_check *check = ah_local_check_find(tools, yyjson_get_str(name));
        if (!check)
            return ah_fail(error, AH_INVALID, "Unknown check preset");
        for (size_t j = 0; j < i; j++)
            if (yyjson_equals(name, yyjson_arr_get(names, j)))
                return ah_fail(error, AH_INVALID, "Duplicate check preset");
    }
    ah_code code = AH_OK;
    yyjson_arr_foreach(names, i, n, name) {
        ah_tool_result result = {0};
        code = ah_local_check_run(tools, ah_local_check_find(tools, yyjson_get_str(name)), call,
                                  &result, error);
        yyjson_doc *parsed = result.json ? yyjson_read(result.json, strlen(result.json), 0) : NULL;
        if (parsed)
            yyjson_mut_arr_append(receipts, yyjson_val_mut_copy(doc, yyjson_doc_get_root(parsed)));
        *failed = *failed || result.is_error || code != AH_OK || !parsed;
        *unknown = *unknown || result.outcome_unknown;
        yyjson_doc_free(parsed);
        free(result.json);
        if (code != AH_OK || *unknown)
            break;
    }
    return code;
}

ah_code ah_local_run_checks(void *context, const ah_tool_call *call, ah_tool_result *result,
                            ah_error *error) {
    ah_error fallback;
    if (!error)
        error = &fallback;
    yyjson_doc *args = ah_local_arguments(call, ah_checks_schema, result, error);
    if (!args)
        return error->code;
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_val *receipts = yyjson_mut_obj_add_arr(doc, root, "checks");
    bool failed = false, unknown = false;
    ah_code code =
        ah_local_check_names(context, yyjson_obj_get(yyjson_doc_get_root(args), "checks"), call,
                             doc, receipts, &failed, &unknown, error);
    yyjson_doc_free(args);
    if (code != AH_OK && !yyjson_mut_arr_size(receipts)) {
        ah_mut_doc_free(doc);
        return ah_local_error(call, result, error, code,
                              "Check preset unavailable or could not start");
    }
    yyjson_mut_obj_add_bool(doc, root, "ok", !failed && code == AH_OK);
    result->is_error = failed || code != AH_OK;
    result->outcome_unknown = true;
    ah_code finished = ah_local_finish(call, result, doc, error);
    if (finished == AH_OK)
        result->outcome_unknown = unknown;
    return unknown ? ah_fail(error, AH_UNCERTAIN, "Check effects need reconciliation") : finished;
}

static ah_code verifier_revision(void *context, const ah_tool_call *call, char revision[65],
                                 ah_error *error) {
    ah_local_tools *tools = context;
    ah_hash hash;
    ah_hash_init(&hash);
    for (size_t i = 0; i < tools->check_count; i++) {
        char input[65];
        ah_code code = inputs_revision(tools, &tools->checks[i], call, input);
        if (code != AH_OK)
            return ah_fail(error, code, "Inputs for check '%s' unavailable or changed",
                           tools->checks[i].name);
        ah_hash_update(&hash, input, 64);
    }
    ah_hash_finish(&hash, revision);
    return AH_OK;
}

static ah_code verifier_check(void *context, const ah_tool_call *call, ah_tool_result *result,
                              ah_error *error) {
    ah_local_tools *tools = context;
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc), *names = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, root);
    for (size_t i = 0; i < tools->check_count; i++)
        yyjson_mut_arr_append(names, yyjson_mut_strcpy(doc, tools->checks[i].name));
    yyjson_mut_obj_add_val(doc, root, "checks", names);
    char *json = ah_mut_write(doc);
    ah_mut_doc_free(doc);
    if (!json)
        return ah_fail(error, AH_IO, "Cannot prepare verifier");
    ah_tool_call inner = *call;
    inner.arguments_json = json;
    /* The public check tool allows up to eight selected names per call. A task
     * recipe runs at most that many registered presets for the same bound. */
    ah_code code = ah_local_run_checks(tools, &inner, result, error);
    free(json);
    return code;
}

ah_code ah_local_tools_verifier(ah_local_tools *tools, ah_task_verifier *out, ah_error *error) {
    if (!tools || !out || !tools->registered || !tools->check_count || tools->check_count > 8)
        return ah_fail(error, AH_INVALID,
                       "Register one to eight check presets before creating the verifier");
    for (size_t i = 0; i < tools->check_count; i++)
        if (tools->checks[i].approval)
            return ah_fail(
                error, AH_AUTH,
                "Task verifier requires host-authorized checks without per-operation approval");
    ah_hash hash;
    ah_hash_init(&hash);
    for (size_t i = 0; i < tools->check_count; i++)
        ah_hash_update(&hash, tools->checks[i].definition, 64);
    ah_hash_finish(&hash, tools->check_revision);
    *out = (ah_task_verifier){tools->check_revision, verifier_revision, verifier_check, tools};
    return AH_OK;
}
