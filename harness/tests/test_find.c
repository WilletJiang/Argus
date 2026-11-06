#include "workflow_fixture.h"
#include <stdio.h>

/* Independent line-mask oracle: collect every line, mark context around all
 * matches, then emit contiguous marked runs. Production uses a six-entry ring. */
static void compare(workflow *w, const char *text, const char *query, unsigned context) {
    wf_put(w, "find.txt", text);
    size_t starts[512], ends[512], count = 0, start = 0, length = strlen(text);
    bool selected[512] = {0};
    for (size_t i = 0; i < length; i++) {
        if (text[i] == '\n') {
            starts[count] = start;
            ends[count++] = i + 1;
            start = i + 1;
        }
        assert(count < 512);
    }
    if (start < length) {
        starts[count] = start;
        ends[count++] = length;
    }
    size_t needle = strlen(query);
    for (size_t i = 0; i < count; i++) {
        bool hit = false;
        for (size_t j = starts[i]; j + needle <= ends[i]; j++)
            if (!memcmp(text + j, query, needle)) {
                hit = true;
                break;
            }
        if (!hit)
            continue;
        size_t from = i > context ? i - context : 0,
               to = i + context < count ? i + context : count - 1;
        for (size_t j = from; j <= to; j++)
            selected[j] = true;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *args = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, args);
    yyjson_mut_obj_add_str(doc, args, "path", "find.txt");
    yyjson_mut_obj_add_str(doc, args, "query", query);
    yyjson_mut_obj_add_uint(doc, args, "context_lines", context);
    yyjson_mut_obj_add_uint(doc, args, "max_results", 100);
    char *json = yyjson_mut_write(doc, 0, NULL);
    assert(json);
    yyjson_mut_doc_free(doc);
    wf_result result = wf_call(w, ah_local_find, json, 0);
    free(json);
    assert(result.code == AH_OK && !result.failed);
    yyjson_val *rows = yyjson_obj_get(result.root, "results");
    size_t groups = 0;
    for (size_t i = 0; i < count; i++) {
        if (!selected[i])
            continue;
        size_t first = i;
        while (i + 1 < count && selected[i + 1])
            i++;
        yyjson_val *row = yyjson_arr_get(rows, groups++), *content = yyjson_obj_get(row, "content");
        assert(row && yyjson_get_uint(yyjson_obj_get(row, "start_line")) == first + 1);
        assert(yyjson_get_uint(yyjson_obj_get(row, "end_line")) == i + 1);
        assert(yyjson_get_uint(yyjson_obj_get(row, "offset_bytes")) == starts[first]);
        size_t bytes = ends[i] - starts[first];
        assert(yyjson_get_len(content) == bytes &&
               !memcmp(yyjson_get_str(content), text + starts[first], bytes));
        assert(!yyjson_get_bool(yyjson_obj_get(row, "truncated")));
    }
    assert(yyjson_arr_size(rows) == groups);
    assert(yyjson_get_uint(yyjson_obj_get(result.root, "total_excerpts")) == groups);
    assert(!yyjson_get_bool(yyjson_obj_get(result.root, "truncated")));
    wf_result_free(&result);
}

static uint32_t random_step(uint32_t *seed) {
    *seed = *seed * 1664525u + 1013904223u;
    return *seed;
}
static void utf8_boundaries(void) {
    const struct {
        const char *bytes;
        size_t size, complete;
        bool valid;
    } cases[] = {{"\xc2\xa2", 2, 2, true},
                 {"\xe2\x82\xac", 3, 3, true},
                 {"\xf0\x9f\x99\x82", 4, 4, true},
                 {"\xc2", 1, 0, true},
                 {"\xe2\x82", 2, 0, true},
                 {"\xf0\x9f\x99", 3, 0, true},
                 {"\x80", 1, 0, false},
                 {"\xc0\x80", 2, 0, false},
                 {"\xed\xa0\x80", 3, 0, false},
                 {"\xf4\x90\x80\x80", 4, 0, false},
                 {"\xff", 1, 0, false}};
    for (size_t prefix = 0; prefix < 33; prefix++)
        for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
            char storage[64];
            char *text = storage + 1; /* deliberately unaligned */
            memset(text, 'x', prefix);
            memcpy(text + prefix, cases[i].bytes, cases[i].size);
            size_t complete = 0;
            assert(ah_local_utf8(text, prefix + cases[i].size, &complete) == cases[i].valid);
            assert(complete == prefix + cases[i].complete);
        }
}
int main(int argc, char **argv) {
    (void)argc;
    utf8_boundaries();
    workflow w;
    wf_open(&w, argv[0]);
    const char *edge[] = {
        "", "\n", "aa", "aa\n", "\naa\n", "aaa\n\n\n\naaaa", "a\na\na\n\nb\na\n", "雪🙂\r\n🙂雪"};
    for (unsigned c = 0; c <= 5; c++)
        for (size_t i = 0; i < sizeof(edge) / sizeof(*edge); i++)
            compare(&w, edge[i], "a", c);
    uint32_t seed = 92251;
    const char *chunks[] = {"a", "b", "ab", "needle", "雪", "🙂", " ", "\"", "\\", "\r", "\t"};
    const char *queries[] = {"a", "aa", "ab", "needle", "雪", "🙂", "\"", "missing"};
    for (unsigned trial = 0; trial < 160; trial++) {
        char text[16384];
        size_t at = 0;
        unsigned lines = random_step(&seed) % 80;
        for (unsigned i = 0; i < lines; i++) {
            unsigned parts = random_step(&seed) % 10;
            for (unsigned j = 0; j < parts; j++) {
                const char *chunk = chunks[random_step(&seed) % (sizeof(chunks) / sizeof(*chunks))];
                size_t n = strlen(chunk);
                assert(at + n + 2 < sizeof(text));
                memcpy(text + at, chunk, n);
                at += n;
            }
            if (i + 1 < lines || random_step(&seed) % 2)
                text[at++] = '\n';
        }
        text[at] = 0;
        const char *query = queries[random_step(&seed) % (sizeof(queries) / sizeof(*queries))];
        compare(&w, text, query, random_step(&seed) % 6);
    }
    char *dense = malloc(1024 * 1024 + 1);
    assert(dense);
    memset(dense, 'a', 1024 * 1024);
    dense[1024 * 1024] = 0;
    wf_put(&w, "dense.txt", dense);
    free(dense);
    wf_result result = wf_call(&w, ah_local_find, "{\"path\":\"dense.txt\",\"query\":\"aaa\"}", 0);
    assert(!result.failed && yyjson_get_uint(yyjson_obj_get(result.root, "total_excerpts")) == 1);
    yyjson_val *row = yyjson_arr_get(yyjson_obj_get(result.root, "results"), 0);
    assert(yyjson_get_uint(yyjson_obj_get(row, "start_line")) == 1);
    assert(yyjson_get_uint(yyjson_obj_get(row, "end_line")) == 1);
    assert(yyjson_is_true(yyjson_obj_get(row, "truncated")));
    assert(yyjson_get_uint(yyjson_obj_get(row, "next_offset_bytes")) > 0);
    wf_result_free(&result);
    wf_close(&w);
    puts("find: independent context oracle and dense overlapping matches passed");
    return 0;
}
