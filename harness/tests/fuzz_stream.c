#include "../src/providers/stream.h"
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t length) {
    if (length > 1024 * 1024)
        return 0;
    ah_stream stream;
    ah_stream_init(&stream, 65536, 1024 * 1024, NULL, NULL);
    size_t offset = 0;
    while (offset < length) {
        size_t chunk = 1 + data[offset] % 97;
        if (chunk > length - offset)
            chunk = length - offset;
        if (ah_stream_feed(&stream, (const char *)data + offset, chunk))
            break;
        offset += chunk;
    }
    char *response = NULL;
    ah_stream_finish(&stream, &response);
    free(response);
    ah_stream_destroy(&stream);
    return 0;
}
