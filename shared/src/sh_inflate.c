/*
 * sh_inflate.c - Zlib compression/decompression wrapper
 *
 * Uses miniz for zlib/deflate decompression of PBF blobs
 * and compression for PNG/MVT output.
 */

#include "sh_inflate.h"

/* Configure miniz: we only need inflate/deflate, not ZIP */
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
 * Decompression
 * ============================================================================ */

SHStatus sh_inflate(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t dst_len,
                    size_t *actual_len)
{
    if (!src || !dst || !actual_len) {
        return SH_ERROR_INVALID_PARAM;
    }

    mz_stream stream;
    memset(&stream, 0, sizeof(stream));

    stream.next_in = src;
    stream.avail_in = (mz_uint32)src_len;
    stream.next_out = dst;
    stream.avail_out = (mz_uint32)dst_len;

    int status = mz_inflateInit(&stream);
    if (status != MZ_OK) {
        return SH_ERROR_DECOMPRESS;
    }

    status = mz_inflate(&stream, MZ_FINISH);

    if (status != MZ_STREAM_END) {
        mz_inflateEnd(&stream);
        return SH_ERROR_DECOMPRESS;
    }

    *actual_len = stream.total_out;
    mz_inflateEnd(&stream);
    return SH_OK;
}

SHStatus sh_inflate_raw(const uint8_t *src, size_t src_len,
                        uint8_t *dst, size_t dst_len,
                        size_t *actual_len)
{
    if (!src || !dst || !actual_len) {
        return SH_ERROR_INVALID_PARAM;
    }

    tinfl_status status = tinfl_decompress_mem_to_mem(
        dst, dst_len,
        src, src_len,
        TINFL_FLAG_PARSE_ZLIB_HEADER
    );

    if (status == TINFL_STATUS_DONE || (int)status >= 0) {
        *actual_len = (size_t)status;
        return SH_OK;
    }

    return SH_ERROR_DECOMPRESS;
}

uint8_t *sh_inflate_alloc(const uint8_t *src, size_t src_len,
                          size_t expected_len, size_t *actual_len)
{
    if (!src || expected_len == 0) {
        return NULL;
    }

    uint8_t *dst = malloc(expected_len);
    if (!dst) {
        return NULL;
    }

    SHStatus status = sh_inflate(src, src_len, dst, expected_len, actual_len);
    if (status != SH_OK) {
        free(dst);
        return NULL;
    }

    return dst;
}

/* ============================================================================
 * Compression
 * ============================================================================ */

SHStatus sh_deflate(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t dst_capacity,
                    size_t *actual_len, int level)
{
    if (!src || !dst || !actual_len) {
        return SH_ERROR_INVALID_PARAM;
    }

    mz_ulong dst_len = (mz_ulong)dst_capacity;

    int status = mz_compress2(dst, &dst_len, src, (mz_ulong)src_len, level);
    if (status != MZ_OK) {
        return SH_ERROR_COMPRESS;
    }

    *actual_len = (size_t)dst_len;
    return SH_OK;
}
