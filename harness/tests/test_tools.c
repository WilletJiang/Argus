#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "tools_test.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static ah_tool registered[5];
static size_t tool_count;

ah_code ah_register_tool(ah_engine *engine, const ah_tool *tool, ah_error *error) {
    (void)engine;
    (void)error;
    CHECK(tool_count < 5);
    registered[tool_count++] = *tool;
    return AH_OK;
}

static ah_tool *lookup(const char *name) {
    for (size_t i = 0; i < tool_count; ++i)
        if (!strcmp(registered[i].name, name))
            return &registered[i];
    return NULL;
}

yyjson_doc *tools_invoke(const char *name, const char *args, size_t limit, uint64_t deadline,
                         ah_cancel_check cancelled, void *cancel_context, ah_code expected,
                         bool unknown) {
    ah_tool *tool = lookup(name);
    CHECK(tool);
    ah_tool_call call = {.session_id = "test",
                         .operation_id = "test",
                         .arguments_json = args,
                         .cancelled = cancelled,
                         .cancel_context = cancel_context,
                         .deadline_ms = deadline,
                         .output_limit = limit};
    ah_tool_result result = {0};
    ah_error error = {0};
    ah_code code = tool->execute(tool->context, &call, &result, &error);
    ah_code callback_expected = unknown ? expected : AH_OK;
    if (code != callback_expected)
        fprintf(stderr, "%s: got %s expected %s: %s\n", name, ah_code_name(code),
                ah_code_name(callback_expected), error.message);
    CHECK(code == callback_expected);
    CHECK(result.is_error == (expected != AH_OK));
    CHECK(result.outcome_unknown == unknown);
    CHECK(result.json);
    CHECK(!limit || strlen(result.json) <= limit);
    yyjson_doc *doc = yyjson_read(result.json, strlen(result.json), 0);
    CHECK(doc);
    if (expected != AH_OK)
        CHECK(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "error"),
                                ah_code_name(expected)));
    free(result.json);
    return doc;
}

void tools_expect(const char *name, const char *args, ah_code code) {
    yyjson_doc_free(tools_invoke(name, args, 4096, ah_now_ms() + 1000, NULL, NULL, code, false));
}

void tools_put_file(const char *path, const void *data, size_t length) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    CHECK(write(fd, data, length) == (ssize_t)length);
    CHECK(close(fd) == 0);
}

static void clear_directory(const char *path) {
    DIR *dir = opendir(path);
    CHECK(dir);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char child[PATH_MAX];
        CHECK(snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) < (int)sizeof(child));
        struct stat st;
        CHECK(lstat(child, &st) == 0);
        if (S_ISDIR(st.st_mode))
            clear_directory(child);
        else
            CHECK(unlink(child) == 0);
    }
    closedir(dir);
    CHECK(rmdir(path) == 0);
}

static char *exec_arguments(const char *program, const char *mode, const char *extra) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_val *argv = yyjson_mut_obj_add_arr(doc, root, "argv");
    yyjson_mut_arr_append(argv, yyjson_mut_str(doc, program));
    yyjson_mut_arr_append(argv, yyjson_mut_str(doc, "--child"));
    yyjson_mut_arr_append(argv, yyjson_mut_str(doc, mode));
    if (extra)
        yyjson_mut_arr_append(argv, yyjson_mut_str(doc, extra));
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    CHECK(json);
    return json;
}

static int child(int argc, char **argv) {
    CHECK(argc >= 3);
    if (!strcmp(argv[2], "args")) {
        CHECK(argc == 4);
        printf("%s", argv[3]);
    } else if (!strcmp(argv[2], "environment")) {
        CHECK(argc == 4);
        char cwd[PATH_MAX];
        CHECK(getcwd(cwd, sizeof(cwd)));
        char byte;
        printf("secret=%d;fd=%d;stdin=%zd;cwd=%s", getenv("AH_TEST_SECRET") != NULL,
               fcntl(atoi(argv[3]), F_GETFD) >= 0, read(STDIN_FILENO, &byte, 1), cwd);
    } else if (!strcmp(argv[2], "flood")) {
        char data[4096];
        memset(data, 'x', sizeof(data));
        for (unsigned i = 0; i < 128; ++i) {
            CHECK(write(STDOUT_FILENO, data, sizeof(data)) == (ssize_t)sizeof(data));
            CHECK(write(STDERR_FILENO, data, sizeof(data)) == (ssize_t)sizeof(data));
        }
    } else if (!strcmp(argv[2], "binary")) {
        unsigned char data[] = {0xff, 0x00, 0x80};
        CHECK(write(STDOUT_FILENO, data, sizeof(data)) == (ssize_t)sizeof(data));
    } else if (!strcmp(argv[2], "hang")) {
        signal(SIGTERM, SIG_IGN);
        for (;;)
            pause();
    } else if (!strcmp(argv[2], "descendant")) {
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (!pid)
            for (;;)
                pause();
    } else if (!strcmp(argv[2], "closed_descendant")) {
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (!pid) {
            signal(SIGTERM, SIG_IGN);
            close(STDIN_FILENO);
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
            for (;;)
                pause();
        }
        printf("%ld", (long)pid);
        fflush(stdout);
        for (;;)
            pause();
    } else
        return 7;
    return 0;
}

static bool cancel_now(void *context) {
    (void)context;
    return true;
}
static bool cancel_after(void *context) {
    return ah_now_ms() >= *(uint64_t *)context;
}
static bool cancel_before_commit(void *context) {
    return ++*(unsigned *)context >= 3;
}

static void test_schema(void) {
    ah_error error;
    const char *schema =
        "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"integer\",\"minimum\":1,"
        "\"maximum\":3}},\"required\":[\"x\"],\"additionalProperties\":false}";
    CHECK(ah_schema_validate(schema, "{\"x\":2}", &error) == AH_OK);
    CHECK(ah_schema_validate(schema, "{\"x\":2.0}", &error) == AH_OK);
    CHECK(ah_schema_validate(schema, "{\"x\":0}", &error) == AH_INVALID);
    CHECK(ah_schema_validate(schema, "{\"x\":4}", &error) == AH_INVALID);
    CHECK(ah_schema_validate(schema, "{\"x\":2,\"y\":0}", &error) == AH_INVALID);
    CHECK(ah_schema_validate(schema, "{}", &error) == AH_INVALID);
    CHECK(ah_schema_validate(schema, "{\"x\":1,\"x\":2}", &error) == AH_INVALID);
    CHECK(
        ah_schema_validate("{\"type\":\"object\",\"properties\":{\"unused\":{\"pattern\":\".*\"}}}",
                           "{}", &error) == AH_INVALID);
    CHECK(ah_schema_validate("{\"type\":\"string\",\"minLength\":1,\"maxLength\":1}", "\"你\"",
                             &error) == AH_OK);
    CHECK(ah_schema_validate(
              "{\"type\":\"array\",\"items\":{\"type\":\"boolean\"},\"minItems\":1,\"maxItems\":2}",
              "[true,false]", &error) == AH_OK);
    CHECK(ah_schema_validate("{\"type\":\"array\",\"items\":{\"type\":\"boolean\"}}", "[1]",
                             &error) == AH_INVALID);
    CHECK(ah_schema_validate("{\"type\":\"string\",\"enum\":[\"a\",\"b\"]}", "\"c\"", &error) ==
          AH_INVALID);
    CHECK(ah_schema_validate("{\"enum\":[{\"x\":[1]}]}", "{\"x\":[1.0]}", &error) == AH_OK);
    CHECK(ah_schema_validate("{\"enum\":[9007199254740993]}", "9007199254740992.0", &error) ==
          AH_INVALID);
    CHECK(ah_schema_validate("{\"type\":\"null\",\"type\":\"string\"}", "null", &error) ==
          AH_INVALID);
    CHECK(ah_schema_validate("{\"type\":\"string\",\"minLength\":4,\"maxLength\":3}", "\"abc\"",
                             &error) == AH_INVALID);
    CHECK(ah_schema_validate("{\"type\":\"integer\",\"minimum\":18446744073709551615}",
                             "18446744073709551614", &error) == AH_INVALID);
    CHECK(ah_schema_validate("{}", "18446744073709551616", &error) == AH_INVALID);
    CHECK(ah_schema_validate("{}", "-9223372036854775809", &error) == AH_INVALID);
    char deep[160];
    memset(deep, '[', 65);
    deep[65] = '0';
    memset(deep + 66, ']', 65);
    deep[131] = 0;
    CHECK(ah_schema_validate("{}", deep, &error) == AH_INVALID);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--child"))
        return child(argc, argv);
    test_schema();
    char executable[PATH_MAX];
    CHECK(realpath(argv[0], executable));
    char workspace[] = "/tmp/ah-tools-XXXXXX";
    CHECK(mkdtemp(workspace));
    char canonical_workspace[PATH_MAX];
    CHECK(realpath(workspace, canonical_workspace));
    char outside[] = "/tmp/ah-tools-outside-XXXXXX";
    int outside_fd = mkstemp(outside);
    CHECK(outside_fd >= 0);
    CHECK(write(outside_fd, "outside", 7) == 7);
    CHECK(close(outside_fd) == 0);
    ah_error error;
    ah_local_tools *tools;
    ah_local_tools_config config = {.workspace = workspace, .approve_writes = true};
    CHECK(ah_local_tools_open(&config, &tools, &error) == AH_OK);
    CHECK(ah_local_tools_register(tools, (ah_engine *)(void *)&config, &error) == AH_OK);
    CHECK(tool_count == 4 && !lookup("exec") && lookup("write_file")->requires_approval);
    CHECK(lookup("read_file")->parallel_safe && lookup("list_directory")->parallel_safe);
    CHECK(!lookup("write_file")->parallel_safe && !lookup("edit_file")->parallel_safe);
    CHECK(lookup("edit_file")->requires_approval);
    test_file_pages(workspace);
    test_file_edits(workspace);
    tools_expect("write_file", "{\"path\":\"hello.txt\",\"content\":\"hello 世界\"}", AH_OK);
    yyjson_doc *doc =
        tools_invoke("read_file", "{\"path\":\"hello.txt\"}", 4096, 0, NULL, NULL, AH_OK, false);
    CHECK(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "content"), "hello 世界"));
    yyjson_doc_free(doc);
    tools_expect("read_file", "{\"path\":\"../escape\"}", AH_INVALID);
    tools_expect("read_file", "{\"path\":\"/etc/passwd\"}", AH_INVALID);
    tools_expect("write_file", "{\"path\":\"hello.txt\\u0000elsewhere\",\"content\":\"bad\"}",
                 AH_INVALID);
    tools_expect("write_file", "{\"path\":\"../escape\",\"content\":\"bad\"}", AH_INVALID);
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/link", workspace);
    CHECK(symlink(outside, path) == 0);
    tools_expect("read_file", "{\"path\":\"link\"}", AH_IO);
    tools_expect("write_file", "{\"path\":\"link\",\"content\":\"bad\"}", AH_INVALID);
    tools_expect("edit_file", "{\"path\":\"link\",\"old_text\":\"outside\",\"new_text\":\"bad\"}",
                 AH_IO);
    snprintf(path, sizeof(path), "%s/dirlink", workspace);
    CHECK(symlink("/tmp", path) == 0);
    tools_expect("read_file", "{\"path\":\"dirlink/file\"}", AH_INVALID);
    tools_expect("write_file", "{\"path\":\"dirlink/file\",\"content\":\"bad\"}", AH_INVALID);
    snprintf(path, sizeof(path), "%s/fifo", workspace);
    CHECK(mkfifo(path, 0600) == 0);
    uint64_t begin = ah_now_ms();
    tools_expect("read_file", "{\"path\":\"fifo\"}", AH_INVALID);
    CHECK(ah_now_ms() - begin < 500);
    tools_expect("write_file", "{\"path\":\"fifo\",\"content\":\"bad\"}", AH_INVALID);
    unsigned char binary[] = {0xff, 0x00};
    snprintf(path, sizeof(path), "%s/binary", workspace);
    tools_put_file(path, binary, sizeof(binary));
    tools_expect("read_file", "{\"path\":\"binary\"}", AH_INVALID);
    tools_expect("edit_file", "{\"path\":\"binary\",\"old_text\":\"x\",\"new_text\":\"y\"}",
                 AH_INVALID);
    char many[16384];
    memset(many, 'a', sizeof(many));
    snprintf(path, sizeof(path), "%s/many", workspace);
    tools_put_file(path, many, sizeof(many));
    doc = tools_invoke("read_file", "{\"path\":\"many\"}", 512, 0, NULL, NULL, AH_OK, false);
    CHECK(yyjson_get_bool(yyjson_obj_get(yyjson_doc_get_root(doc), "truncated")));
    yyjson_doc_free(doc);
    doc = tools_invoke("list_directory", "{\"path\":\".\"}", 128, 0, NULL, NULL, AH_OK, false);
    CHECK(yyjson_get_bool(yyjson_obj_get(yyjson_doc_get_root(doc), "truncated")));
    yyjson_doc_free(doc);
    doc = tools_invoke("write_file", "{\"path\":\"cancelled\",\"content\":\"bad\"}", 4096, 0,
                       cancel_now, NULL, AH_CANCELLED, false);
    yyjson_doc_free(doc);
    snprintf(path, sizeof(path), "%s/cancelled", workspace);
    CHECK(access(path, F_OK) != 0);
    unsigned cancel_checks = 0;
    doc = tools_invoke("write_file", "{\"path\":\"hello.txt\",\"content\":\"replacement\"}", 4096,
                       0, cancel_before_commit, &cancel_checks, AH_CANCELLED, false);
    yyjson_doc_free(doc);
    doc = tools_invoke("read_file", "{\"path\":\"hello.txt\"}", 4096, 0, NULL, NULL, AH_OK, false);
    CHECK(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "content"), "hello 世界"));
    yyjson_doc_free(doc);
    DIR *no_temporary = opendir(workspace);
    CHECK(no_temporary);
    struct dirent *entry;
    while ((entry = readdir(no_temporary)))
        CHECK(strncmp(entry->d_name, ".ah-write-", 10));
    closedir(no_temporary);
    ah_local_tools_close(tools);
    config.enable_exec = true;
    config.approve_writes = false;
    tool_count = 0;
    CHECK(ah_local_tools_open(&config, &tools, &error) == AH_OK);
    CHECK(ah_local_tools_register(tools, (ah_engine *)(void *)&config, &error) == AH_OK);
    CHECK(tool_count == 5 && lookup("exec")->requires_approval &&
          !lookup("write_file")->requires_approval && !lookup("edit_file")->requires_approval);
    const char *literal = "space $HOME ; $(touch forbidden) * ' \"";
    char *args = exec_arguments(executable, "args", literal);
    doc = tools_invoke("exec", args, 4096, ah_now_ms() + 5000, NULL, NULL, AH_OK, false);
    free(args);
    CHECK(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "stdout"), literal));
    yyjson_doc_free(doc);
    CHECK(setenv("AH_TEST_SECRET", "must-not-leak", 1) == 0);
    int inherited = open(outside, O_RDONLY);
    CHECK(inherited >= 0);
    char fd_number[32];
    snprintf(fd_number, sizeof(fd_number), "%d", inherited);
    args = exec_arguments(executable, "environment", fd_number);
    doc = tools_invoke("exec", args, 4096, ah_now_ms() + 5000, NULL, NULL, AH_OK, false);
    free(args);
    close(inherited);
    const char *environment = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "stdout"));
    CHECK(environment && strstr(environment, "secret=0;fd=0;stdin=0;cwd="));
    CHECK(strstr(environment, canonical_workspace));
    yyjson_doc_free(doc);
    unsetenv("AH_TEST_SECRET");
    args = exec_arguments(executable, "flood", NULL);
    doc = tools_invoke("exec", args, 1024, ah_now_ms() + 5000, NULL, NULL, AH_OK, false);
    free(args);
    CHECK(yyjson_get_bool(yyjson_obj_get(yyjson_doc_get_root(doc), "truncated")));
    CHECK(yyjson_get_int(yyjson_obj_get(yyjson_doc_get_root(doc), "exit_code")) == 0);
    yyjson_doc_free(doc);
    args = exec_arguments(executable, "binary", NULL);
    doc = tools_invoke("exec", args, 1024, ah_now_ms() + 5000, NULL, NULL, AH_OK, false);
    free(args);
    CHECK(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "stdout_encoding"), "base64"));
    CHECK(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "stdout"), "/wCA"));
    yyjson_doc_free(doc);
    args = exec_arguments(executable, "hang", NULL);
    begin = ah_now_ms();
    doc = tools_invoke("exec", args, 1024, begin + 100, NULL, NULL, AH_LIMIT, true);
    yyjson_doc_free(doc);
    CHECK(ah_now_ms() - begin < 2000);
    uint64_t cancel_at = ah_now_ms() + 100;
    doc = tools_invoke("exec", args, 1024, ah_now_ms() + 5000, cancel_after, &cancel_at,
                       AH_CANCELLED, true);
    yyjson_doc_free(doc);
    free(args);
    args = exec_arguments(executable, "descendant", NULL);
    doc = tools_invoke("exec", args, 1024, ah_now_ms() + 100, NULL, NULL, AH_LIMIT, true);
    yyjson_doc_free(doc);
    free(args);
    args = exec_arguments(executable, "closed_descendant", NULL);
    begin = ah_now_ms();
    doc = tools_invoke("exec", args, 1024, begin + 100, NULL, NULL, AH_LIMIT, true);
    free(args);
    pid_t descendant =
        (pid_t)atoi(yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "stdout")));
    CHECK(descendant > 0);
    /* The full TERM grace must elapse even after the leader exits and pipes close. */
    CHECK(ah_now_ms() - begin >= 280);
    kill(descendant, SIGKILL); /* Harmless if already reaped; prevents leftovers on regression. */
    yyjson_doc_free(doc);
    tools_expect("exec", "{\"argv\":[\"/bin/echo\\u0000bad\"]}", AH_INVALID);
    tools_expect("exec", "{\"argv\":\"echo unsafe\"}", AH_INVALID);
    ah_local_tools_close(tools);
    outside_fd = open(outside, O_RDONLY);
    CHECK(outside_fd >= 0);
    char check[8] = {0};
    CHECK(read(outside_fd, check, 7) == 7);
    close(outside_fd);
    CHECK(!strcmp(check, "outside"));
    CHECK(unlink(outside) == 0);
    clear_directory(workspace);
    puts("local tools and schema tests passed");
    return 0;
}
