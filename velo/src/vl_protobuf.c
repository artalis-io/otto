/*
 * vl_protobuf.c - Minimal Protocol Buffers decoder
 *
 * Implements just enough protobuf decoding to parse OSM PBF files.
 * Supports varint, zigzag, length-delimited, and fixed-width fields.
 */

#include "vl_types.h"
#include <stdint.h>
#include <string.h>

/* ============================================================================
 * Wire Types
 * ============================================================================ */

#define PB_WIRE_VARINT          0
#define PB_WIRE_FIXED64         1
#define PB_WIRE_LENGTH_DELIM    2
#define PB_WIRE_START_GROUP     3  /* Deprecated */
#define PB_WIRE_END_GROUP       4  /* Deprecated */
#define PB_WIRE_FIXED32         5

/* ============================================================================
 * Varint Decoding
 * ============================================================================ */

/*
 * Read a varint from buffer.
 * Returns number of bytes consumed, or 0 on error.
 */
int vl_pb_read_varint(const uint8_t *buf, size_t len, uint64_t *value)
{
    if (len == 0 || !buf || !value) return 0;

    *value = 0;
    int shift = 0;
    size_t i = 0;

    do {
        if (i >= len || shift >= 64) return 0;

        uint64_t byte = buf[i];
        *value |= (byte & 0x7F) << shift;
        shift += 7;
        i++;

        if ((byte & 0x80) == 0) {
            return (int)i;
        }
    } while (1);
}

/*
 * Read a signed varint using zigzag encoding.
 */
int vl_pb_read_svarint(const uint8_t *buf, size_t len, int64_t *value)
{
    uint64_t uval;
    int n = vl_pb_read_varint(buf, len, &uval);
    if (n == 0) return 0;

    /* Zigzag decode: (n >> 1) ^ -(n & 1) */
    *value = (int64_t)((uval >> 1) ^ (uint64_t)(-(int64_t)(uval & 1)));
    return n;
}

/* ============================================================================
 * Field Tag Decoding
 * ============================================================================ */

/*
 * Read a field tag (field number + wire type).
 */
int vl_pb_read_tag(const uint8_t *buf, size_t len, uint32_t *field, uint32_t *wire_type)
{
    uint64_t tag;
    int n = vl_pb_read_varint(buf, len, &tag);
    if (n == 0) return 0;

    *field = (uint32_t)(tag >> 3);
    *wire_type = (uint32_t)(tag & 0x7);
    return n;
}

/* ============================================================================
 * Field Skipping
 * ============================================================================ */

/*
 * Skip a field based on its wire type.
 * Returns number of bytes to skip (excluding the tag), or 0 on error.
 */
int vl_pb_skip_field(const uint8_t *buf, size_t len, uint32_t wire_type)
{
    switch (wire_type) {
    case PB_WIRE_VARINT: {
        uint64_t dummy;
        return vl_pb_read_varint(buf, len, &dummy);
    }

    case PB_WIRE_FIXED64:
        return (len >= 8) ? 8 : 0;

    case PB_WIRE_FIXED32:
        return (len >= 4) ? 4 : 0;

    case PB_WIRE_LENGTH_DELIM: {
        uint64_t field_len;
        int n = vl_pb_read_varint(buf, len, &field_len);
        if (n == 0 || (size_t)(n + field_len) > len) return 0;
        return n + (int)field_len;
    }

    default:
        /* Unknown or deprecated wire type */
        return 0;
    }
}

/* ============================================================================
 * Fixed-Width Reading
 * ============================================================================ */

/*
 * Read a 32-bit little-endian value.
 */
int vl_pb_read_fixed32(const uint8_t *buf, size_t len, uint32_t *value)
{
    if (len < 4) return 0;
    *value = (uint32_t)buf[0] |
             ((uint32_t)buf[1] << 8) |
             ((uint32_t)buf[2] << 16) |
             ((uint32_t)buf[3] << 24);
    return 4;
}

/*
 * Read a 64-bit little-endian value.
 */
int vl_pb_read_fixed64(const uint8_t *buf, size_t len, uint64_t *value)
{
    if (len < 8) return 0;
    *value = (uint64_t)buf[0] |
             ((uint64_t)buf[1] << 8) |
             ((uint64_t)buf[2] << 16) |
             ((uint64_t)buf[3] << 24) |
             ((uint64_t)buf[4] << 32) |
             ((uint64_t)buf[5] << 40) |
             ((uint64_t)buf[6] << 48) |
             ((uint64_t)buf[7] << 56);
    return 8;
}

/* ============================================================================
 * Packed Repeated Field Decoding
 * ============================================================================ */

/*
 * Read a packed repeated varint field.
 * Calls callback for each value.
 * Returns VL_OK on success.
 */
VLStatus vl_pb_read_packed_varint(const uint8_t *buf, size_t len,
                                  void (*callback)(uint64_t value, void *ctx),
                                  void *ctx)
{
    size_t offset = 0;
    while (offset < len) {
        uint64_t value;
        int n = vl_pb_read_varint(buf + offset, len - offset, &value);
        if (n == 0) return VL_ERROR_PARSE_ERROR;
        callback(value, ctx);
        offset += n;
    }
    return VL_OK;
}

/*
 * Read a packed repeated signed varint field (zigzag encoded).
 */
VLStatus vl_pb_read_packed_svarint(const uint8_t *buf, size_t len,
                                   void (*callback)(int64_t value, void *ctx),
                                   void *ctx)
{
    size_t offset = 0;
    while (offset < len) {
        int64_t value;
        int n = vl_pb_read_svarint(buf + offset, len - offset, &value);
        if (n == 0) return VL_ERROR_PARSE_ERROR;
        callback(value, ctx);
        offset += n;
    }
    return VL_OK;
}

/*
 * Count number of varints in a packed field.
 */
size_t vl_pb_count_packed_varint(const uint8_t *buf, size_t len)
{
    size_t count = 0;
    size_t offset = 0;
    while (offset < len) {
        uint64_t dummy;
        int n = vl_pb_read_varint(buf + offset, len - offset, &dummy);
        if (n == 0) break;
        count++;
        offset += n;
    }
    return count;
}

/* ============================================================================
 * Packed Array Reading (Direct to Array)
 * ============================================================================ */

/*
 * Read packed varints directly into array.
 * Returns number of values read.
 */
size_t vl_pb_read_packed_varint_array(const uint8_t *buf, size_t len,
                                      uint64_t *out, size_t out_capacity)
{
    size_t count = 0;
    size_t offset = 0;
    while (offset < len && count < out_capacity) {
        int n = vl_pb_read_varint(buf + offset, len - offset, &out[count]);
        if (n == 0) break;
        count++;
        offset += n;
    }
    return count;
}

/*
 * Read packed signed varints directly into array (zigzag decoded).
 */
size_t vl_pb_read_packed_svarint_array(const uint8_t *buf, size_t len,
                                       int64_t *out, size_t out_capacity)
{
    size_t count = 0;
    size_t offset = 0;
    while (offset < len && count < out_capacity) {
        int n = vl_pb_read_svarint(buf + offset, len - offset, &out[count]);
        if (n == 0) break;
        count++;
        offset += n;
    }
    return count;
}

/* ============================================================================
 * Delta Decoding (for DenseNodes)
 * ============================================================================ */

/*
 * Delta-decode an array of signed integers in place.
 * arr[i] becomes arr[i] + arr[i-1] (with arr[-1] = 0).
 */
void vl_pb_delta_decode_i64(int64_t *arr, size_t count)
{
    if (count == 0) return;
    for (size_t i = 1; i < count; i++) {
        arr[i] += arr[i - 1];
    }
}

/*
 * Delta-decode an array of unsigned integers in place.
 */
void vl_pb_delta_decode_u64(uint64_t *arr, size_t count)
{
    if (count == 0) return;
    for (size_t i = 1; i < count; i++) {
        arr[i] += arr[i - 1];
    }
}
