#include "cli.h"
#include "../internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cli_print(cli_context *cli, const char *json) {
    pthread_mutex_lock(&cli->output);
    puts(json ? json : "null");
    fflush(stdout);
    pthread_mutex_unlock(&cli->output);
}
void cli_observe(void *context, const ah_event *event) {
    cli_context *cli = context;
    if (!cli->json_events && !cli->rpc)
        return;
    yyjson_mut_doc *doc = ah_mut_doc_new();
    if (!doc)
        return;
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, obj);
    yyjson_mut_obj_add_str(doc, obj, "event", event->type);
    yyjson_mut_obj_add_uint(doc, obj, "seq", event->seq);
    if (event->session_id)
        yyjson_mut_obj_add_str(doc, obj, "session", event->session_id);
    if (event->operation_id)
        yyjson_mut_obj_add_str(doc, obj, "operation", event->operation_id);
    yyjson_doc *payload = event->json ? yyjson_read(event->json, strlen(event->json), 0) : NULL;
    if (payload)
        yyjson_mut_obj_add_val(doc, obj, "data",
                               yyjson_val_mut_copy(doc, yyjson_doc_get_root(payload)));
    char *text = ah_mut_write(doc);
    if (text)
        cli_print(cli, text);
    free(text);
    yyjson_doc_free(payload);
    ah_mut_doc_free(doc);
}
