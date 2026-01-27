/*
 * ct_inflate.c - Zlib decompression wrapper using miniz
 */

#include "ct_types.h"
#include "miniz.h"
#include "miniz_tinfl.h"

CTStatus ct_inflate(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t dst_len, size_t *actual_len)
{
    mz_stream stream;
    memset(&stream, 0, sizeof(stream));

    stream.next_in = src;
    stream.avail_in = (mz_uint32)src_len;
    stream.next_out = dst;
    stream.avail_out = (mz_uint32)dst_len;

    int status = mz_inflateInit(&stream);
    if (status != MZ_OK) {
        return CT_ERROR_PARSE_ERROR;
    }

    status = mz_inflate(&stream, MZ_FINISH);

    if (status != MZ_STREAM_END) {
        mz_inflateEnd(&stream);
        return CT_ERROR_PARSE_ERROR;
    }

    if (actual_len) {
        *actual_len = stream.total_out;
    }

    mz_inflateEnd(&stream);
    return CT_OK;
}

/* Deflate compression (for PNG) */
CTStatus ct_deflate(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t dst_capacity, size_t *actual_len,
                    int level)
{
    mz_ulong dst_len = (mz_ulong)dst_capacity;

    int status = mz_compress2(dst, &dst_len, src, (mz_ulong)src_len, level);
    if (status != MZ_OK) {
        return CT_ERROR_INTERNAL;
    }

    if (actual_len) {
        *actual_len = (size_t)dst_len;
    }

    return CT_OK;
}
