#ifndef AH_TOOLS_TEST_H
#define AH_TOOLS_TEST_H
#include "ah/tools.h"
#include "yyjson.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
yyjson_doc *tools_invoke(const char *name, const char *args, size_t limit, uint64_t deadline,
                         ah_cancel_check cancelled, void *cancel_context, ah_code expected,
                         bool unknown);
void tools_expect(const char *name, const char *args, ah_code code);
void tools_put_file(const char *path, const void *data, size_t length);
void test_file_pages(const char *workspace);
void test_file_edits(const char *workspace);
#endif
