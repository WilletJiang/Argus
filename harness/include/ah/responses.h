#ifndef AH_RESPONSES_H
#define AH_RESPONSES_H
#include "ah.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ah_responses ah_responses;
typedef struct {
    uint64_t us;
    bool present; /* Distinguishes unavailable/unobserved from a real zero. */
} ah_responses_time;
typedef struct {
    unsigned attempt; /* One-based within complete(); retries are separate receipts. */
    ah_code result;
    long http_status;
    bool retry_planned, terminal_completed; /* A planned retry may be cancelled in backoff. */
    size_t request_bytes, response_bytes;   /* JSON body configured; body bytes received. */
    uint64_t elapsed_us; /* Monotonic attempt duration; excludes retry backoff. */
    bool elapsed_present;
    /* first_byte includes HTTP/proxy headers. first_content requires nonempty
     * output text, function arguments or custom-tool input; excludes reasoning.
     * first_output_item tracks output_item.done, including reasoning.
     * terminal marks a validated completed event or a provider failure. */
    ah_responses_time first_byte, upload_complete, first_content, first_output_item, terminal;
    /* Cumulative libcurl timings from transfer start; zero can mean a skipped
     * phase (e.g. reused connection or no TLS). These are not additive durations. */
    ah_responses_time dns, connect, tls, pretransfer, curl_first_byte, curl_total;
    long new_connections;
    bool new_connections_present;
} ah_responses_metrics;
/* Content-free, once after each started transport attempt, including failures.
 * No callback for preflight/setup rejection. Runs on the owner thread while the
 * provider is busy; must not block, reenter, close, or change its token. Copy the
 * receipt if retaining it. A completed terminal alone does not authorize tools:
 * only the final complete() return of AH_OK passes the transport barrier. */
typedef void (*ah_responses_observer)(void *context, const ah_responses_metrics *metrics);
typedef struct {
    const char *endpoint; /* Full /v1/responses URL. HTTPS; loopback HTTP test opt-in. */
    const char *bearer_token;
    uint64_t timeout_ms;
    size_t max_response_bytes;
    size_t max_event_bytes;
    unsigned max_retries; /* Only before observable output; bounded transient errors. */
    bool allow_loopback_http;
    ah_responses_observer observe;
    void *observe_context;
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
