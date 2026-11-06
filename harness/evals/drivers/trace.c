#include "trace.h"
#include "internal.h"
#include <stdio.h>

void eval_transport_observe(void *context, const ah_responses_metrics *m) {
    (void)context;
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "event", "provider.metrics");
    yyjson_mut_obj_add_uint(doc, root, "attempt", m->attempt);
    yyjson_mut_obj_add_str(doc, root, "code", ah_code_name(m->result));
    yyjson_mut_obj_add_int(doc, root, "http_status", m->http_status);
    yyjson_mut_obj_add_uint(doc, root, "request_bytes", m->request_bytes);
    yyjson_mut_obj_add_uint(doc, root, "response_bytes", m->response_bytes);
    yyjson_mut_obj_add_bool(doc, root, "retry_planned", m->retry_planned);
    if (m->elapsed_present)
        yyjson_mut_obj_add_uint(doc, root, "elapsed_us", m->elapsed_us);
    else
        yyjson_mut_obj_add_null(doc, root, "elapsed_us");
    if (m->new_connections_present)
        yyjson_mut_obj_add_int(doc, root, "new_connections", m->new_connections);
    else
        yyjson_mut_obj_add_null(doc, root, "new_connections");
    const char *names[] = {"first_byte_us",    "upload_complete_us",
                           "first_content_us", "first_item_us",
                           "terminal_us",      "dns_us",
                           "connect_us",       "tls_us",
                           "pretransfer_us",   "curl_first_byte_us",
                           "curl_total_us"};
    ah_responses_time times[] = {m->first_byte,    m->upload_complete,
                                 m->first_content, m->first_output_item,
                                 m->terminal,      m->dns,
                                 m->connect,       m->tls,
                                 m->pretransfer,   m->curl_first_byte,
                                 m->curl_total};
    for (size_t i = 0; i < sizeof(times) / sizeof(*times); i++) {
        if (times[i].present)
            yyjson_mut_obj_add_uint(doc, root, names[i], times[i].us);
        else
            yyjson_mut_obj_add_null(doc, root, names[i]);
    }
    char *line = ah_mut_write(doc);
    ah_mut_doc_free(doc);
    if (line) {
        puts(line);
        fflush(stdout);
        free(line);
    }
}

static void response_event(const char *json, ah_code code, const ah_error *error,
                           uint64_t elapsed) {
    yyjson_doc *parsed = json ? yyjson_read(json, strlen(json), 0) : NULL;
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc), *data = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "event", "provider.response");
    yyjson_mut_obj_add_uint(doc, root, "elapsed_ms", elapsed);
    yyjson_mut_obj_add_val(doc, root, "data", data);
    const char *fields[] = {"id",
                            "model",
                            "status",
                            "service_tier",
                            "usage",
                            "reasoning",
                            "prompt_cache_diagnostics",
                            "prompt_cache_key",
                            "prompt_cache_options",
                            "prompt_cache_retention"};
    for (size_t i = 0; parsed && i < sizeof(fields) / sizeof(*fields); i++) {
        yyjson_val *value = yyjson_obj_get(yyjson_doc_get_root(parsed), fields[i]);
        if (value)
            yyjson_mut_obj_add_val(doc, data, fields[i], yyjson_val_mut_copy(doc, value));
    }
    if (code != AH_OK) {
        yyjson_mut_val *failure = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, failure, "code", ah_code_name(code));
        yyjson_mut_obj_add_strcpy(doc, failure, "message", error ? error->message : "");
        yyjson_mut_obj_add_val(doc, root, "transport_error", failure);
    }
    char *line = ah_mut_write(doc);
    if (line) {
        puts(line);
        fflush(stdout);
    }
    free(line);
    ah_mut_doc_free(doc);
    yyjson_doc_free(parsed);
}
static ah_code complete(void *context, const char *request, ah_delta_sink delta, void *delta_ctx,
                        ah_cancel_check cancelled, void *cancel_ctx, char **response,
                        ah_error *error) {
    eval_model *model = context;
    yyjson_doc *parsed = yyjson_read(request, strlen(request), 0);
    yyjson_mut_doc *doc = ah_mut_doc_new();
    if (!parsed || !doc) {
        yyjson_doc_free(parsed);
        ah_mut_doc_free(doc);
        return ah_fail(error, AH_IO, "cannot prepare evaluation request");
    }
    yyjson_mut_val *root = yyjson_val_mut_copy(doc, yyjson_doc_get_root(parsed));
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_val *reasoning = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, reasoning, "effort", model->effort);
    yyjson_mut_obj_add_val(doc, root, "reasoning", reasoning);
    if (model->omit_cache_key)
        yyjson_mut_obj_remove_str(root, "prompt_cache_key");
    if (model->diagnostics && model->comparison_id) {
        yyjson_mut_val *options = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, options, "comparison_response_id", model->comparison_id);
        yyjson_mut_obj_add_val(doc, root, "prompt_cache_options", options);
    }
    char *json = ah_mut_write(doc);
    yyjson_doc_free(parsed);
    ah_mut_doc_free(doc);
    if (!json)
        return ah_fail(error, AH_IO, "cannot encode evaluation request");
    /* Optional stdout event is consumed in memory by the evaluator, which
     * persists fingerprints, not the request content. Never includes headers. */
    if (model->trace) {
        printf("{\"event\":\"provider.request\",\"data\":%s}\n", json);
        fflush(stdout);
    }
    uint64_t started = ah_now_ms();
    ah_code code = model->inner.complete(model->inner.context, json, delta, delta_ctx, cancelled,
                                         cancel_ctx, response, error);
    if (model->trace)
        response_event(*response, code, error, ah_now_ms() - started);
    free(json);
    if (code == AH_OK && model->diagnostics && *response) {
        parsed = yyjson_read(*response, strlen(*response), 0);
        const char *id =
            parsed ? yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(parsed), "id")) : NULL;
        char *next = id && strlen(id) <= 512 ? ah_strdup(id) : NULL;
        free(model->comparison_id);
        model->comparison_id = next;
        yyjson_doc_free(parsed);
    }
    return code;
}
ah_provider eval_model_provider(eval_model *model) {
    return (ah_provider){complete, model};
}
void eval_model_close(eval_model *model) {
    free(model->comparison_id);
    model->comparison_id = NULL;
}
