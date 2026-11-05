#include "local_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

void ah_local_stat_hash(ah_hash *hash, const struct stat *st) {
#ifdef __APPLE__
    struct timespec modified = st->st_mtimespec, changed = st->st_ctimespec;
#else
    struct timespec modified = st->st_mtim, changed = st->st_ctim;
#endif
    char fields[256];
    int n =
        snprintf(fields, sizeof(fields), "%llu:%llu:%llu:%llu:%llu:%lld:%lld:%ld:%lld:%ld",
                 (unsigned long long)st->st_dev, (unsigned long long)st->st_ino,
                 (unsigned long long)st->st_mode, (unsigned long long)st->st_uid,
                 (unsigned long long)st->st_gid, (long long)st->st_size, (long long)modified.tv_sec,
                 modified.tv_nsec, (long long)changed.tv_sec, changed.tv_nsec);
    ah_hash_update(hash, fields, (size_t)n);
}

ah_code ah_local_input_revision(ah_local_tools *tools, yyjson_val *path, const ah_tool_call *call,
                                char revision[65]) {
    const char *name = yyjson_get_str(path);
    char leaf[4097];
    int parent = -1, fd = -1;
    if (name && !strcmp(name, "."))
        fd = openat(tools->workspace_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    else {
        parent = ah_local_parent_fd(tools, path, leaf);
        if (parent >= 0)
            fd = openat(parent, leaf, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    }
    if (parent >= 0)
        close(parent);
    if (fd < 0)
        return AH_IO;
    struct stat st;
    ah_code code = fstat(fd, &st) ? AH_IO : AH_OK;
    bool directory = code == AH_OK && S_ISDIR(st.st_mode);
    if (code == AH_OK && !directory) {
        char content[65];
        struct stat after;
        code = ah_local_revision(fd, call, content);
        if (code == AH_OK && (fstat(fd, &after) || !ah_local_same_file(&st, &after)))
            code = AH_INVALID;
        if (code == AH_OK) {
            ah_hash hash;
            ah_hash_init(&hash);
            ah_hash_update(&hash, content, 64);
            ah_local_stat_hash(&hash, &after);
            ah_hash_finish(&hash, revision);
        }
    }
    close(fd);
    if (!directory || code != AH_OK)
        return code;
    ah_paths paths;
    code = ah_local_paths(tools, path, call, &paths);
    if (code == AH_OK && (paths.truncated || paths.skipped))
        code = AH_LIMIT;
    char topology[65];
    ah_hash_finish(&paths.topology, topology);
    ah_hash hash;
    ah_hash_init(&hash);
    ah_hash_update(&hash, topology, 64);
    ah_local_stat_hash(&hash, &st);
    size_t bytes = 0;
    for (size_t i = 0; code == AH_OK && i < paths.count; i++) {
        char digest[65];
        parent = ah_local_parent_path(tools, paths.items[i], strlen(paths.items[i]), leaf);
        fd =
            parent >= 0 ? openat(parent, leaf, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC) : -1;
        if (parent >= 0)
            close(parent);
        if (fd < 0) {
            code = AH_IO;
            break;
        }
        if (fstat(fd, &st) || st.st_size < 0)
            code = AH_IO;
        else {
            bytes += (size_t)st.st_size;
            if (bytes > 32 * 1024 * 1024)
                code = AH_LIMIT;
        }
        if (code == AH_OK)
            code = ah_local_revision(fd, call, digest);
        struct stat after;
        if (code == AH_OK && (fstat(fd, &after) || !ah_local_same_file(&st, &after)))
            code = AH_INVALID;
        close(fd);
        if (code != AH_OK)
            break;
        ah_hash_update(&hash, paths.items[i], strlen(paths.items[i]) + 1);
        ah_hash_update(&hash, digest, 64);
        ah_local_stat_hash(&hash, &after);
    }
    ah_local_paths_free(&paths);
    if (code == AH_OK)
        ah_hash_finish(&hash, revision);
    return code;
}

ah_code ah_local_conflict(const ah_tool_call *call, ah_tool_result *result, ah_error *error,
                          const char *path, const char *source, size_t length,
                          const char *message) {
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *out = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, out);
    char revision[65];
    ah_hash_bytes(source, length, revision);
    size_t limit = ah_local_view_limit(call), keep = limit > 1024 ? (limit - 1024) / 6 : 0,
           complete;
    if (keep > 4096)
        keep = 4096;
    if (keep > length)
        keep = length;
    ah_local_utf8(source, keep, &complete);
    keep = complete;
    yyjson_mut_obj_add_bool(doc, out, "ok", false);
    yyjson_mut_obj_add_str(doc, out, "error", "conflict");
    yyjson_mut_obj_add_strcpy(doc, out, "path", path);
    yyjson_mut_obj_add_strcpy(doc, out, "message", message);
    yyjson_mut_obj_add_strcpy(doc, out, "revision", revision);
    yyjson_mut_obj_add_strncpy(doc, out, "content", source, keep);
    yyjson_mut_obj_add_bool(doc, out, "truncated", keep < length);
    yyjson_mut_obj_add_uint(doc, out, "next_offset_bytes", keep);
    result->is_error = true;
    return ah_local_finish(call, result, doc, error);
}

ah_code ah_local_revision(int fd, const ah_tool_call *call, char revision[65]) {
    struct stat before, after;
    off_t position = lseek(fd, 0, SEEK_CUR);
    if (position < 0 || fstat(fd, &before) || !S_ISREG(before.st_mode))
        return AH_IO;
    if (before.st_size < 0 || before.st_size > AH_LOCAL_MAX_BYTES)
        return AH_LIMIT;
    if (lseek(fd, 0, SEEK_SET) < 0)
        return AH_IO;
    ah_hash hash;
    ah_hash_init(&hash);
    char buffer[8192];
    size_t total = 0;
    ah_code code = AH_OK;
    while ((code = ah_local_stopped(call)) == AH_OK) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            code = AH_IO;
            break;
        }
        if (!n)
            break;
        total += (size_t)n;
        if (total > AH_LOCAL_MAX_BYTES) {
            code = AH_LIMIT;
            break;
        }
        ah_hash_update(&hash, buffer, (size_t)n);
    }
    if (code == AH_OK && (fstat(fd, &after) || !ah_local_same_file(&before, &after)))
        code = AH_INVALID;
    if (lseek(fd, position, SEEK_SET) < 0)
        code = AH_IO;
    if (code == AH_OK)
        ah_hash_finish(&hash, revision);
    return code;
}

bool ah_local_same_file(const struct stat *a, const struct stat *b) {
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

ah_code ah_local_source(int parent, const char *leaf, const ah_tool_call *call,
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
    if (code == AH_OK && (fstat(fd, &after) || !ah_local_same_file(original, &after)))
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
