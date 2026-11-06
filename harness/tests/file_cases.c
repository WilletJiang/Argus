#include "tools_test.h"
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    const char *path;
    const char *workspace;
    bool changed;
} concurrent_edit;
static bool change_before_commit(void *context) {
    concurrent_edit *change = context;
    DIR *dir = opendir(change->workspace);
    CHECK(dir);
    struct dirent *entry;
    bool prepared = false;
    while ((entry = readdir(dir)))
        if (!strncmp(entry->d_name, ".ah-write-", 10))
            prepared = true;
    closedir(dir);
    if (prepared && !change->changed) {
        change->changed = true;
        tools_put_file(change->path, "external", 8);
    }
    return false;
}

void test_file_pages(const char *workspace) {
    tools_expect("write_file", "{\"path\":\"pages\",\"content\":\"first\\n世界\\nlast\"}", AH_OK);
    yyjson_doc *doc =
        tools_invoke("read_file", "{\"path\":\"pages\",\"offset_line\":2,\"max_lines\":1}", 4096, 0,
                     NULL, NULL, AH_OK, false);
    yyjson_val *root = yyjson_doc_get_root(doc);
    CHECK(yyjson_equals_str(yyjson_obj_get(root, "content"), "世界\n"));
    CHECK(yyjson_equals_str(yyjson_obj_get(root, "revision"),
                            "47dfffbfb0b250ac3f8427075e09f2f6346b7182103c38c9ed764ffb1f2a6da4"));
    CHECK(yyjson_equals_str(yyjson_obj_get(root, "path"), "pages"));
    CHECK(yyjson_get_uint(yyjson_obj_get(root, "start_line")) == 2);
    CHECK(yyjson_get_uint(yyjson_obj_get(root, "next_line")) == 3);
    CHECK(yyjson_get_uint(yyjson_obj_get(root, "offset_bytes")) == 6);
    CHECK(yyjson_get_uint(yyjson_obj_get(root, "next_offset_bytes")) == 13);
    CHECK(yyjson_get_uint(yyjson_obj_get(root, "lines")) == 1);
    CHECK(yyjson_get_bool(yyjson_obj_get(root, "truncated")));
    CHECK(!yyjson_get_bool(yyjson_obj_get(root, "partial_line")));
    yyjson_doc_free(doc);
    doc = tools_invoke("read_file", "{\"path\":\"pages\",\"offset_line\":3.0,\"max_lines\":1.0}",
                       4096, 0, NULL, NULL, AH_OK, false);
    root = yyjson_doc_get_root(doc);
    CHECK(yyjson_equals_str(yyjson_obj_get(root, "content"), "last"));
    CHECK(yyjson_get_uint(yyjson_obj_get(root, "lines")) == 1);
    CHECK(yyjson_is_null(yyjson_obj_get(root, "next_line")));
    CHECK(yyjson_is_null(yyjson_obj_get(root, "next_offset_bytes")));
    CHECK(!yyjson_get_bool(yyjson_obj_get(root, "truncated")));
    yyjson_doc_free(doc);
    tools_expect("read_file", "{\"path\":\"pages\",\"offset_line\":0}", AH_INVALID);
    tools_expect("read_file", "{\"path\":\"pages\",\"max_lines\":1001}", AH_INVALID);
    tools_expect("read_file", "{\"path\":\"pages\",\"offset_line\":2,\"offset_bytes\":6}",
                 AH_INVALID);
    tools_expect("read_file", "{\"path\":\"pages\",\"offset_bytes\":7}", AH_INVALID);
    tools_expect("write_file", "{\"path\":\"empty\",\"content\":\"\"}", AH_OK);
    const char *empty_args[] = {"{\"path\":\"empty\"}", "{\"path\":\"pages\",\"offset_line\":99}"};
    for (size_t i = 0; i < 2; ++i) {
        doc = tools_invoke("read_file", empty_args[i], 4096, 0, NULL, NULL, AH_OK, false);
        root = yyjson_doc_get_root(doc);
        CHECK(yyjson_equals_str(yyjson_obj_get(root, "content"), ""));
        CHECK(yyjson_get_uint(yyjson_obj_get(root, "lines")) == 0);
        CHECK(yyjson_is_null(yyjson_obj_get(root, "next_offset_bytes")));
        yyjson_doc_free(doc);
    }
    char path[PATH_MAX], rows[402];
    for (size_t i = 0; i < sizeof(rows); i += 2)
        memcpy(rows + i, "x\n", 2);
    snprintf(path, sizeof(path), "%s/pages", workspace);
    tools_put_file(path, rows, sizeof(rows));
    doc = tools_invoke("read_file", "{\"path\":\"pages\"}", 0, 0, NULL, NULL, AH_OK, false);
    root = yyjson_doc_get_root(doc);
    CHECK(yyjson_get_uint(yyjson_obj_get(root, "lines")) == 200);
    CHECK(yyjson_get_uint(yyjson_obj_get(root, "bytes")) == 400);
    CHECK(yyjson_get_uint(yyjson_obj_get(root, "next_line")) == 201);
    yyjson_doc_free(doc);
    char *long_line = malloc(75004);
    CHECK(long_line);
    for (size_t i = 0; i < 75000; i += 3)
        memcpy(long_line + i, "世", 3);
    memcpy(long_line + 75000, "end\n", 4);
    tools_put_file(path, long_line, 75004);
    size_t copied = 0, pages = 0;
    do {
        char args[128];
        snprintf(args, sizeof(args), "{\"path\":\"pages\",\"offset_bytes\":%zu}", copied);
        doc = tools_invoke("read_file", args, 1024 * 1024, 0, NULL, NULL, AH_OK, false);
        root = yyjson_doc_get_root(doc);
        yyjson_val *content = yyjson_obj_get(root, "content");
        size_t length = yyjson_get_len(content);
        CHECK(length && copied + length <= 75004);
        CHECK(!memcmp(yyjson_get_str(content), long_line + copied, length));
        CHECK(yyjson_is_null(yyjson_obj_get(root, "start_line")));
        copied += length;
        if (copied < 75004) {
            CHECK(yyjson_get_uint(yyjson_obj_get(root, "next_offset_bytes")) == copied);
            CHECK(yyjson_get_bool(yyjson_obj_get(root, "partial_line")));
        } else
            CHECK(yyjson_is_null(yyjson_obj_get(root, "next_offset_bytes")));
        yyjson_doc_free(doc);
        ++pages;
    } while (copied < 75004);
    CHECK(pages == 2);
    long_line[65535] = '\n';
    long_line[65536] = 'x';
    for (size_t size = 65536; size <= 65537; ++size) {
        tools_put_file(path, long_line, size);
        doc = tools_invoke("read_file", "{\"path\":\"pages\",\"max_lines\":1000}", 1024 * 1024, 0,
                           NULL, NULL, AH_OK, false);
        root = yyjson_doc_get_root(doc);
        CHECK(yyjson_get_uint(yyjson_obj_get(root, "bytes")) == 65536);
        CHECK(yyjson_get_bool(yyjson_obj_get(root, "truncated")) == (size > 65536));
        if (size == 65536)
            CHECK(yyjson_is_null(yyjson_obj_get(root, "next_offset_bytes")));
        else
            CHECK(yyjson_get_uint(yyjson_obj_get(root, "next_offset_bytes")) == 65536);
        yyjson_doc_free(doc);
    }
    doc = tools_invoke("read_file", "{\"path\":\"pages\",\"offset_bytes\":65536}", 4096, 0, NULL,
                       NULL, AH_OK, false);
    CHECK(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "content"), "x"));
    yyjson_doc_free(doc);
    free(long_line);
    const char escaped[] = "\001\"\\\t\n";
    tools_put_file(path, escaped, sizeof(escaped) - 1);
    doc = tools_invoke("read_file", "{\"path\":\"pages\"}", 512, 0, NULL, NULL, AH_OK, false);
    CHECK(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "content"), escaped));
    yyjson_doc_free(doc);
    char controls[4096];
    memset(controls, '\001', sizeof(controls));
    tools_put_file(path, controls, sizeof(controls));
    copied = 0;
    do {
        char args[128];
        snprintf(args, sizeof(args), "{\"path\":\"pages\",\"offset_bytes\":%zu}", copied);
        doc = tools_invoke("read_file", args, 512, 0, NULL, NULL, AH_OK, false);
        root = yyjson_doc_get_root(doc);
        yyjson_val *content = yyjson_obj_get(root, "content");
        size_t length = yyjson_get_len(content);
        CHECK(length && copied + length <= sizeof(controls));
        CHECK(!memcmp(yyjson_get_str(content), controls + copied, length));
        copied += length;
        if (copied < sizeof(controls))
            CHECK(yyjson_get_uint(yyjson_obj_get(root, "next_offset_bytes")) == copied);
        else
            CHECK(yyjson_is_null(yyjson_obj_get(root, "next_offset_bytes")));
        yyjson_doc_free(doc);
    } while (copied < sizeof(controls));
}

void test_file_edits(const char *workspace) {
    tools_expect("write_file", "{\"path\":\"edit\",\"content\":\"prefix 世界 suffix\"}", AH_OK);
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/edit", workspace);
    CHECK(chmod(path, 0751) == 0);
    tools_expect("apply_changes",
                 "{\"files\":[{\"path\":\"edit\",\"edits\":[{\"old_text\":\"世界\",\"new_text\":"
                 "\"你好\"}]}]}",
                 AH_OK);
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 07777) == 0751);
    yyjson_doc *doc =
        tools_invoke("read_file", "{\"path\":\"edit\"}", 4096, 0, NULL, NULL, AH_OK, false);
    CHECK(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "content"),
                            "prefix 你好 suffix"));
    yyjson_doc_free(doc);
    tools_expect("apply_changes",
                 "{\"files\":[{\"path\":\"edit\",\"edits\":[{\"old_text\":\"世界\",\"new_text\":"
                 "\"bad\"}]}]}",
                 AH_INVALID);
    tools_expect(
        "apply_changes",
        "{\"files\":[{\"path\":\"edit\",\"edits\":[{\"old_text\":\"\",\"new_text\":\"bad\"}]}]}",
        AH_INVALID);
    tools_expect("apply_changes",
                 "{\"files\":[{\"path\":\"edit\",\"edits\":[{\"old_text\":\"你好\",\"new_text\":"
                 "\"\\u0000\"}]}]}",
                 AH_INVALID);
    tools_expect("write_file", "{\"path\":\"edit\",\"content\":\"aaa\"}", AH_OK);
    tools_expect(
        "apply_changes",
        "{\"files\":[{\"path\":\"edit\",\"edits\":[{\"old_text\":\"aa\",\"new_text\":\"b\"}]}]}",
        AH_INVALID);
    tools_expect(
        "apply_changes",
        "{\"files\":[{\"path\":\"edit\",\"edits\":[{\"old_text\":\"aaa\",\"new_text\":\"\"}]}]}",
        AH_OK);
    CHECK(stat(path, &st) == 0 && st.st_size == 0 && (st.st_mode & 07777) == 0751);
    tools_expect(
        "apply_changes",
        "{\"files\":[{\"path\":\"missing\",\"edits\":[{\"old_text\":\"x\",\"new_text\":\"y\"}]}]}",
        AH_IO);
    tools_expect("apply_changes",
                 "{\"files\":[{\"path\":\"../"
                 "escape\",\"edits\":[{\"old_text\":\"x\",\"new_text\":\"y\"}]}]}",
                 AH_INVALID);
    tools_put_file(path, "source", 6);
    concurrent_edit change = {.path = path, .workspace = workspace};
    doc = tools_invoke("apply_changes",
                       "{\"files\":[{\"path\":\"edit\",\"edits\":[{\"old_text\":\"source\",\"new_"
                       "text\":\"edited\"}]}]}",
                       4096, 0, change_before_commit, &change, AH_INVALID, false);
    yyjson_doc_free(doc);
    doc = tools_invoke("read_file", "{\"path\":\"edit\"}", 4096, 0, NULL, NULL, AH_OK, false);
    CHECK(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "content"), "external"));
    yyjson_doc_free(doc);
    char *ambiguous = malloc(100000);
    CHECK(ambiguous);
    memset(ambiguous, 'a', 100000);
    memcpy(ambiguous, "needle", 6);
    memcpy(ambiguous + 99994, "needle", 6);
    tools_put_file(path, ambiguous, 100000);
    tools_expect("apply_changes",
                 "{\"files\":[{\"path\":\"edit\",\"edits\":[{\"old_text\":\"needle\",\"new_text\":"
                 "\"x\"}]}]}",
                 AH_INVALID);
    /* The unified tool retains the former single-edit range above 64 KiB. */
    yyjson_mut_doc *builder = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(builder);
    yyjson_mut_doc_set_root(builder, root);
    yyjson_mut_val *files = yyjson_mut_obj_add_arr(builder, root, "files"),
                   *file = yyjson_mut_obj(builder);
    yyjson_mut_arr_append(files, file);
    yyjson_mut_obj_add_str(builder, file, "path", "edit");
    yyjson_mut_val *edits = yyjson_mut_obj_add_arr(builder, file, "edits"),
                   *edit = yyjson_mut_obj(builder);
    yyjson_mut_arr_append(edits, edit);
    yyjson_mut_obj_add_strn(builder, edit, "old_text", ambiguous, 100000);
    yyjson_mut_obj_add_str(builder, edit, "new_text", "small");
    char *arguments = yyjson_mut_write(builder, 0, NULL);
    CHECK(arguments);
    yyjson_mut_doc_free(builder);
    free(ambiguous);
    tools_expect("apply_changes", arguments, AH_OK);
    free(arguments);
    CHECK(stat(path, &st) == 0 && st.st_size == 5);
    int fd = open(path, O_WRONLY);
    CHECK(fd >= 0 && ftruncate(fd, 1024 * 1024 + 1) == 0 && close(fd) == 0);
    tools_expect(
        "apply_changes",
        "{\"files\":[{\"path\":\"edit\",\"edits\":[{\"old_text\":\"x\",\"new_text\":\"y\"}]}]}",
        AH_LIMIT);
}
