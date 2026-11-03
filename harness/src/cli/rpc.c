#include "cli.h"
#include "../internal.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RPC_LIMIT (1024u * 1024u)
typedef struct {
    cli_context *cli;
    pthread_t thread;
    atomic_bool done;
    bool started;
    char session[AH_ID_CAP];
    char *request_id;
} run_job;

static void reply(cli_context *cli, const char *id, ah_code code, const char *result,
                  const ah_error *error) {
    yyjson_mut_doc *doc = ah_mut_doc_new();
    if (!doc)
        return;
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, obj);
    yyjson_doc *id_doc = id ? yyjson_read(id, strlen(id), 0) : NULL;
    if (id_doc)
        yyjson_mut_obj_add_val(doc, obj, "id",
                               yyjson_val_mut_copy(doc, yyjson_doc_get_root(id_doc)));
    yyjson_mut_obj_add_str(doc, obj, "status", ah_code_name(code));
    yyjson_doc *body = result ? yyjson_read(result, strlen(result), 0) : NULL;
    if (body)
        yyjson_mut_obj_add_val(doc, obj, "result",
                               yyjson_val_mut_copy(doc, yyjson_doc_get_root(body)));
    if (code != AH_OK && error && error->message[0])
        yyjson_mut_obj_add_str(doc, obj, "error", error->message);
    char *text = ah_mut_write(doc);
    if (text)
        cli_print(cli, text);
    free(text);
    yyjson_doc_free(body);
    yyjson_doc_free(id_doc);
    ah_mut_doc_free(doc);
}
static void *run_worker(void *context) {
    run_job *job = context;
    ah_error error = {0};
    ah_code code = ah_run(job->cli->engine, job->session, &error);
    char *state = NULL;
    ah_error ignored = {0};
    (void)ah_session_json(job->cli->engine, job->session, &state, &ignored);
    reply(job->cli, job->request_id, code, state, &error);
    ah_free(state);
    atomic_store(&job->done, true);
    return NULL;
}
static const char *str(yyjson_val *obj, const char *key) {
    return yyjson_get_str(yyjson_obj_get(obj, key));
}
static void invalid(cli_context *cli, const char *id, const char *message) {
    ah_error error = {.code = AH_INVALID};
    snprintf(error.message, sizeof(error.message), "%s", message);
    reply(cli, id, AH_INVALID, NULL, &error);
}
static void dispatch(cli_context *cli, run_job *job, const char *line) {
    yyjson_doc *doc = yyjson_read(line, strlen(line), 0);
    yyjson_val *obj = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!yyjson_is_obj(obj) || ah_json_check(obj, NULL) != AH_OK) {
        invalid(cli, NULL, "Expected unambiguous bounded JSON object");
        yyjson_doc_free(doc);
        return;
    }
    yyjson_val *id_val = yyjson_obj_get(obj, "id");
    char *id = id_val ? yyjson_val_write(id_val, 0, NULL) : NULL;
    const char *method = str(obj, "method"), *session = str(obj, "session");
    const char *op = str(obj, "operation");
    ah_error error = {0};
    ah_code code = AH_INVALID;
    char *result = NULL;
    char new_id[AH_ID_CAP] = {0};
    bool running = job->started && !atomic_load(&job->done);
    if (!method) {
        invalid(cli, id, "Missing method");
        goto done;
    }
    if (!strcmp(method, "cancel")) {
        ah_cancel(cli->engine);
        reply(cli, id, AH_OK, "{\"requested\":true}", NULL);
        goto done;
    }
    if (!strcmp(method, "steer")) {
        code = ah_steer(cli->engine, session, str(obj, "command"), str(obj, "text"), &error);
        reply(cli, id, code, code == AH_OK ? "{\"queued\":true}" : NULL, &error);
        goto done;
    }
    if (running) {
        reply(cli, id, AH_BUSY, NULL, NULL);
        goto done;
    }
    if (job->started) {
        pthread_join(job->thread, NULL);
        job->started = false;
        free(job->request_id);
        job->request_id = NULL;
    }
    if (!strcmp(method, "create")) {
        code = ah_session_create(cli->engine, str(obj, "model"),
                                 str(obj, "instructions") ? str(obj, "instructions") : "", new_id,
                                 &error);
        if (code == AH_OK)
            code = ah_session_json(cli->engine, new_id, &result, &error);
    } else if (!strcmp(method, "list"))
        code = ah_sessions_json(cli->engine, &result, &error);
    else if (!session || strlen(session) >= AH_ID_CAP) {
        invalid(cli, id, "Missing or invalid session");
        goto done;
    } else if (!strcmp(method, "submit"))
        code = ah_submit(cli->engine, session, str(obj, "command"), str(obj, "text"), &error);
    else if (!strcmp(method, "run")) {
        memcpy(job->session, session, strlen(session) + 1);
        job->request_id = id;
        id = NULL;
        atomic_store(&job->done, false);
        if (pthread_create(&job->thread, NULL, run_worker, job) != 0) {
            invalid(cli, job->request_id, "Unable to start run worker");
            free(job->request_id);
            job->request_id = NULL;
        } else
            job->started = true;
        goto done;
    } else if (!strcmp(method, "inspect"))
        code = ah_session_json(cli->engine, session, &result, &error);
    else if (!strcmp(method, "resume"))
        code = ah_resume(cli->engine, session, &error);
    else if (!strcmp(method, "decide")) {
        yyjson_val *allow = yyjson_obj_get(obj, "allow");
        if (!yyjson_is_bool(allow)) {
            invalid(cli, id, "allow must be boolean");
            goto done;
        }
        code = ah_decide(cli->engine, session, op, yyjson_get_bool(allow), &error);
    } else if (!strcmp(method, "reply") || !strcmp(method, "resolve")) {
        yyjson_val *value = yyjson_obj_get(obj, "value");
        char *value_json = value ? yyjson_val_write(value, 0, NULL) : NULL;
        if (!strcmp(method, "reply"))
            code = ah_reply(cli->engine, session, op, value_json, &error);
        else {
            yyjson_val *flag = yyjson_obj_get(obj, "is_error");
            if (flag && !yyjson_is_bool(flag)) {
                free(value_json);
                invalid(cli, id, "is_error must be boolean");
                goto done;
            }
            code = ah_resolve(cli->engine, session, op, value_json, yyjson_get_bool(flag), &error);
        }
        free(value_json);
    } else if (!strcmp(method, "fork")) {
        code = ah_fork(cli->engine, session, new_id, &error);
        if (code == AH_OK)
            code = ah_session_json(cli->engine, new_id, &result, &error);
    } else if (!strcmp(method, "compact")) {
        yyjson_val *keep = yyjson_obj_get(obj, "keep_turns");
        if (!yyjson_is_uint(keep) || yyjson_get_uint(keep) > 10000) {
            invalid(cli, id, "keep_turns must be 0..10000");
            goto done;
        }
        code = ah_compact(cli->engine, session, str(obj, "summary"),
                          (unsigned)yyjson_get_uint(keep), &error);
    } else if (!strcmp(method, "events")) {
        yyjson_val *after = yyjson_obj_get(obj, "after");
        if (after && (!yyjson_is_uint(after) || yyjson_get_uint(after) > INT64_MAX)) {
            invalid(cli, id, "after must be a nonnegative event sequence");
            goto done;
        }
        code = ah_events(cli->engine, session, yyjson_get_uint(after), cli_observe, cli, &error);
    } else {
        invalid(cli, id, "Unknown method");
        goto done;
    }
    reply(cli, id, code, result, &error);
    ah_free(result);
done:
    free(id);
    yyjson_doc_free(doc);
}
int cli_rpc(cli_context *cli) {
    run_job job = {.cli = cli};
    atomic_init(&job.done, true);
    char *line = malloc(RPC_LIMIT + 1);
    if (!line)
        return 1;
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    int shutdown = cli_shutdown_fd();
    if (flags < 0 || shutdown < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) != 0) {
        free(line);
        return 1;
    }
    size_t used = 0;
    bool overflow = false;
    int result = 0;
    for (;;) {
        struct pollfd ready[2] = {{STDIN_FILENO, POLLIN, 0}, {shutdown, POLLIN, 0}};
        if (poll(ready, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            result = 1;
            break;
        }
        /* SIGTERM takes priority over queued commands, including an idle stdin. */
        if (ready[1].revents)
            break;
        if (ready[0].revents & POLLNVAL) {
            result = 1;
            break;
        }
        if (!(ready[0].revents & (POLLIN | POLLHUP | POLLERR)))
            continue;
        char buffer[4096];
        ssize_t length = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (length < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            result = 1;
            break;
        }
        if (length == 0) {
            if (used || overflow)
                invalid(cli, NULL, "Incomplete RPC frame at EOF");
            break;
        }
        for (ssize_t i = 0; i < length; ++i) {
            unsigned char ch = (unsigned char)buffer[i];
            if (ch == '\n') {
                line[used] = 0;
                if (overflow)
                    invalid(cli, NULL, "RPC frame exceeds 1 MiB or contains NUL");
                else if (used)
                    dispatch(cli, &job, line);
                used = 0;
                overflow = false;
            } else if (ch == 0)
                overflow = true;
            else if (used < RPC_LIMIT)
                line[used++] = (char)ch;
            else
                overflow = true;
        }
    }
    if (fcntl(STDIN_FILENO, F_SETFL, flags) != 0)
        result = 1;
    free(line);
    if (job.started) {
        if (!atomic_load(&job.done))
            ah_cancel(cli->engine);
        pthread_join(job.thread, NULL);
        free(job.request_id);
    }
    return result;
}
