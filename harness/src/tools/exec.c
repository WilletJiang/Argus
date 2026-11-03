#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "local_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>

const char ah_exec_schema[] = "{\"type\":\"object\",\"properties\":{\"argv\":{\"type\":\"array\","
                              "\"minItems\":1,\"maxItems\":128,"
                              "\"items\":{\"type\":\"string\",\"maxLength\":32768}}},\"required\":["
                              "\"argv\"],\"additionalProperties\":false}";

static void close_pair(int pair[2]) {
    if (pair[0] >= 0)
        close(pair[0]);
    if (pair[1] >= 0)
        close(pair[1]);
}

static int make_pipe(int pair[2]) {
    if (pipe(pair))
        return errno;
    for (unsigned i = 0; i < 2; ++i) {
        if (pair[i] < 3) {
            int moved = fcntl(pair[i], F_DUPFD_CLOEXEC, 3);
            if (moved < 0) {
                int saved = errno;
                close_pair(pair);
                pair[0] = pair[1] = -1;
                return saved;
            }
            close(pair[i]);
            pair[i] = moved;
        }
        if (fcntl(pair[i], F_SETFD, FD_CLOEXEC)) {
            int saved = errno;
            close_pair(pair);
            pair[0] = pair[1] = -1;
            return saved;
        }
    }
    if (fcntl(pair[0], F_SETFL, O_NONBLOCK)) {
        int saved = errno;
        close_pair(pair);
        pair[0] = pair[1] = -1;
        return saved;
    }
    return 0;
}

static int start_child(ah_local_tools *tools, char **argv, int out[2], int err[2], pid_t *pid) {
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc)
        return rc;
    rc = posix_spawnattr_init(&attributes);
    if (rc) {
        posix_spawn_file_actions_destroy(&actions);
        return rc;
    }
    short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef __APPLE__
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    sigset_t mask, defaults;
    sigemptyset(&mask);
    sigemptyset(&defaults);
    int signals[] = {SIGINT, SIGTERM, SIGPIPE, SIGHUP, SIGQUIT, SIGCHLD};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i)
        sigaddset(&defaults, signals[i]);
    if (!rc)
        rc = posix_spawnattr_setflags(&attributes, flags);
    if (!rc)
        rc = posix_spawnattr_setpgroup(&attributes, 0);
    if (!rc)
        rc = posix_spawnattr_setsigmask(&attributes, &mask);
    if (!rc)
        rc = posix_spawnattr_setsigdefault(&attributes, &defaults);
#if defined(__APPLE__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (!rc)
        rc = posix_spawn_file_actions_addfchdir_np(&actions, tools->workspace_fd);
#pragma clang diagnostic pop
#elif defined(__GLIBC__)
    if (!rc)
        rc = posix_spawn_file_actions_addfchdir_np(&actions, tools->workspace_fd);
#else
    rc = ENOTSUP; /* Fail closed where descriptor-safe cwd/close-all is unavailable. */
#endif
    if (!rc)
        rc = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (!rc)
        rc = posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    if (!rc)
        rc = posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO);
#ifdef __APPLE__
    if (!rc)
        rc = posix_spawn_file_actions_addclose(&actions, tools->workspace_fd);
    if (!rc)
        rc = posix_spawn_file_actions_addclose(&actions, out[0]);
    if (!rc)
        rc = posix_spawn_file_actions_addclose(&actions, out[1]);
    if (!rc)
        rc = posix_spawn_file_actions_addclose(&actions, err[0]);
    if (!rc)
        rc = posix_spawn_file_actions_addclose(&actions, err[1]);
#elif defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 34)
    if (!rc)
        rc = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#else
    rc = ENOTSUP;
#endif
#else
    rc = ENOTSUP;
#endif
    /* No inherited environment, provider credentials, HOME, startup files, or loader overrides. */
    char *environment[] = {"PATH=/usr/bin:/bin", "LANG=C", "LC_ALL=C", NULL};
    if (!rc) {
        if (strchr(argv[0], '/'))
            rc = posix_spawn(pid, argv[0], &actions, &attributes, argv, environment);
        else {
            char executable[32780];
            snprintf(executable, sizeof(executable), "/usr/bin/%s", argv[0]);
            rc = posix_spawn(pid, executable, &actions, &attributes, argv, environment);
            if (rc == ENOENT) {
                snprintf(executable, sizeof(executable), "/bin/%s", argv[0]);
                rc = posix_spawn(pid, executable, &actions, &attributes, argv, environment);
            }
        }
    }
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    return rc;
}

typedef struct {
    int fd;
    char *bytes;
    size_t length;
} capture;

/* A bounded chunk per loop keeps a continuously writing child from starving cancellation. */
static bool drain(capture *stream, size_t *remaining, bool *truncated) {
    char buffer[8192];
    for (unsigned i = 0; i < 8 && stream->fd >= 0; ++i) {
        ssize_t n = read(stream->fd, buffer, sizeof(buffer));
        if (n > 0) {
            size_t keep = (size_t)n < *remaining ? (size_t)n : *remaining;
            memcpy(stream->bytes + stream->length, buffer, keep);
            stream->length += keep;
            *remaining -= keep;
            if (keep < (size_t)n)
                *truncated = true;
        } else if (!n) {
            close(stream->fd);
            stream->fd = -1;
        } else if (errno == EINTR)
            continue;
        else if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        else
            return false;
    }
    return true;
}

static bool add_capture(yyjson_mut_doc *doc, yyjson_mut_val *root, const char *key,
                        const char *encoding_key, capture *stream, bool truncated) {
    size_t complete;
    if (ah_local_utf8(stream->bytes, stream->length, &complete) &&
        (complete == stream->length || truncated)) {
        return yyjson_mut_obj_add_strncpy(doc, root, key, stream->bytes, complete) &&
               yyjson_mut_obj_add_str(doc, root, encoding_key, "utf-8");
    }
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t length = ((stream->length + 2) / 3) * 4;
    char *encoded = malloc(length + 1);
    if (!encoded)
        return false;
    size_t at = 0;
    for (size_t i = 0; i < stream->length; i += 3) {
        unsigned a = (unsigned char)stream->bytes[i];
        unsigned b = i + 1 < stream->length ? (unsigned char)stream->bytes[i + 1] : 0;
        unsigned c = i + 2 < stream->length ? (unsigned char)stream->bytes[i + 2] : 0;
        encoded[at++] = alphabet[a >> 2];
        encoded[at++] = alphabet[((a & 3) << 4) | (b >> 4)];
        encoded[at++] = i + 1 < stream->length ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        encoded[at++] = i + 2 < stream->length ? alphabet[c & 63] : '=';
    }
    encoded[length] = '\0';
    bool ok = yyjson_mut_obj_add_strncpy(doc, root, key, encoded, length) &&
              yyjson_mut_obj_add_str(doc, root, encoding_key, "base64");
    free(encoded);
    return ok;
}

ah_code ah_local_exec(void *context, const ah_tool_call *call, ah_tool_result *result,
                      ah_error *error) {
    ah_local_tools *tools = context;
    memset(result, 0, sizeof(*result));
    if (!tools->enable_exec)
        return ah_local_error(call, result, error, AH_AUTH, "Process execution is disabled");
    ah_code code = ah_schema_validate(ah_exec_schema, call->arguments_json, error);
    if (code != AH_OK)
        return ah_local_error(call, result, error, code, "Invalid process arguments");
    code = ah_local_stopped(call);
    if (code != AH_OK)
        return ah_local_error(call, result, error, code, "Process cancelled before launch");
    if (ah_local_limit(call) < 320)
        return ah_local_error(call, result, error, AH_LIMIT,
                              "Output limit too small for process receipt");
    yyjson_doc *args = yyjson_read(call->arguments_json, strlen(call->arguments_json), 0);
    yyjson_val *array = yyjson_obj_get(yyjson_doc_get_root(args), "argv");
    char *argv[129] = {0};
    size_t i, count, bytes = 0;
    yyjson_val *argument;
    yyjson_arr_foreach(array, i, count, argument) {
        size_t length = yyjson_get_len(argument);
        bytes += length;
        if (memchr(yyjson_get_str(argument), '\0', length) || bytes > 131072 || (!i && !length)) {
            yyjson_doc_free(args);
            return ah_local_error(call, result, error, AH_INVALID,
                                  "argv contains NUL, empty executable, or excessive data");
        }
        argv[i] = (char *)yyjson_get_str(argument);
    }
    size_t limit = ah_local_limit(call), capacity = limit > 320 ? (limit - 320) / 6 : 0;
    capture streams[2] = {{-1, malloc(capacity + 1), 0}, {-1, malloc(capacity + 1), 0}};
    if (!streams[0].bytes || !streams[1].bytes) {
        free(streams[0].bytes);
        free(streams[1].bytes);
        yyjson_doc_free(args);
        return ah_local_error(call, result, error, AH_IO, "Out of memory");
    }
    int out[2] = {-1, -1}, err[2] = {-1, -1};
    int rc = make_pipe(out);
    if (!rc)
        rc = make_pipe(err);
    pid_t pid = -1;
    if (!rc)
        rc = start_child(tools, argv, out, err, &pid);
    yyjson_doc_free(args);
    if (rc) {
        close_pair(out);
        close_pair(err);
        free(streams[0].bytes);
        free(streams[1].bytes);
        return ah_local_error(call, result, error, AH_IO, "Cannot launch process safely");
    }
    close(out[1]);
    close(err[1]);
    streams[0].fd = out[0];
    streams[1].fd = err[0];
    bool truncated = false, reaped = false, killing = false;
    int status = 0;
    uint64_t stopped_at = 0;
    uint64_t fallback_deadline = ah_now_ms() + 60000;
    size_t remaining = capacity;
    while (!reaped || streams[0].fd >= 0 || streams[1].fd >= 0 || (stopped_at && !killing)) {
        if (code == AH_OK)
            code = ah_local_stopped(call);
        if (code == AH_OK && !call->deadline_ms && ah_now_ms() >= fallback_deadline)
            code = AH_LIMIT;
        if (code != AH_OK && !stopped_at) {
            stopped_at = ah_now_ms();
            result->outcome_unknown = true;
            kill(-pid, SIGTERM);
        }
        if (stopped_at && ah_now_ms() - stopped_at >= 200 && !killing) {
            kill(-pid, SIGKILL);
            killing = true;
        }
        if (stopped_at && ah_now_ms() - stopped_at >= 500) {
            for (unsigned j = 0; j < 2; ++j)
                if (streams[j].fd >= 0) {
                    close(streams[j].fd);
                    streams[j].fd = -1;
                }
        }
        for (unsigned j = 0; j < 2; ++j)
            if (streams[j].fd >= 0 && !drain(&streams[j], &remaining, &truncated) && code == AH_OK)
                code = AH_IO;
        if (!reaped) {
            pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid)
                reaped = true;
            else if (waited < 0 && errno != EINTR) {
                reaped = true;
                code = AH_UNCERTAIN;
                result->outcome_unknown = true;
            }
        }
        if (reaped && streams[0].fd < 0 && streams[1].fd < 0 && (!stopped_at || killing))
            break;
        struct pollfd fds[2] = {{streams[0].fd, POLLIN, 0}, {streams[1].fd, POLLIN, 0}};
        if (poll(fds, 2, 20) < 0 && errno != EINTR && code == AH_OK)
            code = AH_IO;
    }
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    result->is_error = code != AH_OK || !WIFEXITED(status) || WEXITSTATUS(status) != 0;
    yyjson_mut_obj_add_bool(doc, root, "ok", !result->is_error);
    if (WIFEXITED(status))
        yyjson_mut_obj_add_int(doc, root, "exit_code", WEXITSTATUS(status));
    else
        yyjson_mut_obj_add_null(doc, root, "exit_code");
    if (WIFSIGNALED(status))
        yyjson_mut_obj_add_int(doc, root, "signal", WTERMSIG(status));
    else
        yyjson_mut_obj_add_null(doc, root, "signal");
    bool encoded = add_capture(doc, root, "stdout", "stdout_encoding", &streams[0], truncated) &&
                   add_capture(doc, root, "stderr", "stderr_encoding", &streams[1], truncated);
    yyjson_mut_obj_add_bool(doc, root, "truncated", truncated);
    yyjson_mut_obj_add_bool(doc, root, "outcome_unknown", result->outcome_unknown);
    if (code != AH_OK)
        yyjson_mut_obj_add_str(doc, root, "error", ah_code_name(code));
    free(streams[0].bytes);
    free(streams[1].bytes);
    if (!encoded) {
        ah_mut_doc_free(doc);
        result->outcome_unknown = true;
        return ah_local_error(call, result, error, AH_IO, "Could not encode process output");
    }
    bool outcome_unknown = result->outcome_unknown;
    result->outcome_unknown = true;
    ah_code finish = ah_local_finish(call, result, doc, error);
    if (finish != AH_OK)
        return finish;
    result->outcome_unknown = outcome_unknown;
    return code == AH_OK
               ? AH_OK
               : ah_fail(error, code, "Process stopped or failed; side effects may have occurred");
}
