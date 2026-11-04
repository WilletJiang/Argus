/* Persistent Responses transport for controlled cache probes, never a tool executor. */
#include "ah/responses.h"
#include "internal.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t stopped;
static void interrupt(int number) {
    (void)number;
    stopped = 1;
}
static bool cancelled(void *context) {
    (void)context;
    return stopped != 0;
}

int main(void) {
    ah_responses_config config;
    ah_responses_config_init(&config);
    config.bearer_token = getenv("AH_ACCESS_TOKEN");
    config.max_retries = 0;
    config.timeout_ms = 60000;
    const char *endpoint = getenv("AH_EVAL_ENDPOINT");
    if (endpoint)
        config.endpoint = endpoint;
    const char *loopback = getenv("AH_EVAL_ALLOW_LOOPBACK");
    config.allow_loopback_http = loopback && !strcmp(loopback, "1");
    ah_error error = {0};
    ah_responses *provider = NULL;
    ah_code code = ah_responses_open(&config, &provider, &error);
    if (code != AH_OK) {
        fprintf(stderr, "%s\n", error.message);
        return 1;
    }
    signal(SIGINT, interrupt);
    signal(SIGTERM, interrupt);
    ah_provider transport = ah_responses_provider(provider);
    const size_t capacity = 1024 * 1024;
    char *request = malloc(capacity);
    if (!request) {
        ah_responses_close(provider);
        return 1;
    }
    int exit_code = 0;
    while (!stopped && fgets(request, (int)capacity, stdin)) {
        size_t length = strlen(request);
        if (!length || request[length - 1] != '\n' || !ah_json_valid(request)) {
            fputs("Expected one bounded JSON request per line\n", stderr);
            exit_code = 2;
            break;
        }
        char *response = NULL;
        uint64_t started = ah_now_ms();
        code = transport.complete(transport.context, request, NULL, NULL, cancelled, NULL,
                                  &response, &error);
        uint64_t elapsed = ah_now_ms() - started;
        yyjson_mut_doc *doc = ah_mut_doc_new();
        yyjson_mut_val *root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_obj_add_bool(doc, root, "ok", code == AH_OK);
        yyjson_mut_obj_add_uint(doc, root, "elapsed_ms", elapsed);
        yyjson_doc *parsed = response ? yyjson_read(response, strlen(response), 0) : NULL;
        if (parsed)
            yyjson_mut_obj_add_val(doc, root, "response",
                                   yyjson_val_mut_copy(doc, yyjson_doc_get_root(parsed)));
        if (code != AH_OK) {
            yyjson_mut_obj_add_str(doc, root, "code", ah_code_name(code));
            yyjson_mut_obj_add_strcpy(doc, root, "error", error.message);
        }
        char *line = ah_mut_write(doc);
        if (line) {
            puts(line);
            fflush(stdout);
        } else
            exit_code = 1;
        free(line);
        yyjson_doc_free(parsed);
        ah_mut_doc_free(doc);
        free(response);
        if (exit_code || stopped)
            break;
    }
    free(request);
    ah_responses_close(provider);
    return exit_code;
}
