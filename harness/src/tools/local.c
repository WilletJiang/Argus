#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "local_internal.h"
#include <errno.h>
#include <fcntl.h>

size_t ah_local_limit(const ah_tool_call *call) {
    size_t limit = call->output_limit ? call->output_limit : 65536;
    return limit < AH_LOCAL_MAX_BYTES ? limit : AH_LOCAL_MAX_BYTES;
}

size_t ah_local_view_limit(const ah_tool_call *call) {
    size_t limit = ah_local_limit(call);
    if (call->model_output_limit && call->model_output_limit < limit)
        limit = call->model_output_limit;
    return limit > 32 ? limit - 32 : 0; /* core is_error/result envelope */
}

ah_code ah_local_stopped(const ah_tool_call *call) {
    if (call->cancelled && call->cancelled(call->cancel_context))
        return AH_CANCELLED;
    if (call->deadline_ms && ah_now_ms() >= call->deadline_ms)
        return AH_LIMIT;
    return AH_OK;
}

ah_code ah_local_finish(const ah_tool_call *call, ah_tool_result *result, yyjson_mut_doc *doc,
                        ah_error *error) {
    char *json = ah_mut_write(doc);
    ah_mut_doc_free(doc);
    if (!json)
        return ah_local_error(call, result, error, AH_IO, "Could not encode tool output");
    if (strlen(json) > ah_local_limit(call)) {
        free(json);
        return ah_local_error(call, result, error, AH_LIMIT, "Output limit exceeded");
    }
    result->json = json;
    ah_error_clear(error);
    return AH_OK;
}

ah_code ah_local_error(const ah_tool_call *call, ah_tool_result *result, ah_error *error,
                       ah_code code, const char *message) {
    result->is_error = true;
    yyjson_mut_doc *doc = ah_mut_doc_new();
    if (doc) {
        yyjson_mut_val *root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_obj_add_bool(doc, root, "ok", false);
        yyjson_mut_obj_add_str(doc, root, "error", ah_code_name(code));
        yyjson_mut_obj_add_strcpy(doc, root, "message", message);
        result->json = ah_mut_write(doc);
        ah_mut_doc_free(doc);
    }
    if (result->json && strlen(result->json) > ah_local_limit(call)) {
        free(result->json);
        result->json = ah_strdup(ah_local_limit(call) >= 2 ? "{}" : "0");
    }
    if (!result->json)
        return ah_fail(error, AH_IO, "Could not encode tool failure receipt");
    /* A trustworthy failure receipt completes the callback without an ambiguous effect. */
    if (!result->outcome_unknown) {
        ah_error_clear(error);
        return AH_OK;
    }
    return ah_fail(error, code, "%s", message);
}

/* Returns a valid prefix for output truncation, but rejects malformed UTF-8. */
bool ah_local_utf8(const char *text, size_t length, size_t *complete) {
    size_t i = 0;
    *complete = 0;
    while (i < length) {
        /* Most source text is ASCII. memcpy permits unaligned input without
         * aliasing assumptions; the byte-high-bit mask works on either endian. */
        while (length - i >= sizeof(uint64_t)) {
            uint64_t word;
            memcpy(&word, text + i, sizeof(word));
            if (word & UINT64_C(0x8080808080808080))
                break;
            i += sizeof(word);
            *complete = i;
        }
        if (i == length)
            break;
        unsigned char c = (unsigned char)text[i];
        if (c < 0x80) {
            ++i;
            *complete = i;
            continue;
        }
        unsigned n = c >= 0xc2 && c <= 0xdf   ? 2
                     : c >= 0xe0 && c <= 0xef ? 3
                     : c >= 0xf0 && c <= 0xf4 ? 4
                                              : 0;
        if (!n)
            return false;
        if (length - i < n)
            return true;
        for (unsigned j = 1; j < n; ++j)
            if (((unsigned char)text[i + j] & 0xc0) != 0x80)
                return false;
        unsigned char second = (unsigned char)text[i + 1];
        if ((c == 0xe0 && second < 0xa0) || (c == 0xed && second >= 0xa0) ||
            (c == 0xf0 && second < 0x90) || (c == 0xf4 && second >= 0x90))
            return false;
        i += n;
        *complete = i;
    }
    return true;
}

/* Walk each directory using the pinned workspace descriptor. Never follow links. */
int ah_local_parent_fd(ah_local_tools *tools, yyjson_val *path, char leaf[4097]) {
    return ah_local_parent_path(tools, yyjson_get_str(path), yyjson_get_len(path), leaf);
}
int ah_local_parent_path(ah_local_tools *tools, const char *text, size_t length, char leaf[4097]) {
    if (!text || !length || length > 4096 || text[0] == '/' || memchr(text, '\0', length)) {
        errno = EINVAL;
        return -1;
    }
    char copy[4097];
    memcpy(copy, text, length);
    copy[length] = '\0';
    int fd = fcntl(tools->workspace_fd, F_DUPFD_CLOEXEC, 3);
    if (fd < 0)
        return -1;
    char *part = copy;
    unsigned count = 0;
    for (;;) {
        char *slash = strchr(part, '/');
        if (slash)
            *slash = '\0';
        if (!*part || !strcmp(part, ".") || !strcmp(part, "..") || ++count > 64) {
            close(fd);
            errno = EINVAL;
            return -1;
        }
        if (!slash) {
            strcpy(leaf, part);
            return fd;
        }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        int saved = errno;
        close(fd);
        if (next < 0) {
            errno = saved;
            return -1;
        }
        fd = next;
        part = slash + 1;
    }
}

yyjson_doc *ah_local_arguments(const ah_tool_call *call, const char *schema, ah_tool_result *result,
                               ah_error *error) {
    memset(result, 0, sizeof(*result));
    yyjson_doc *doc;
    ah_code code = ah_schema_parse(schema, call->arguments_json, &doc, error);
    if (code == AH_OK)
        code = ah_local_stopped(call);
    if (code != AH_OK) {
        yyjson_doc_free(doc);
        ah_local_error(call, result, error, code,
                       "Invalid arguments or cancelled/deadline reached");
        return NULL;
    }
    return doc;
}

ah_code ah_local_tools_open(const ah_local_tools_config *config, ah_local_tools **out,
                            ah_error *error) {
    if (out)
        *out = NULL;
    if (!config || !config->workspace || !*config->workspace || !out)
        return ah_fail(error, AH_INVALID, "An explicit workspace is required");
    int fd = open(config->workspace, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return ah_fail(error, AH_IO, "Cannot open workspace: %s", strerror(errno));
    if (fd < 3) {
        int moved = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        close(fd);
        if (moved < 0)
            return ah_fail(error, AH_IO, "Cannot anchor workspace descriptor");
        fd = moved;
    }
    ah_local_tools *tools = calloc(1, sizeof(*tools));
    if (!tools) {
        close(fd);
        return ah_fail(error, AH_IO, "Out of memory");
    }
    tools->workspace_fd = fd;
    tools->enable_exec = config->enable_exec;
    tools->approve_writes = config->approve_writes;
    tools->enable_workflows = config->enable_workflows;
    *out = tools;
    ah_error_clear(error);
    return AH_OK;
}

ah_code ah_local_tools_register(ah_local_tools *tools, ah_engine *engine, ah_error *error) {
    if (!tools || !engine || tools->registered)
        return ah_fail(error, AH_INVALID, "Missing local tools or engine");
    /* Even a partial registration has copied policies into the engine. Freeze
     * presets before publishing any callback that can consult their context. */
    tools->registered = true;
    bool approval = tools->approve_writes;
    for (size_t i = 0; i < tools->check_count; i++)
        approval |= tools->checks[i].approval;
    ah_tool definitions[] = {
        {"apply_changes",
         "Apply one or more files' non-overlapping exact hunks against optional base_revision "
         "from read_file/find. Each old_text must match once in the original file. All files are "
         "validated first; per-file writes are atomic, the group is not. Optionally run named host "
         "checks after all edits succeed. On partial/unknown results inspect receipts; never "
         "replay "
         "the whole group. Use one file and one edit for a single replacement.",
         ah_apply_schema, AH_EFFECT_EXTERNAL, approval, ah_local_apply, tools, false},
        {"find",
         "Search a single-line literal in workspace UTF-8 files and return merged source "
         "excerpts with SHA-256 revisions. Use path '.' for root; optional glob matches relative "
         "paths. "
         "Skips symlinks, .git, non-text and files over 1 MiB. Bounded scan: narrow path/glob when "
         "scan_truncated. Paginate with next_offset and snapshot; use read_file for truncated "
         "excerpts.",
         ah_find_schema, AH_EFFECT_READ, false, ah_local_find, tools, true},
        {"read_file",
         "Read UTF-8 file lines (default 200, max 1000) from offset_line (default 1). "
         "Use next_offset_bytes as offset_bytes for exact continuation, including partial lines. "
         "Line and byte offsets are mutually exclusive; refuses symlinks. Returns a SHA-256 "
         "revision "
         "for files up to 1 MiB. Identical content already visible in context may be referenced; "
         "set force=true to repeat the content. Group independent reads in one response.",
         ah_read_schema, AH_EFFECT_READ, false, ah_local_read_file, tools, true},
        {"write_file",
         "Atomically replace a workspace-relative regular file with UTF-8 content; refuses "
         "symlinks.",
         ah_write_schema, AH_EFFECT_WRITE, tools->approve_writes, ah_local_write_file, tools,
         false},
        {"exec",
         "Execute argv in the workspace with broad local process authority. This is NOT a sandbox.",
         ah_exec_schema, AH_EFFECT_EXTERNAL, true, ah_local_exec, tools, false}};
    size_t count = sizeof(definitions) / sizeof(*definitions);
    for (size_t i = 0; i < count; ++i) {
        if ((!tools->enable_workflows && !strcmp(definitions[i].name, "find")) ||
            (!tools->enable_exec && !strcmp(definitions[i].name, "exec")))
            continue;
        ah_code code = ah_register_tool(engine, &definitions[i], error);
        if (code != AH_OK)
            return code;
    }
    ah_code code = ah_local_checks_register(tools, engine, error);
    return code;
}

void ah_local_tools_close(ah_local_tools *tools) {
    if (!tools)
        return;
    close(tools->workspace_fd);
    ah_local_checks_free(tools);
    free(tools);
}
