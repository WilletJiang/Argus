#ifndef AH_INTERNAL_H
#define AH_INTERNAL_H
#include "ah/ah.h"
#include "yyjson.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
ah_code ah_fail(ah_error *error, ah_code code, const char *format, ...);
char *ah_strdup(const char *value);
char *ah_json_write(yyjson_val *value);
/* Mutable documents track allocation failure. Once poisoned, serialization
 * fails even if earlier fields still form valid JSON. Strings remain malloc-owned.
 * The budget counts cumulative successful allocation/growth bytes in yyjson,
 * including document storage, pools and writer buffers; frees do not refund it. */
yyjson_mut_doc *ah_mut_doc_new(void);
yyjson_mut_doc *ah_mut_doc_new_budget(size_t bytes);
void ah_mut_doc_free(yyjson_mut_doc *doc);
char *ah_mut_write(yyjson_mut_doc *doc);
bool ah_json_valid(const char *json);
ah_code ah_json_check(yyjson_val *value, ah_error *error);
ah_code ah_schema_check(const char *schema, ah_error *error);
ah_code ah_schema_parse(const char *schema, const char *value, yyjson_doc **parsed,
                        ah_error *error);
void ah_error_clear(ah_error *error);
uint64_t ah_now_us(void);
#endif
