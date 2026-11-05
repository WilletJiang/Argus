#include "responses_internal.h"
#include "stream.h"
#include <stdio.h>
#include <strings.h>
#include <time.h>

typedef struct {
    ah_stream stream;
    responses_measurement measurement;
    bool measure;
    ah_cancel_check cancelled;
    void *cancel_context;
    uint64_t deadline;
    long status;
    size_t received, header_bytes, limit;
    unsigned retry_after;
    bool content_type, content_type_seen, header_complete;
    ah_error error;
} transfer;

static ah_code check_stop(transfer *t) {
    if (t->error.code)
        return t->error.code;
    if (t->cancelled && t->cancelled(t->cancel_context))
        return ah_fail(&t->error, AH_CANCELLED, "provider request cancelled");
    if (ah_now_ms() >= t->deadline)
        return ah_fail(&t->error, AH_LIMIT, "provider deadline exceeded");
    return AH_OK;
}

static int progress(void *context, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)a;
    (void)b;
    transfer *t = context;
    if (t->measure && c > 0 && d >= c)
        responses_stamp(&t->measurement.metrics.upload_complete, t->measurement.started);
    return check_stop(context) != AH_OK;
}

static size_t header(char *bytes, size_t size, size_t count, void *context) {
    transfer *t = context;
    if (!size || count > SIZE_MAX / size)
        return 0;
    size_t length = size * count;
    if (t->measure && length)
        responses_stamp(&t->measurement.metrics.first_byte, t->measurement.started);
    if (check_stop(t))
        return 0;
    if (length > 65536 - t->header_bytes) {
        ah_fail(&t->error, AH_LIMIT, "HTTP header limit exceeded");
        return 0;
    }
    t->header_bytes += length;
    if (length >= 5 && !memcmp(bytes, "HTTP/", 5)) {
        const char *space = memchr(bytes, ' ', length);
        if (!space || bytes + length - space < 4 || space[1] < '1' || space[1] > '5' ||
            space[2] < '0' || space[2] > '9' || space[3] < '0' || space[3] > '9') {
            ah_fail(&t->error, AH_PROTOCOL, "invalid HTTP status line");
            return 0;
        }
        t->status = (space[1] - '0') * 100 + (space[2] - '0') * 10 + space[3] - '0';
        t->content_type = false;
        t->content_type_seen = false;
        t->header_complete = false;
        t->retry_after = 0;
    } else if ((length == 2 && bytes[0] == '\r' && bytes[1] == '\n') ||
               (length == 1 && bytes[0] == '\n')) {
        t->header_complete = true;
    } else if (length >= 13 && !strncasecmp(bytes, "Content-Type:", 13)) {
        if (t->content_type_seen) {
            ah_fail(&t->error, AH_PROTOCOL, "duplicate Content-Type header");
            return 0;
        }
        t->content_type_seen = true;
        size_t offset = 13;
        while (offset < length && (bytes[offset] == ' ' || bytes[offset] == '\t'))
            offset++;
        const char *expected = "text/event-stream";
        size_t n = strlen(expected);
        t->content_type =
            length >= offset + n && !strncasecmp(bytes + offset, expected, n) &&
            (length == offset + n || bytes[offset + n] == ';' || bytes[offset + n] == '\r' ||
             bytes[offset + n] == '\n' || bytes[offset + n] == ' ');
    } else if (length >= 12 && !strncasecmp(bytes, "Retry-After:", 12)) {
        size_t offset = 12;
        while (offset < length && bytes[offset] == ' ')
            offset++;
        unsigned seconds = 0;
        while (offset < length && bytes[offset] >= '0' && bytes[offset] <= '9') {
            seconds = seconds * 10 + (unsigned)(bytes[offset++] - '0');
            if (seconds > 30) {
                seconds = 30;
                break;
            }
        }
        t->retry_after = seconds * 1000;
    }
    return length;
}

static size_t body(char *bytes, size_t size, size_t count, void *context) {
    transfer *t = context;
    if (!size || count > SIZE_MAX / size)
        return 0;
    size_t length = size * count;
    if (t->measure) {
        t->measurement.metrics.response_bytes += length;
        if (length)
            responses_stamp(&t->measurement.metrics.first_byte, t->measurement.started);
    }
    if (check_stop(t))
        return 0;
    if (length > t->limit - t->received) {
        ah_fail(&t->error, AH_LIMIT, "response byte limit exceeded");
        return 0;
    }
    t->received += length;
    if (t->status < 200 || t->status >= 300)
        return length;
    if (!t->header_complete || (t->content_type_seen && !t->content_type)) {
        ah_fail(&t->error, AH_PROTOCOL, "expected text/event-stream response");
        return 0;
    }
    if (ah_stream_feed(&t->stream, bytes, length)) {
        t->error = t->stream.error;
        return 0;
    }
    return length;
}

static CURLcode perform(ah_responses *provider, transfer *t) {
    if (curl_multi_add_handle(provider->multi, provider->easy) != CURLM_OK)
        return CURLE_FAILED_INIT;
    int running = 1;
    CURLcode result = CURLE_FAILED_INIT;
    while (running && !check_stop(t)) {
        t->measurement.curl_started = true;
        if (curl_multi_perform(provider->multi, &running) != CURLM_OK)
            break;
        if (running && !check_stop(t)) {
            uint64_t now = ah_now_ms();
            uint64_t remaining = now < t->deadline ? t->deadline - now : 0;
            int wait_ms = remaining < 50 ? (int)remaining : 50;
            if (curl_multi_poll(provider->multi, NULL, 0, wait_ms, NULL) != CURLM_OK)
                break;
        }
    }
    int pending;
    CURLMsg *message;
    while ((message = curl_multi_info_read(provider->multi, &pending)))
        if (message->msg == CURLMSG_DONE && message->easy_handle == provider->easy)
            result = message->data.result;
    curl_multi_remove_handle(provider->multi, provider->easy);
    return result;
}

static bool retryable(CURLcode code) {
    return code == CURLE_COULDNT_CONNECT || code == CURLE_COULDNT_RESOLVE_HOST ||
           code == CURLE_SEND_ERROR || code == CURLE_RECV_ERROR || code == CURLE_GOT_NOTHING ||
           code == CURLE_PARTIAL_FILE || code == CURLE_HTTP2 || code == CURLE_HTTP2_STREAM;
}

/* ChatGPT cache affinity uses this header independently of the JSON key. Derive
 * it from each request, not provider state: one transport can serve many sessions. */
static bool request_route(const char *json, char route[525]) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    bool valid = yyjson_is_obj(root) && ah_json_check(root, NULL) == AH_OK;
    route[0] = 0;
    yyjson_val *key = valid ? yyjson_obj_get(root, "prompt_cache_key") : NULL;
    if (key && !yyjson_is_null(key)) {
        const char *value = yyjson_get_str(key);
        size_t length = yyjson_get_len(key);
        valid = value && length && length <= 512;
        for (size_t i = 0; valid && i < length; i++)
            valid = (unsigned char)value[i] >= 0x21 && (unsigned char)value[i] <= 0x7e;
        if (valid)
            snprintf(route, 525, "session-id: %s", value);
    }
    yyjson_doc_free(doc);
    return valid;
}

ah_code responses_complete(void *context, const char *request_json, ah_delta_sink delta,
                           void *delta_context, ah_cancel_check cancelled, void *cancel_context,
                           char **response_json, ah_error *error) {
    ah_responses *provider = context;
    ah_error_clear(error);
    if (response_json)
        *response_json = NULL;
    char route[525];
    if (!provider || !response_json || !request_json || !request_route(request_json, route))
        return ah_fail(error, AH_INVALID, "invalid provider request");
    if (provider->busy)
        return ah_fail(error, AH_BUSY, "provider is running");
    if (!provider->token)
        return ah_fail(error, AH_AUTH, "provider requires a bearer token");
    provider->busy = true;
    uint64_t now = ah_now_ms();
    uint64_t deadline = provider->config.timeout_ms > UINT64_MAX - now
                            ? UINT64_MAX
                            : now + provider->config.timeout_ms;
    size_t auth_size = strlen(provider->token) + 23;
    char *auth = malloc(auth_size);
    struct curl_slist *headers = NULL;
    ah_code result = AH_IO;
    if (!auth)
        goto cleanup;
    snprintf(auth, auth_size, "Authorization: Bearer %s", provider->token);
    headers = curl_slist_append(NULL, auth);
    if (!headers)
        goto cleanup;
    struct curl_slist *next = curl_slist_append(headers, "Content-Type: application/json");
    if (!next)
        goto cleanup;
    headers = next;
    next = curl_slist_append(headers, "Accept: text/event-stream");
    if (!next)
        goto cleanup;
    headers = next;
    if (*route) {
        next = curl_slist_append(headers, route);
        if (!next)
            goto cleanup;
        headers = next;
    }
    curl_easy_reset(provider->easy); /* Keeps reusable connections. */
#define SET(option, value)                                                                         \
    do {                                                                                           \
        if (curl_easy_setopt(provider->easy, option, value) != CURLE_OK)                           \
            goto cleanup;                                                                          \
    } while (0)
    SET(CURLOPT_URL, provider->endpoint);
    SET(CURLOPT_HTTPHEADER, headers);
    SET(CURLOPT_POSTFIELDS, request_json);
    SET(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(request_json));
    SET(CURLOPT_POST, 1L);
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_NOPROGRESS, 0L);
    SET(CURLOPT_XFERINFOFUNCTION, progress);
    SET(CURLOPT_WRITEFUNCTION, body);
    SET(CURLOPT_HEADERFUNCTION, header);
    if (provider->loopback)
        SET(CURLOPT_PROXY, "");
#if LIBCURL_VERSION_NUM >= 0x075500
    SET(CURLOPT_PROTOCOLS_STR, provider->config.allow_loopback_http ? "https,http" : "https");
#else
    SET(CURLOPT_PROTOCOLS,
        (long)(CURLPROTO_HTTPS | (provider->config.allow_loopback_http ? CURLPROTO_HTTP : 0)));
#endif
    for (unsigned attempt = 0; attempt <= provider->config.max_retries; attempt++) {
        transfer t = {.cancelled = cancelled,
                      .cancel_context = cancel_context,
                      .deadline = deadline,
                      .limit = provider->config.max_response_bytes,
                      .measure = provider->config.observe != NULL};
        ah_stream_init(&t.stream, provider->config.max_event_bytes, t.limit, delta, delta_context);
        if (t.measure) {
            t.measurement.metrics.attempt = attempt + 1;
            t.measurement.metrics.request_bytes = strlen(request_json);
            t.stream.phase = responses_phase;
            t.stream.phase_context = &t.measurement;
        }
        SET(CURLOPT_WRITEDATA, &t);
        SET(CURLOPT_HEADERDATA, &t);
        SET(CURLOPT_XFERINFODATA, &t);
        now = ah_now_ms();
        uint64_t remaining = now < deadline ? deadline - now : 1;
        SET(CURLOPT_TIMEOUT_MS, (long)remaining);
        if (t.measure)
            t.measurement.started = ah_now_us();
        CURLcode curl_result = perform(provider, &t);
        bool local_failure = t.error.code != AH_OK;
        if (t.error.code)
            result = t.error.code;
        else if (t.status == 401 || t.status == 403)
            result = ah_fail(&t.error, AH_AUTH, "provider authentication failed");
        else if (t.status == 429)
            result = ah_fail(&t.error, AH_LIMIT, "provider rate or usage limit");
        else if (t.status >= 500)
            result = ah_fail(&t.error, AH_NETWORK, "provider HTTP %ld", t.status);
        else if (curl_result == CURLE_OPERATION_TIMEDOUT)
            result = ah_fail(&t.error, AH_LIMIT, "provider deadline exceeded");
        else if (curl_result != CURLE_OK)
            result =
                ah_fail(&t.error, AH_NETWORK, "provider transport failure (%d)", (int)curl_result);
        else if (t.status < 200 || t.status >= 300)
            result = ah_fail(&t.error, AH_PROTOCOL, "provider HTTP %ld", t.status);
        else if (t.content_type_seen && !t.content_type)
            result = ah_fail(&t.error, AH_PROTOCOL, "expected text/event-stream response");
        else {
            result = ah_stream_finish(&t.stream, response_json);
            t.error = t.stream.error;
        }
        bool retry = result && !local_failure && result != AH_CANCELLED && result != AH_PROTOCOL &&
                     result != AH_AUTH && !t.stream.observed_output && !t.stream.terminal &&
                     attempt < provider->config.max_retries &&
                     (t.status == 429 || t.status >= 500 || retryable(curl_result)) &&
                     ah_now_ms() < deadline;
        if (error)
            *error = t.error;
        if (t.measure) {
            t.measurement.metrics.result = result;
            t.measurement.metrics.http_status = t.status;
            t.measurement.metrics.retry_planned = retry;
            t.measurement.metrics.terminal_completed = t.stream.terminal;
            responses_observe(provider, &t.measurement);
        }
        ah_stream_destroy(&t.stream);
        if (!retry)
            break;
        unsigned delay = t.retry_after ? t.retry_after : 200U << attempt;
        uint64_t until = ah_now_ms() + delay;
        t.error = (ah_error){0};
        while (ah_now_ms() < until && !check_stop(&t)) {
            struct timespec pause = {.tv_nsec = 50000000};
            nanosleep(&pause, NULL);
        }
        if (t.error.code) {
            result = t.error.code;
            if (error)
                *error = t.error;
            break;
        }
    }
#undef SET
cleanup:
    curl_easy_setopt(provider->easy, CURLOPT_HTTPHEADER, NULL);
    for (struct curl_slist *item = headers; item; item = item->next)
        responses_wipe(item->data);
    curl_slist_free_all(headers);
    responses_wipe(auth);
    free(auth);
    provider->busy = false;
    if (result == AH_IO && (!error || !error->code))
        ah_fail(error, AH_IO, "provider allocation or setup failed");
    return result;
}
