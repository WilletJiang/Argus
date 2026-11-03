/* Local orchestration benchmark. No network/model inference or filesystem tools. */
#include <ah/ah.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static ah_code model(void *context, const char *request, ah_delta_sink delta, void *delta_ctx,
                     ah_cancel_check cancelled, void *cancel_ctx, char **out, ah_error *error) {
    (void)context;
    (void)request;
    (void)delta;
    (void)delta_ctx;
    (void)error;
    if (cancelled(cancel_ctx))
        return AH_CANCELLED;
    const char *value = "{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":"
                        "\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"ok\"}]}]}";
    *out = malloc(strlen(value) + 1);
    if (!*out)
        return AH_IO;
    strcpy(*out, value);
    return AH_OK;
}
static double clock_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec * 1000 + (double)now.tv_nsec / 1000000;
}
static int compare(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static int benchmark(const char *path, const char *label) {
    ah_config config;
    ah_config_init(&config);
    config.database_path = path;
    config.provider.complete = model;
    ah_engine *engine = NULL;
    ah_error error = {0};
    char session[AH_ID_CAP];
    ah_code rc = ah_open(&config, &engine, &error);
    if (rc == AH_OK)
        rc = ah_session_create(engine, "synthetic", "", session, &error);
    double samples[100];
    size_t count = 0;
    for (; count < 100 && rc == AH_OK; ++count) {
        char command[32];
        snprintf(command, sizeof(command), "sample-%zu", count);
        double start = clock_ms();
        rc = ah_submit(engine, session, command, "ping", &error);
        if (rc == AH_OK)
            rc = ah_run(engine, session, &error);
        samples[count] = clock_ms() - start;
    }
    ah_close(engine);
    if (rc != AH_OK) {
        fprintf(stderr, "%s: %s\n", ah_code_name(rc), error.message);
        return 1;
    }
    qsort(samples, count, sizeof(*samples), compare);
    printf("{\"mode\":\"%s\",\"runs\":%zu,\"p50_ms\":%.3f,\"p95_ms\":%.3f,\"max_ms\":%.3f,"
           "\"network\":false}\n",
           label, count, samples[49], samples[94], samples[99]);
    return 0;
}
int main(void) {
    char root[] = "/tmp/ah-benchmark-XXXXXX";
    if (!mkdtemp(root))
        return 1;
    char path[256], lock[262];
    snprintf(path, sizeof(path), "%s/state.db", root);
    snprintf(lock, sizeof(lock), "%s.lock", path);
    int rc = benchmark(":memory:", "memory");
    if (!rc)
        rc = benchmark(path, "sqlite-full");
    unlink(path);
    unlink(lock);
    rmdir(root);
    return rc;
}
