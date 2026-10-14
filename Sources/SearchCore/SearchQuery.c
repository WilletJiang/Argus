#include "SearchQuery.h"
#include "SearchTokenizer.h"

#include <math.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

int argus_query_parse(SearchQuery *query, const char *text) {
    memset(query, 0, sizeof(*query));
    char *folded = argus_fold_text(text);
    if (!folded) return 0;
    size_t bytes = strlen(folded);
    if (bytes >= sizeof(query->text)) { free(folded); return 0; }
    memcpy(query->text, folded, bytes + 1);
    memcpy(query->prefix, folded, bytes + 1);
    free(folded);
    char *current = query->text;
    while (*current && query->count < ARGUS_MAX_TERMS) {
        int width;
        if (!argus_token_character(argus_codepoint(current, (int)strlen(current), &width))) {
            memset(current, 0, (size_t)width); current += width; continue;
        }
        query->terms[query->count++] = current;
        while (*current) {
            uint32_t point = argus_codepoint(current, (int)strlen(current), &width);
            if (!argus_token_character(point)) { memset(current, 0, (size_t)width); current += width; break; }
            current += width;
        }
    }
    while (*current) {
        int width;
        if (argus_token_character(argus_codepoint(current, (int)strlen(current), &width))) return 0;
        current += width;
    }
    return 1;
}

static void append_token(sqlite3_str *builder, char prefix, const char *text, int bytes, int wildcard) {
    sqlite3_str_appendchar(builder, 1, '"');
    sqlite3_str_appendchar(builder, 1, prefix);
    sqlite3_str_append(builder, text, bytes);
    sqlite3_str_appendall(builder, wildcard ? "\"*" : "\"");
}

static char *match_expression(SearchQuery *query, int body, int fuzzy, int positional) {
    sqlite3_str *builder = sqlite3_str_new(NULL);
    if (fuzzy) sqlite3_str_appendall(builder, "{nname naliases} : (");
    for (int t = 0; t < query->count; ++t) {
        if (t) sqlite3_str_appendall(builder, " AND ");
        const char *term = query->terms[t];
        int bytes = (int)strlen(term), width;
        uint32_t first = argus_codepoint(term, bytes, &width);
        if (fuzzy) {
            sqlite3_str_appendchar(builder, 1, '(');
            append_token(builder, 'w', term, width, 1);
            int previous = 0, offset = 0, characters = 0;
            while (offset < bytes) {
                int character_width;
                argus_codepoint(term + offset, bytes - offset, &character_width);
                if (offset + character_width < bytes) previous = offset;
                offset += character_width; ++characters;
            }
            if (characters >= 4) {
                sqlite3_str_appendall(builder, " OR ");
                append_token(builder, 'b', term + previous, bytes - previous, 0);
            }
            sqlite3_str_appendchar(builder, 1, ')');
        } else if (bytes == width && argus_cjk_character(first)) {
            append_token(builder, 'u', term, bytes, 0);
        } else if (bytes == width || (body && !argus_cjk_character(first))) {
            append_token(builder, 'w', term, bytes, 1);
        } else {
            int offset = 0, emitted = 0;
            if (positional) sqlite3_str_appendchar(builder, 1, '(');
            /* Cap gram conjunctions for long pasted paths, without losing correctness:
               ranking validates every complete term after candidate retrieval. */
            while (offset < bytes && (positional || emitted < 24)) {
                int first_width, second_width;
                argus_codepoint(term + offset, bytes - offset, &first_width);
                int next = offset + first_width;
                if (next >= bytes) break;
                argus_codepoint(term + next, bytes - next, &second_width);
                if (emitted++) sqlite3_str_appendall(builder, positional ? " + " : " AND ");
                append_token(builder, 'b', term + offset, first_width + second_width, 0);
                offset = next;
            }
            if (positional) sqlite3_str_appendchar(builder, 1, ')');
        }
    }
    if (fuzzy) sqlite3_str_appendall(builder, ")");
    return sqlite3_str_finish(builder);
}

char *argus_query_match(SearchQuery *query, int body, int fuzzy) {
    return match_expression(query, body, fuzzy, 0);
}

char *argus_query_body_expression(SearchQuery *query, int positional) {
    return match_expression(query, 1, 0, positional);
}

static int codepoints(const char *text, uint32_t *out, int capacity) {
    int count = 0, bytes = (int)strlen(text), offset = 0;
    while (offset < bytes && count < capacity) {
        int width;
        out[count++] = argus_codepoint(text + offset, bytes - offset, &width);
        offset += width;
    }
    return offset == bytes ? count : -1;
}

static int edit_distance(const char *left, const char *right, int right_bytes) {
    if (right_bytes > 256) return 3;
    char buffer[257]; memcpy(buffer, right, (size_t)right_bytes); buffer[right_bytes] = 0;
    uint32_t a[64], b[64];
    int n = codepoints(left, a, 64), m = codepoints(buffer, b, 64);
    if (n < 3 || n > 64 || m < 0 || abs(n - m) > 2) return 3;
    int previous[65], current[65], before_previous[65];
    for (int j = 0; j <= m; ++j) previous[j] = before_previous[j] = j;
    for (int i = 1; i <= n; ++i) {
        current[0] = i;
        int minimum = i;
        for (int j = 1; j <= m; ++j) {
            int deletion = previous[j] + 1, insertion = current[j - 1] + 1;
            int substitution = previous[j - 1] + (a[i - 1] != b[j - 1]);
            int value = deletion < insertion ? deletion : insertion;
            if (substitution < value) value = substitution;
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] &&
                before_previous[j - 2] + 1 < value) value = before_previous[j - 2] + 1;
            current[j] = value;
            if (value < minimum) minimum = value;
        }
        if (minimum > 2) return 3;
        memcpy(before_previous, previous, sizeof(previous));
        memcpy(previous, current, sizeof(previous));
    }
    return previous[m];
}

static double fuzzy_term(const char *term, const char *text) {
    double best = 0;
    int bytes = (int)strlen(text), offset = 0;
    while (offset < bytes) {
        int width;
        if (!argus_token_character(argus_codepoint(text + offset, bytes - offset, &width))) {
            offset += width; continue;
        }
        int start = offset;
        while (offset < bytes && argus_token_character(argus_codepoint(text + offset, bytes - offset, &width)))
            offset += width;
        int distance = edit_distance(term, text + start, offset - start);
        if (distance <= 2) {
            double score = distance == 0 ? 80 : distance == 1 ? 42 : 24;
            if (score > best) best = score;
        }
    }
    uint32_t needle[64], haystack[256];
    int n = codepoints(term, needle, 64), m = codepoints(text, haystack, 256);
    if (n >= 3 && m >= n) {
        int matched = 0, first = -1, last = 0;
        for (int i = 0; i < m && matched < n; ++i) {
            if (haystack[i] == needle[matched]) {
                if (first < 0) first = i;
                last = i; ++matched;
            }
        }
        if (matched == n && last - first <= n * 6) {
            double score = 18.0 - (last - first - n + 1) * 0.8;
            if (score > best) best = score;
        }
    }
    return best;
}

static double term_score(const char *term, const char *text, double weight) {
    const char *found = strstr(text, term);
    if (!found) return 0;
    size_t length = strlen(term);
    int boundary = found == text || !argus_token_character((unsigned char)found[-1]);
    int end = !found[length] || !argus_token_character((unsigned char)found[length]);
    return weight + (boundary ? 15 : 0) + (boundary && end ? 10 : 0) -
           fmin((double)(found - text) * 0.05, 8.0);
}

double argus_query_score(SearchQuery *query, const char *name, const char *path,
                             const char *aliases, int fuzzy) {
    double sum = 0;
    int was_fuzzy = 0;
    for (int t = 0; t < query->count; ++t) {
        const char *term = query->terms[t];
        double best = term_score(term, name, 75);
        double alias = term_score(term, aliases, 55), directory = term_score(term, path, 25);
        if (alias > best) best = alias;
        if (directory > best) best = directory;
        if (!best && fuzzy) {
            best = fuzzy_term(term, name);
            alias = fuzzy_term(term, aliases);
            if (alias > best) best = alias;
            was_fuzzy = 1;
        }
        if (!best) return 0;
        sum += best;
    }
    double score = (was_fuzzy ? 180 : 500) + sum / query->count;
    if (query->count == 1 && !was_fuzzy) {
        const char *term = query->terms[0];
        size_t length = strlen(term);
        if (!strcmp(name, term)) score = 1000;
        else if (!strncmp(name, term, length)) score = name[length] == '.' ? 970 : 900;
        else if (strstr(name, term)) score = 780;
        else if (!strcmp(aliases, term)) score = 850;
        else if (!strncmp(aliases, term, length)) score = 810;
    }
    return score - fmin((double)strlen(name) * 0.025, 8.0);
}

int argus_query_body_matches(SearchQuery *query, const char *body) {
    char *folded = NULL;
    for (int t = 0; t < query->count; ++t) {
        int width;
        if (!argus_cjk_character(argus_codepoint(query->terms[t], (int)strlen(query->terms[t]), &width))) continue;
        if (!folded) {
            folded = argus_fold_text(body);
            if (!folded) return -1;
        }
        if (!strstr(folded, query->terms[t])) { free(folded); return 0; }
    }
    free(folded);
    return 1;
}
