#ifndef AH_RESPONSES_H
#define AH_RESPONSES_H
#include "ah.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ah_responses ah_responses;
typedef struct {
    const char *endpoint; /* Full /v1/responses URL. HTTPS; loopback HTTP test opt-in. */
    const char *bearer_token;
    uint64_t timeout_ms;
    size_t max_response_bytes;
    size_t max_event_bytes;
    unsigned max_retries; /* Only before observable output; bounded transient errors. */
    bool allow_loopback_http;
} ah_responses_config;
void ah_responses_config_init(ah_responses_config *config);
ah_code ah_responses_open(const ah_responses_config *config, ah_responses **out, ah_error *error);
void ah_responses_close(ah_responses *provider);
/* Each request's prompt_cache_key also supplies the ChatGPT session-id header.
 * Keys must be 1..512 printable non-space ASCII bytes; omit/null sends neither
 * a derived routing header nor any cached route from a previous request. */
ah_provider ah_responses_provider(ah_responses *provider);
/* Owner-thread only, copies token; host handles OAuth and refresh in its vault. */
ah_code ah_responses_set_token(ah_responses *provider, const char *token, ah_error *error);
#ifdef __cplusplus
}
#endif
#endif
