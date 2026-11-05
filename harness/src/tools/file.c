#include "local_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>

const char ah_write_schema[] =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"minLength\":1,"
    "\"maxLength\":4096},\"content\":{\"type\":\"string\",\"maxLength\":1048576}},"
    "\"required\":[\"path\",\"content\"],\"additionalProperties\":false}";
ah_code ah_local_replace(int parent, const char *leaf, const char *content, size_t length,
                         const struct stat *original, const ah_tool_call *call,
                         ah_tool_result *result, ah_error *error) {
    struct stat st;
    int exists = fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW);
    if ((!exists && !S_ISREG(st.st_mode)) || (exists && errno != ENOENT) ||
        (original && (exists || !ah_local_same_file(original, &st))))
        return ah_local_error(call, result, error, AH_INVALID,
                              "Write target is not regular or changed during edit");
    mode_t mode = exists ? 0600 : st.st_mode & 07777;
    char temporary[96];
    int fd = -1;
    for (unsigned i = 0; i < 100; ++i) {
        snprintf(temporary, sizeof(temporary), ".ah-write-%ld-%llu-%u", (long)getpid(),
                 (unsigned long long)ah_now_ms(), i);
        fd = openat(parent, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST)
            break;
    }
    ah_code code = fd < 0 ? AH_IO : AH_OK;
    size_t done = 0;
    while (code == AH_OK && done < length) {
        code = ah_local_stopped(call);
        if (code != AH_OK)
            break;
        ssize_t n = write(fd, content + done, length - done);
        if (n > 0)
            done += (size_t)n;
        else if (n < 0 && errno == EINTR)
            continue;
        else
            code = AH_IO;
    }
    if (code == AH_OK && (fchmod(fd, mode) || fsync(fd)))
        code = AH_IO;
    if (fd >= 0 && close(fd) && code == AH_OK)
        code = AH_IO;
    if (code == AH_OK)
        code = ah_local_stopped(call);
    /* This detects observed changes, not a compare-and-swap against external writers. */
    if (code == AH_OK) {
        exists = fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW);
        if ((!exists && !S_ISREG(st.st_mode)) || (exists && errno != ENOENT) ||
            (original && (exists || !ah_local_same_file(original, &st))))
            code = AH_INVALID;
    }
    bool committed = false;
    if (code == AH_OK) {
        if (renameat(parent, temporary, parent, leaf))
            code = AH_IO;
        else {
            committed = true;
            if (fsync(parent)) {
                code = AH_UNCERTAIN;
                result->outcome_unknown = true;
            }
        }
    }
    if (!committed && fd >= 0)
        unlinkat(parent, temporary, 0);
    if (code != AH_OK)
        return ah_local_error(call, result, error, code,
                              committed
                                  ? "File replaced, but directory durability is uncertain"
                                  : "Write failed, target changed, or cancelled before commit");
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *output = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, output);
    yyjson_mut_obj_add_bool(doc, output, "ok", true);
    yyjson_mut_obj_add_uint(doc, output, "bytes", length);
    char revision[65];
    ah_hash_bytes(content, length, revision);
    yyjson_mut_obj_add_strcpy(doc, output, "revision", revision);
    yyjson_mut_obj_add_str(doc, output, "status", "applied");
    result->outcome_unknown = true;
    code = ah_local_finish(call, result, doc, error);
    if (code == AH_OK)
        result->outcome_unknown = false;
    return code;
}

static yyjson_doc *write_arguments(const ah_tool_call *call, const char *schema,
                                   ah_tool_result *result, ah_error *error) {
    yyjson_doc *args = ah_local_arguments(call, schema, result, error);
    if (args && ah_local_limit(call) < 256) {
        yyjson_doc_free(args);
        args = NULL;
        ah_local_error(call, result, error, AH_LIMIT, "Output limit too small for write receipt");
    }
    return args;
}

ah_code ah_local_write_file(void *context, const ah_tool_call *call, ah_tool_result *result,
                            ah_error *error) {
    ah_error local_error;
    if (!error)
        error = &local_error;
    yyjson_doc *args = write_arguments(call, ah_write_schema, result, error);
    if (!args)
        return error->code;
    yyjson_val *root = yyjson_doc_get_root(args), *content = yyjson_obj_get(root, "content");
    size_t length = yyjson_get_len(content);
    if (length > AH_LOCAL_MAX_BYTES) {
        yyjson_doc_free(args);
        return ah_local_error(call, result, error, AH_LIMIT, "File content exceeds byte limit");
    }
    char leaf[4097];
    int parent = ah_local_parent_fd(context, yyjson_obj_get(root, "path"), leaf);
    ah_code code = parent < 0
                       ? ah_local_error(call, result, error, AH_INVALID,
                                        "Path is invalid or cannot be traversed without symlinks")
                       : ah_local_replace(parent, leaf, yyjson_get_str(content), length, NULL, call,
                                          result, error);
    if (parent >= 0)
        close(parent);
    yyjson_doc_free(args);
    return code;
}
