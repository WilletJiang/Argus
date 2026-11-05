#include "responses_internal.h"
#include "stream.h"

void responses_stamp(ah_responses_time *value, uint64_t started) {
    if (!value->present) {
        uint64_t now = ah_now_us();
        if (started && now >= started) {
            value->us = now - started;
            value->present = true;
        }
    }
}

void responses_phase(void *context, ah_stream_phase phase) {
    responses_measurement *measurement = context;
    ah_responses_metrics *metrics = &measurement->metrics;
    ah_responses_time *value = phase == AH_STREAM_CONTENT       ? &metrics->first_content
                               : phase == AH_STREAM_OUTPUT_ITEM ? &metrics->first_output_item
                                                                : &metrics->terminal;
    responses_stamp(value, measurement->started);
}

static ah_responses_time timing(CURL *easy, CURLINFO name) {
    curl_off_t value = 0;
    if (curl_easy_getinfo(easy, name, &value) != CURLE_OK || value < 0)
        return (ah_responses_time){0};
    return (ah_responses_time){.us = (uint64_t)value, .present = true};
}

void responses_observe(ah_responses *provider, responses_measurement *measurement) {
    if (!provider->config.observe)
        return;
    ah_responses_metrics *metrics = &measurement->metrics;
    uint64_t now = ah_now_us();
    metrics->elapsed_present = measurement->started && now >= measurement->started;
    metrics->elapsed_us = metrics->elapsed_present ? now - measurement->started : 0;
    if (measurement->curl_started) {
        metrics->dns = timing(provider->easy, CURLINFO_NAMELOOKUP_TIME_T);
        metrics->connect = timing(provider->easy, CURLINFO_CONNECT_TIME_T);
        metrics->tls = timing(provider->easy, CURLINFO_APPCONNECT_TIME_T);
        metrics->pretransfer = timing(provider->easy, CURLINFO_PRETRANSFER_TIME_T);
        metrics->curl_first_byte = timing(provider->easy, CURLINFO_STARTTRANSFER_TIME_T);
        metrics->curl_total = timing(provider->easy, CURLINFO_TOTAL_TIME_T);
        metrics->new_connections_present = curl_easy_getinfo(provider->easy, CURLINFO_NUM_CONNECTS,
                                                             &metrics->new_connections) == CURLE_OK;
    }
    provider->config.observe(provider->config.observe_context, metrics);
}
