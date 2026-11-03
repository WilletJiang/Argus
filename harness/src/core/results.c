#include "core.h"

static size_t utf8_prefix(const char *text, size_t length) {
    while (length && ((unsigned char)text[length] & 0xc0) == 0x80)
        length--;
    return length;
}

/* Stored once with the receipt. Later budget changes must not rewrite history. */
char *core_result_view(ah_engine *e, const char *id, const char *json, bool is_error) {
    size_t length = strlen(json), budget = e->config.max_model_tool_bytes;
    yyjson_doc *parsed = yyjson_read(json, length, 0);
    if (!parsed)
        return NULL;
    size_t preview = budget && length > budget ? utf8_prefix(json, budget / 2) : length;
    bool truncated = preview < length;
    for (;;) {
        yyjson_mut_doc *d = ah_mut_doc_new();
        if (!d) {
            yyjson_doc_free(parsed);
            return NULL;
        }
        yyjson_mut_val *root = yyjson_mut_obj(d);
        yyjson_mut_doc_set_root(d, root);
        yyjson_mut_obj_add_bool(d, root, "is_error", is_error);
        yyjson_mut_val *value =
            truncated ? yyjson_mut_obj(d) : yyjson_val_mut_copy(d, yyjson_doc_get_root(parsed));
        if (truncated) {
            yyjson_mut_obj_add_bool(d, value, "truncated", true);
            yyjson_mut_obj_add_strcpy(d, value, "operation_id", id);
            yyjson_mut_obj_add_uint(d, value, "total_bytes", length);
            yyjson_mut_obj_add_str(d, value, "retrieval_tool", "read_result");
            yyjson_mut_obj_add_strncpy(d, value, "preview", json, preview);
        }
        yyjson_mut_obj_add_val(d, root, "result", value);
        char *out = ah_mut_write(d);
        ah_mut_doc_free(d);
        if (!out || !budget || strlen(out) <= budget) {
            yyjson_doc_free(parsed);
            return out;
        }
        free(out);
        if (truncated && !preview) {
            yyjson_doc_free(parsed);
            return NULL;
        }
        truncated = true;
        preview = utf8_prefix(json, preview / 2);
    }
}

static char *result_page(const char *id, const char *json, size_t total, size_t offset,
                         size_t length, size_t budget) {
    for (;;) {
        yyjson_mut_doc *d = ah_mut_doc_new();
        if (!d)
            return NULL;
        yyjson_mut_val *root = yyjson_mut_obj(d);
        yyjson_mut_doc_set_root(d, root);
        yyjson_mut_obj_add_strcpy(d, root, "operation_id", id);
        yyjson_mut_obj_add_uint(d, root, "total_bytes", total);
        yyjson_mut_obj_add_uint(d, root, "offset", offset);
        yyjson_mut_obj_add_uint(d, root, "next_offset", offset + length);
        yyjson_mut_obj_add_bool(d, root, "truncated", offset + length < total);
        yyjson_mut_obj_add_strncpy(d, root, "content", json + offset, length);
        char *out = ah_mut_write(d);
        ah_mut_doc_free(d);
        if (!out || strlen(out) <= budget)
            return out;
        free(out);
        if (!length)
            return NULL;
        length = utf8_prefix(json + offset, length / 2);
        if (!length && offset < total)
            return NULL;
    }
}

static ah_code read_page(ah_engine *e, const ah_tool_call *call, const char *id, const char *json,
                         size_t total, double offset_number, size_t limit, ah_tool_result *result,
                         ah_error *error) {
    if (offset_number > (double)total)
        return ah_fail(error, AH_INVALID, "result offset exceeds length");
    size_t offset = (size_t)offset_number;
    if (offset < total && ((unsigned char)json[offset] & 0xc0) == 0x80)
        return ah_fail(error, AH_INVALID, "result offset must be a UTF-8 boundary");
    size_t budget = e->config.max_model_tool_bytes;
    /* Leave room for the is_error/result envelope so pages never turn into
     * another reference to a truncated result. */
    if (!budget || budget > call->output_limit)
        budget = call->output_limit;
    if (budget <= 32)
        return ah_fail(error, AH_LIMIT, "result page budget too small");
    if (limit > total - offset)
        limit = total - offset;
    limit = utf8_prefix(json + offset, limit);
    if (!limit && offset < total)
        return ah_fail(error, AH_INVALID, "limit cannot fit next UTF-8 character");
    result->json = result_page(id, json, total, offset, limit, budget - 32);
    return result->json ? AH_OK : ah_fail(error, AH_LIMIT, "cannot encode bounded result page");
}

ah_code core_read_result(void *context, const ah_tool_call *call, ah_tool_result *result,
                         ah_error *error) {
    ah_engine *e = context;
    /* The owner validates the builtin schema before dispatching this callback. */
    yyjson_doc *args = yyjson_read(call->arguments_json, strlen(call->arguments_json), 0);
    if (!args)
        return ah_fail(error, AH_INVALID, "invalid read_result arguments");
    yyjson_val *root = yyjson_doc_get_root(args);
    const char *id = yyjson_get_str(yyjson_obj_get(root, "operation_id"));
    yyjson_val *offset_arg = yyjson_obj_get(root, "offset"),
               *limit_arg = yyjson_obj_get(root, "limit");
    double offset = offset_arg ? yyjson_get_num(offset_arg) : 0;
    size_t limit = limit_arg ? (size_t)yyjson_get_num(limit_arg) : 4096;
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(e,
                              "SELECT o.result FROM result_refs r JOIN operations o "
                              "ON o.id=r.operation_id WHERE r.session_id=? AND r.operation_id=? "
                              "AND o.result IS NOT NULL",
                              &s, error);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, call->session_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, id, -1, SQLITE_TRANSIENT);
        int step = sqlite3_step(s);
        if (step == SQLITE_ROW)
            rc = read_page(e, call, id, (const char *)sqlite3_column_text(s, 0),
                           (size_t)sqlite3_column_bytes(s, 0), offset, limit, result, error);
        else
            rc = ah_fail(error, step == SQLITE_DONE ? AH_NOT_FOUND : AH_IO,
                         "saved result unavailable to this session");
    }
    sqlite3_finalize(s);
    yyjson_doc_free(args);
    return rc;
}
