/* A complete offline embedding example. Replace only the provider for live use. */
#include <ah/ah.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ah_code complete(void *context, const char *request, ah_delta_sink delta,
                        void *delta_context, ah_cancel_check cancelled, void *cancel_context,
                        char **response, ah_error *error) {
    (void)context;
    (void)request;
    (void)error;
    if (cancelled(cancel_context))
        return AH_CANCELLED;
    const char *json =
        "{\"status\":\"completed\",\"output\":[{\"type\":\"message\","
        "\"role\":\"assistant\",\"status\":\"completed\",\"content\":[{\"type\":\"output_text\","
        "\"text\":\"Hello from the standalone C runtime.\"}]}]}";
    *response = malloc(strlen(json) + 1);
    if (!*response)
        return AH_IO;
    memcpy(*response, json, strlen(json) + 1);
    if (delta)
        delta(delta_context, "Hello", 5);
    return AH_OK;
}
int main(void) {
    ah_config config;
    ah_config_init(&config);
    config.database_path = ":memory:";
    config.provider.complete = complete;
    ah_engine *engine = NULL;
    ah_error error = {0};
    char id[AH_ID_CAP];
    ah_code code = ah_open(&config, &engine, &error);
    if (code == AH_OK)
        code = ah_session_create(engine, "offline-example", "Be concise.", id, &error);
    if (code == AH_OK)
        code = ah_submit(engine, id, "example-1", "Hello", &error);
    if (code == AH_OK)
        code = ah_run(engine, id, &error);
    char *json = NULL;
    if (code == AH_OK)
        code = ah_session_json(engine, id, &json, &error);
    if (json) {
        puts(json);
        ah_free(json);
    }
    if (code != AH_OK)
        fprintf(stderr, "%s\n", error.message);
    ah_close(engine);
    return code == AH_OK ? 0 : 1;
}
