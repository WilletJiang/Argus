#include "local_internal.h"
#include <fnmatch.h>

const char ah_find_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":4096},"
    "\"query\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":1024},"
    "\"glob\":{\"type\":\"string\",\"maxLength\":256},"
    "\"context_lines\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":5},"
    "\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100},"
    "\"offset\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":10000000},"
    "\"snapshot\":{\"type\":\"string\",\"minLength\":64,\"maxLength\":64}},"
    "\"required\":[\"path\",\"query\"],\"additionalProperties\":false}";

typedef struct {
    yyjson_mut_doc *doc;
    yyjson_mut_val *results;
    size_t offset, total, emitted, max_results, remaining;
    unsigned context;
    bool full;
} find_output;

static void excerpt(find_output *out, const char *path, const char *revision, const char *source,
                    size_t offset, size_t end, size_t first, size_t last, const char *query) {
    size_t ordinal = out->total++;
    if (ordinal < out->offset || out->full)
        return;
    size_t length = end - offset;
    yyjson_mut_val *row = yyjson_mut_obj(out->doc);
    yyjson_mut_obj_add_strcpy(out->doc, row, "path", path);
    yyjson_mut_obj_add_strcpy(out->doc, row, "revision", revision);
    yyjson_mut_obj_add_uint(out->doc, row, "start_line", first + 1);
    yyjson_mut_obj_add_uint(out->doc, row, "end_line", last + 1);
    yyjson_mut_obj_add_strcpy(out->doc, row, "match", query);
    yyjson_mut_obj_add_uint(out->doc, row, "offset_bytes", offset);
    /* Cap individual excerpts too; the continuation is an exact read_file offset. */
    size_t cap = out->remaining > 1024 ? (out->remaining - 1024) / 6 : 0;
    if (cap > 8192)
        cap = 8192;
    size_t keep = length < cap ? length : cap, complete = 0;
    if (!keep) {
        out->full = true;
        return;
    }
    if (!ah_local_utf8(source + offset, keep, &complete)) {
        out->full = true;
        return;
    }
    keep = complete;
    yyjson_mut_obj_add_strncpy(out->doc, row, "content", source + offset, keep);
    yyjson_mut_obj_add_bool(out->doc, row, "truncated", keep < length);
    if (keep < length)
        yyjson_mut_obj_add_uint(out->doc, row, "next_offset_bytes", offset + keep);
    char *encoded = yyjson_mut_val_write(row, 0, NULL);
    size_t bytes = encoded ? strlen(encoded) + 1 : SIZE_MAX;
    free(encoded);
    if (bytes > out->remaining) {
        out->full = true;
        return;
    }
    yyjson_mut_arr_append(out->results, row);
    out->remaining -= bytes;
    out->emitted++;
    if (out->emitted >= out->max_results)
        out->full = true;
}

/* A matching line contributes one context range, regardless of hit count.
 * Retain only the preceding context line starts, not an index for the whole file. */
static ah_code find_source(find_output *out, const char *path, const char *revision,
                           const char *source, size_t length, const char *query,
                           const ah_tool_call *call) {
    const char *match = strstr(source, query);
    if (!match)
        return ah_local_stopped(call);
    size_t previous[6] = {0}, cursor = 0, line = 0;
    size_t group_start = 0, group_end = 0, first = 0, last = 0;
    bool pending = false;
    while (match) {
        ah_code code = ah_local_stopped(call);
        if (code != AH_OK)
            return code;
        size_t position = (size_t)(match - source);
        const char *newline;
        while ((newline = memchr(source + cursor, '\n', position - cursor))) {
            cursor = (size_t)(newline - source) + 1;
            previous[++line % 6] = cursor;
            if (!(line % 256) && (code = ah_local_stopped(call)) != AH_OK)
                return code;
        }
        size_t start_line = line > out->context ? line - out->context : 0;
        size_t start = previous[start_line % 6];
        newline = memchr(match, '\n', length - position);
        size_t next_line = newline ? (size_t)(newline - source) + 1 : length;
        size_t end = next_line, end_line = line;
        for (unsigned i = 0; i < out->context && end < length; i++) {
            newline = memchr(source + end, '\n', length - end);
            end = newline ? (size_t)(newline - source) + 1 : length;
            end_line++;
        }
        if (pending && start_line <= last + 1) {
            if (end > group_end) {
                group_end = end;
                last = end_line;
            }
        } else {
            if (pending)
                excerpt(out, path, revision, source, group_start, group_end, first, last, query);
            group_start = start;
            group_end = end;
            first = start_line;
            last = end_line;
            pending = true;
        }
        if (next_line == length)
            break;
        match = strstr(source + next_line, query);
    }
    if (pending)
        excerpt(out, path, revision, source, group_start, group_end, first, last, query);
    return ah_local_stopped(call);
}

ah_code ah_local_find(void *context, const ah_tool_call *call, ah_tool_result *result,
                      ah_error *error) {
    ah_error fallback;
    if (!error)
        error = &fallback;
    yyjson_doc *args = ah_local_arguments(call, ah_find_schema, result, error);
    if (!args)
        return error->code;
    yyjson_val *root = yyjson_doc_get_root(args), *scope = yyjson_obj_get(root, "path"),
               *q = yyjson_obj_get(root, "query"), *g = yyjson_obj_get(root, "glob"),
               *snap = yyjson_obj_get(root, "snapshot");
    const char *query = yyjson_get_str(q), *glob = g ? yyjson_get_str(g) : "*";
    size_t offset = (size_t)yyjson_get_num(yyjson_obj_get(root, "offset"));
    if (memchr(query, 0, yyjson_get_len(q)) || strchr(query, '\n') ||
        (g && memchr(glob, 0, yyjson_get_len(g))) || (offset && !snap)) {
        yyjson_doc_free(args);
        return ah_local_error(call, result, error, AH_INVALID,
                              "Use a single-line literal query and snapshot for pagination");
    }
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *output = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, output);
    find_output out = {.doc = doc,
                       .results = yyjson_mut_obj_add_arr(doc, output, "results"),
                       .offset = offset,
                       .context = 2,
                       .max_results = 20};
    yyjson_val *v = yyjson_obj_get(root, "context_lines");
    if (v)
        out.context = (unsigned)yyjson_get_num(v);
    v = yyjson_obj_get(root, "max_results");
    if (v)
        out.max_results = (size_t)yyjson_get_num(v);
    size_t limit = ah_local_view_limit(call);
    out.remaining = limit > 1024 ? limit - 1024 : 0;
    ah_paths paths;
    ah_code code = ah_local_paths(context, scope, call, &paths, NULL);
    ah_hash hash;
    ah_hash_init(&hash);
    ah_hash_update(&hash, query, strlen(query) + 1);
    ah_hash_update(&hash, glob, strlen(glob) + 1);
    ah_hash_update(&hash, yyjson_get_str(scope), yyjson_get_len(scope) + 1);
    unsigned char context_lines = (unsigned char)out.context;
    ah_hash_update(&hash, &context_lines, 1);
    ah_scan_budget budget = {.remaining = 32 * 1024 * 1024};
    size_t scanned = 0, skipped = paths.skipped;
    for (size_t i = 0; code == AH_OK && i < paths.count; i++) {
        if (fnmatch(glob, paths.items[i], 0))
            continue;
        char leaf[4097], *source = NULL, revision[65];
        size_t length = 0;
        struct stat st;
        int parent = ah_local_parent_path(context, paths.items[i], strlen(paths.items[i]), leaf);
        code = parent < 0 ? AH_IO
                          : ah_local_source(parent, leaf, call, &st, &source, &length, &budget);
        if (parent >= 0)
            close(parent);
        if (budget.exhausted) {
            paths.truncated = true;
            code = AH_OK;
            break;
        }
        if ((code == AH_INVALID || code == AH_LIMIT) && ah_local_stopped(call) == AH_OK) {
            skipped++;
            code = AH_OK;
            continue;
        }
        if (code != AH_OK)
            break;
        ah_hash_bytes(source, length, revision);
        ah_hash_update(&hash, paths.items[i], strlen(paths.items[i]) + 1);
        ah_hash_update(&hash, revision, 64);
        scanned++;
        code = find_source(&out, paths.items[i], revision, source, length, query, call);
        free(source);
    }
    char revision[65];
    ah_hash_finish(&hash, revision);
    bool stale = snap && !yyjson_equals_str(snap, revision);
    yyjson_mut_obj_add_bool(doc, output, "ok", code == AH_OK && !stale);
    yyjson_mut_obj_add_strcpy(doc, output, "snapshot", revision);
    yyjson_mut_obj_add_uint(doc, output, "scanned_files", scanned);
    yyjson_mut_obj_add_uint(doc, output, "skipped_files", skipped);
    yyjson_mut_obj_add_bool(doc, output, "scan_truncated", paths.truncated);
    yyjson_mut_obj_add_uint(doc, output, "total_excerpts", out.total);
    yyjson_mut_obj_add_bool(doc, output, "truncated", offset + out.emitted < out.total);
    if (offset + out.emitted < out.total)
        yyjson_mut_obj_add_uint(doc, output, "next_offset", offset + out.emitted);
    else
        yyjson_mut_obj_add_null(doc, output, "next_offset");
    if (stale) {
        yyjson_mut_obj_remove_str(output, "results");
        yyjson_mut_obj_add_arr(doc, output, "results");
        yyjson_mut_obj_add_str(doc, output, "error", "stale_snapshot");
        yyjson_mut_obj_add_str(doc, output, "message",
                               "Search inputs changed; restart from offset 0");
        result->is_error = true;
    }
    ah_local_paths_free(&paths);
    yyjson_doc_free(args);
    if (code != AH_OK || (!out.emitted && out.total > offset && !stale)) {
        ah_mut_doc_free(doc);
        return ah_local_error(call, result, error, code == AH_OK ? AH_LIMIT : code,
                              "Search interrupted or output budget too small; narrow the scope");
    }
    return ah_local_finish(call, result, doc, error);
}
