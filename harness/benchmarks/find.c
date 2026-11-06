/* Persistent callback timing. No model, journal, child search process or network. */
#include "ah/tools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static ah_tool finder;
ah_code ah_register_tool(ah_engine *engine, const ah_tool *tool, ah_error *error) {
    (void)engine;
    (void)error;
    /* Also builds against the 0.3 baseline for controlled comparisons. */
    if (!strcmp(tool->name, "find") || !strcmp(tool->name, "search"))
        finder = *tool;
    return AH_OK;
}
static uint64_t now_ns(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time))
        return 0;
    return (uint64_t)time.tv_sec * 1000000000 + (uint64_t)time.tv_nsec;
}
static int order(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
int main(int argc, char **argv) {
    if (argc != 4) {
        fputs("usage: bench_find WORKSPACE ARGUMENTS_JSON ROUNDS\n", stderr);
        return 2;
    }
    char *end = NULL;
    unsigned long rounds = strtoul(argv[3], &end, 10);
    if (!*argv[3] || *end || !rounds || rounds > 10000)
        return 2;
    ah_local_tools_config config = {.workspace = argv[1], .enable_workflows = true};
    ah_local_tools *tools = NULL;
    ah_error error = {0};
    if (ah_local_tools_open(&config, &tools, &error) != AH_OK)
        return 1;
    if (ah_local_tools_register(tools, (ah_engine *)&config, &error) != AH_OK || !finder.execute) {
        ah_local_tools_close(tools);
        return 1;
    }
    uint64_t *samples = malloc(rounds * sizeof(*samples));
    if (!samples) {
        ah_local_tools_close(tools);
        return 1;
    }
    char *last = NULL;
    bool is_error = false;
    ah_code code = AH_OK;
    size_t measured = 0;
    for (size_t i = 0; i < rounds + 3; i++) {
        ah_tool_call call = {.session_id = "benchmark",
                             .operation_id = "benchmark",
                             .arguments_json = argv[2],
                             .output_limit = 1048576,
                             .model_output_limit = 65536,
                             .deadline_ms = ah_now_ms() + 30000};
        ah_tool_result result = {0};
        uint64_t started = now_ns();
        code = finder.execute(finder.context, &call, &result, &error);
        uint64_t elapsed = now_ns() - started;
        free(last);
        last = result.json;
        is_error = result.is_error;
        if (code != AH_OK || !last)
            break;
        if (i >= 3)
            samples[measured++] = elapsed;
    }
    bool complete = code == AH_OK && last && measured == rounds;
    if (complete) {
        qsort(samples, rounds, sizeof(*samples), order);
        printf("{\"rounds\":%lu,\"warmup\":3,\"p50_us\":%.3f,\"p95_us\":%.3f,"
               "\"result_bytes\":%zu,\"is_error\":%s,\"result\":%s}\n",
               rounds, samples[(rounds - 1) / 2] / 1000.0,
               samples[(95 * rounds + 99) / 100 - 1] / 1000.0, strlen(last),
               is_error ? "true" : "false", last);
    } else
        fprintf(stderr, "%s\n", error.message);
    free(last);
    free(samples);
    ah_local_tools_close(tools);
    return complete ? 0 : 1;
}
