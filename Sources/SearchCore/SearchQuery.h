#ifndef ARGUS_SEARCH_QUERY_H
#define ARGUS_SEARCH_QUERY_H

#define ARGUS_QUERY_BYTES 2048
#define ARGUS_MAX_TERMS 24

typedef struct {
    char text[ARGUS_QUERY_BYTES];
    char *terms[ARGUS_MAX_TERMS];
    int count;
} SearchQuery;

int argus_query_parse(SearchQuery *query, const char *text);
/* Match expressions are owned by SQLite; release with sqlite3_free. */
char *argus_query_match(SearchQuery *query, int body, int fuzzy);
double argus_query_score(SearchQuery *query, const char *name, const char *path,
                         const char *aliases, int fuzzy);
int argus_query_body_matches(SearchQuery *query, const char *body);

#endif
