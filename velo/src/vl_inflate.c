/*
 * vl_inflate.c - Zlib decompression wrapper
 *
 * Uses miniz for zlib/deflate decompression of PBF blobs.
 */

#include "vl_types.h"

/* Configure miniz: we only need inflate, not deflate or ZIP */
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES

#include "miniz.h"
#include "miniz_tinfl.h"

#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Zlib Decompression
 * ============================================================================ */

/*
 * Decompress zlib-compressed data.
 *
 * src: compressed data
 * src_len: length of compressed data
 * dst: output buffer (must be pre-allocated)
 * dst_len: size of output buffer
 * actual_len: (out) actual decompressed size
 *
 * Returns VL_OK on success.
 */
VLStatus vl_inflate(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t dst_len,
                    size_t *actual_len)
{
    if (!src || !dst || !actual_len) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    mz_ulong dest_len = (mz_ulong)dst_len;

    int result = mz_uncompress(dst, &dest_len, src, (mz_ulong)src_len);

    if (result != MZ_OK) {
        switch (result) {
        case MZ_MEM_ERROR:
            return VL_ERROR_OUT_OF_MEMORY;
        case MZ_BUF_ERROR:
            return VL_ERROR_INVALID_ARGUMENT;  /* Buffer too small */
        case MZ_DATA_ERROR:
            return VL_ERROR_PARSE_ERROR;
        default:
            return VL_ERROR_INTERNAL;
        }
    }

    *actual_len = (size_t)dest_len;
    return VL_OK;
}

/*
 * Decompress raw deflate data (no zlib header).
 */
VLStatus vl_inflate_raw(const uint8_t *src, size_t src_len,
                        uint8_t *dst, size_t dst_len,
                        size_t *actual_len)
{
    if (!src || !dst || !actual_len) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    size_t in_bytes = src_len;
    size_t out_bytes = dst_len;

    tinfl_status status = tinfl_decompress_mem_to_mem(
        dst, out_bytes,
        src, in_bytes,
        TINFL_FLAG_PARSE_ZLIB_HEADER
    );

    if (status == TINFL_STATUS_DONE || (int)status >= 0) {
        *actual_len = (size_t)status;
        return VL_OK;
    }

    return VL_ERROR_PARSE_ERROR;
}

/*
 * Decompress into newly allocated buffer.
 * Caller must free() the returned buffer.
 *
 * Returns NULL on error.
 */
uint8_t *vl_inflate_alloc(const uint8_t *src, size_t src_len,
                          size_t expected_len, size_t *actual_len)
{
    if (!src || expected_len == 0) {
        return NULL;
    }

    uint8_t *dst = malloc(expected_len);
    if (!dst) {
        return NULL;
    }

    VLStatus status = vl_inflate(src, src_len, dst, expected_len, actual_len);
    if (status != VL_OK) {
        free(dst);
        return NULL;
    }

    return dst;
}
