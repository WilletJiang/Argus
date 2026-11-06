#include "local_internal.h"

const char ah_apply_schema[] =
    "{\"type\":\"object\",\"properties\":{\"files\":{\"type\":\"array\",\"minItems\":1,"
    "\"maxItems\":16,"
    "\"items\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":4096},"
    "\"base_revision\":{\"type\":\"string\",\"minLength\":64,\"maxLength\":64},"
    "\"edits\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":64,\"items\":{\"type\":\"object\","
    "\"properties\":{\"old_text\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":1048576},"
    "\"new_text\":{\"type\":\"string\",\"maxLength\":1048576}},"
    "\"required\":[\"old_text\",\"new_text\"],\"additionalProperties\":false}}},"
    "\"required\":[\"path\",\"edits\"],\"additionalProperties\":false}},"
    "\"checks\":{\"type\":\"array\",\"maxItems\":8,\"items\":{\"type\":\"string\",\"minLength\":1,"
    "\"maxLength\":64}}},"
    "\"required\":[\"files\"],\"additionalProperties\":false}";

typedef struct {
    const char *path;
    char leaf[4097], before[65], after[65];
    int parent;
    struct stat stat;
    ah_edit_text edited;
    size_t old_size;
    bool changed;
} change;
static void conflict_receipt(const change *c, const ah_edit_text *source, const char *message,
                             const ah_tool_call *call, ah_tool_result *result, ah_error *error) {
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *out = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, out);
    size_t limit = ah_local_view_limit(call), keep = limit > 1024 ? (limit - 1024) / 6 : 0,
           complete;
    if (keep > 4096)
        keep = 4096;
    if (keep > source->length)
        keep = source->length;
    ah_local_utf8(source->text, keep, &complete);
    keep = complete;
    yyjson_mut_obj_add_bool(doc, out, "ok", false);
    yyjson_mut_obj_add_str(doc, out, "error", "conflict");
    yyjson_mut_obj_add_strcpy(doc, out, "path", c->path);
    yyjson_mut_obj_add_strcpy(doc, out, "message", message);
    yyjson_mut_obj_add_strcpy(doc, out, "revision", c->before);
    yyjson_mut_obj_add_strncpy(doc, out, "content", source->text, keep);
    yyjson_mut_obj_add_bool(doc, out, "truncated", keep < source->length);
    yyjson_mut_obj_add_uint(doc, out, "next_offset_bytes", keep);
    result->is_error = true;
    ah_local_finish(call, result, doc, error);
}

static ah_code prepare(ah_local_tools *tools, change *c, yyjson_val *file, const ah_tool_call *call,
                       ah_tool_result *result, ah_error *error) {
    yyjson_val *path = yyjson_obj_get(file, "path");
    c->path = yyjson_get_str(path);
    c->parent = ah_local_parent_fd(tools, path, c->leaf);
    ah_edit_text source = {0};
    ah_code code = c->parent < 0 ? AH_INVALID
                                 : ah_local_source(c->parent, c->leaf, call, &c->stat, &source.text,
                                                   &source.length, NULL);
    if (code != AH_OK)
        return ah_fail(error, code, "Cannot read stable edit source");
    c->old_size = source.length;
    ah_hash_bytes(source.text, source.length, c->before);
    const char *conflict = NULL;
    yyjson_val *base = yyjson_obj_get(file, "base_revision");
    if (base && !yyjson_equals_str(base, c->before)) {
        conflict = "Base revision changed";
        code = AH_INVALID;
    } else
        code = ah_local_edit(&source, yyjson_obj_get(file, "edits"), call, &c->edited, &conflict,
                             error);
    if (conflict)
        conflict_receipt(c, &source, conflict, call, result, error);
    free(source.text);
    if (code == AH_OK) {
        ah_hash_bytes(c->edited.text, c->edited.length, c->after);
        c->changed = strcmp(c->before, c->after) != 0;
    }
    return code;
}

ah_code ah_local_apply(void *context, const ah_tool_call *call, ah_tool_result *result,
                       ah_error *error) {
    ah_error fallback;
    if (!error)
        error = &fallback;
    ah_local_tools *tools = context;
    yyjson_doc *args = ah_local_arguments(call, ah_apply_schema, result, error);
    if (!args)
        return error->code;
    yyjson_val *root = yyjson_doc_get_root(args), *files = yyjson_obj_get(root, "files"),
               *checks = yyjson_obj_get(root, "checks");
    change changes[16] = {0};
    for (size_t i = 0; i < 16; i++)
        changes[i].parent = -1;
    size_t count = yyjson_arr_size(files), i, n;
    ah_code code = ah_local_validate_checks(tools, checks, error);
    if (code == AH_OK && !call->checkpoint)
        code = ah_fail(error, AH_INVALID, "Durable checkpoints are required");
    if (code == AH_OK && ah_local_limit(call) < 4096)
        code = ah_fail(error, AH_LIMIT, "A 4096-byte result budget is required");
    yyjson_val *file;
    yyjson_arr_foreach(files, i, n, file) {
        if (code != AH_OK)
            break;
        const char *path = yyjson_get_str(yyjson_obj_get(file, "path"));
        for (size_t j = 0; j < i; j++)
            if (!strcmp(changes[j].path, path))
                code = AH_INVALID;
        if (code == AH_OK)
            code = prepare(tools, &changes[i], file, call, result, error);
    }
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *output = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, output);
    yyjson_mut_val *receipts = yyjson_mut_obj_add_arr(doc, output, "files");
    size_t applied = 0, completed = 0;
    bool unknown = false, failed = false;
    for (i = 0; code == AH_OK && i < count; i++) {
        change *c = &changes[i];
        yyjson_mut_val *row = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, row, "path", c->path);
        yyjson_mut_obj_add_strcpy(doc, row, "before_revision", c->before);
        yyjson_mut_obj_add_strcpy(doc, row, "revision", c->after);
        yyjson_mut_obj_add_uint(doc, row, "before_bytes", c->old_size);
        yyjson_mut_obj_add_uint(doc, row, "bytes", c->edited.length);
        if (c->changed) {
            code = ah_local_phase(call, "file.started", c->path, c->before, c->after, error);
            if (code != AH_OK)
                break;
            ah_tool_result written = {0};
            code = ah_local_replace(c->parent, c->leaf, c->edited.text, c->edited.length, &c->stat,
                                    call, &written, error);
            unknown |= written.outcome_unknown;
            bool committed = code == AH_OK && !written.is_error;
            if (committed)
                applied++;
            else if (code == AH_OK)
                code = AH_INVALID;
            yyjson_doc *parsed =
                written.json ? yyjson_read(written.json, strlen(written.json), 0) : NULL;
            if (!committed && parsed)
                yyjson_mut_obj_add_val(doc, row, "failure",
                                       yyjson_val_mut_copy(doc, yyjson_doc_get_root(parsed)));
            yyjson_doc_free(parsed);
            free(written.json);
            if (committed) {
                code = ah_local_phase(call, "file.applied", c->path, c->before, c->after, error);
                if (code != AH_OK)
                    unknown = true;
            }
            yyjson_mut_obj_add_str(doc, row, "status",
                                   unknown     ? "unknown"
                                   : committed ? "applied"
                                               : "not_applied");
        } else
            yyjson_mut_obj_add_str(doc, row, "status", "unchanged");
        yyjson_mut_arr_append(receipts, row);
        if (code == AH_OK && !unknown)
            completed++;
    }
    bool application_unknown = unknown;
    if (code == AH_OK) {
        yyjson_mut_val *check_receipts = yyjson_mut_obj_add_arr(doc, output, "checks");
        code = ah_local_check_names(tools, checks, call, doc, check_receipts, &failed, &unknown,
                                    error);
    } else
        yyjson_mut_obj_add_str(doc, output, "checks_status", "not_run");
    yyjson_mut_obj_add_bool(doc, output, "ok", code == AH_OK && !failed && !unknown);
    yyjson_mut_obj_add_uint(doc, output, "applied_files", applied);
    yyjson_mut_obj_add_str(doc, output, "application",
                           application_unknown  ? "unknown"
                           : completed == count ? "complete"
                           : applied            ? "partial"
                                                : "not_applied");
    yyjson_mut_obj_add_bool(doc, output, "outcome_unknown", unknown);
    if (code != AH_OK)
        yyjson_mut_obj_add_str(doc, output, "error", ah_code_name(code));
    if (result->json) {
        yyjson_doc *conflict = yyjson_read(result->json, strlen(result->json), 0);
        if (conflict)
            yyjson_mut_obj_add_val(doc, output, "conflict",
                                   yyjson_val_mut_copy(doc, yyjson_doc_get_root(conflict)));
        yyjson_doc_free(conflict);
        free(result->json);
        result->json = NULL;
    }
    for (size_t j = 0; j < count; j++) {
        if (changes[j].parent >= 0)
            close(changes[j].parent);
        free(changes[j].edited.text);
    }
    yyjson_doc_free(args);
    result->is_error = code != AH_OK || failed || unknown;
    result->outcome_unknown = applied > 0 || unknown;
    ah_code finish = ah_local_finish(call, result, doc, error);
    if (finish == AH_OK)
        result->outcome_unknown = unknown;
    return unknown ? ah_fail(error, AH_UNCERTAIN,
                             "Changes or checks need reconciliation; do not replay the group")
                   : finish;
}
