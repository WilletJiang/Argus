#ifndef AH_RESPONSES_INTERNAL_H
#define AH_RESPONSES_INTERNAL_H
#include "ah/responses.h"
#include "../internal.h"
#include "stream.h"
#include <curl/curl.h>

struct ah_responses {
    ah_responses_config config;
    char *endpoint, *token;
    CURL *easy;
    CURLM *multi;
    bool loopback, busy;
};

typedef struct {
    uint64_t started;
    bool curl_started;
    ah_responses_metrics metrics;
} responses_measurement;

void responses_stamp(ah_responses_time *value, uint64_t started);
void responses_phase(void *context, ah_stream_phase phase);
void responses_observe(ah_responses *provider, responses_measurement *measurement);

void responses_wipe(char *value);
ah_code responses_complete(void *context, const char *request, ah_delta_sink delta,
                           void *delta_context, ah_cancel_check cancelled, void *cancel_context,
                           char **response, ah_error *error);
#endif
