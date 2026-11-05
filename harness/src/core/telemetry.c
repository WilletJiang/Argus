#include "core.h"

/* Best-effort, content-free observations. Never enter the durable model context. */
void core_metric(ah_engine *e, const char *sid, const char *op, const char *phase, uint64_t start,
                 uint64_t end, size_t bytes, unsigned count, ah_code code) {
    if (!e->config.observer)
        return;
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "phase", phase);
    yyjson_mut_obj_add_uint(doc, root, "started_us", start);
    yyjson_mut_obj_add_uint(doc, root, "duration_us", end >= start ? end - start : 0);
    yyjson_mut_obj_add_uint(doc, root, "bytes", bytes);
    yyjson_mut_obj_add_uint(doc, root, "count", count);
    yyjson_mut_obj_add_str(doc, root, "code", ah_code_name(code));
    char *json = ah_mut_write(doc);
    if (json) {
        ah_event event = {0, sid, op, "runtime.metrics", json};
        bool observing = e->observing;
        e->observing = true;
        e->config.observer(e->config.observer_context, &event);
        e->observing = observing;
    }
    free(json);
    ah_mut_doc_free(doc);
}
