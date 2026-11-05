#include "local_internal.h"

const char ah_apply_schema[] =
    "{\"type\":\"object\",\"properties\":{\"files\":{\"type\":\"array\",\"minItems\":1,"
    "\"maxItems\":16,"
    "\"items\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":4096},"
    "\"base_revision\":{\"type\":\"string\",\"minLength\":64,\"maxLength\":64},"
    "\"edits\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":64,\"items\":{\"type\":\"object\","
    "\"properties\":{\"old_text\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":65536},"
    "\"new_text\":{\"type\":\"string\",\"maxLength\":65536}},"
    "\"required\":[\"old_text\",\"new_text\"],\"additionalProperties\":false}}},"
    "\"required\":[\"path\",\"base_revision\",\"edits\"],\"additionalProperties\":false}},"
    "\"checks\":{\"type\":\"array\",\"maxItems\":8,\"items\":{\"type\":\"string\",\"minLength\":1,"
    "\"maxLength\":64}}},"
    "\"required\":[\"files\"],\"additionalProperties\":false}";

typedef struct {
    const char *path;
    char leaf[4097], before[65], after[65];
    int parent;
    struct stat stat;
    char *source, *edited;
    size_t old_size, new_size;
    bool changed;
} change;
typedef struct {
    size_t position, old_size, new_size;
    const char *text;
} hunk;
static int order_hunks(const void *a, const void *b) {
    size_t x = ((const hunk *)a)->position, y = ((const hunk *)b)->position;
    return x < y ? -1 : x > y;
}

static ah_code prepare(ah_local_tools *tools, change *c, yyjson_val *file, const ah_tool_call *call,
                       ah_tool_result *result, ah_error *error) {
    yyjson_val *path = yyjson_obj_get(file, "path");
    c->path = yyjson_get_str(path);
    c->parent = ah_local_parent_fd(tools, path, c->leaf);
    ah_code code = c->parent < 0 ? AH_INVALID
                                 : ah_local_source(c->parent, c->leaf, call, &c->stat, &c->source,
                                                   &c->old_size);
    if (code != AH_OK)
        return ah_fail(error, code, "Cannot read stable edit source");
    ah_hash_bytes(c->source, c->old_size, c->before);
    const char *conflict = NULL;
    if (!yyjson_equals_str(yyjson_obj_get(file, "base_revision"), c->before))
        conflict = "Base revision changed";
    hunk hunks[64];
    size_t i, n, removed = 0, added = 0;
    yyjson_val *edits = yyjson_obj_get(file, "edits"), *item;
    yyjson_arr_foreach(edits, i, n, item) {
        if (conflict)
            break;
        yyjson_val *old = yyjson_obj_get(item, "old_text"), *new = yyjson_obj_get(item, "new_text");
        const char *needle = yyjson_get_str(old), *text = yyjson_get_str(new);
        size_t a = yyjson_get_len(old), b = yyjson_get_len(new);
        if (memchr(needle, 0, a) || memchr(text, 0, b))
            return ah_fail(error, AH_INVALID, "NUL in edit");
        const char *match = strstr(c->source, needle);
        if (!match || strstr(match + 1, needle)) {
            conflict = "Every old_text must match exactly once in the base file";
            break;
        }
        hunks[i] = (hunk){(size_t)(match - c->source), a, b, text};
    }
    if (!conflict) {
        n = yyjson_arr_size(edits);
        qsort(hunks, n, sizeof(hunks[0]), order_hunks);
        for (i = 0; i < n; i++) {
            if (i && hunks[i].position < hunks[i - 1].position + hunks[i - 1].old_size) {
                conflict = "Edit hunks overlap";
                break;
            }
            removed += hunks[i].old_size;
            added += hunks[i].new_size;
        }
    }
    if (conflict) {
        ah_local_conflict(call, result, error, c->path, c->source, c->old_size, conflict);
        return AH_INVALID;
    }
    if (added > AH_LOCAL_MAX_BYTES - (c->old_size - removed))
        return ah_fail(error, AH_LIMIT, "Edited file exceeds 1 MiB");
    c->new_size = c->old_size - removed + added;
    c->edited = malloc(c->new_size + 1);
    if (!c->edited)
        return ah_fail(error, AH_IO, "Out of memory");
    size_t read = 0, written = 0;
    for (i = 0; i < n; i++) {
        size_t prefix = hunks[i].position - read;
        memcpy(c->edited + written, c->source + read, prefix);
        written += prefix;
        memcpy(c->edited + written, hunks[i].text, hunks[i].new_size);
        written += hunks[i].new_size;
        read = hunks[i].position + hunks[i].old_size;
    }
    memcpy(c->edited + written, c->source + read, c->old_size - read);
    c->edited[c->new_size] = 0;
    ah_hash_bytes(c->edited, c->new_size, c->after);
    c->changed = strcmp(c->before, c->after) != 0;
    return AH_OK;
}

static ah_code validate_checks(ah_local_tools *tools, yyjson_val *names, ah_error *error) {
    size_t i, n;
    yyjson_val *name;
    yyjson_arr_foreach(names, i, n, name) {
        if (!ah_local_check_find(tools, yyjson_get_str(name)))
            return ah_fail(error, AH_INVALID, "Unknown check preset");
        for (size_t j = 0; j < i; j++)
            if (yyjson_equals(name, yyjson_arr_get(names, j)))
                return ah_fail(error, AH_INVALID, "Duplicate check preset");
    }
    return AH_OK;
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
    ah_code code = validate_checks(tools, checks, error);
    if (code == AH_OK && (!call->checkpoint || ah_local_limit(call) < 4096))
        code = ah_fail(error, AH_INVALID,
                       "Durable checkpoints and a 4096-byte result budget are required");
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
        yyjson_mut_obj_add_uint(doc, row, "bytes", c->new_size);
        if (c->changed) {
            code = ah_local_phase(call, "file.started", c->path, c->before, c->after, error);
            if (code != AH_OK)
                break;
            ah_tool_result written = {0};
            code = ah_local_replace(c->parent, c->leaf, c->edited, c->new_size, &c->stat, call,
                                    &written, error);
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
        free(changes[j].source);
        free(changes[j].edited);
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
