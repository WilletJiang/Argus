#include "SearchTokenizer.h"

#include <CoreFoundation/CoreFoundation.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

uint32_t argus_codepoint(const char *text, int bytes, int *width) {
    unsigned char c = (unsigned char)text[0];
    if (c < 0x80) { *width = 1; return c; }
    int n = c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    if (c < 0xc2 || c > 0xf4 || n > bytes) { *width = 1; return 0xfffd; }
    uint32_t point = c & (0x7f >> n);
    for (int i = 1; i < n; ++i) {
        if (((unsigned char)text[i] & 0xc0) != 0x80) { *width = 1; return 0xfffd; }
        point = (point << 6) | ((unsigned char)text[i] & 0x3f);
    }
    if ((n == 2 && point < 0x80) || (n == 3 && point < 0x800) ||
        (n == 4 && point < 0x10000) || point > 0x10ffff ||
        (point >= 0xd800 && point <= 0xdfff)) { *width = 1; return 0xfffd; }
    *width = n;
    return point;
}

int argus_token_character(uint32_t point) {
    if (point < 128) return isalnum((unsigned char)point) || point == '_';
    if (point == 0xfffd) return 0;
    return CFCharacterSetIsLongCharacterMember(
        CFCharacterSetGetPredefined(kCFCharacterSetAlphaNumeric), point);
}

int argus_cjk_character(uint32_t point) {
    return (point >= 0x3400 && point <= 0x9fff) ||
           (point >= 0x20000 && point <= 0x323af) ||
           (point >= 0x3040 && point <= 0x30ff) ||
           (point >= 0xac00 && point <= 0xd7af);
}

char *argus_fold_text(const char *text) {
    if (!text) text = "";
    size_t bytes = strlen(text);
    int ascii = 1;
    for (size_t i = 0; i < bytes; ++i) if ((unsigned char)text[i] >= 128) { ascii = 0; break; }
    if (ascii) {
        char *out = malloc(bytes + 1);
        if (out) for (size_t i = 0; i <= bytes; ++i) out[i] = (char)tolower((unsigned char)text[i]);
        return out;
    }
    CFStringRef source = CFStringCreateWithBytes(NULL, (const UInt8 *)text, (CFIndex)bytes,
                                               kCFStringEncodingUTF8, false);
    if (!source) return NULL;
    CFMutableStringRef folded = CFStringCreateMutableCopy(NULL, 0, source);
    CFRelease(source);
    if (!folded) return NULL;
    CFStringFold(folded, kCFCompareCaseInsensitive | kCFCompareDiacriticInsensitive |
                         kCFCompareWidthInsensitive, NULL);
    CFIndex capacity = CFStringGetMaximumSizeForEncoding(CFStringGetLength(folded),
                                                        kCFStringEncodingUTF8) + 1;
    char *out = malloc((size_t)capacity);
    if (out && !CFStringGetCString(folded, out, capacity, kCFStringEncodingUTF8)) {
        free(out); out = NULL;
    }
    CFRelease(folded);
    return out;
}

/* Metadata stores words plus overlapping two-character grams. Full text stores
   words plus CJK grams only, keeping English documents compact. Query tokens are
   already folded and namespaced, so FTS syntax never comes from user input. */
static int tokenizer_create(void *context, const char **args, int count, Fts5Tokenizer **out) {
    (void)args; (void)count;
    int *mode = malloc(sizeof(*mode));
    if (!mode) return SQLITE_NOMEM;
    *mode = context != NULL;
    *out = (Fts5Tokenizer *)mode;
    return SQLITE_OK;
}

static void tokenizer_delete(Fts5Tokenizer *tokenizer) { free(tokenizer); }

static int tokenize(Fts5Tokenizer *tokenizer, void *context, int flags,
                    const char *text, int bytes,
                    int (*emit)(void *, int, const char *, int, int, int)) {
    if (flags & FTS5_TOKENIZE_QUERY) return emit(context, 0, text, bytes, 0, bytes);
    int body = *(int *)tokenizer;
    int offset = 0;
    while (offset < bytes) {
        int width;
        uint32_t point = argus_codepoint(text + offset, bytes - offset, &width);
        if (!argus_token_character(point)) { offset += width; continue; }
        int start = offset;
        int has_cjk = 0, has_nonascii = 0;
        while (offset < bytes) {
            point = argus_codepoint(text + offset, bytes - offset, &width);
            if (!argus_token_character(point)) break;
            has_cjk |= argus_cjk_character(point);
            has_nonascii |= point >= 128;
            offset += width;
        }
        int length = offset - start;
        char *folded_heap = NULL;
        const char *folded = text + start;
        int folded_length = length;
        if (body && has_nonascii) {
            char word_stack[1024];
            char *word = length < (int)sizeof(word_stack) ? word_stack : malloc((size_t)length + 1);
            if (!word) return SQLITE_NOMEM;
            memcpy(word, text + start, (size_t)length); word[length] = 0;
            folded_heap = argus_fold_text(word);
            if (word != word_stack) free(word);
            if (!folded_heap) return SQLITE_NOMEM;
            folded = folded_heap;
            folded_length = (int)strlen(folded);
        }
        if (body && has_nonascii && !has_cjk) {
            int current = 0;
            while (current < folded_length) {
                int character_width;
                uint32_t folded_point = argus_codepoint(folded + current, folded_length - current, &character_width);
                has_cjk |= argus_cjk_character(folded_point);
                current += character_width;
            }
        }
        char token_stack[1024];
        char *token = folded_length + 2 <= (int)sizeof(token_stack) ? token_stack : malloc((size_t)folded_length + 2);
        if (!token) { free(folded_heap); return SQLITE_NOMEM; }
        token[0] = 'w';
        if (body && !has_nonascii) {
            for (int i = 0; i < length; ++i) token[i + 1] = (char)tolower((unsigned char)folded[i]);
        } else memcpy(token + 1, folded, (size_t)folded_length);
        int result = emit(context, 0, token, folded_length + 1, start, offset);
        if (result == SQLITE_OK && has_cjk) {
            int current = 0;
            while (current < folded_length) {
                int width;
                uint32_t point = argus_codepoint(folded + current, folded_length - current, &width);
                if (argus_cjk_character(point)) {
                    token[0] = 'u'; memcpy(token + 1, folded + current, (size_t)width);
                    result = emit(context, FTS5_TOKEN_COLOCATED, token, width + 1, start, offset);
                    if (result != SQLITE_OK) break;
                }
                current += width;
            }
        }
        if (result == SQLITE_OK && (!body || has_cjk)) {
            int current = 0;
            while (current < folded_length) {
                int first, second;
                argus_codepoint(folded + current, folded_length - current, &first);
                int next = current + first;
                if (next >= folded_length) break;
                argus_codepoint(folded + next, folded_length - next, &second);
                token[0] = 'b';
                memcpy(token + 1, folded + current, (size_t)(first + second));
                result = emit(context, FTS5_TOKEN_COLOCATED, token, first + second + 1,
                              start, offset);
                if (result != SQLITE_OK) break;
                current = next;
            }
        }
        if (token != token_stack) free(token);
        free(folded_heap);
        if (result != SQLITE_OK) return result;
    }
    return SQLITE_OK;
}

typedef struct {
    int target;
    int position;
    int byte_offset;
} ExcerptPosition;

static int excerpt_token(void *context, int flags, const char *token, int bytes, int start, int end) {
    (void)token; (void)bytes; (void)end;
    ExcerptPosition *position = context;
    if (!(flags & FTS5_TOKEN_COLOCATED)) ++position->position;
    if (position->position != position->target) return SQLITE_OK;
    position->byte_offset = start;
    return SQLITE_DONE;
}

/* SQLite's general snippet scorer considers many windows when a term repeats.
   One indexed match gives a useful excerpt with linear, bounded tokenization. */
static void excerpt(const Fts5ExtensionApi *api, Fts5Context *fts, sqlite3_context *result,
                    int count, sqlite3_value **values) {
    (void)count; (void)values;
    const char *text;
    int bytes, column, word;
    Fts5PhraseIter iterator;
    int status = api->xColumnText(fts, 0, &text, &bytes);
    if (status == SQLITE_OK) status = api->xPhraseFirst(fts, 0, &iterator, &column, &word);
    if (status != SQLITE_OK) { sqlite3_result_error_code(result, status); return; }
    ExcerptPosition position = {column < 0 ? 0 : word, -1, 0};
    status = api->xTokenize(fts, text, bytes, &position, excerpt_token);
    if (status != SQLITE_OK && status != SQLITE_DONE) {
        sqlite3_result_error_code(result, status); return;
    }
    int start = position.byte_offset > 80 ? position.byte_offset - 80 : 0;
    while (start && ((unsigned char)text[start] & 0xc0) == 0x80) --start;
    char output[512];
    int written = 0;
    if (start) { memcpy(output, "…", 3); written = 3; }
    int length = bytes - start;
    int available = (int)sizeof(output) - written - 4;
    if (length > available) length = available;
    while (length && start + length < bytes &&
           ((unsigned char)text[start + length] & 0xc0) == 0x80) --length;
    memcpy(output + written, text + start, (size_t)length); written += length;
    if (start + length < bytes) { memcpy(output + written, "…", 3); written += 3; }
    output[written] = 0;
    sqlite3_result_text(result, output, written, SQLITE_TRANSIENT);
}

int argus_register_tokenizers(sqlite3 *database) {
    fts5_api *api = NULL;
    sqlite3_stmt *statement = NULL;
    int result = sqlite3_prepare_v2(database, "SELECT fts5(?1)", -1, &statement, NULL);
    if (result != SQLITE_OK) return result;
    sqlite3_bind_pointer(statement, 1, &api, "fts5_api_ptr", NULL);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (!api) return SQLITE_ERROR;
    fts5_tokenizer tokenizer = {tokenizer_create, tokenizer_delete, tokenize};
    result = api->xCreateTokenizer(api, "argus_meta", NULL, &tokenizer, NULL);
    if (result != SQLITE_OK) return result;
    result = api->xCreateTokenizer(api, "argus_body", (void *)1, &tokenizer, NULL);
    if (result != SQLITE_OK) return result;
    return api->xCreateFunction(api, "argus_excerpt", NULL, excerpt, NULL);
}
