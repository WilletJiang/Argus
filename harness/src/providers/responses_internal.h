#ifndef AH_RESPONSES_INTERNAL_H
#define AH_RESPONSES_INTERNAL_H
#include "ah/responses.h"
#include "../internal.h"
#include <curl/curl.h>

struct ah_responses {
    ah_responses_config config;
    char *endpoint, *token;
    CURL *easy;
    CURLM *multi;
    bool loopback, busy;
};

void responses_wipe(char *value);
ah_code responses_complete(void *context, const char *request, ah_delta_sink delta,
                           void *delta_context, ah_cancel_check cancelled, void *cancel_context,
                           char **response, ah_error *error);
#endif
