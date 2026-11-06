#include "responses_internal.h"
#include <limits.h>
#include <pthread.h>
#include <strings.h>

static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
static CURLcode curl_init_code;
static void init_curl(void) {
    curl_init_code = curl_global_init(CURL_GLOBAL_DEFAULT);
}
void responses_wipe(char *value) {
    if (!value)
        return;
    volatile unsigned char *p = (volatile unsigned char *)value;
    size_t length = strlen(value);
    while (length--)
        *p++ = 0;
}

void ah_responses_config_init(ah_responses_config *config) {
    if (!config)
        return;
    *config = (ah_responses_config){.endpoint = "https://api.openai.com/v1/responses",
                                    .timeout_ms = 120000,
                                    .max_response_bytes = 32 * 1024 * 1024,
                                    .max_event_bytes = 8 * 1024 * 1024,
                                    .max_retries = 2};
}

static bool url_part_present(CURLU *url, CURLUPart part) {
    char *value = NULL;
    bool present = curl_url_get(url, part, &value, 0) == CURLUE_OK;
    curl_free(value);
    return present;
}

static bool literal_loopback(const char *endpoint) {
    const char *authority = strstr(endpoint, "://");
    if (!authority)
        return false;
    authority += 3;
    const char *allowed[] = {"127.0.0.1", "[::1]", "localhost"};
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        size_t length = strlen(allowed[i]);
        if (!strncasecmp(authority, allowed[i], length) &&
            (!authority[length] || authority[length] == ':' || authority[length] == '/'))
            return true;
    }
    return false;
}

static bool valid_endpoint(const ah_responses_config *config, bool *loopback) {
    if (!config->endpoint || strlen(config->endpoint) > 8192 || strchr(config->endpoint, '?') ||
        strchr(config->endpoint, '#'))
        return false;
    for (const unsigned char *p = (const unsigned char *)config->endpoint; *p; p++)
        if (*p <= 0x20 || *p == 0x7f)
            return false;
    CURLU *url = curl_url();
    char *scheme = NULL, *host = NULL;
    bool valid = false;
    if (!url || curl_url_set(url, CURLUPART_URL, config->endpoint, 0))
        goto done;
    if (curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) ||
        curl_url_get(url, CURLUPART_HOST, &host, 0))
        goto done;
    if (url_part_present(url, CURLUPART_USER) || url_part_present(url, CURLUPART_PASSWORD) ||
        url_part_present(url, CURLUPART_OPTIONS) || url_part_present(url, CURLUPART_QUERY) ||
        url_part_present(url, CURLUPART_FRAGMENT))
        goto done;
    *loopback =
        !strcmp(host, "127.0.0.1") || !strcasecmp(host, "localhost") || !strcmp(host, "[::1]");
    valid = !strcasecmp(scheme, "https") ||
            (!strcasecmp(scheme, "http") && config->allow_loopback_http && *loopback &&
             literal_loopback(config->endpoint));
    /* Older curl versions resolve localhost through externally configured DNS. */
    if (!strcasecmp(scheme, "http") && !strcasecmp(host, "localhost") &&
        curl_version_info(CURLVERSION_NOW)->version_num < 0x074e00)
        valid = false;
done:
    curl_free(scheme);
    curl_free(host);
    if (url)
        curl_url_cleanup(url);
    return valid;
}

ah_code ah_responses_set_token(ah_responses *provider, const char *token, ah_error *error) {
    ah_error_clear(error);
    if (!provider || !token || !*token || strlen(token) > 16384)
        return ah_fail(error, AH_INVALID, "invalid bearer token");
    if (provider->busy)
        return ah_fail(error, AH_BUSY, "provider is running");
    for (const unsigned char *p = (const unsigned char *)token; *p; p++)
        if (*p < 0x21 || *p > 0x7e)
            return ah_fail(error, AH_INVALID, "invalid bearer token");
    char *copy = ah_strdup(token);
    if (!copy)
        return ah_fail(error, AH_IO, "token allocation failed");
    responses_wipe(provider->token);
    free(provider->token);
    provider->token = copy;
    return AH_OK;
}

ah_code ah_responses_open(const ah_responses_config *config, ah_responses **out, ah_error *error) {
    ah_error_clear(error);
    if (out)
        *out = NULL;
    if (!config || !out || !config->timeout_ms || config->timeout_ms > LONG_MAX ||
        !config->max_event_bytes || config->max_event_bytes > config->max_response_bytes ||
        config->max_response_bytes > 256 * 1024 * 1024 || config->max_retries > 5)
        return ah_fail(error, AH_INVALID, "invalid Responses configuration");
    pthread_once(&curl_once, init_curl);
    if (curl_init_code != CURLE_OK)
        return ah_fail(error, AH_IO, "curl initialization failed");
    const curl_version_info_data *version = curl_version_info(CURLVERSION_NOW);
    if (!(version->features & CURL_VERSION_ASYNCHDNS))
        return ah_fail(error, AH_INVALID,
                       "curl must support asynchronous DNS for bounded cancellation");
    bool loopback = false;
    if (!valid_endpoint(config, &loopback))
        return ah_fail(error, AH_INVALID,
                       "endpoint must be HTTPS without credentials, query, or fragment");
    ah_responses *provider = calloc(1, sizeof(*provider));
    if (!provider)
        return ah_fail(error, AH_IO, "provider allocation failed");
    provider->config = *config;
    provider->config.bearer_token = NULL;
    provider->endpoint = ah_strdup(config->endpoint);
    provider->config.endpoint = provider->endpoint;
    provider->loopback = loopback;
    provider->easy = curl_easy_init();
    provider->multi = curl_multi_init();
    if (!provider->endpoint || !provider->easy || !provider->multi) {
        ah_responses_close(provider);
        return ah_fail(error, AH_IO, "provider initialization failed");
    }
    if (config->bearer_token) {
        ah_code code = ah_responses_set_token(provider, config->bearer_token, error);
        if (code) {
            ah_responses_close(provider);
            return code;
        }
    }
    *out = provider;
    return AH_OK;
}

void ah_responses_close(ah_responses *provider) {
    if (!provider)
        return;
    curl_easy_cleanup(provider->easy);
    curl_multi_cleanup(provider->multi);
    responses_wipe(provider->token);
    free(provider->token);
    free(provider->endpoint);
    free(provider);
}

ah_provider ah_responses_provider(ah_responses *provider) {
    return (ah_provider){.complete = responses_complete, .context = provider};
}
