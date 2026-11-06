#include "../src/providers/stream.h"
#include "internal.h"
#include <assert.h>
#include <stdio.h>

static const char call[] = "{\"id\":\"fc\",\"type\":\"function_call\",\"status\":\"completed\","
                           "\"namespace\":\"local\",\"call_id\":\"call\",\"name\":\"read_file\","
                           "\"arguments\":\"{\\\"path\\\":\\\"SPEC.md\\\"}\"}";
static const char reasoning[] =
    "{\"id\":\"rs\",\"type\":\"reasoning\",\"summary\":[],\"encrypted_content\":\"complete\"}";
static const char added[] = "{\"id\":\"fc\",\"type\":\"function_call\",\"status\":\"in_progress\"}";
static const char terminal[] =
    "data: "
    "{\"type\":\"response.completed\",\"response\":{\"id\":\"r\",\"status\":\"completed\","
    "\"output\":[],\"usage\":{\"input_tokens\":7,\"output_tokens\":19}}}\n\n";

static ah_code event(ah_stream *s, const char *type, unsigned index, const char *item,
                     size_t chunk) {
    char wire[2048];
    int length =
        snprintf(wire, sizeof(wire), "data: {\"type\":\"%s\",\"output_index\":%u,\"item\":%s}\n\n",
                 type, index, item);
    assert(length > 0 && (size_t)length < sizeof(wire));
    for (size_t offset = 0; offset < (size_t)length;) {
        size_t count = (size_t)length - offset;
        if (count > chunk)
            count = chunk;
        ah_code code = ah_stream_feed(s, wire + offset, count);
        if (code)
            return code;
        offset += count;
    }
    return AH_OK;
}
static void rejected(ah_stream *s, ah_code expected) {
    char *response = NULL;
    assert(ah_stream_finish(s, &response) == expected);
    assert(!response);
    ah_stream_destroy(s);
}
static void assembled(void) {
    for (size_t chunk = 1; chunk < 256; chunk++) {
        ah_stream s;
        ah_stream_init(&s, 4096, 16384, NULL, NULL);
        assert(event(&s, "response.output_item.added", 1, added, chunk) == AH_OK);
        assert(event(&s, "response.output_item.done", 1, call, chunk) == AH_OK);
        assert(event(&s, "response.output_item.done", 0, reasoning, chunk) == AH_OK);
        assert(!s.terminal && !s.response && s.observed_output);
        assert(ah_stream_feed(&s, terminal, strlen(terminal)) == AH_OK);
        char *response = NULL;
        assert(ah_stream_finish(&s, &response) == AH_OK);
        yyjson_doc *doc = yyjson_read(response, strlen(response), 0);
        yyjson_val *root = yyjson_doc_get_root(doc), *out = yyjson_obj_get(root, "output");
        assert(yyjson_arr_size(out) == 2);
        assert(yyjson_equals_str(yyjson_obj_get(yyjson_arr_get(out, 0), "encrypted_content"),
                                 "complete"));
        assert(yyjson_equals_str(yyjson_obj_get(yyjson_arr_get(out, 1), "arguments"),
                                 "{\"path\":\"SPEC.md\"}"));
        assert(yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(root, "usage"), "output_tokens")) ==
               19);
        yyjson_doc_free(doc);
        free(response);
        ah_stream_destroy(&s);
    }
}
static void barriers(void) {
    ah_stream s;
    ah_stream_init(&s, 4096, 16384, NULL, NULL);
    assert(event(&s, "response.output_item.done", 0, call, 1) == AH_OK);
    rejected(&s, AH_PROTOCOL); /* Complete item is not a successful response. */
    ah_stream_init(&s, 4096, 16384, NULL, NULL);
    assert(event(&s, "response.output_item.done", 0, call, 1) == AH_OK);
    const char failed[] = "data: "
                          "{\"type\":\"response.failed\",\"response\":{\"error\":{\"code\":\"rate_"
                          "limit_exceeded\"}}}\n\n";
    assert(ah_stream_feed(&s, failed, strlen(failed)) == AH_LIMIT);
    rejected(&s, AH_LIMIT);
    ah_stream_init(&s, 4096, 16384, NULL, NULL);
    assert(event(&s, "response.output_item.added", 0, added, 1) == AH_OK);
    assert(ah_stream_feed(&s, terminal, strlen(terminal)) == AH_PROTOCOL);
    rejected(&s, AH_PROTOCOL);
}
static void malformed(void) {
    ah_stream s;
    ah_stream_init(&s, 4096, 16384, NULL, NULL);
    assert(event(&s, "response.output_item.done", 1, call, 1) == AH_OK);
    assert(ah_stream_feed(&s, terminal, strlen(terminal)) == AH_PROTOCOL); /* index gap */
    rejected(&s, AH_PROTOCOL);
    ah_stream_init(&s, 4096, 16384, NULL, NULL);
    assert(event(&s, "response.output_item.done", 4096, call, 1) == AH_PROTOCOL);
    rejected(&s, AH_PROTOCOL);
    ah_stream_init(&s, 4096, 16384, NULL, NULL);
    assert(event(&s, "response.output_item.done", 0, call, 1) == AH_OK);
    assert(event(&s, "response.output_item.done", 0, call, 1) == AH_PROTOCOL);
    rejected(&s, AH_PROTOCOL);
    ah_stream_init(&s, 4096, 16384, NULL, NULL);
    assert(event(&s, "response.output_item.added", 0, added, 1) == AH_OK);
    assert(event(&s, "response.output_item.done", 0, reasoning, 1) == AH_PROTOCOL);
    rejected(&s, AH_PROTOCOL);
    ah_stream_init(&s, 4096, 16384, NULL, NULL);
    assert(event(&s, "response.output_item.done", 0, call, 1) == AH_OK);
    assert(event(&s, "response.output_item.done", 1, call, 1) == AH_PROTOCOL);
    rejected(&s, AH_PROTOCOL);
}
static void type_conflict(void) {
    ah_stream s;
    ah_stream_init(&s, 4096, 16384, NULL, NULL);
    assert(event(&s, "response.output_item.added", 0, added, 1) == AH_OK);
    assert(event(&s, "response.output_item.done", 0,
                 "{\"id\":\"fc\",\"type\":\"reasoning\",\"summary\":[]}", 1) == AH_PROTOCOL);
    rejected(&s, AH_PROTOCOL);
}
static void snapshots(void) {
    for (unsigned mode = 0; mode < 4; mode++) {
        ah_stream s;
        ah_stream_init(&s, 4096, 16384, NULL, NULL);
        assert(event(&s, mode == 3 ? "response.output_item.added" : "response.output_item.done", 0,
                     mode == 3   ? added
                     : mode == 2 ? reasoning
                                 : call,
                     1) == AH_OK);
        const char *item =
            mode == 1   ? "{\"id\":\"fc\",\"type\":\"function_call\",\"arguments\":\"different\"}"
            : mode == 2 ? "{\"id\":\"rs\",\"type\":\"reasoning\",\"encrypted_content\":null}"
                        : call;
        char wire[2048];
        snprintf(wire, sizeof(wire),
                 "data: "
                 "{\"type\":\"response.completed\",\"response\":{\"status\":\"completed\","
                 "\"output\":[%s]}}\n\n",
                 item);
        ah_code expected = mode == 1 ? AH_PROTOCOL : AH_OK;
        assert(ah_stream_feed(&s, wire, strlen(wire)) == expected);
        char *response = NULL;
        assert(ah_stream_finish(&s, &response) == expected);
        if (mode == 2)
            assert(strstr(response, "\"encrypted_content\":\"complete\""));
        free(response);
        ah_stream_destroy(&s);
    }
}
int main(void) {
    assembled();
    barriers();
    malformed();
    snapshots();
    type_conflict();
    puts("streamed output items: assembly, terminal barrier, identity, ordering and bounds passed");
    return 0;
}
