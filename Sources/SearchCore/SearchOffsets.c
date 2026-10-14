#include "SearchTokenizer.h"
#include <stdlib.h>
#include <string.h>

/* Two varint deltas per source word. All colocated grams share that word's offset. */
typedef struct {
    unsigned char *data;
    int bytes, capacity, position, last_position, last_offset;
} OffsetBuilder;

static int append_integer(OffsetBuilder *builder, unsigned value) {
    do {
        if (builder->bytes == builder->capacity) {
            int capacity = builder->capacity ? builder->capacity * 2 : 256;
            if (capacity > 262144) return SQLITE_TOOBIG;
            void *data = realloc(builder->data, (size_t)capacity);
            if (!data) return SQLITE_NOMEM;
            builder->data = data; builder->capacity = capacity;
        }
        unsigned char byte = value & 127;
        value >>= 7;
        builder->data[builder->bytes++] = byte | (value ? 128 : 0);
    } while (value);
    return SQLITE_OK;
}

static int record_offset(void *context, int flags, const char *token, int bytes, int start, int end) {
    (void)token; (void)bytes; (void)end;
    OffsetBuilder *builder = context;
    if (flags & FTS5_TOKEN_COLOCATED) return SQLITE_OK;
    ++builder->position;
    if (builder->position && start == builder->last_offset) return SQLITE_OK;
    int status = append_integer(builder, (unsigned)(builder->position - builder->last_position));
    if (status == SQLITE_OK) status = append_integer(builder, (unsigned)(start - builder->last_offset));
    builder->last_position = builder->position;
    builder->last_offset = start;
    return status;
}

static void build_offsets(sqlite3_context *context, int count, sqlite3_value **values) {
    (void)count;
    if (sqlite3_value_type(values[0]) == SQLITE_NULL) { sqlite3_result_null(context); return; }
    const char *text = (const char *)sqlite3_value_text(values[0]);
    if (!text) { sqlite3_result_error_nomem(context); return; }
    OffsetBuilder builder = {.position = -1};
    int status = argus_tokenize_body(text, sqlite3_value_bytes(values[0]), &builder, record_offset);
    if (status != SQLITE_OK) {
        free(builder.data); sqlite3_result_error_code(context, status); return;
    }
    if (!builder.data) {
        sqlite3_result_blob(context, "", 0, SQLITE_STATIC);
    } else sqlite3_result_blob(context, builder.data, builder.bytes, free);
}

int argus_register_offsets(sqlite3 *database) {
    return sqlite3_create_function_v2(database, "argus_offsets", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                                      NULL, build_offsets, NULL, NULL, NULL);
}

static int read_integer(const unsigned char **cursor, const unsigned char *end, unsigned *value) {
    *value = 0;
    for (unsigned shift = 0; shift <= 28 && *cursor < end; shift += 7) {
        unsigned byte = *(*cursor)++;
        if (shift == 28 && (byte & 240)) return 0;
        *value |= (byte & 127) << shift;
        if (!(byte & 128)) return 1;
    }
    return 0;
}

int argus_excerpt_offset(const void *data, int bytes, int target) {
    if (!data || bytes < 0 || target < 0) return -1;
    const unsigned char *cursor = data, *end = cursor + bytes;
    unsigned position = 0, offset = 0;
    int selected = 0;
    while (cursor < end) {
        unsigned position_delta, offset_delta;
        if (!read_integer(&cursor, end, &position_delta) ||
            !read_integer(&cursor, end, &offset_delta) ||
            position_delta > 262144 - position || offset_delta > 32768 - offset) return -1;
        position += position_delta; offset += offset_delta;
        if (position > (unsigned)target) break;
        selected = (int)offset;
    }
    return selected;
}

void argus_excerpt_text(char output[512], const char *text, int bytes, int byte_offset) {
    int start = byte_offset > 80 ? byte_offset - 80 : 0;
    if (start > bytes) start = bytes;
    while (start && start < bytes && ((unsigned char)text[start] & 0xc0) == 0x80) --start;
    int written = 0;
    if (start) { memcpy(output, "…", 3); written = 3; }
    int length = bytes - start;
    int available = 512 - written - 4;
    if (length > available) length = available;
    while (length && start + length < bytes &&
           ((unsigned char)text[start + length] & 0xc0) == 0x80) --length;
    memcpy(output + written, text + start, (size_t)length); written += length;
    if (start + length < bytes) { memcpy(output + written, "…", 3); written += 3; }
    output[written] = 0;
}
