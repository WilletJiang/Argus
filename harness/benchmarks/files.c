/* Real local tool callbacks; excludes the engine, model, approval, and event log. */
#include <ah/tools.h>
#include "yyjson.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static ah_tool registered[16];
static size_t tool_count;

/* Capture the production tool definitions without constructing an engine. */
ah_code ah_register_tool(ah_engine *engine, const ah_tool *tool, ah_error *error) {
    (void)engine;
    (void)error;
    if (tool_count == sizeof(registered) / sizeof(*registered))
        return AH_LIMIT;
    registered[tool_count++] = *tool;
    return AH_OK;
}

static ah_tool *lookup(const char *name) {
    for (size_t i = 0; i < tool_count; ++i)
        if (!strcmp(registered[i].name, name))
            return &registered[i];
    return NULL;
}

static double clock_us(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return -1;
    return (double)now.tv_sec * 1000000 + (double)now.tv_nsec / 1000;
}

static int compare(const void *left, const void *right) {
    double a = *(const double *)left, b = *(const double *)right;
    return (a > b) - (a < b);
}

static bool measure(const char *label, const char *name, const char *arguments,
                    size_t expected_bytes, size_t runs) {
    ah_tool *tool = lookup(name);
    double *samples = malloc(runs * sizeof(*samples));
    if (!tool || !samples) {
        free(samples);
        return false;
    }
    ah_tool_call call = {.session_id = "benchmark",
                         .operation_id = "benchmark",
                         .arguments_json = arguments,
                         .output_limit = 524288};
    size_t receipt_bytes = 0;
    bool ok = true;
    const size_t warmup = 20;
    for (size_t i = 0; i < runs + warmup; ++i) {
        ah_tool_result result = {0};
        ah_error error = {0};
        double start = clock_us();
        ah_code code = tool->execute(tool->context, &call, &result, &error);
        double end = clock_us();
        if (code != AH_OK || result.is_error || result.outcome_unknown || !result.json ||
            start < 0 || end < start) {
            fprintf(stderr, "%s callback failed: %s\n", label, error.message);
            free(result.json);
            ok = false;
            break;
        }
        if (i >= warmup)
            samples[i - warmup] = end - start;
        receipt_bytes = strlen(result.json);
        yyjson_doc *doc = yyjson_read(result.json, receipt_bytes, 0);
        yyjson_val *root = yyjson_doc_get_root(doc);
        bool valid = doc && yyjson_get_uint(yyjson_obj_get(root, "bytes")) == expected_bytes;
        if (tool->effect == AH_EFFECT_READ)
            valid = valid && yyjson_get_len(yyjson_obj_get(root, "content")) == expected_bytes &&
                    !yyjson_get_bool(yyjson_obj_get(root, "truncated"));
        yyjson_doc_free(doc);
        free(result.json);
        if (!valid) {
            fprintf(stderr, "%s returned an incomplete or invalid receipt\n", label);
            ok = false;
            break;
        }
    }
    if (ok) {
        qsort(samples, runs, sizeof(*samples), compare);
        printf("{\"case\":\"%s\",\"runs\":%zu,\"warmup_runs\":%zu,\"file_bytes\":%zu,"
               "\"returned_content_bytes\":%zu,\"receipt_json_bytes\":%zu,"
               "\"p50_us\":%.3f,\"p95_us\":%.3f,\"max_us\":%.3f,"
               "\"file_and_directory_fsync\":%s,\"network\":false,\"model\":false}\n",
               label, runs, warmup, expected_bytes,
               tool->effect == AH_EFFECT_READ ? expected_bytes : 0, receipt_bytes,
               samples[(runs - 1) / 2], samples[(95 * runs + 99) / 100 - 1], samples[runs - 1],
               tool->effect == AH_EFFECT_WRITE ? "true" : "false");
    }
    free(samples);
    return ok;
}

static bool put_fixture(const char *workspace, const char *leaf, const char *data, size_t length) {
    char path[256];
    if (snprintf(path, sizeof(path), "%s/%s", workspace, leaf) >= (int)sizeof(path))
        return false;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return false;
    size_t done = 0;
    while (done < length) {
        ssize_t n = write(fd, data + done, length - done);
        if (n > 0)
            done += (size_t)n;
        else if (n < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    bool closed = close(fd) == 0;
    return done == length && closed;
}

static char *write_arguments(const char *data, size_t length) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "path", "write4k");
    yyjson_mut_obj_add_strn(doc, root, "content", data, length);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

int main(int argc, char **argv) {
    size_t runs = 200;
    if (argc == 2) {
        char *end;
        errno = 0;
        unsigned long value = strtoul(argv[1], &end, 10);
        if (errno || !*argv[1] || *end || !value || value > 1000000) {
            fprintf(stderr, "runs must be an integer from 1 to 1000000\n");
            return 2;
        }
        runs = (size_t)value;
    } else if (argc != 1) {
        fprintf(stderr, "usage: bench_files [runs]\n");
        return 2;
    }
    char workspace[] = "/tmp/argus-files-XXXXXX";
    if (!mkdtemp(workspace))
        return 1;
    char fixture[65536];
    memset(fixture, 'x', sizeof(fixture));
    for (size_t i = 127; i < sizeof(fixture); i += 128)
        fixture[i] = '\n';
    char *arguments = write_arguments(fixture, 4096);
    ah_local_tools *tools = NULL;
    ah_local_tools_config config = {.workspace = workspace};
    ah_error error = {0};
    bool ok = arguments && put_fixture(workspace, "read4k", fixture, 4096) &&
              put_fixture(workspace, "read64k", fixture, sizeof(fixture)) &&
              ah_local_tools_open(&config, &tools, &error) == AH_OK &&
              ah_local_tools_register(tools, (ah_engine *)(void *)&config, &error) == AH_OK;
    if (ok)
        ok = measure("read4k-warm-cache", "read_file", "{\"path\":\"read4k\",\"max_lines\":1000}",
                     4096, runs) &&
             measure("read64k-warm-cache", "read_file", "{\"path\":\"read64k\",\"max_lines\":1000}",
                     sizeof(fixture), runs) &&
             measure("write4k-atomic", "write_file", arguments, 4096, runs);
    ah_local_tools_close(tools);
    free(arguments);
    const char *leaves[] = {"read4k", "read64k", "write4k"};
    for (size_t i = 0; i < sizeof(leaves) / sizeof(*leaves); ++i) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", workspace, leaves[i]);
        if (unlink(path) && errno != ENOENT)
            ok = false;
    }
    if (rmdir(workspace))
        ok = false;
    return ok ? 0 : 1;
}
