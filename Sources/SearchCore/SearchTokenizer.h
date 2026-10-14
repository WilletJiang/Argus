#ifndef ARGUS_SEARCH_TOKENIZER_H
#define ARGUS_SEARCH_TOKENIZER_H

#include <sqlite3.h>
#include <stdint.h>

/* Internal helpers. Folded strings are malloc-owned by the caller. */
char *argus_fold_text(const char *text);
uint32_t argus_codepoint(const char *text, int bytes, int *width);
int argus_token_character(uint32_t point);
int argus_cjk_character(uint32_t point);
int argus_register_tokenizers(sqlite3 *database);
int argus_tokenize_body(const char *text, int bytes, void *context,
                       int (*emit)(void *, int, const char *, int, int, int));
int argus_register_offsets(sqlite3 *database);
int argus_excerpt_offset(const void *offsets, int bytes, int position);
void argus_excerpt_text(char out[512], const char *text, int bytes, int byte_offset);

#endif
