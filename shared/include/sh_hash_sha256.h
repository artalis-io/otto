/*
 * sh_hash_sha256.h - SHA-256 Hash for OTTO Platform
 *
 * Standalone FIPS 180-4 SHA-256 implementation. Zero external dependencies.
 * Used for file fingerprinting in the Nexus ingestion pipeline.
 *
 * Usage:
 *   char hex[65];
 *   sh_sha256_hex(data, len, hex);
 *   printf("SHA-256: %s\n", hex);
 *
 * Incremental usage:
 *   ShSha256 ctx;
 *   sh_sha256_init(&ctx);
 *   sh_sha256_update(&ctx, chunk1, len1);
 *   sh_sha256_update(&ctx, chunk2, len2);
 *   uint8_t digest[32];
 *   sh_sha256_final(&ctx, digest);
 */

#ifndef SH_HASH_SHA256_H
#define SH_HASH_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Data Structures
 * ============================================================================ */

typedef struct {
    uint32_t state[8];    /* Hash state (H0..H7) */
    uint64_t count;       /* Total bytes processed */
    uint8_t buffer[64];   /* Partial block buffer */
} ShSha256;

/* ============================================================================
 * API
 * ============================================================================ */

/*
 * Initialize SHA-256 context with initial hash values.
 */
void sh_sha256_init(ShSha256 *ctx);

/*
 * Feed data into the hash. Can be called multiple times.
 *
 * @param ctx   Context (must be initialized)
 * @param data  Data to hash (NULL with len=0 is a no-op)
 * @param len   Length of data
 */
void sh_sha256_update(ShSha256 *ctx, const void *data, size_t len);

/*
 * Finalize hash and produce 32-byte digest.
 * Context should not be reused after this call.
 *
 * @param ctx    Context
 * @param digest Output buffer (32 bytes)
 */
void sh_sha256_final(ShSha256 *ctx, uint8_t digest[32]);

/*
 * Convenience: hash bytes and produce 64-char lowercase hex string.
 *
 * @param data  Data to hash
 * @param len   Length of data
 * @param out   Output buffer (65 bytes: 64 hex chars + null terminator)
 */
void sh_sha256_hex(const void *data, size_t len, char out[65]);

#ifdef __cplusplus
}
#endif

#endif /* SH_HASH_SHA256_H */
