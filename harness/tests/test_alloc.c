#include "internal.h"
#include <assert.h>
#include <stdio.h>

static void initial_failure(void) {
    assert(ah_mut_doc_new_budget(0) == NULL);
    assert(ah_mut_doc_new_budget(sizeof(yyjson_mut_doc) - 1) == NULL);
    yyjson_mut_doc *doc = ah_mut_doc_new_budget(sizeof(yyjson_mut_doc));
    assert(doc);
    assert(yyjson_mut_obj(doc) == NULL);
    assert(ah_mut_write(doc) == NULL);
    ah_mut_doc_free(doc);
    ah_mut_doc_free(NULL);
    assert(ah_mut_write(NULL) == NULL);
}

static void builder_failure_is_sticky(void) {
    yyjson_mut_doc *doc = ah_mut_doc_new_budget(4096);
    assert(doc);
    yyjson_mut_val *object = yyjson_mut_obj(doc);
    assert(object);
    yyjson_mut_doc_set_root(doc, object);
    assert(yyjson_mut_obj_add_strcpy(doc, object, "kept", "first field"));
    char *before = ah_mut_write(doc);
    assert(before && !strcmp(before, "{\"kept\":\"first field\"}"));
    free(before);

    char large[8192];
    memset(large, 'x', sizeof(large) - 1);
    large[sizeof(large) - 1] = '\0';
    assert(!yyjson_mut_obj_add_strcpy(doc, object, "required", large));
    /* yyjson retains a valid, incomplete object after the failed addition. */
    char *partial = yyjson_mut_write(doc, 0, NULL);
    assert(partial && !strcmp(partial, "{\"kept\":\"first field\"}"));
    free(partial);
    assert(ah_mut_write(doc) == NULL);
    yyjson_mut_doc_set_root(doc, yyjson_mut_null(doc));
    assert(ah_mut_write(doc) == NULL);
    ah_mut_doc_free(doc);
}

static void serialization_failure(void) {
    size_t bytes = sizeof(yyjson_mut_doc) + 2 * sizeof(yyjson_mut_val);
    yyjson_mut_doc *doc = ah_mut_doc_new_budget(bytes);
    assert(doc);
    assert(yyjson_mut_doc_set_val_pool_size(doc, 1));
    yyjson_mut_val *object = yyjson_mut_obj(doc);
    assert(object);
    yyjson_mut_doc_set_root(doc, object);
    /* The complete document fits exactly. No writer allocation fits. */
    assert(ah_mut_write(doc) == NULL);
    assert(ah_mut_write(doc) == NULL);
    ah_mut_doc_free(doc);
}

static void reallocation_failure(void) {
    yyjson_mut_doc *doc = ah_mut_doc_new_budget(sizeof(yyjson_mut_doc) + 64);
    assert(doc);
    unsigned char *memory = doc->alc.malloc(doc->alc.ctx, 32);
    assert(memory);
    memset(memory, 0x5a, 32);
    assert(doc->alc.realloc(doc->alc.ctx, memory, 32, 65) == NULL);
    for (size_t i = 0; i < 32; i++)
        assert(memory[i] == 0x5a);
    doc->alc.free(doc->alc.ctx, memory);
    assert(doc->alc.malloc(doc->alc.ctx, 1) == NULL);
    assert(ah_mut_write(doc) == NULL);
    ah_mut_doc_free(doc);
}

static void success_and_ownership(void) {
    yyjson_mut_doc *doc = ah_mut_doc_new();
    assert(doc);
    yyjson_mut_val *object = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, object);
    assert(yyjson_mut_obj_add_bool(doc, object, "complete", true));
    char *json = ah_mut_write(doc);
    assert(json);
    ah_mut_doc_free(doc);
    assert(!strcmp(json, "{\"complete\":true}"));
    free(json);

    yyjson_mut_doc *untracked = yyjson_mut_doc_new(NULL);
    assert(untracked);
    yyjson_mut_doc_set_root(untracked, yyjson_mut_obj(untracked));
    assert(ah_mut_write(untracked) == NULL);
    ah_mut_doc_free(untracked);
}

static void budget_sweep_never_returns_partial(void) {
    unsigned successes = 0, failures = 0;
    const char expected[] = "{\"name\":\"operation\",\"ready\":true,\"count\":17}";
    for (size_t bytes = 0; bytes < 8192; bytes += 17) {
        yyjson_mut_doc *doc = ah_mut_doc_new_budget(bytes);
        if (!doc) {
            failures++;
            continue;
        }
        yyjson_mut_val *object = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, object);
        /* Match production callers: individual addition results are ignored. */
        yyjson_mut_obj_add_strcpy(doc, object, "name", "operation");
        yyjson_mut_obj_add_bool(doc, object, "ready", true);
        yyjson_mut_obj_add_uint(doc, object, "count", 17);
        char *json = ah_mut_write(doc);
        if (json) {
            assert(!strcmp(json, expected));
            successes++;
        } else {
            failures++;
        }
        ah_mut_doc_free(doc);
        free(json);
    }
    assert(successes && failures);
}

int main(void) {
    initial_failure();
    builder_failure_is_sticky();
    serialization_failure();
    reallocation_failure();
    success_and_ownership();
    budget_sweep_never_returns_partial();
    puts("allocation tests passed");
    return 0;
}
