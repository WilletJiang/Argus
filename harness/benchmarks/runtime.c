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
static int benchmark(const char *path, const char *label, size_t seed_bytes) {
    ah_config config;
    ah_config_init(&config);
    config.database_path = path;
    config.provider.complete = model;
    if (seed_bytes)
        config.max_context_bytes = seed_bytes + 1024 * 1024;
    ah_engine *engine = NULL;
    ah_error error = {0};
    char session[AH_ID_CAP];
    ah_code rc = ah_open(&config, &engine, &error);
    if (rc == AH_OK)
        rc = ah_session_create(engine, "synthetic", "", session, &error);
    /* Seed a retained user item outside the timed interval to measure history
     * assembly costs separately from initial ingestion. No model inference. */
    if (rc == AH_OK && seed_bytes) {
        char *seed = malloc(seed_bytes + 1);
        if (!seed) {
            ah_close(engine);
            return 1;
        }
        memset(seed, 'x', seed_bytes);
        seed[seed_bytes] = 0;
        rc = ah_submit(engine, session, "seed", seed, &error);
        free(seed);
        if (rc == AH_OK)
            rc = ah_run(engine, session, &error);
    }
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
           "\"network\":false,\"seed_context_bytes\":%zu}\n",
           label, count, samples[49], samples[94], samples[99], seed_bytes);
    return 0;
}
int main(int argc, char **argv) {
    size_t seed_bytes = 0;
    if (argc > 2)
        return 2;
    if (argc == 2) {
        if (argv[1][0] < '0' || argv[1][0] > '9')
            return 2;
        char *end = NULL;
        unsigned long long value = strtoull(argv[1], &end, 10);
        if (!*argv[1] || *end || value > 8 * 1024 * 1024)
            return 2;
        seed_bytes = (size_t)value;
    }
    char root[] = "/tmp/ah-benchmark-XXXXXX";
    if (!mkdtemp(root))
        return 1;
    char path[256], lock[262];
    snprintf(path, sizeof(path), "%s/state.db", root);
    snprintf(lock, sizeof(lock), "%s.lock", path);
    int rc = benchmark(":memory:", "memory", seed_bytes);
    if (!rc)
        rc = benchmark(path, "sqlite-full", seed_bytes);
    unlink(path);
    unlink(lock);
    rmdir(root);
    return rc;
}
