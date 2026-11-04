#include "stream.h"
#include "../internal.h"

#define MAX_STREAM_ITEMS 4096

typedef struct {
    char *id, *type, *done;
    bool added;
} output_item;
struct ah_stream_items {
    output_item *values;
    size_t count;
};

ah_code ah_stream_item(ah_stream *s, yyjson_val *event, bool done) {
    yyjson_val *index = yyjson_obj_get(event, "output_index"),
               *item = yyjson_obj_get(event, "item"), *id = yyjson_obj_get(item, "id");
    if (!yyjson_is_uint(index) || yyjson_get_uint(index) >= MAX_STREAM_ITEMS ||
        !yyjson_is_obj(item) || !yyjson_is_str(yyjson_obj_get(item, "type")) ||
        !yyjson_is_str(id) || !yyjson_get_len(id))
        return ah_fail(&s->error, AH_PROTOCOL, "invalid streamed output item");
    size_t position = (size_t)yyjson_get_uint(index);
    if (!s->items) {
        s->items = calloc(1, sizeof(*s->items));
        if (!s->items)
            return ah_fail(&s->error, AH_IO, "output item allocation failed");
    }
    if (position >= s->items->count) {
        size_t count = position + 1;
        output_item *values = realloc(s->items->values, count * sizeof(*values));
        if (!values)
            return ah_fail(&s->error, AH_IO, "output item allocation failed");
        memset(values + s->items->count, 0, (count - s->items->count) * sizeof(*values));
        s->items->values = values;
        s->items->count = count;
    }
    output_item *slot = &s->items->values[position];
    const char *identity = yyjson_get_str(id);
    const char *type = yyjson_get_str(yyjson_obj_get(item, "type"));
    for (size_t i = 0; i < s->items->count; i++)
        if (i != position && s->items->values[i].id && !strcmp(s->items->values[i].id, identity))
            return ah_fail(&s->error, AH_PROTOCOL, "duplicate output item identity");
    if (slot->done || (!done && slot->added) || (slot->id && strcmp(slot->id, identity)) ||
        (slot->type && strcmp(slot->type, type)))
        return ah_fail(&s->error, AH_PROTOCOL, "duplicate or conflicting output item");
    if (!slot->id)
        slot->id = ah_strdup(identity);
    if (!slot->type)
        slot->type = ah_strdup(type);
    if (!slot->id || !slot->type)
        return ah_fail(&s->error, AH_IO, "output identity allocation failed");
    if (done) {
        slot->done = yyjson_val_write(item, 0, NULL);
        if (!slot->done)
            return ah_fail(&s->error, AH_IO, "output item allocation failed");
    } else
        slot->added = true;
    s->observed_output = true;
    return AH_OK;
}

/* Item-done carries the authoritative completed payload (including encrypted
 * reasoning). A terminal snapshot may omit it, but may not contradict it. */
static bool compatible(yyjson_val *done, yyjson_val *snapshot) {
    if (!yyjson_is_obj(snapshot))
        return false;
    yyjson_obj_iter iter = yyjson_obj_iter_with(snapshot);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter))) {
        const char *name = yyjson_get_str(key);
        yyjson_val *value = yyjson_obj_get(done, name);
        if (value && strcmp(name, "encrypted_content") &&
            !yyjson_equals(value, yyjson_obj_iter_get_val(key)))
            return false;
    }
    return true;
}

ah_code ah_stream_response(ah_stream *s, yyjson_val *response) {
    yyjson_val *snapshot = yyjson_obj_get(response, "output");
    if (!s->items) {
        s->response = yyjson_val_write(response, 0, NULL);
        return s->response ? AH_OK : ah_fail(&s->error, AH_IO, "response allocation failed");
    }
    yyjson_mut_doc *doc = ah_mut_doc_new();
    if (!doc)
        return ah_fail(&s->error, AH_IO, "response allocation failed");
    yyjson_mut_val *root = yyjson_val_mut_copy(doc, response), *output = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_remove_str(root, "output");
    yyjson_mut_obj_add_val(doc, root, "output", output);
    size_t count = yyjson_arr_size(snapshot);
    if (s->items->count > count)
        count = s->items->count;
    ah_code code = AH_OK;
    for (size_t i = 0; i < count && code == AH_OK; i++) {
        output_item *slot = i < s->items->count ? &s->items->values[i] : NULL;
        yyjson_val *final = yyjson_arr_get(snapshot, i);
        yyjson_doc *item_doc =
            slot && slot->done ? yyjson_read(slot->done, strlen(slot->done), 0) : NULL;
        yyjson_val *item = item_doc ? yyjson_doc_get_root(item_doc) : final;
        if ((slot && slot->done && !item_doc))
            code = ah_fail(&s->error, AH_IO, "output item decode failed");
        else if (!yyjson_is_obj(item) ||
                 (slot && slot->id && !yyjson_equals_str(yyjson_obj_get(item, "id"), slot->id)) ||
                 (slot && slot->type &&
                  !yyjson_equals_str(yyjson_obj_get(item, "type"), slot->type)) ||
                 (item_doc && final && !compatible(item, final)))
            code = ah_fail(&s->error, AH_PROTOCOL, "missing or conflicting completed output item");
        else
            yyjson_mut_arr_append(output, yyjson_val_mut_copy(doc, item));
        yyjson_doc_free(item_doc);
    }
    if (code == AH_OK) {
        s->response = ah_mut_write(doc);
        if (!s->response)
            code = ah_fail(&s->error, AH_IO, "response allocation failed");
        else if (strlen(s->response) > s->response_limit)
            code = ah_fail(&s->error, AH_LIMIT, "assembled response byte limit exceeded");
    }
    ah_mut_doc_free(doc);
    return code;
}

void ah_stream_items_destroy(ah_stream *s) {
    if (!s->items)
        return;
    for (size_t i = 0; i < s->items->count; i++) {
        free(s->items->values[i].id);
        free(s->items->values[i].type);
        free(s->items->values[i].done);
    }
    free(s->items->values);
    free(s->items);
    s->items = NULL;
}
