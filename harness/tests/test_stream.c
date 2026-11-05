#include "../src/providers/stream.h"
#include "ah/responses.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char terminal[] = "data: "
                               "{\"type\":\"response.completed\",\"response\":{\"id\":\"r\","
                               "\"status\":\"completed\",\"output\":[]}}\n\n";
static const char text_event[] =
    "data: {\"type\":\"response.output_text.delta\",\"delta\":\"hello 🌍\"}\n\n";
typedef struct {
    char value[1024];
    size_t length;
} text_buffer;
static void collect(void *context, const char *text, size_t length) {
    text_buffer *buffer = context;
    assert(length < sizeof(buffer->value) - buffer->length);
    memcpy(buffer->value + buffer->length, text, length);
    buffer->length += length;
    buffer->value[buffer->length] = 0;
}

static void fragments(void) {
    const char stream[] = "\xef\xbb\xbf: comment\r\nevent: ignored-name\r\n"
                          "data: {\"type\":\"response.output_text.delta\",\r\n"
                          "data: \"delta\":\"hello 🌍\"}\r\n\r\n"
                          "data: {\"type\":\"future.event\",\"payload\":{}}\r\r"
                          "data: "
                          "{\"type\":\"response.completed\",\"response\":{\"id\":\"r\",\"status\":"
                          "\"completed\",\"output\":[]}}\r\n\r\n";
    for (size_t chunk = 1; chunk <= sizeof(stream); chunk++) {
        text_buffer text = {0};
        ah_stream s;
        ah_stream_init(&s, 4096, 16384, collect, &text);
        size_t offset = 0;
        while (offset < sizeof(stream) - 1) {
            size_t count = sizeof(stream) - 1 - offset;
            if (count > chunk)
                count = chunk;
            assert(ah_stream_feed(&s, stream + offset, count) == AH_OK);
            offset += count;
        }
        char *response = NULL;
        assert(ah_stream_finish(&s, &response) == AH_OK);
        assert(response && strstr(response, "\"output\":[]"));
        assert(!strcmp(text.value, "hello 🌍"));
        free(response);
        ah_stream_destroy(&s);
    }
}

static void expect(const char *bytes, size_t length, ah_code wanted) {
    ah_stream s;
    ah_stream_init(&s, 4096, 16384, NULL, NULL);
    ah_code code = ah_stream_feed(&s, bytes, length);
    char *response = NULL;
    if (!code)
        code = ah_stream_finish(&s, &response);
    assert(code == wanted);
    if (code)
        assert(!response);
    free(response);
    ah_stream_destroy(&s);
}

static void failures(void) {
    expect(terminal, strlen(terminal) - 1, AH_PROTOCOL);
    expect("", 0, AH_PROTOCOL);
    expect(text_event, strlen(text_event), AH_PROTOCOL);
    const char malformed[] = "data: {broken}\n\n";
    expect(malformed, strlen(malformed), AH_PROTOCOL);
    const char invalid_utf8[] = "data: {\"type\":\"x\",\"value\":\"\xc0\xaf\"}\n\n";
    expect(invalid_utf8, sizeof(invalid_utf8) - 1, AH_PROTOCOL);
    const char incomplete[] = "data: "
                              "{\"type\":\"response.incomplete\",\"response\":{\"incomplete_"
                              "details\":{\"reason\":\"max_output_tokens\"}}}\n\n";
    expect(incomplete, strlen(incomplete), AH_LIMIT);
    const char subscription_limit[] =
        "data: "
        "{\"type\":\"response.failed\",\"response\":{\"error\":{\"code\":\"subscription_sharing_"
        "usage_limit_exceeded\"}}}\n\n";
    expect(subscription_limit, strlen(subscription_limit), AH_LIMIT);
    const char subscription_unavailable[] =
        "data: "
        "{\"type\":\"response.failed\",\"response\":{\"error\":{\"code\":\"subscription_sharing_"
        "usage_unavailable\"}}}\n\n";
    expect(subscription_unavailable, strlen(subscription_unavailable), AH_LIMIT);
    const char false_terminal[] =
        "data: "
        "{\"type\":\"response.completed\",\"response\":{\"status\":\"failed\",\"output\":[]}}\n\n";
    expect(false_terminal, strlen(false_terminal), AH_PROTOCOL);
    const char done[] = "data: [DONE]\n\n";
    expect(done, strlen(done), AH_PROTOCOL);
    const char bad_delta[] = "data: {\"type\":\"response.output_text.delta\",\"delta\":{}}\n\n";
    expect(bad_delta, strlen(bad_delta), AH_PROTOCOL);
    const char nul_type[] = "data: {\"type\":\"response.completed\\u0000ignored\"}\n\n";
    expect(nul_type, strlen(nul_type), AH_PROTOCOL);
    const char nul_delta[] =
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"before\\u0000after\"}\n\n";
    text_buffer rejected = {0};
    ah_stream checked;
    ah_stream_init(&checked, 4096, 16384, collect, &rejected);
    assert(ah_stream_feed(&checked, nul_delta, strlen(nul_delta)) == AH_PROTOCOL);
    assert(rejected.length == 0);
    ah_stream_destroy(&checked);
    const char duplicate_key[] =
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"a\",\"d\\u0065lta\":\"b\"}\n\n";
    expect(duplicate_key, strlen(duplicate_key), AH_PROTOCOL);
    const char nested_duplicate[] = "data: "
                                    "{\"type\":\"response.completed\",\"response\":{\"status\":"
                                    "\"completed\",\"output\":[],\"output\":[{}]}}\n\n";
    expect(nested_duplicate, strlen(nested_duplicate), AH_PROTOCOL);
    char deep[512];
    size_t offset = (size_t)snprintf(deep, sizeof(deep), "data: {\"type\":\"x\",\"nested\":");
    for (size_t i = 0; i < 65; i++)
        deep[offset++] = '[';
    deep[offset++] = '0';
    for (size_t i = 0; i < 65; i++)
        deep[offset++] = ']';
    memcpy(deep + offset, "}\n\n", 4);
    expect(deep, offset + 3, AH_LIMIT);
    char twice[sizeof(terminal) * 2];
    snprintf(twice, sizeof(twice), "%s%s", terminal, terminal);
    expect(twice, strlen(twice), AH_PROTOCOL);
    for (size_t length = 0; length + 1 < strlen(terminal); length++)
        expect(terminal, length, AH_PROTOCOL);
    ah_stream s;
    ah_stream_init(&s, 16, 100, NULL, NULL);
    assert(ah_stream_feed(&s, terminal, strlen(terminal)) == AH_LIMIT);
    ah_stream_destroy(&s);
    ah_stream_init(&s, 4096, 10, NULL, NULL);
    assert(ah_stream_feed(&s, terminal, strlen(terminal)) == AH_LIMIT);
    ah_stream_destroy(&s);
}

static void configuration(void) {
    const char *bad[] = {"http://example.com/v1/responses",
                         "https://u:p@example.com/v1/responses",
                         "https://example.com/v1/responses?key=secret",
                         "https://example.com/v1/responses#x",
                         "https://example.com/v1/responses?",
                         "https://example.com/v1/responses#",
                         "file:///tmp/response",
                         "http://127.0.0.2/v1/responses",
                         "http://127.1/v1/responses",
                         "http://2130706433/v1/responses",
                         "http://0x7f000001/v1/responses"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ah_responses_config config;
        ah_responses_config_init(&config);
        config.endpoint = bad[i];
        config.allow_loopback_http = true;
        ah_responses *provider = NULL;
        ah_error error;
        assert(ah_responses_open(&config, &provider, &error) == AH_INVALID);
        assert(!provider);
    }
    ah_responses_config config;
    ah_responses_config_init(&config);
    ah_responses *provider = NULL;
    ah_error error;
    assert(ah_responses_open(&config, &provider, &error) == AH_OK);
    ah_provider callback = ah_responses_provider(provider);
    char *response = NULL;
    assert(callback.complete(callback.context, "{}", NULL, NULL, NULL, NULL, &response, &error) ==
           AH_AUTH);
    assert(!response);
    assert(ah_responses_set_token(provider, "abc\r\nInjected: x", &error) == AH_INVALID);
    assert(ah_responses_set_token(provider, "local-test", &error) == AH_OK);
    ah_responses_close(provider);
}

typedef struct {
    unsigned content, item, terminal;
} phases;
static void phase(void *context, ah_stream_phase kind) {
    phases *seen = context;
    if (kind == AH_STREAM_CONTENT)
        seen->content++;
    else if (kind == AH_STREAM_OUTPUT_ITEM)
        seen->item++;
    else if (kind == AH_STREAM_TERMINAL)
        seen->terminal++;
}
static void content_phases(void) {
    struct {
        const char *event;
        unsigned content, item, terminal;
    } cases[] = {
        {"{\"type\":\"response.created\",\"response\":{}}", 0, 0, 0},
        {"{\"type\":\"response.in_progress\",\"response\":{}}", 0, 0, 0},
        {"{\"type\":\"response.output_text.delta\",\"delta\":\"\"}", 0, 0, 0},
        {"{\"type\":\"response.output_text.delta\",\"delta\":\" \"}", 1, 0, 0},
        {"{\"type\":\"response.function_call_arguments.delta\",\"delta\":\"{}\"}", 1, 0, 0},
        {"{\"type\":\"response.function_call_arguments.delta\",\"delta\":\"\"}", 0, 0, 0},
        {"{\"type\":\"response.custom_tool_call_input.delta\",\"delta\":\"call()\"}", 1, 0, 0},
        {"{\"type\":\"response.custom_tool_call_input.delta\",\"delta\":\"\"}", 0, 0, 0},
        {"{\"type\":\"response.reasoning_text.delta\",\"delta\":\"thinking\"}", 0, 0, 0},
        {"{\"type\":\"future.delta\",\"delta\":\"not classified\"}", 0, 0, 0},
        {"{\"type\":\"response.output_item.done\",\"output_index\":0,"
         "\"item\":{\"id\":\"f\",\"type\":\"function_call\",\"arguments\":\"\"}}",
         0, 1, 0},
        {"{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"m\","
         "\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"hello\"}]}}",
         1, 1, 0},
        {"{\"type\":\"response.output_item.done\",\"output_index\":0,"
         "\"item\":{\"id\":\"c\",\"type\":\"custom_tool_call\",\"input\":\"call()\"}}",
         1, 1, 0},
        {"{\"type\":\"response.completed\",\"response\":{\"status\":\"completed\","
         "\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":"
         "\"done\"}]}]}}",
         1, 0, 1},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        phases seen = {0};
        ah_stream stream;
        ah_stream_init(&stream, 4096, 16384, NULL, NULL);
        stream.phase = phase;
        stream.phase_context = &seen;
        char wire[1024];
        int length = snprintf(wire, sizeof(wire), "data: %s\n\n", cases[i].event);
        assert(length > 0 && (size_t)length < sizeof(wire));
        assert(ah_stream_feed(&stream, wire, (size_t)length) == AH_OK);
        assert(seen.content == cases[i].content && seen.item == cases[i].item &&
               seen.terminal == cases[i].terminal);
        ah_stream_destroy(&stream);
    }
}

int main(void) {
    fragments();
    failures();
    configuration();
    content_phases();
    puts("stream: fragmentation, terminal, limits, UTF-8, endpoint checks passed");
    return 0;
}
