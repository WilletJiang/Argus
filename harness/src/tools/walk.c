#include "local_internal.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

static int compare_paths(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static ah_code collect(int fd, const char *prefix, unsigned depth, const ah_tool_call *call,
                       ah_paths *paths, ah_hash *topology) {
    if (depth > 32) {
        paths->truncated = true;
        close(fd);
        return AH_OK;
    }
    DIR *dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        return AH_IO;
    }
    ah_code code = AH_OK;
    struct dirent *entry;
    while ((code = ah_local_stopped(call)) == AH_OK) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            if (errno)
                code = AH_IO;
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
            !strcmp(entry->d_name, ".git") || !strncmp(entry->d_name, ".ah-write-", 10))
            continue;
        if (++paths->visited > 16384 || paths->count == 2048) {
            paths->truncated = true;
            break;
        }
        size_t complete, n = strlen(entry->d_name);
        if (!ah_local_utf8(entry->d_name, n, &complete) || complete != n) {
            paths->skipped++;
            continue;
        }
        char path[4097];
        if (snprintf(path, sizeof(path), "%s%s%s", prefix, *prefix ? "/" : "", entry->d_name) >=
            (int)sizeof(path)) {
            paths->skipped++;
            continue;
        }
        char kind;
#if defined(_DIRENT_HAVE_D_TYPE) || defined(__APPLE__)
        /* Search only needs entry kinds. Every later open still pins and checks
         * the descriptor. Verification also needs directory metadata. */
        if (!topology && entry->d_type != DT_UNKNOWN)
            kind = entry->d_type == DT_DIR ? 'd' : entry->d_type == DT_REG ? 'f' : 'x';
        else
#endif
        {
            struct stat st;
            if (fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
                code = AH_IO;
                break;
            }
            kind = S_ISDIR(st.st_mode) ? 'd' : S_ISREG(st.st_mode) ? 'f' : 'x';
            if (topology) {
                ah_hash_update(topology, path, strlen(path) + 1);
                ah_hash_update(topology, &kind, 1);
                if (kind == 'd')
                    ah_local_stat_hash(topology, &st);
            }
        }
        if (kind == 'd') {
            int child =
                openat(dirfd(dir), entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (child < 0) {
                code = AH_IO;
                break;
            }
            code = collect(child, path, depth + 1, call, paths, topology);
            if (code != AH_OK || paths->truncated)
                break;
        } else if (kind == 'f') {
            char *copy = ah_strdup(path);
            if (!copy) {
                code = AH_IO;
                break;
            }
            paths->items[paths->count++] = copy;
        } else
            paths->skipped++;
    }
    closedir(dir);
    return code;
}

ah_code ah_local_paths(ah_local_tools *tools, yyjson_val *scope, const ah_tool_call *call,
                       ah_paths *paths, ah_hash *topology) {
    memset(paths, 0, sizeof(*paths));
    if (topology)
        ah_hash_init(topology);
    const char *path = yyjson_get_str(scope);
    if (!path || !*path || memchr(path, 0, yyjson_get_len(scope)))
        return AH_INVALID;
    int fd = -1;
    if (!strcmp(path, "."))
        fd = openat(tools->workspace_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    else {
        char leaf[4097];
        int parent = ah_local_parent_fd(tools, scope, leaf);
        if (parent < 0)
            return AH_INVALID;
        fd = openat(parent, leaf, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        close(parent);
    }
    if (fd < 0)
        return AH_IO;
    struct stat st;
    ah_code code = AH_OK;
    if (fstat(fd, &st)) {
        close(fd);
        return AH_IO;
    }
    if (S_ISREG(st.st_mode)) {
        paths->items[0] = ah_strdup(path);
        if (!paths->items[0])
            code = AH_IO;
        else
            paths->count = 1;
        close(fd);
    } else if (S_ISDIR(st.st_mode))
        code = collect(fd, !strcmp(path, ".") ? "" : path, 0, call, paths, topology);
    else {
        close(fd);
        code = AH_INVALID;
    }
    qsort(paths->items, paths->count, sizeof(paths->items[0]), compare_paths);
    return code;
}
void ah_local_paths_free(ah_paths *paths) {
    for (size_t i = 0; i < paths->count; i++)
        free(paths->items[i]);
}
