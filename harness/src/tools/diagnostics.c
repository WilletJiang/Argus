#include "local_internal.h"
#include <ctype.h>
#include <stdio.h>

/* Resolve only declared source inputs, never paths suggested by arbitrary logs.
 * Recognize common path:line and Python File "path", line N diagnostics. */
void ah_local_diagnostics(ah_local_tools *tools, ah_local_check *check, const ah_tool_call *call,
                          yyjson_val *process, yyjson_mut_doc *output, yyjson_mut_val *root) {
    yyjson_doc *inputs = yyjson_read(check->inputs, strlen(check->inputs), 0);
    if (!inputs)
        return;
    yyjson_mut_val *excerpts = yyjson_mut_obj_add_arr(output, root, "source_excerpts");
    const char *fields[] = {"stderr", "stdout"};
    const char *encodings[] = {"stderr_encoding", "stdout_encoding"};
    for (unsigned stream = 0; stream < 2 && yyjson_mut_arr_size(excerpts) < 3; stream++) {
        if (!yyjson_equals_str(yyjson_obj_get(process, encodings[stream]), "utf-8"))
            continue;
        const char *text = yyjson_get_str(yyjson_obj_get(process, fields[stream]));
        if (!text)
            continue;
        size_t i, n;
        yyjson_val *path;
        yyjson_arr_foreach(yyjson_doc_get_root(inputs), i, n, path) {
            if (yyjson_mut_arr_size(excerpts) == 3 || ah_local_stopped(call) != AH_OK)
                break;
            ah_paths paths;
            if (ah_local_paths(tools, path, call, &paths) != AH_OK) {
                ah_local_paths_free(&paths);
                continue;
            }
            for (size_t pi = 0; pi < paths.count && yyjson_mut_arr_size(excerpts) < 3; pi++) {
                const char *name = paths.items[pi], *match = strstr(text, name);
                if (!match)
                    continue;
                const char *tail = match + strlen(name);
                if (!strncmp(tail, "\", line ", 8))
                    tail += 8;
                else if (*tail == ':')
                    tail++;
                else
                    continue;
                if (!isdigit((unsigned char)*tail))
                    continue;
                unsigned long line = strtoul(tail, NULL, 10);
                if (!line || line > 2147483647UL)
                    continue;
                uint64_t start = line > 2 ? line - 2 : 1;
                bool duplicate = false;
                for (size_t j = 0; j < yyjson_mut_arr_size(excerpts); j++) {
                    yyjson_mut_val *prior = yyjson_mut_arr_get(excerpts, j);
                    const char *prior_path = yyjson_mut_get_str(yyjson_mut_obj_get(prior, "path"));
                    if (prior_path && !strcmp(prior_path, name) &&
                        yyjson_mut_get_uint(yyjson_mut_obj_get(prior, "start_line")) == start)
                        duplicate = true;
                }
                if (duplicate)
                    continue;
                yyjson_mut_doc *args = ah_mut_doc_new();
                yyjson_mut_val *request = yyjson_mut_obj(args);
                yyjson_mut_doc_set_root(args, request);
                yyjson_mut_obj_add_strcpy(args, request, "path", name);
                yyjson_mut_obj_add_uint(args, request, "offset_line", start);
                yyjson_mut_obj_add_uint(args, request, "max_lines", 5);
                char *json = ah_mut_write(args);
                ah_mut_doc_free(args);
                if (!json)
                    break;
                ah_tool_call inner = *call;
                inner.arguments_json = json;
                inner.output_limit = 2048;
                inner.model_output_limit = 2048;
                ah_tool_result result = {0};
                ah_error ignored = {0};
                ah_code code = ah_local_read_file(tools, &inner, &result, &ignored);
                yyjson_doc *parsed =
                    result.json ? yyjson_read(result.json, strlen(result.json), 0) : NULL;
                if (code == AH_OK && !result.is_error && parsed)
                    yyjson_mut_arr_append(excerpts,
                                          yyjson_val_mut_copy(output, yyjson_doc_get_root(parsed)));
                yyjson_doc_free(parsed);
                free(result.json);
                free(json);
            }
            ah_local_paths_free(&paths);
        }
    }
    yyjson_doc_free(inputs);
}
