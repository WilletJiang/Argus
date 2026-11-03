#include "ah/responses.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t start, cancel_after;
    size_t deltas;
} fixture;
static bool cancelled(void *context) {
    fixture *f = context;
    return f->cancel_after && ah_now_ms() - f->start >= f->cancel_after;
}
static void delta(void *context, const char *text, size_t length) {
    (void)text;
    ((fixture *)context)->deltas += length;
}
int main(int argc, char **argv) {
    if (argc != 5)
        return 2;
    ah_responses_config config;
    ah_responses_config_init(&config);
    config.endpoint = argv[1];
    config.allow_loopback_http = true;
    config.bearer_token = "local-test-token";
    config.timeout_ms = strtoull(argv[2], NULL, 10);
    config.max_response_bytes = 8192;
    config.max_event_bytes = 4096;
    config.max_retries = 2;
    ah_error error;
    ah_responses *responses = NULL;
    ah_code code = ah_responses_open(&config, &responses, &error);
    if (code) {
        printf("{\"code\":%d}\n", code);
        return 0;
    }
    fixture f = {.start = ah_now_ms(), .cancel_after = strtoull(argv[3], NULL, 10)};
    ah_provider provider = ah_responses_provider(responses);
    char *json = NULL;
    code = provider.complete(provider.context, "{\"model\":\"test\",\"input\":[],\"stream\":true}",
                             delta, &f, cancelled, &f, &json, &error);
    if (!strcmp(argv[4], "repeat") && code == AH_OK) {
        free(json);
        json = NULL;
        code =
            provider.complete(provider.context, "{\"model\":\"test\",\"input\":[],\"stream\":true}",
                              delta, &f, cancelled, &f, &json, &error);
    }
    printf("{\"code\":%d,\"deltas\":%zu,\"elapsed\":%llu,\"response\":%s}\n", code, f.deltas,
           (unsigned long long)(ah_now_ms() - f.start), json ? json : "null");
    free(json);
    ah_responses_close(responses);
    return 0;
}
