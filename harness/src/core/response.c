#include "core.h"

static const char ask_schema[] =
    "{\"type\":\"object\",\"properties\":{\"question\":{\"type\":\"string\"},\"options\":{\"type\":"
    "\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"question\"],"
    "\"additionalProperties\":false}";
static const char wait_schema[] =
    "{\"type\":\"object\",\"properties\":{\"reason\":{\"type\":\"string\"}},\"required\":["
    "\"reason\"],\"additionalProperties\":false}";
static const char result_schema[] =
    "{\"type\":\"object\",\"properties\":{\"operation_id\":{\"type\":\"string\"},"
    "\"offset\":{\"type\":\"integer\",\"minimum\":0},\"limit\":{\"type\":\"integer\","
    "\"minimum\":1,\"maximum\":65536}},\"required\":[\"operation_id\"],\"additionalProperties\":"
    "false}";
const char *core_builtin_schema(const char *name) {
    if (!strcmp(name, "ask") || !strcmp(name, "harness_ask"))
        return ask_schema;
    if (!strcmp(name, "wait") || !strcmp(name, "harness_wait"))
        return wait_schema;
    if (!strcmp(name, "read_result"))
        return result_schema;
    return NULL;
}
static bool add_definition(yyjson_mut_doc *d, yyjson_mut_val *arr, const char *name,
                           const char *description, const char *schema) {
    yyjson_doc *parsed = yyjson_read(schema, strlen(schema), 0);
    if (!parsed)
        return false;
    yyjson_mut_val *tool = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, tool, "type", "function");
    yyjson_mut_obj_add_strcpy(d, tool, "name", name);
    yyjson_mut_obj_add_strcpy(d, tool, "description", description);
    yyjson_mut_obj_add_bool(d, tool, "strict", false);
    yyjson_mut_obj_add_val(d, tool, "parameters",
                           yyjson_val_mut_copy(d, yyjson_doc_get_root(parsed)));
    yyjson_doc_free(parsed);
    return yyjson_mut_arr_append(arr, tool);
}
ah_code core_request(ah_engine *e, const core_session *session, char **json, ah_error *err) {
    *json = NULL;
    sqlite3_stmt *measure = NULL;
    ah_code measured = core_prepare(
        e, "SELECT COALESCE(SUM(length(json)),0) FROM items WHERE session_id=? AND active=1",
        &measure, err);
    if (measured != AH_OK)
        return measured;
    sqlite3_bind_text(measure, 1, session->id, -1, SQLITE_TRANSIENT);
    int measured_step = sqlite3_step(measure);
    sqlite3_int64 bytes = measured_step == SQLITE_ROW ? sqlite3_column_int64(measure, 0) : -1;
    sqlite3_finalize(measure);
    if (bytes < 0)
        return ah_fail(err, AH_IO, "cannot measure model context");
    if ((uint64_t)bytes > e->config.max_context_bytes)
        return ah_fail(err, AH_LIMIT,
                       "stored context exceeds byte budget; compact after all tools are settled");
    yyjson_mut_doc *d = ah_mut_doc_new();
    if (!d)
        return ah_fail(err, AH_IO, "out of memory");
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);
    yyjson_mut_obj_add_strcpy(d, root, "model", session->model);
    yyjson_mut_obj_add_strcpy(d, root, "prompt_cache_key", session->id);
    yyjson_mut_obj_add_strcpy(d, root, "instructions", session->instructions);
    yyjson_mut_obj_add_bool(d, root, "store", false);
    yyjson_mut_obj_add_bool(d, root, "stream", true);
    yyjson_mut_val *include = yyjson_mut_arr(d);
    yyjson_mut_arr_add_str(d, include, "reasoning.encrypted_content");
    yyjson_mut_obj_add_val(d, root, "include", include);
    yyjson_mut_val *input = yyjson_mut_arr(d);
    yyjson_mut_obj_add_val(d, root, "input", input);
    if (*session->summary) {
        yyjson_mut_val *item = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, item, "role", "user");
        yyjson_mut_obj_add_strcpy(d, item, "content", session->summary);
        yyjson_mut_arr_append(input, item);
    }
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(
        e, "SELECT json FROM items WHERE session_id=? AND active=1 ORDER BY seq", &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
        int step;
        while ((step = sqlite3_step(s)) == SQLITE_ROW) {
            const char *text = (const char *)sqlite3_column_text(s, 0);
            yyjson_doc *item = yyjson_read(text, strlen(text), 0);
            if (!item) {
                rc = ah_fail(err, AH_IO, "corrupt stored context");
                break;
            }
            yyjson_mut_arr_append(input, yyjson_val_mut_copy(d, yyjson_doc_get_root(item)));
            yyjson_doc_free(item);
        }
        if (rc == AH_OK && step != SQLITE_DONE)
            rc = ah_fail(err, AH_IO, "context read failed");
    }
    sqlite3_finalize(s);
    yyjson_mut_val *tools = yyjson_mut_arr(d), *space = yyjson_mut_obj(d),
                   *functions = yyjson_mut_arr(d);
    yyjson_mut_obj_add_val(d, root, "tools", tools);
    yyjson_mut_arr_append(tools, space);
    yyjson_mut_obj_add_str(d, space, "type", "namespace");
    yyjson_mut_obj_add_str(d, space, "name", "local");
    yyjson_mut_obj_add_str(d, space, "description", "Host controlled tools and input requests.");
    yyjson_mut_obj_add_val(d, space, "tools", functions);
    if (!add_definition(d, functions, "ask", "Ask the user a question and wait for a JSON reply.",
                        ask_schema) ||
        !add_definition(d, functions, "wait",
                        "Pause until the host explicitly supplies a JSON continuation.",
                        wait_schema) ||
        !add_definition(d, functions, "read_result",
                        "Read a saved tool result as UTF-8 JSON text. Use next_offset to continue; "
                        "offset defaults to 0 and limit to 4096 bytes.",
                        result_schema))
        rc = ah_fail(err, AH_IO, "cannot encode built-in definitions");
    for (size_t i = 0; i < e->tool_count && rc == AH_OK; i++)
        if (!add_definition(d, functions, e->tools[i].name, e->tools[i].description,
                            e->tools[i].schema))
            rc = ah_fail(err, AH_IO, "cannot encode tool definition");
    if (rc == AH_OK) {
        *json = ah_mut_write(d);
        if (!*json)
            rc = ah_fail(err, AH_IO, "out of memory");
        else if (strlen(*json) > e->config.max_context_bytes) {
            free(*json);
            *json = NULL;
            rc = ah_fail(
                err, AH_LIMIT,
                "model request exceeds context byte budget; compact after all tools are settled");
        }
    }
    ah_mut_doc_free(d);
    return rc;
}
static bool optional_string(yyjson_val *object, const char *key, bool nullable) {
    yyjson_val *value = yyjson_obj_get(object, key);
    return !value || (nullable && yyjson_is_null(value)) || yyjson_is_str(value);
}
static bool content_parts(yyjson_val *parts, const char *text_type) {
    if (!yyjson_is_arr(parts))
        return false;
    size_t index, count;
    yyjson_val *part;
    yyjson_arr_foreach(parts, index, count, part) {
        if (!yyjson_is_obj(part))
            return false;
        const char *type = yyjson_get_str(yyjson_obj_get(part, "type"));
        if (!type)
            return false;
        if (text_type) {
            if (strcmp(type, text_type) || !yyjson_is_str(yyjson_obj_get(part, "text")))
                return false;
        } else if (!strcmp(type, "output_text")) {
            if (!yyjson_is_str(yyjson_obj_get(part, "text")))
                return false;
            yyjson_val *annotations = yyjson_obj_get(part, "annotations"),
                       *logprobs = yyjson_obj_get(part, "logprobs");
            if ((annotations && !yyjson_is_arr(annotations)) ||
                (logprobs && !yyjson_is_arr(logprobs)))
                return false;
        } else if (!strcmp(type, "refusal")) {
            if (!yyjson_is_str(yyjson_obj_get(part, "refusal")))
                return false;
        } else
            return false;
    }
    return true;
}
static ah_code validate_response(ah_engine *e, yyjson_val *root, ah_error *err) {
    const char *status = yyjson_get_str(yyjson_obj_get(root, "status"));
    yyjson_val *out = yyjson_obj_get(root, "output");
    if (!yyjson_is_obj(root) || !status || strcmp(status, "completed") || !yyjson_is_arr(out))
        return ah_fail(err, AH_PROTOCOL,
                       "provider must return a completed Responses object with output array");
    if (!optional_string(root, "id", false) || !optional_string(root, "object", false))
        return ah_fail(err, AH_PROTOCOL, "response identity fields must be strings");
    yyjson_val *failure = yyjson_obj_get(root, "error"),
               *incomplete = yyjson_obj_get(root, "incomplete_details");
    if ((failure && !yyjson_is_null(failure)) || (incomplete && !yyjson_is_null(incomplete)))
        return ah_fail(err, AH_PROTOCOL, "completed response contains failure details");
    size_t index, max;
    yyjson_val *item;
    yyjson_arr_foreach(out, index, max, item) {
        const char *type = yyjson_get_str(yyjson_obj_get(item, "type"));
        if (!yyjson_is_obj(item) || !type)
            return ah_fail(err, AH_PROTOCOL, "output item must be an object with a type");
        if (!optional_string(item, "id", !strcmp(type, "function_call")) ||
            !optional_string(item, "status", strcmp(type, "message") != 0))
            return ah_fail(err, AH_PROTOCOL, "output identity and status must be strings");
        const char *item_status = yyjson_get_str(yyjson_obj_get(item, "status"));
        if (item_status && strcmp(item_status, "completed"))
            return ah_fail(err, AH_PROTOCOL, "output item is not completed");
        if (!strcmp(type, "function_call")) {
            const char *name = yyjson_get_str(yyjson_obj_get(item, "name")),
                       *args = yyjson_get_str(yyjson_obj_get(item, "arguments")),
                       *call = yyjson_get_str(yyjson_obj_get(item, "call_id"));
            const char *space = yyjson_get_str(yyjson_obj_get(item, "namespace"));
            if (!optional_string(item, "namespace", true))
                return ah_fail(err, AH_PROTOCOL, "function namespace must be a string");
            if (!name || !*name || strlen(name) > 64 || !args ||
                strlen(args) > e->config.max_tool_arguments_bytes || !call || !*call ||
                strlen(call) > 256 || (space && strcmp(space, "local")))
                return ah_fail(err, AH_PROTOCOL, "malformed or oversized function call");
            for (size_t j = 0; j < index; j++) {
                yyjson_val *prior = yyjson_arr_get(out, j);
                const char *other = yyjson_get_str(yyjson_obj_get(prior, "call_id"));
                if (other && !strcmp(other, call))
                    return ah_fail(err, AH_PROTOCOL, "duplicate call_id in response");
            }
        } else if (!strcmp(type, "message")) {
            const char *role = yyjson_get_str(yyjson_obj_get(item, "role"));
            if (!role || strcmp(role, "assistant") ||
                !content_parts(yyjson_obj_get(item, "content"), NULL))
                return ah_fail(err, AH_PROTOCOL, "malformed assistant message");
        } else if (!strcmp(type, "reasoning")) {
            yyjson_val *summary = yyjson_obj_get(item, "summary"),
                       *content = yyjson_obj_get(item, "content");
            if (!optional_string(item, "encrypted_content", true) ||
                (summary && !content_parts(summary, "summary_text")) ||
                (content && !yyjson_is_null(content) && !content_parts(content, "reasoning_text")))
                return ah_fail(err, AH_PROTOCOL, "malformed reasoning item");
        } else
            return ah_fail(err, AH_PROTOCOL, "unsupported output item type: %s", type);
    }
    return AH_OK;
}
ah_code core_accept_response(ah_engine *e, core_session *session, const char *json, ah_error *err) {
    if (!json || strlen(json) > e->config.max_output_bytes)
        return ah_fail(err, AH_LIMIT, "provider response exceeds output budget");
    yyjson_doc *d = yyjson_read(json, strlen(json), 0);
    if (!d)
        return ah_fail(err, AH_PROTOCOL, "provider returned invalid JSON");
    yyjson_val *root = yyjson_doc_get_root(d);
    ah_error validation = {0};
    ah_code rc = ah_json_check(root, &validation);
    if (rc != AH_OK)
        rc = ah_fail(err, AH_PROTOCOL, "invalid provider JSON: %s", validation.message);
    else
        rc = validate_response(e, root, err);
    uint64_t after = 0;
    if (rc == AH_OK)
        rc = core_begin(e, &after, err);
    yyjson_val *output = yyjson_obj_get(root, "output"), *item;
    size_t index, max, calls = 0;
    yyjson_arr_foreach(output, index, max, item) {
        if (rc != AH_OK)
            break;
        char *serialized = ah_json_write(item);
        if (!serialized) {
            rc = ah_fail(err, AH_IO, "out of memory");
            break;
        }
        rc = core_add_item(e, session->id, session->turn, serialized, err);
        free(serialized);
        const char *type = yyjson_get_str(yyjson_obj_get(item, "type"));
        if (rc == AH_OK && type && !strcmp(type, "function_call")) {
            const char *name = yyjson_get_str(yyjson_obj_get(item, "name")),
                       *args = yyjson_get_str(yyjson_obj_get(item, "arguments")),
                       *call = yyjson_get_str(yyjson_obj_get(item, "call_id"));
            core_tool *tool = core_find_tool(e, name);
            char id[AH_ID_CAP];
            rc = core_id(id, err);
            sqlite3_stmt *s = NULL;
            if (rc == AH_OK)
                rc = core_prepare(e, "INSERT INTO call_ids(session_id,call_id) VALUES(?,?)", &s,
                                  err);
            if (rc == AH_OK) {
                sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 2, call, -1, SQLITE_TRANSIENT);
                rc = core_done(e, s, err);
            }
            if (rc == AH_OK)
                rc = core_prepare(e,
                                  "INSERT INTO "
                                  "operations(id,session_id,call_id,name,arguments,effect,approval,"
                                  "schema,status) VALUES(?,?,?,?,?,?,?,?,'pending')",
                                  &s, err);
            if (rc == AH_OK) {
                sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 2, session->id, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 3, call, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 4, name, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 5, args, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(s, 6, tool ? tool->tool.effect : AH_EFFECT_READ);
                sqlite3_bind_int(s, 7, tool ? tool->tool.requires_approval : 0);
                const char *schema = tool ? tool->schema : core_builtin_schema(name);
                if (schema)
                    sqlite3_bind_text(s, 8, schema, -1, SQLITE_TRANSIENT);
                rc = core_done(e, s, err);
            }
            if (rc == AH_INVALID)
                rc = ah_fail(err, AH_PROTOCOL, "duplicate call_id across session");
            if (rc == AH_OK) {
                char *intent = ah_json_write(item);
                if (!intent)
                    rc = ah_fail(err, AH_IO, "out of memory");
                else {
                    rc = core_event(e, session->id, id, "tool.intent", intent, err);
                    free(intent);
                }
            }
            calls++;
        }
    }
    if (rc == AH_OK)
        rc = core_event(e, session->id, NULL, "model.completed", json, err);
    if (rc == AH_OK && calls == 0) {
        pthread_mutex_lock(&e->mailbox_lock);
        bool queued = false;
        for (core_steering *message = e->mailbox_head; message; message = message->next) {
            if (!strcmp(message->session, session->id)) {
                queued = true;
                break;
            }
        }
        if (!queued)
            e->active_session[0] = '\0';
        pthread_mutex_unlock(&e->mailbox_lock);
        if (!queued) {
            rc = core_state(e, session->id, AH_COMPLETED, err);
            session->completion_committed = rc == AH_OK;
        }
        session->state = AH_COMPLETED;
    }
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    yyjson_doc_free(d);
    return rc;
}
