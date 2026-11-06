#include "local_internal.h"

typedef struct {
    size_t position, old_size, new_size;
    const char *text;
} hunk;
static int order_hunks(const void *a, const void *b) {
    size_t x = ((const hunk *)a)->position, y = ((const hunk *)b)->position;
    return x < y ? -1 : x > y;
}

/* For a batch of exact edits, a rare byte cheaply rules out most positions.
 * Fall back to libc for common anchors or long needles where verifying every
 * candidate could compare more bytes than the entire file. */
static const char *unique_match(const ah_edit_text *source, const char *needle, size_t length,
                                const unsigned *frequency) {
    if (length > source->length)
        return NULL;
    if (frequency) {
        size_t anchor = 0;
        for (size_t i = 1; i < length; ++i)
            if (frequency[(unsigned char)needle[i]] < frequency[(unsigned char)needle[anchor]])
                anchor = i;
        unsigned candidates = frequency[(unsigned char)needle[anchor]];
        if (!candidates)
            return NULL;
        if (candidates <= source->length / 64 && candidates <= source->length / length) {
            const char *cursor = source->text + anchor;
            const char *end = cursor + (source->length - length + 1);
            const char *found = NULL, *hit;
            while ((hit = memchr(cursor, (unsigned char)needle[anchor], (size_t)(end - cursor)))) {
                if (!memcmp(hit - anchor, needle, length)) {
                    if (found)
                        return NULL;
                    found = hit - anchor;
                }
                cursor = hit + 1;
            }
            return found;
        }
    }
    const char *found = strstr(source->text, needle);
    return found && !strstr(found + 1, needle) ? found : NULL;
}

ah_code ah_local_edit(const ah_edit_text *source, yyjson_val *edits, const ah_tool_call *call,
                      ah_edit_text *edited, const char **conflict, ah_error *error) {
    hunk hunks[64];
    size_t i, n, removed = 0, added = 0;
    yyjson_val *item;
    unsigned frequency[256];
    bool use_frequency = yyjson_arr_size(edits) >= 8 && source->length >= 65536;
    if (use_frequency) {
        memset(frequency, 0, sizeof(frequency));
        for (i = 0; i < source->length; ++i)
            frequency[(unsigned char)source->text[i]]++;
    }
    yyjson_arr_foreach(edits, i, n, item) {
        ah_code stopped = ah_local_stopped(call);
        if (stopped != AH_OK)
            return ah_fail(error, stopped, "Edit interrupted before preparation completed");
        yyjson_val *old = yyjson_obj_get(item, "old_text"), *new = yyjson_obj_get(item, "new_text");
        const char *needle = yyjson_get_str(old), *text = yyjson_get_str(new);
        size_t a = yyjson_get_len(old), b = yyjson_get_len(new);
        if (a > AH_LOCAL_MAX_BYTES || b > AH_LOCAL_MAX_BYTES)
            return ah_fail(error, AH_LIMIT, "Edit text exceeds 1 MiB");
        if (memchr(needle, 0, a) || memchr(text, 0, b))
            return ah_fail(error, AH_INVALID, "NUL in edit");
        const char *match = unique_match(source, needle, a, use_frequency ? frequency : NULL);
        if (!match) {
            *conflict = "Every old_text must match exactly once in the base file";
            return AH_INVALID;
        }
        hunks[i] = (hunk){(size_t)(match - source->text), a, b, text};
    }
    qsort(hunks, n, sizeof(hunks[0]), order_hunks);
    for (i = 0; i < n; i++) {
        if (i && hunks[i].position < hunks[i - 1].position + hunks[i - 1].old_size) {
            *conflict = "Edit hunks overlap";
            return AH_INVALID;
        }
        removed += hunks[i].old_size;
        added += hunks[i].new_size;
    }
    if (added > AH_LOCAL_MAX_BYTES - (source->length - removed))
        return ah_fail(error, AH_LIMIT, "Edited file exceeds 1 MiB");
    edited->length = source->length - removed + added;
    edited->text = malloc(edited->length + 1);
    if (!edited->text)
        return ah_fail(error, AH_IO, "Out of memory");
    size_t read = 0, written = 0;
    for (i = 0; i < n; i++) {
        size_t prefix = hunks[i].position - read;
        memcpy(edited->text + written, source->text + read, prefix);
        written += prefix;
        memcpy(edited->text + written, hunks[i].text, hunks[i].new_size);
        written += hunks[i].new_size;
        read = hunks[i].position + hunks[i].old_size;
    }
    memcpy(edited->text + written, source->text + read, source->length - read);
    edited->text[edited->length] = 0;
    return AH_OK;
}
