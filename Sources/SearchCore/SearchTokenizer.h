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

#endif
