#ifndef AH_HASH_H
#define AH_HASH_H
#include <stddef.h>
#include <stdint.h>
#if defined(__APPLE__) && !defined(AH_PORTABLE_HASH)
#include <CommonCrypto/CommonDigest.h>
#endif

/* SHA-256 content revisions, not an authentication API. */
typedef struct {
#if defined(__APPLE__) && !defined(AH_PORTABLE_HASH)
    CC_SHA256_CTX native;
#else
    uint32_t state[8];
    uint64_t bytes;
    unsigned char block[64];
    size_t used;
#endif
} ah_hash;
void ah_hash_init(ah_hash *hash);
void ah_hash_update(ah_hash *hash, const void *data, size_t length);
void ah_hash_finish(ah_hash *hash, char hex[65]);
void ah_hash_bytes(const void *data, size_t length, char hex[65]);
#endif
