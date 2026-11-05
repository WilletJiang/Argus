#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <stdio.h>
#include <time.h>

ah_code ah_fail(ah_error *error, ah_code code, const char *format, ...) {
    if (error) {
        error->code = code;
        va_list args;
        va_start(args, format);
        vsnprintf(error->message, sizeof(error->message), format, args);
        va_end(args);
    }
    return code;
}
void ah_error_clear(ah_error *error) {
    if (error) {
        error->code = AH_OK;
        error->message[0] = '\0';
    }
}
char *ah_strdup(const char *value) {
    if (!value)
        return NULL;
    size_t n = strlen(value);
    char *copy = malloc(n + 1);
    if (copy)
        memcpy(copy, value, n + 1);
    return copy;
}
char *ah_json_write(yyjson_val *value) {
    return value ? yyjson_val_write(value, 0, NULL) : NULL;
}
typedef struct {
    size_t remaining;
    bool failed;
} json_allocation;

static bool json_reserve(json_allocation *allocation, size_t bytes) {
    if (allocation->failed || bytes > allocation->remaining) {
        allocation->failed = true;
        return false;
    }
    return true;
}
static void *json_malloc(void *context, size_t bytes) {
    json_allocation *allocation = context;
    if (!json_reserve(allocation, bytes))
        return NULL;
    void *memory = malloc(bytes);
    if (!memory) {
        allocation->failed = true;
        return NULL;
    }
    allocation->remaining -= bytes;
    return memory;
}
static void *json_realloc(void *context, void *memory, size_t old_size, size_t bytes) {
    json_allocation *allocation = context;
    size_t growth = !memory ? bytes : (bytes > old_size ? bytes - old_size : 0);
    if (!json_reserve(allocation, growth))
        return NULL;
    if (!bytes) {
        free(memory);
        return NULL;
    }
    void *resized = realloc(memory, bytes);
    if (!resized) {
        allocation->failed = true;
        return NULL;
    }
    allocation->remaining -= growth;
    return resized;
}
static void json_free(void *context, void *memory) {
    (void)context;
    free(memory);
}
static json_allocation *json_tracker(yyjson_mut_doc *doc) {
    if (!doc || doc->alc.malloc != json_malloc || doc->alc.realloc != json_realloc ||
        doc->alc.free != json_free)
        return NULL;
    return doc->alc.ctx;
}
yyjson_mut_doc *ah_mut_doc_new_budget(size_t bytes) {
    json_allocation *allocation = malloc(sizeof(*allocation));
    if (!allocation)
        return NULL;
    *allocation = (json_allocation){.remaining = bytes};
    yyjson_alc allocator = {json_malloc, json_realloc, json_free, allocation};
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&allocator);
    if (!doc)
        free(allocation);
    return doc;
}
yyjson_mut_doc *ah_mut_doc_new(void) {
    return ah_mut_doc_new_budget(SIZE_MAX);
}
void ah_mut_doc_free(yyjson_mut_doc *doc) {
    json_allocation *allocation = json_tracker(doc);
    /* yyjson copies the allocator into the document and uses it to release its
     * pools and the document itself. Its context must survive those callbacks. */
    yyjson_mut_doc_free(doc);
    free(allocation);
}
char *ah_mut_write(yyjson_mut_doc *doc) {
    json_allocation *allocation = json_tracker(doc);
    if (!allocation || allocation->failed)
        return NULL;
    char *json = yyjson_mut_write_opts(doc, 0, &doc->alc, NULL, NULL);
    if (allocation->failed) {
        free(json);
        return NULL;
    }
    return json;
}
bool ah_json_valid(const char *json) {
    if (!json)
        return false;
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    bool valid = doc && ah_json_check(yyjson_doc_get_root(doc), NULL) == AH_OK;
    yyjson_doc_free(doc);
    return valid;
}
void ah_free(void *memory) {
    free(memory);
}
uint64_t ah_now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
        return 0;
    return (uint64_t)t.tv_sec * UINT64_C(1000) + (uint64_t)t.tv_nsec / UINT64_C(1000000);
}
uint64_t ah_now_us(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time))
        return 0;
    return (uint64_t)time.tv_sec * 1000000 + (uint64_t)time.tv_nsec / 1000;
}
const char *ah_code_name(ah_code code) {
    static const char *const names[] = {"ok",        "invalid",   "io",     "protocol",
                                        "limit",     "cancelled", "busy",   "not_found",
                                        "uncertain", "auth",      "network"};
    return (unsigned)code < sizeof(names) / sizeof(names[0]) ? names[code] : "unknown";
}
const char *ah_state_name(ah_state state) {
    static const char *const names[] = {
        "idle",      "ready",   "running", "waiting_approval", "waiting_input",
        "completed", "stopped", "failed",  "outcome_unknown"};
    return (unsigned)state < sizeof(names) / sizeof(names[0]) ? names[state] : "unknown";
}

static int key_compare(const void *a, const void *b) {
    yyjson_val *x = *(yyjson_val *const *)a, *y = *(yyjson_val *const *)b;
    size_t xn = yyjson_get_len(x), yn = yyjson_get_len(y), n = xn < yn ? xn : yn;
    int cmp = memcmp(yyjson_get_str(x), yyjson_get_str(y), n);
    return cmp ? cmp : (xn > yn) - (xn < yn);
}
static ah_code check_value(yyjson_val *v, unsigned depth, size_t *left, ah_error *error) {
    if (!v || depth > 64 || !*left)
        return ah_fail(error, AH_LIMIT, "JSON structural limit exceeded");
    --*left;
    if (yyjson_is_str(v) && memchr(yyjson_get_str(v), 0, yyjson_get_len(v)))
        return ah_fail(error, AH_PROTOCOL, "Embedded NUL is not allowed in protocol strings");
    size_t i, n;
    yyjson_val *item;
    if (yyjson_is_arr(v)) {
        yyjson_arr_foreach(v, i, n, item) {
            ah_code rc = check_value(item, depth + 1, left, error);
            if (rc != AH_OK)
                return rc;
        }
    } else if (yyjson_is_obj(v)) {
        n = yyjson_obj_size(v);
        if (n > *left / 2)
            return ah_fail(error, AH_LIMIT, "JSON object limit exceeded");
        if (!n)
            return AH_OK;
        yyjson_val **keys = malloc(n * sizeof(*keys));
        if (!keys)
            return ah_fail(error, AH_IO, "JSON validation allocation failed");
        yyjson_obj_iter it = yyjson_obj_iter_with(v);
        for (i = 0; i < n; ++i)
            keys[i] = yyjson_obj_iter_next(&it);
        qsort(keys, n, sizeof(*keys), key_compare);
        for (i = 1; i < n; ++i) {
            if (!key_compare(&keys[i - 1], &keys[i])) {
                free(keys);
                return ah_fail(error, AH_PROTOCOL, "Duplicate JSON member");
            }
        }
        free(keys);
        it = yyjson_obj_iter_with(v);
        while ((item = yyjson_obj_iter_next(&it))) {
            ah_code rc = check_value(item, depth + 1, left, error);
            if (rc == AH_OK)
                rc = check_value(yyjson_obj_iter_get_val(item), depth + 1, left, error);
            if (rc != AH_OK)
                return rc;
        }
    }
    return AH_OK;
}
ah_code ah_json_check(yyjson_val *value, ah_error *error) {
    size_t remaining = 65536;
    return check_value(value, 0, &remaining, error);
}
