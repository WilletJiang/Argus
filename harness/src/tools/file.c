#include "local_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>

const char ah_write_schema[] =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"minLength\":1,"
    "\"maxLength\":4096},\"content\":{\"type\":\"string\",\"maxLength\":1048576}},"
    "\"required\":[\"path\",\"content\"],\"additionalProperties\":false}";
const char ah_edit_schema[] =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"minLength\":1,"
    "\"maxLength\":4096},\"old_text\":{\"type\":\"string\",\"minLength\":1,"
    "\"maxLength\":1048576},\"new_text\":{\"type\":\"string\",\"maxLength\":1048576}},"
    "\"required\":[\"path\",\"old_text\",\"new_text\"],\"additionalProperties\":false}";

static bool same_file(const struct stat *a, const struct stat *b) {
#ifdef __APPLE__
    bool times = a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec &&
                 a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
                 a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec &&
                 a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
    bool times = a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
                 a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
                 a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
    return times && a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
           a->st_mode == b->st_mode;
}

static ah_code replace_file(int parent, const char *leaf, const char *content, size_t length,
                            const struct stat *original, const ah_tool_call *call,
                            ah_tool_result *result, ah_error *error) {
    struct stat st;
    int exists = fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW);
    if ((!exists && !S_ISREG(st.st_mode)) || (exists && errno != ENOENT) ||
        (original && (exists || !same_file(original, &st))))
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
            (original && (exists || !same_file(original, &st))))
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
    result->outcome_unknown = true;
    code = ah_local_finish(call, result, doc, error);
    if (code == AH_OK)
        result->outcome_unknown = false;
    return code;
}

static yyjson_doc *write_arguments(const ah_tool_call *call, const char *schema,
                                   ah_tool_result *result, ah_error *error) {
    yyjson_doc *args = ah_local_arguments(call, schema, result, error);
    if (args && ah_local_limit(call) < 64) {
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
                       : replace_file(parent, leaf, yyjson_get_str(content), length, NULL, call,
                                      result, error);
    if (parent >= 0)
        close(parent);
    yyjson_doc_free(args);
    return code;
}

static ah_code read_edit_source(int parent, const char *leaf, const ah_tool_call *call,
                                struct stat *original, char **text, size_t *length) {
    int fd = openat(parent, leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return AH_IO;
    ah_code code = AH_OK;
    if (fstat(fd, original) || !S_ISREG(original->st_mode))
        code = AH_INVALID;
    else if (original->st_size < 0 || original->st_size > AH_LOCAL_MAX_BYTES)
        code = AH_LIMIT;
    char *data = code == AH_OK ? malloc(AH_LOCAL_MAX_BYTES + 2) : NULL;
    if (code == AH_OK && !data)
        code = AH_IO;
    size_t used = 0;
    while (code == AH_OK) {
        code = ah_local_stopped(call);
        if (code != AH_OK)
            break;
        ssize_t n = read(fd, data + used, AH_LOCAL_MAX_BYTES + 1 - used);
        if (n > 0) {
            used += (size_t)n;
            if (used > AH_LOCAL_MAX_BYTES)
                code = AH_LIMIT;
        } else if (!n)
            break;
        else if (errno != EINTR)
            code = AH_IO;
    }
    struct stat after;
    if (code == AH_OK && (fstat(fd, &after) || !same_file(original, &after)))
        code = AH_INVALID;
    close(fd);
    size_t complete;
    if (code == AH_OK &&
        (memchr(data, '\0', used) || !ah_local_utf8(data, used, &complete) || complete != used))
        code = AH_INVALID;
    if (code != AH_OK) {
        free(data);
        return code;
    }
    data[used] = '\0';
    *text = data;
    *length = used;
    return AH_OK;
}

ah_code ah_local_edit_file(void *context, const ah_tool_call *call, ah_tool_result *result,
                           ah_error *error) {
    ah_error local_error;
    if (!error)
        error = &local_error;
    yyjson_doc *args = write_arguments(call, ah_edit_schema, result, error);
    if (!args)
        return error->code;
    yyjson_val *root = yyjson_doc_get_root(args), *old = yyjson_obj_get(root, "old_text"),
               *replacement = yyjson_obj_get(root, "new_text");
    const char *old_text = yyjson_get_str(old), *new_text = yyjson_get_str(replacement);
    size_t old_length = yyjson_get_len(old), new_length = yyjson_get_len(replacement);
    char leaf[4097], *source = NULL, *edited = NULL;
    int parent = -1;
    struct stat original;
    size_t length = 0;
    const char *message = "Edit text exceeds byte limit or contains NUL bytes";
    ah_code code = AH_INVALID;
    if (old_length > AH_LOCAL_MAX_BYTES || new_length > AH_LOCAL_MAX_BYTES)
        code = AH_LIMIT;
    else if (!memchr(old_text, '\0', old_length) && !memchr(new_text, '\0', new_length)) {
        parent = ah_local_parent_fd(context, yyjson_obj_get(root, "path"), leaf);
        message = "Cannot read complete UTF-8 edit source (max 1 MiB), or path is invalid";
        code = parent < 0 ? AH_INVALID
                          : read_edit_source(parent, leaf, call, &original, &source, &length);
    }
    char *match = code == AH_OK ? strstr(source, old_text) : NULL;
    if (code == AH_OK && (!match || strstr(match + 1, old_text))) {
        message = "old_text must match exactly once; include more surrounding context";
        code = AH_INVALID;
    }
    if (code == AH_OK && new_length > AH_LOCAL_MAX_BYTES - (length - old_length)) {
        message = "Edited file exceeds byte limit";
        code = AH_LIMIT;
    }
    size_t new_size = code == AH_OK ? length - old_length + new_length : 0;
    if (code == AH_OK) {
        edited = malloc(new_size + 1);
        if (!edited) {
            message = "Out of memory";
            code = AH_IO;
        }
    }
    if (code == AH_OK) {
        size_t prefix = (size_t)(match - source);
        memcpy(edited, source, prefix);
        memcpy(edited + prefix, new_text, new_length);
        memcpy(edited + prefix + new_length, match + old_length, length - prefix - old_length);
        code = replace_file(parent, leaf, edited, new_size, &original, call, result, error);
    } else
        code = ah_local_error(call, result, error, code, message);
    free(edited);
    free(source);
    if (parent >= 0)
        close(parent);
    yyjson_doc_free(args);
    return code;
}
