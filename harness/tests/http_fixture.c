#include "ah/responses.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t start, cancel_after;
    size_t deltas;
    ah_responses *provider;
    ah_responses_metrics metrics[8];
    size_t metric_count;
    bool observer_guarded, cancel_immediately;
} fixture;
static bool cancelled(void *context) {
    fixture *f = context;
    return f->cancel_immediately || (f->cancel_after && ah_now_ms() - f->start >= f->cancel_after);
}
static void delta(void *context, const char *text, size_t length) {
    (void)text;
    ((fixture *)context)->deltas += length;
}
static void observe(void *context, const ah_responses_metrics *metrics) {
    fixture *f = context;
    if (f->metric_count >= sizeof(f->metrics) / sizeof(f->metrics[0]))
        abort();
    f->metrics[f->metric_count++] = *metrics;
    ah_error error;
    ah_provider provider = ah_responses_provider(f->provider);
    char *response = NULL;
    bool busy = ah_responses_set_token(f->provider, "must-not-replace", &error) == AH_BUSY &&
                provider.complete(provider.context, "{}", NULL, NULL, NULL, NULL, &response,
                                  &error) == AH_BUSY &&
                !response;
    f->observer_guarded = f->observer_guarded && busy;
}
static void timing(const char *name, ah_responses_time value) {
    printf(",\"%s\":{\"us\":%llu,\"present\":%s}", name, (unsigned long long)value.us,
           value.present ? "true" : "false");
}
static void metrics(const fixture *f) {
    printf(",\"observer_guarded\":%s,\"metrics\":[", f->observer_guarded ? "true" : "false");
    for (size_t i = 0; i < f->metric_count; i++) {
        const ah_responses_metrics *m = &f->metrics[i];
        printf("%s{\"attempt\":%u,\"result\":%d,\"http_status\":%ld,\"retry_planned\":%s,"
               "\"terminal_completed\":%s,\"request_bytes\":%zu,\"response_bytes\":%zu,"
               "\"elapsed_us\":%llu,\"elapsed_present\":%s,\"new_connections\":%ld,\"new_"
               "connections_present\":%s",
               i ? "," : "", m->attempt, m->result, m->http_status,
               m->retry_planned ? "true" : "false", m->terminal_completed ? "true" : "false",
               m->request_bytes, m->response_bytes, (unsigned long long)m->elapsed_us,
               m->elapsed_present ? "true" : "false", m->new_connections,
               m->new_connections_present ? "true" : "false");
#define TIME(field) timing(#field, m->field)
        TIME(first_byte);
        TIME(upload_complete);
        TIME(first_content);
        TIME(first_output_item);
        TIME(terminal);
        TIME(dns);
        TIME(connect);
        TIME(tls);
        TIME(pretransfer);
        TIME(curl_first_byte);
        TIME(curl_total);
#undef TIME
        putchar('}');
    }
    putchar(']');
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
    fixture f = {.observer_guarded = true};
    if (strcmp(argv[4], "no-observer")) {
        config.observe = observe;
        config.observe_context = &f;
    }
    ah_error error;
    ah_responses *responses = NULL;
    ah_code code = ah_responses_open(&config, &responses, &error);
    if (code) {
        printf("{\"code\":%d}\n", code);
        return 0;
    }
    f.provider = responses;
    f.start = ah_now_ms();
    f.cancel_after = strtoull(argv[3], NULL, 10);
    ah_provider provider = ah_responses_provider(responses);
    char *json = NULL;
    code = provider.complete(provider.context, "{\"model\":\"test\",\"input\":[],\"stream\":true}",
                             delta, &f, cancelled, &f, &json, &error);
    if ((!strcmp(argv[4], "repeat") || !strcmp(argv[4], "cancel-repeat")) && code == AH_OK) {
        f.cancel_immediately = !strcmp(argv[4], "cancel-repeat");
        free(json);
        json = NULL;
        code =
            provider.complete(provider.context, "{\"model\":\"test\",\"input\":[],\"stream\":true}",
                              delta, &f, cancelled, &f, &json, &error);
    }
    printf("{\"code\":%d,\"deltas\":%zu,\"elapsed\":%llu,\"response\":%s", code, f.deltas,
           (unsigned long long)(ah_now_ms() - f.start), json ? json : "null");
    metrics(&f);
    puts("}");
    free(json);
    ah_responses_close(responses);
    return 0;
}
