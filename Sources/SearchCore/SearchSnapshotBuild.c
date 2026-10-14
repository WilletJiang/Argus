#include "SearchSnapshot.h"
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc/malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { uint32_t hash, offset; } StringBucket;
typedef struct { const char *name; sqlite3_int64 id; uint32_t slot; } NameKey;
typedef struct {
    ArgusIndex *index;
    uint64_t epoch;
    SnapshotHeader header;
    SnapshotRow *rows;
    SnapshotParent *parents;
    uint32_t *parent_table, parent_capacity, parent_count;
    char *strings;
    uint32_t string_size, string_capacity;
    StringBucket *table;
    uint32_t table_capacity, table_count;
    SnapshotPlane planes[ARGUS_PLANES];
    uint32_t *names;
    unsigned char *postings;
    size_t fixed_bytes;
} SnapshotBuilder;

static int cancelled(SnapshotBuilder *builder) {
    return atomic_load_explicit(&builder->index->interrupt_epoch, memory_order_relaxed) != builder->epoch;
}

static int within_budget(SnapshotBuilder *b, size_t strings, size_t table, size_t parents, size_t extra) {
    return b->fixed_bytes + strings + table + parents + extra <= ARGUS_SNAPSHOT_BUDGET;
}

static uint32_t hash_string(const char *text) {
    uint32_t hash = 2166136261;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) hash = (hash ^ *p) * 16777619;
    return hash ? hash : 1;
}

static int grow_table(SnapshotBuilder *b) {
    uint32_t capacity = b->table_capacity ? b->table_capacity * 2 : 1024;
    if (!within_budget(b, b->string_capacity, (size_t)capacity * sizeof(StringBucket),
                       b->parent_capacity * sizeof(uint32_t), b->table_capacity * sizeof(StringBucket))) return 0;
    StringBucket *table = calloc(capacity, sizeof(*table));
    if (!table) return 0;
    for (uint32_t i = 0; i < b->table_capacity; ++i) {
        StringBucket entry = b->table[i];
        if (!entry.hash) continue;
        uint32_t slot = entry.hash & (capacity - 1);
        while (table[slot].hash) slot = (slot + 1) & (capacity - 1);
        table[slot] = entry;
    }
    free(b->table); b->table = table; b->table_capacity = capacity;
    return 1;
}

static int intern(SnapshotBuilder *b, const char *text, uint32_t *result) {
    if (!*text) { *result = 0; return 1; }
    if (!b->table_capacity || (b->table_count + 1) * 4 > b->table_capacity * 3) {
        if (!grow_table(b)) return 0;
    }
    uint32_t hash = hash_string(text), slot = hash & (b->table_capacity - 1);
    while (b->table[slot].hash) {
        if (b->table[slot].hash == hash && !strcmp(b->strings + b->table[slot].offset, text)) {
            *result = b->table[slot].offset; return 1;
        }
        slot = (slot + 1) & (b->table_capacity - 1);
    }
    size_t length = strlen(text) + 1, needed = b->string_size + length;
    if (needed > ARGUS_SNAPSHOT_BUDGET) return 0;
    if (needed > b->string_capacity) {
        uint32_t capacity = b->string_capacity;
        while (capacity < needed) capacity *= 2;
        if (!within_budget(b, capacity, b->table_capacity * sizeof(StringBucket),
                           b->parent_capacity * sizeof(uint32_t), b->string_capacity)) return 0;
        void *strings = realloc(b->strings, capacity);
        if (!strings) return 0;
        b->strings = strings; b->string_capacity = capacity;
    }
    *result = b->string_size;
    memcpy(b->strings + b->string_size, text, length); b->string_size += (uint32_t)length;
    b->table[slot] = (StringBucket){hash, *result}; ++b->table_count;
    return 1;
}

static uint32_t parent_hash(uint32_t path, uint32_t normalized) {
    return (path * 2654435761U) ^ (normalized * 2246822519U);
}

static int intern_parent(SnapshotBuilder *b, uint32_t path, uint32_t normalized, uint32_t *result) {
    if (!b->parent_capacity || (b->parent_count + 1) * 2 > b->parent_capacity) {
        uint32_t capacity = b->parent_capacity ? b->parent_capacity * 2 : 256;
        if (!within_budget(b, b->string_capacity, b->table_capacity * sizeof(StringBucket),
                           capacity * sizeof(uint32_t), b->parent_capacity * sizeof(uint32_t))) return 0;
        uint32_t *table = calloc(capacity, sizeof(*table));
        if (!table) return 0;
        for (uint32_t i = 0; i < b->parent_count; ++i) {
            SnapshotParent p = b->parents[i];
            uint32_t slot = parent_hash(p.path, p.normalized_path) & (capacity - 1);
            while (table[slot]) slot = (slot + 1) & (capacity - 1);
            table[slot] = i + 1;
        }
        free(b->parent_table); b->parent_table = table; b->parent_capacity = capacity;
    }
    uint32_t slot = parent_hash(path, normalized) & (b->parent_capacity - 1);
    while (b->parent_table[slot]) {
        uint32_t i = b->parent_table[slot] - 1;
        if (b->parents[i].path == path && b->parents[i].normalized_path == normalized) { *result = i; return 1; }
        slot = (slot + 1) & (b->parent_capacity - 1);
    }
    if (b->parent_count >= b->header.count) return 0;
    *result = b->parent_count++;
    b->parents[*result] = (SnapshotParent){path, normalized};
    b->parent_table[slot] = *result + 1;
    return 1;
}

static int split_path(SnapshotBuilder *b, const char *text, uint32_t *parent, uint32_t *basename) {
    const char *slash = strrchr(text, '/');
    if (!slash) { *parent = 0; return intern(b, text, basename); }
    size_t length = slash == text ? 1 : (size_t)(slash - text);
    if (length > ARGUS_NORMAL_PATH_BYTES) return 0;
    char prefix[ARGUS_NORMAL_PATH_BYTES + 1];
    memcpy(prefix, text, length); prefix[length] = 0;
    return intern(b, prefix, parent) && intern(b, slash + 1, basename);
}

static int read_rows(SnapshotBuilder *b) {
    sqlite3_stmt *rows;
    if (!argus_prepare(b->index, "SELECT id,nname,npath,naliases,path,kind FROM entries ORDER BY id", &rows)) return 0;
    uint32_t count = 0;
    int status, ok = 1;
    while ((status = sqlite3_step(rows)) == SQLITE_ROW) {
        if (count >= b->header.count || (!(count & 31) && cancelled(b))) { ok = 0; break; }
        SnapshotRow *r = b->rows + count++;
        r->id = sqlite3_column_int64(rows, 0); r->kind = sqlite3_column_int(rows, 5);
        uint32_t parent, normalized_parent;
        if (strlen(argus_column_text(rows, 2)) > ARGUS_NORMAL_PATH_BYTES ||
            !intern(b, argus_column_text(rows, 1), &r->name) ||
            !intern(b, argus_column_text(rows, 3), &r->aliases) ||
            !split_path(b, argus_column_text(rows, 4), &parent, &r->basename) ||
            !split_path(b, argus_column_text(rows, 2), &normalized_parent, &r->normalized_basename) ||
            !intern_parent(b, parent, normalized_parent, &r->parent)) { ok = 0; break; }
    }
    sqlite3_finalize(rows);
    return ok && status == SQLITE_DONE && count == b->header.count;
}

static void field_planes(const char *text, unsigned char seen[ARGUS_PLANES]) {
    int previous = -1;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        int current = argus_ascii_character(*p);
        if (current < 0) { previous = -1; continue; }
        seen[ARGUS_ALPHABET * ARGUS_ALPHABET + current] = 1;
        if (previous >= 0) seen[previous * ARGUS_ALPHABET + current] = 1;
        previous = current;
    }
}

static void row_planes(SnapshotBuilder *b, uint32_t row, unsigned char seen[ARGUS_PLANES]) {
    SnapshotRow *r = b->rows + row;
    field_planes(b->strings + r->name, seen);
    field_planes(b->strings + r->aliases, seen);
    field_planes(b->strings + b->parents[r->parent].normalized_path, seen);
    field_planes(b->strings + r->normalized_basename, seen);
    seen[ARGUS_TEXT_PLANES + (r->kind == 2 ? 0 : 1)] = 1;
}

static int compare_names(const void *left, const void *right) {
    const NameKey *a = left, *b = right;
    int comparison = strcmp(a->name, b->name);
    return comparison ? comparison : a->id < b->id ? -1 : a->id > b->id;
}

static int build_postings(SnapshotBuilder *b) {
    /* Count unique grams per row first; never allocate a dense matrix for every gram. */
    for (uint32_t row = 0; row < b->header.count; ++row) {
        if (!(row & 31) && cancelled(b)) return 0;
        unsigned char seen[ARGUS_PLANES] = {0}; row_planes(b, row, seen);
        for (int p = 0; p < ARGUS_PLANES; ++p) b->planes[p].count += seen[p];
    }
    size_t bytes = 0, words = (b->header.count + 63) / 64;
    for (int p = 0; p < ARGUS_PLANES; ++p) {
        SnapshotPlane *plane = b->planes + p;
        plane->dense = (size_t)plane->count * 4 > words * 8;
        bytes = argus_align8(bytes); plane->offset = (uint32_t)bytes;
        bytes += plane->dense ? words * 8 : plane->count * 4;
        if (bytes > ARGUS_SNAPSHOT_BUDGET) return 0;
    }
    b->header.parents = b->parent_count; b->header.strings = b->string_size; b->header.postings = (uint32_t)bytes;
    if (argus_snapshot_size(&b->header) > ARGUS_SNAPSHOT_BUDGET ||
        !within_budget(b, b->string_capacity, 0, 0, bytes + b->header.count * sizeof(NameKey))) return 0;
    b->postings = calloc(bytes ? bytes : 1, 1);
    NameKey *names = malloc(b->header.count * sizeof(*names));
    if (!b->postings || !names) { free(names); return 0; }
    uint32_t written[ARGUS_PLANES] = {0};
    for (uint32_t row = 0; row < b->header.count; ++row) {
        if (!(row & 31) && cancelled(b)) { free(names); return 0; }
        unsigned char seen[ARGUS_PLANES] = {0}; row_planes(b, row, seen);
        for (int p = 0; p < ARGUS_PLANES; ++p) if (seen[p]) {
            SnapshotPlane *plane = b->planes + p;
            if (plane->dense) ((uint64_t *)(b->postings + plane->offset))[row / 64] |= UINT64_C(1) << (row % 64);
            else ((uint32_t *)(b->postings + plane->offset))[written[p]++] = row;
        }
        names[row] = (NameKey){b->strings + b->rows[row].name, b->rows[row].id, row};
    }
    qsort(names, b->header.count, sizeof(*names), compare_names);
    for (uint32_t i = 0; i < b->header.count; ++i) b->names[i] = names[i].slot;
    free(names);
    return !cancelled(b);
}

static int write_bytes(SnapshotBuilder *b, int fd, CC_SHA256_CTX *hash, const void *data, size_t bytes) {
    const unsigned char *cursor = data;
    while (bytes) {
        if (cancelled(b)) return 0;
        size_t chunk = bytes < 65536 ? bytes : 65536;
        ssize_t written = write(fd, cursor, chunk);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return 0;
        if (hash) CC_SHA256_Update(hash, cursor, (CC_LONG)written);
        cursor += written; bytes -= (size_t)written;
    }
    return 1;
}

static int save_snapshot(SnapshotBuilder *b) {
    const char *database = sqlite3_db_filename(b->index->db, "main");
    char path[8192], temporary[8192];
    int persistent = database && *database;
    if (persistent) {
        if (snprintf(path, sizeof(path), "%s.search", database) >= (int)sizeof(path) ||
            snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path) >= (int)sizeof(temporary)) return -1;
    } else strcpy(temporary, "/tmp/argus-snapshot-XXXXXX");
    int fd = mkstemp(temporary);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    CC_SHA256_CTX hash; CC_SHA256_Init(&hash);
    unsigned char padding[8] = {0};
    size_t name_bytes = b->header.count * sizeof(uint32_t);
    size_t name_padding = argus_align8(name_bytes) - name_bytes;
    size_t string_padding = argus_align8(b->header.strings) - b->header.strings;
    int ok = write_bytes(b, fd, NULL, &b->header, sizeof(b->header)) &&
        write_bytes(b, fd, &hash, b->rows, b->header.count * sizeof(SnapshotRow)) &&
        write_bytes(b, fd, &hash, b->parents, b->header.parents * sizeof(SnapshotParent)) &&
        write_bytes(b, fd, &hash, b->names, name_bytes) && write_bytes(b, fd, &hash, padding, name_padding) &&
        write_bytes(b, fd, &hash, b->planes, sizeof(b->planes)) &&
        write_bytes(b, fd, &hash, b->strings, b->header.strings) && write_bytes(b, fd, &hash, padding, string_padding) &&
        write_bytes(b, fd, &hash, b->postings, b->header.postings);
    CC_SHA256_Final(b->header.digest, &hash);
    if (ok) ok = pwrite(fd, &b->header, sizeof(b->header), 0) == sizeof(b->header);
    if (ok && persistent) ok = rename(temporary, path) == 0;
    unlink(temporary);
    if (!ok) { close(fd); return -1; }
    return fd;
}

int argus_snapshot_prepare(ArgusIndex *index) {
    if (!sqlite3_get_autocommit(index->db)) return 0;
    if (!argus_execute(index, "BEGIN")) return 0;
    SnapshotBuilder b = {.index = index, .epoch = atomic_load_explicit(&index->interrupt_epoch, memory_order_relaxed)};
    int count, ok = 0, descriptor = -1;
    if (!argus_snapshot_stamp(index, &b.header.generation, b.header.identity, &count)) goto cleanup;
    if (count < 256 || count > 500000) {
        argus_snapshot_close(index->snapshot); index->snapshot = NULL;
        ok = 1; goto cleanup;
    }
    if (index->snapshot && index->snapshot->header->generation == b.header.generation &&
        !memcmp(index->snapshot->header->identity, b.header.identity, 16)) { ok = 1; goto cleanup; }
    argus_snapshot_close(index->snapshot); index->snapshot = NULL;
    ArgusSnapshot *loaded = argus_snapshot_load(index, b.header.generation, b.header.identity);
    if (loaded) {
        argus_snapshot_close(index->snapshot); index->snapshot = loaded; ok = 1; goto cleanup;
    }
    b.header.magic = UINT64_C(0x41524755534d4554); b.header.version = 1; b.header.count = (uint32_t)count;
    b.fixed_bytes = (size_t)count * (sizeof(SnapshotRow) + sizeof(SnapshotParent) + sizeof(uint32_t));
    b.rows = calloc(count, sizeof(SnapshotRow)); b.parents = calloc(count, sizeof(SnapshotParent));
    b.names = malloc(count * sizeof(uint32_t)); b.string_capacity = 4096;
    b.strings = calloc(b.string_capacity, 1); b.string_size = 1;
    if (!b.rows || !b.parents || !b.names || !b.strings || !read_rows(&b)) goto cleanup;
    free(b.table); b.table = NULL; b.table_capacity = 0;
    free(b.parent_table); b.parent_table = NULL; b.parent_capacity = 0;
    if (!build_postings(&b)) goto cleanup;
    descriptor = save_snapshot(&b);
cleanup:
    free(b.rows); free(b.parents); free(b.names); free(b.strings);
    free(b.table); free(b.parent_table); free(b.postings);
    sqlite3_exec(index->db, "COMMIT", NULL, NULL, NULL);
    malloc_zone_pressure_relief(NULL, 0);
    // Release builder arenas before touching mapped pages: construction and retained cache do not overlap.
    if (descriptor >= 0) {
        if (!cancelled(&b)) {
            index->snapshot = argus_snapshot_map(descriptor, b.header.generation, b.header.identity);
            ok = index->snapshot != NULL;
        }
        close(descriptor);
    }
    return cancelled(&b) ? -1 : ok;
}
