#include "SearchSnapshot.h"
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

size_t argus_snapshot_size(const SnapshotHeader *h) {
    size_t size = sizeof(*h) + (size_t)h->count * sizeof(SnapshotRow) + (size_t)h->parents * sizeof(SnapshotParent);
    size = argus_align8(size + (size_t)h->count * sizeof(uint32_t));
    return argus_align8(size + ARGUS_PLANES * sizeof(SnapshotPlane) + h->strings) + h->postings;
}

static int strings_valid(const ArgusSnapshot *s) {
    if (!s->header->strings || s->strings[0] || s->strings[s->header->strings - 1]) return 0;
    for (uint32_t i = 0; i < s->header->parents; ++i) {
        if (s->parents[i].path >= s->header->strings || s->parents[i].normalized_path >= s->header->strings) return 0;
    }
    for (uint32_t i = 0; i < s->header->count; ++i) {
        const SnapshotRow *r = s->rows + i;
        if (r->name >= s->header->strings || r->aliases >= s->header->strings ||
            r->basename >= s->header->strings || r->normalized_basename >= s->header->strings ||
            r->parent >= s->header->parents || r->kind < 0 || r->kind > 2 || s->names[i] >= s->header->count) return 0;
    }
    for (int i = 0; i < ARGUS_PLANES; ++i) {
        const SnapshotPlane *p = s->planes + i;
        size_t bytes = p->dense ? (size_t)((s->header->count + 63) / 64) * 8 : (size_t)p->count * 4;
        if (p->count > s->header->count || p->offset % 8 || p->offset > s->header->postings ||
            bytes > s->header->postings - p->offset) return 0;
    }
    return 1;
}

ArgusSnapshot *argus_snapshot_map(int descriptor, uint64_t generation, const unsigned char identity[16]) {
    struct stat attributes;
    if (fstat(descriptor, &attributes) || attributes.st_size < (off_t)sizeof(SnapshotHeader) ||
        attributes.st_size > ARGUS_SNAPSHOT_BUDGET) return NULL;
    size_t bytes = (size_t)attributes.st_size;
    void *mapping = mmap(NULL, bytes, PROT_READ, MAP_PRIVATE, descriptor, 0);
    if (mapping == MAP_FAILED) return NULL;
    const SnapshotHeader *h = mapping;
    if (h->magic != UINT64_C(0x41524755534d4554) || h->version != 1 || h->count > 1000000 ||
        h->parents > h->count || h->generation != generation || memcmp(h->identity, identity, 16) ||
        h->strings > ARGUS_SNAPSHOT_BUDGET || h->postings > ARGUS_SNAPSHOT_BUDGET ||
        argus_snapshot_size(h) != bytes) { munmap(mapping, bytes); return NULL; }
    unsigned char digest[32];
    CC_SHA256((const unsigned char *)mapping + sizeof(*h), (CC_LONG)(bytes - sizeof(*h)), digest);
    if (memcmp(digest, h->digest, sizeof(digest))) { munmap(mapping, bytes); return NULL; }
    ArgusSnapshot *s = calloc(1, sizeof(*s));
    if (!s) { munmap(mapping, bytes); return NULL; }
    s->mapping = mapping; s->bytes = bytes; s->header = h;
    size_t offset = sizeof(*h);
    s->rows = (const SnapshotRow *)((const unsigned char *)mapping + offset); offset += h->count * sizeof(SnapshotRow);
    s->parents = (const SnapshotParent *)((const unsigned char *)mapping + offset); offset += h->parents * sizeof(SnapshotParent);
    s->names = (const uint32_t *)((const unsigned char *)mapping + offset); offset = argus_align8(offset + h->count * sizeof(uint32_t));
    s->planes = (const SnapshotPlane *)((const unsigned char *)mapping + offset); offset += ARGUS_PLANES * sizeof(SnapshotPlane);
    s->strings = (const char *)mapping + offset; offset = argus_align8(offset + h->strings);
    s->postings = (const unsigned char *)mapping + offset;
    size_t scratch = (size_t)((h->count + 63) / 64) * sizeof(uint64_t);
    s->scratch = malloc(scratch); s->scratch2 = malloc(scratch);
    if (!s->scratch || !s->scratch2 || !strings_valid(s)) { argus_snapshot_close(s); return NULL; }
    return s;
}

void argus_snapshot_close(ArgusSnapshot *snapshot) {
    if (!snapshot) return;
    munmap(snapshot->mapping, snapshot->bytes);
    free(snapshot->scratch); free(snapshot->scratch2); free(snapshot);
}

ArgusSnapshot *argus_snapshot_load(ArgusIndex *index, uint64_t generation, const unsigned char identity[16]) {
    const char *database = sqlite3_db_filename(index->db, "main");
    if (!database || !*database) return NULL;
    char path[8192];
    if (snprintf(path, sizeof(path), "%s.search", database) >= (int)sizeof(path)) return NULL;
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return NULL;
    ArgusSnapshot *snapshot = argus_snapshot_map(descriptor, generation, identity);
    close(descriptor);
    return snapshot;
}

int argus_snapshot_stamp(ArgusIndex *index, uint64_t *generation, unsigned char identity[16], int *count) {
    sqlite3_stmt **slot = index->search_statements + 8;
    if (!*slot && !argus_prepare(index,
        "SELECT metadata_generation,metadata_id,row_count FROM stats WHERE id=1", slot)) return 0;
    sqlite3_reset(*slot);
    if (sqlite3_step(*slot) != SQLITE_ROW || sqlite3_column_bytes(*slot, 1) != 16) {
        sqlite3_reset(*slot); return argus_sql_error(index);
    }
    *generation = (uint64_t)sqlite3_column_int64(*slot, 0);
    memcpy(identity, sqlite3_column_blob(*slot, 1), 16);
    sqlite3_int64 rows = sqlite3_column_int64(*slot, 2);
    *count = rows >= 0 && rows <= 1000000 ? (int)rows : -1;
    sqlite3_reset(*slot);
    return 1;
}

int argus_snapshot_path(const ArgusSnapshot *s, const SnapshotRow *r, int normalized, char *out, size_t capacity) {
    const SnapshotParent *parent = s->parents + r->parent;
    const char *directory = s->strings + (normalized ? parent->normalized_path : parent->path);
    const char *basename = s->strings + (normalized ? r->normalized_basename : r->basename);
    size_t prefix = strlen(directory), suffix = strlen(basename);
    int separator = prefix && !(prefix == 1 && directory[0] == '/');
    if (prefix + separator + suffix >= capacity) return 0;
    memcpy(out, directory, prefix);
    if (separator) out[prefix++] = '/';
    memcpy(out + prefix, basename, suffix + 1);
    return 1;
}

static int consider(ArgusSearch *search, ArgusSnapshot *s, uint32_t slot) {
    const SnapshotRow *r = s->rows + slot;
    char normalized[ARGUS_NORMAL_PATH_BYTES + 1], path[4096];
    if (!argus_snapshot_path(s, r, 1, normalized, sizeof(normalized)) ||
        !argus_snapshot_path(s, r, 0, path, sizeof(path))) return argus_fail(search->index, "Invalid snapshot path");
    double score = argus_query_score(&search->query, s->strings + r->name, normalized, s->strings + r->aliases, 0);
    return score <= 0 || argus_search_consider(search, r->id, path, score, 0, 0);
}

static int includes(ArgusSearch *search, int kind) {
    return search->filter == -1 || (search->filter == 0 ? kind != 2 : kind == search->filter);
}

int argus_snapshot_search(ArgusSearch *search) {
    if (!search->owns_transaction) return 0;
    int ids[ARGUS_MAX_TERMS * 24 + 1], terms = 0;
    for (int t = 0; t < search->query.count; ++t) {
        const unsigned char *text = (const unsigned char *)search->query.terms[t];
        int length = (int)strlen((const char *)text);
        if (search->query.count > 1 && length == 1) return 0;
        for (int i = 0; i < length; ++i) if (argus_ascii_character(text[i]) < 0) return 0;
        if (length == 1) ids[terms++] = ARGUS_ALPHABET * ARGUS_ALPHABET + argus_ascii_character(text[0]);
        else for (int i = 0; i < length - 1 && i < 24; ++i)
            ids[terms++] = argus_ascii_character(text[i]) * ARGUS_ALPHABET + argus_ascii_character(text[i + 1]);
    }
    if (search->filter != -1) ids[terms++] = ARGUS_TEXT_PLANES + (search->filter == 2 ? 0 : 1);
    ArgusIndex *index = search->index;
    uint64_t generation; unsigned char identity[16]; int count;
    if (!argus_snapshot_stamp(index, &generation, identity, &count)) return -1;
    if (index->snapshot && (index->snapshot->header->generation != generation ||
                           memcmp(index->snapshot->header->identity, identity, 16))) {
        argus_snapshot_close(index->snapshot); index->snapshot = NULL;
    }
    if (!index->snapshot && (!index->snapshot_checked || index->checked_generation != generation ||
                            memcmp(index->checked_identity, identity, 16))) {
        index->snapshot = argus_snapshot_load(index, generation, identity);
        index->snapshot_checked = 1; index->checked_generation = generation;
        memcpy(index->checked_identity, identity, 16);
    }
    ArgusSnapshot *s = index->snapshot;
    if (!s || (int)s->header->count != count) return 0;
    const char *prefix = search->query.prefix;
    size_t length = strlen(prefix);
    uint32_t low = 0, high = s->header->count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (strcmp(s->strings + s->rows[s->names[middle]].name, prefix) < 0) low = middle + 1;
        else high = middle;
    }
    int visited = 0;
    for (uint32_t i = low; i < s->header->count && visited < 512; ++i) {
        uint32_t slot = s->names[i];
        const SnapshotRow *r = s->rows + slot;
        if (strncmp(s->strings + r->name, prefix, length)) break;
        if (!(i & 31) && argus_search_cancelled(search)) return -1;
        if (!includes(search, r->kind)) continue;
        ++visited;
        if (!consider(search, s, slot)) return -1;
    }
    size_t words = (s->header->count + 63) / 64;
    memset(s->scratch, 0, words * 8);
    for (int t = 0; t < terms; ++t) {
        if (argus_search_cancelled(search)) return -1;
        const SnapshotPlane *p = s->planes + ids[t];
        if (!p->count) { memset(s->scratch, 0, words * 8); break; }
        const uint64_t *bits;
        if (p->dense) bits = (const uint64_t *)(s->postings + p->offset);
        else {
            memset(s->scratch2, 0, words * 8);
            const uint32_t *rows = (const uint32_t *)(s->postings + p->offset);
            for (uint32_t i = 0; i < p->count; ++i) s->scratch2[rows[i] / 64] |= UINT64_C(1) << (rows[i] % 64);
            bits = s->scratch2;
        }
        if (!t) memcpy(s->scratch, bits, words * 8);
        else {
            uint64_t *restrict destination = s->scratch;
            const uint64_t *restrict source = bits;
            for (size_t i = 0; i < words; ++i) destination[i] &= source[i];
        }
    }
    visited = 0;
    for (size_t w = 0; w < words && visited < 4096; ++w) {
        uint64_t bits = s->scratch[w];
        while (bits && visited < 4096) {
            uint32_t slot = (uint32_t)(w * 64 + __builtin_ctzll(bits)); bits &= bits - 1;
            if (!(visited++ & 31) && argus_search_cancelled(search)) return -1;
            if (!consider(search, s, slot)) return -1;
        }
    }
    return 1;
}
