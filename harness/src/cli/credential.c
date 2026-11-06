#include "cli.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char *cli_load_token(const char *path) {
    if (!path) {
        const char *env = getenv("AH_ACCESS_TOKEN");
        if (!env)
            return NULL;
        size_t n = strlen(env);
        char *copy = malloc(n + 1);
        if (copy)
            memcpy(copy, env, n + 1);
        return copy;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0077) ||
        st.st_size <= 0 || st.st_size > 65536) {
        close(fd);
        return NULL;
    }
    size_t size = (size_t)st.st_size, n = 0;
    char *data = calloc(size + 1, 1);
    if (!data) {
        close(fd);
        return NULL;
    }
    while (n < size) {
        ssize_t r = read(fd, data + n, size - n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            free(data);
            close(fd);
            return NULL;
        }
        n += (size_t)r;
    }
    close(fd);
    if (memchr(data, 0, n)) {
        free(data);
        return NULL;
    }
    while (n && (data[n - 1] == '\n' || data[n - 1] == '\r'))
        data[--n] = 0;
    if (data[0] == '{') {
        yyjson_doc *doc = yyjson_read(data, n, 0);
        const char *token =
            doc ? yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "access_token")) : NULL;
        char *copy = NULL;
        if (token && *token) {
            copy = malloc(strlen(token) + 1);
            if (copy)
                strcpy(copy, token);
        }
        volatile unsigned char *wipe = (volatile unsigned char *)data;
        for (size_t i = 0; i < size; ++i)
            wipe[i] = 0;
        free(data);
        yyjson_doc_free(doc);
        return copy;
    }
    return data;
}
