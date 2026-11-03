#include "stream.h"
#include "../internal.h"

void ah_stream_init(ah_stream *s, size_t event_limit, size_t response_limit, ah_delta_sink delta,
                    void *context) {
    memset(s, 0, sizeof(*s));
    s->event_limit = event_limit;
    s->response_limit = response_limit;
    s->delta = delta;
    s->delta_context = context;
}

static ah_code fail(ah_stream *s, ah_code code, const char *message) {
    return ah_fail(&s->error, code, "%s", message);
}

static bool append(char **buffer, size_t *length, size_t *capacity, const char *bytes, size_t count,
                   size_t limit) {
    if (*length > limit || count > limit - *length)
        return false;
    size_t needed = *length + count + 1;
    if (needed > *capacity) {
        size_t next = *capacity ? *capacity : 256;
        while (next < needed) {
            if (next > (limit + 1) / 2) {
                next = limit + 1;
                break;
            }
            next *= 2;
        }
        char *resized = realloc(*buffer, next);
        if (!resized)
            return false;
        *buffer = resized;
        *capacity = next;
    }
    memcpy(*buffer + *length, bytes, count);
    *length += count;
    (*buffer)[*length] = '\0';
    return true;
}

/* Validates the whole wire stream, including comments and unknown fields. */
static bool utf8_byte(ah_stream *s, unsigned char byte) {
    if (s->utf8_left) {
        if (byte < s->utf8_min || byte > s->utf8_max)
            return false;
        s->utf8_left--;
        s->utf8_min = 0x80;
        s->utf8_max = 0xbf;
        return true;
    }
    if (byte < 0x80)
        return byte != 0;
    s->utf8_min = 0x80;
    s->utf8_max = 0xbf;
    if (byte >= 0xc2 && byte <= 0xdf)
        s->utf8_left = 1;
    else if (byte >= 0xe0 && byte <= 0xef) {
        s->utf8_left = 2;
        if (byte == 0xe0)
            s->utf8_min = 0xa0;
        if (byte == 0xed)
            s->utf8_max = 0x9f;
    } else if (byte >= 0xf0 && byte <= 0xf4) {
        s->utf8_left = 3;
        if (byte == 0xf0)
            s->utf8_min = 0x90;
        if (byte == 0xf4)
            s->utf8_max = 0x8f;
    } else
        return false;
    return true;
}

static bool equal(yyjson_val *value, const char *text) {
    return yyjson_is_str(value) && yyjson_equals_str(value, text);
}

static ah_code event(ah_stream *s) {
    if (!s->data_len)
        return AH_OK;
    if (s->terminal)
        return fail(s, AH_PROTOCOL, "event after terminal response");
    yyjson_doc *doc = yyjson_read(s->data, s->data_len, 0);
    if (!doc)
        return fail(s, AH_PROTOCOL, "invalid JSON in SSE event");
    yyjson_val *root = yyjson_doc_get_root(doc);
    ah_code checked = ah_json_check(root, &s->error);
    if (checked) {
        yyjson_doc_free(doc);
        return checked;
    }
    yyjson_val *type = yyjson_obj_get(root, "type");
    ah_code code = AH_OK;
    if (!yyjson_is_obj(root) || !yyjson_is_str(type)) {
        code = fail(s, AH_PROTOCOL, "SSE event requires a type");
    } else if (equal(type, "response.completed")) {
        yyjson_val *response = yyjson_obj_get(root, "response");
        if (!yyjson_is_obj(response) || !equal(yyjson_obj_get(response, "status"), "completed") ||
            !yyjson_is_arr(yyjson_obj_get(response, "output"))) {
            code = fail(s, AH_PROTOCOL, "invalid completed response");
        } else {
            s->response = yyjson_val_write(response, 0, NULL);
            if (!s->response)
                code = fail(s, AH_IO, "response allocation failed");
            else
                s->terminal = true;
        }
    } else if (equal(type, "response.failed") || equal(type, "response.incomplete") ||
               equal(type, "error")) {
        yyjson_val *response = yyjson_obj_get(root, "response");
        yyjson_val *error = yyjson_obj_get(response, "error");
        yyjson_val *reason =
            yyjson_obj_get(yyjson_obj_get(response, "incomplete_details"), "reason");
        yyjson_val *error_code = yyjson_obj_get(error, "code");
        if (!error_code)
            error_code = yyjson_obj_get(root, "code");
        ah_code status = AH_PROTOCOL;
        if (equal(error_code, "rate_limit_exceeded") || equal(error_code, "insufficient_quota") ||
            equal(error_code, "subscription_sharing_usage_limit_exceeded") ||
            equal(error_code, "subscription_sharing_usage_unavailable") ||
            equal(reason, "max_output_tokens"))
            status = AH_LIMIT;
        else if (equal(error_code, "invalid_api_key") || equal(error_code, "authentication_error"))
            status = AH_AUTH;
        code = fail(s, status, "provider returned a failed or incomplete response");
    } else if (equal(type, "response.output_text.delta")) {
        yyjson_val *delta = yyjson_obj_get(root, "delta");
        if (!yyjson_is_str(delta))
            code = fail(s, AH_PROTOCOL, "text delta is not a string");
        else {
            s->observed_output = true;
            if (s->delta)
                s->delta(s->delta_context, yyjson_get_str(delta), yyjson_get_len(delta));
        }
    } else if (!equal(type, "response.created") && !equal(type, "response.in_progress") &&
               !equal(type, "response.queued")) {
        /* Unknown semantic events are ignored, but make retries unsafe. */
        s->observed_output = true;
    }
    yyjson_doc_free(doc);
    return code;
}

static ah_code line(ah_stream *s) {
    if (!s->line_len) {
        ah_code code = event(s);
        s->data_len = 0;
        s->frame_bytes = 0;
        return code;
    }
    if (s->line[0] == ':')
        return AH_OK;
    size_t colon = 0;
    while (colon < s->line_len && s->line[colon] != ':')
        colon++;
    if (colon != 4 || memcmp(s->line, "data", 4))
        return AH_OK;
    size_t start = colon < s->line_len ? colon + 1 : colon;
    if (start < s->line_len && s->line[start] == ' ')
        start++;
    if (s->data_len && !append(&s->data, &s->data_len, &s->data_cap, "\n", 1, s->event_limit))
        return fail(s, AH_IO, "SSE data allocation failed");
    if (!append(&s->data, &s->data_len, &s->data_cap, s->line + start, s->line_len - start,
                s->event_limit))
        return fail(s, AH_IO, "SSE data allocation failed");
    return AH_OK;
}

ah_code ah_stream_feed(ah_stream *s, const char *bytes, size_t length) {
    if (s->error.code)
        return s->error.code;
    if (length > s->response_limit - s->total_bytes)
        return fail(s, AH_LIMIT, "response byte limit exceeded");
    size_t start = s->total_bytes;
    s->total_bytes += length;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)bytes[i];
        if (!utf8_byte(s, byte))
            return fail(s, AH_PROTOCOL, "invalid UTF-8 in SSE stream");
        if (s->after_cr && byte == '\n') {
            s->after_cr = false;
            continue;
        }
        s->after_cr = false;
        if (s->frame_bytes >= s->event_limit)
            return fail(s, AH_LIMIT, "SSE event byte limit exceeded");
        s->frame_bytes++;
        if (byte == '\r' || byte == '\n') {
            ah_code code = line(s);
            s->line_len = 0;
            s->after_cr = byte == '\r';
            if (code)
                return code;
        } else if (!append(&s->line, &s->line_len, &s->line_cap, bytes + i, 1, s->event_limit)) {
            return fail(s, AH_IO, "SSE line allocation failed");
        } else if (start + i == 2 && s->line_len == 3 && !memcmp(s->line, "\xef\xbb\xbf", 3)) {
            s->line_len = 0; /* SSE permits one initial UTF-8 byte-order mark. */
        }
    }
    return AH_OK;
}

ah_code ah_stream_finish(ah_stream *s, char **response) {
    *response = NULL;
    if (s->error.code)
        return s->error.code;
    if (s->utf8_left || s->line_len || s->data_len || !s->terminal)
        return fail(s, AH_PROTOCOL, "stream ended before a complete terminal event");
    *response = s->response;
    s->response = NULL;
    return AH_OK;
}

void ah_stream_destroy(ah_stream *s) {
    free(s->line);
    free(s->data);
    free(s->response);
    memset(s, 0, sizeof(*s));
}
