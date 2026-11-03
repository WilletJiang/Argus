#ifndef AH_RESPONSE_STREAM_H
#define AH_RESPONSE_STREAM_H
#include "ah/ah.h"

/* Private, transport-independent incremental Responses SSE decoder. */
typedef struct {
    char *line, *data, *response;
    size_t line_len, line_cap, data_len, data_cap, frame_bytes, total_bytes;
    size_t event_limit, response_limit;
    unsigned utf8_left, utf8_min, utf8_max;
    bool after_cr, terminal, observed_output;
    ah_delta_sink delta;
    void *delta_context;
    ah_error error;
} ah_stream;

void ah_stream_init(ah_stream *stream, size_t event_limit, size_t response_limit,
                    ah_delta_sink delta, void *context);
ah_code ah_stream_feed(ah_stream *stream, const char *bytes, size_t length);
ah_code ah_stream_finish(ah_stream *stream, char **response);
void ah_stream_destroy(ah_stream *stream);
#endif
