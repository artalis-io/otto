/*
 * sh_inflate.h - Zlib compression/decompression wrapper
 *
 * Uses miniz for zlib/deflate decompression of PBF blobs
 * and compression for PNG/MVT output.
 *
 * Used by both velo (routing) and carta (tiles).
 */

#ifndef SH_INFLATE_H
#define SH_INFLATE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Status Codes
 * ============================================================================ */

typedef enum {
    SH_OK = 0,
    SH_ERROR_INVALID_PARAM,
    SH_ERROR_DECOMPRESS,
    SH_ERROR_COMPRESS,
    SH_ERROR_OUT_OF_MEMORY
} SHStatus;

/* ============================================================================
 * Decompression
 * ============================================================================ */

/*
 * Decompress zlib-compressed data (with zlib header).
 *
 * src: compressed data
 * src_len: length of compressed data
 * dst: output buffer (must be pre-allocated)
 * dst_len: size of output buffer
 * actual_len: (out) actual decompressed size
 *
 * Returns SH_OK on success.
 */
SHStatus sh_inflate(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t dst_len,
                    size_t *actual_len);

/*
 * Decompress raw deflate data (no zlib header).
 *
 * Returns SH_OK on success.
 */
SHStatus sh_inflate_raw(const uint8_t *src, size_t src_len,
                        uint8_t *dst, size_t dst_len,
                        size_t *actual_len);

/*
 * Decompress into newly allocated buffer.
 * Caller must free() the returned buffer.
 *
 * src: compressed data
 * src_len: length of compressed data
 * expected_len: expected decompressed size (buffer will be this size)
 * actual_len: (out) actual decompressed size
 *
 * Returns NULL on error.
 */
uint8_t *sh_inflate_alloc(const uint8_t *src, size_t src_len,
                          size_t expected_len, size_t *actual_len);

/* ============================================================================
 * Compression
 * ============================================================================ */

/*
 * Compress data using zlib deflate.
 *
 * src: uncompressed data
 * src_len: length of data
 * dst: output buffer (must be pre-allocated)
 * dst_capacity: size of output buffer
 * actual_len: (out) actual compressed size
 * level: compression level (0-9, 6 is default)
 *
 * Returns SH_OK on success.
 */
SHStatus sh_deflate(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t dst_capacity,
                    size_t *actual_len, int level);

#ifdef __cplusplus
}
#endif

#endif /* SH_INFLATE_H */
