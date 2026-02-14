/*
 * sh_hash.h - FNV-1a Hash Functions
 *
 * Provides 32-bit and 64-bit FNV-1a hash functions for byte buffers
 * and null-terminated strings. Used for hash tables, content fingerprinting,
 * and change detection across OTTO modules.
 */

#ifndef SHARED_SH_HASH_H
#define SHARED_SH_HASH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* FNV-1a constants */
#define SH_FNV1A_64_OFFSET 14695981039346656037ULL
#define SH_FNV1A_64_PRIME  1099511628211ULL
#define SH_FNV1A_32_OFFSET 2166136261u
#define SH_FNV1A_32_PRIME  16777619u

/* 64-bit FNV-1a hash of a byte buffer */
static inline uint64_t sh_fnv1a_64(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = SH_FNV1A_64_OFFSET;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= SH_FNV1A_64_PRIME;
    }
    return h;
}

/* 64-bit FNV-1a hash of a null-terminated string */
static inline uint64_t sh_fnv1a_64_str(const char *s)
{
    return sh_fnv1a_64(s, strlen(s));
}

/* 32-bit FNV-1a hash of a byte buffer */
static inline uint32_t sh_fnv1a_32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = SH_FNV1A_32_OFFSET;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= SH_FNV1A_32_PRIME;
    }
    return h;
}

/* 32-bit FNV-1a hash of a null-terminated string */
static inline uint32_t sh_fnv1a_32_str(const char *s)
{
    return sh_fnv1a_32(s, strlen(s));
}

#endif /* SHARED_SH_HASH_H */
