#ifndef ARGUS_SEARCH_SNAPSHOT_H
#define ARGUS_SEARCH_SNAPSHOT_H
#include "SearchIndex.h"
#define ARGUS_ALPHABET 37
#define ARGUS_TEXT_PLANES (ARGUS_ALPHABET * ARGUS_ALPHABET + ARGUS_ALPHABET)
#define ARGUS_PLANES (ARGUS_TEXT_PLANES + 2)
#define ARGUS_SNAPSHOT_BUDGET (64U * 1024U * 1024U)
#define ARGUS_NORMAL_PATH_BYTES 16384

typedef struct { uint64_t magic; uint32_t version, count, parents, strings, postings;
                 uint64_t generation; unsigned char identity[16], digest[32]; } SnapshotHeader;
typedef struct { sqlite3_int64 id; uint32_t name, aliases, parent, basename, normalized_basename; int kind; } SnapshotRow;
typedef struct { uint32_t path, normalized_path; } SnapshotParent;
typedef struct { uint32_t count, offset, dense; } SnapshotPlane;
struct ArgusSnapshot {
    void *mapping;
    size_t bytes;
    const SnapshotHeader *header;
    const SnapshotRow *rows;
    const SnapshotParent *parents;
    const uint32_t *names;
    const SnapshotPlane *planes;
    const char *strings;
    const unsigned char *postings;
    uint64_t *scratch, *scratch2;
};
size_t argus_snapshot_size(const SnapshotHeader *header);
ArgusSnapshot *argus_snapshot_load(ArgusIndex *index, uint64_t generation, const unsigned char identity[16]);
int argus_snapshot_stamp(ArgusIndex *index, uint64_t *generation, unsigned char identity[16], int *count);
int argus_snapshot_path(const ArgusSnapshot *snapshot, const SnapshotRow *row, int normalized, char *out, size_t capacity);
ArgusSnapshot *argus_snapshot_map(int descriptor, uint64_t generation, const unsigned char identity[16]);
static inline int argus_ascii_character(unsigned char value) {
    if (value >= 'a' && value <= 'z') return value - 'a';
    if (value >= '0' && value <= '9') return value - '0' + 26;
    return value == '_' ? 36 : -1;
}
static inline size_t argus_align8(size_t size) { return (size + 7) & ~(size_t)7; }
#endif
