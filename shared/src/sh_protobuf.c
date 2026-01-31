/*
 * sh_protobuf.c - Minimal Protocol Buffers encoding/decoding
 *
 * Implements just enough protobuf decoding/encoding to parse OSM PBF files
 * and generate MVT tiles. Supports varint, zigzag, length-delimited, and
 * fixed-width fields.
 */

#include "sh_protobuf.h"
#include <string.h>

/* ============================================================================
 * Varint Reading
 * ============================================================================ */

int sh_pb_read_varint(const uint8_t *buf, size_t len, uint64_t *value)
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

int sh_pb_read_svarint(const uint8_t *buf, size_t len, int64_t *value)
{
    uint64_t uval;
    int n = sh_pb_read_varint(buf, len, &uval);
    if (n == 0) return 0;

    /* Zigzag decode: (n >> 1) ^ -(n & 1) */
    *value = (int64_t)((uval >> 1) ^ (uint64_t)(-(int64_t)(uval & 1)));
    return n;
}

/* ============================================================================
 * Field Tag Decoding
 * ============================================================================ */

int sh_pb_read_tag(const uint8_t *buf, size_t len, uint32_t *field, uint32_t *wire_type)
{
    uint64_t tag;
    int n = sh_pb_read_varint(buf, len, &tag);
    if (n == 0) return 0;

    *field = (uint32_t)(tag >> 3);
    *wire_type = (uint32_t)(tag & 0x7);
    return n;
}

int sh_pb_skip_field(const uint8_t *buf, size_t len, uint32_t wire_type)
{
    switch (wire_type) {
    case SH_PB_WIRE_VARINT: {
        uint64_t dummy;
        return sh_pb_read_varint(buf, len, &dummy);
    }

    case SH_PB_WIRE_FIXED64:
        return (len >= 8) ? 8 : 0;

    case SH_PB_WIRE_FIXED32:
        return (len >= 4) ? 4 : 0;

    case SH_PB_WIRE_LENGTH_DELIM: {
        uint64_t field_len;
        int n = sh_pb_read_varint(buf, len, &field_len);
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

int sh_pb_read_fixed32(const uint8_t *buf, size_t len, uint32_t *value)
{
    if (len < 4) return 0;
    *value = (uint32_t)buf[0] |
             ((uint32_t)buf[1] << 8) |
             ((uint32_t)buf[2] << 16) |
             ((uint32_t)buf[3] << 24);
    return 4;
}

int sh_pb_read_fixed64(const uint8_t *buf, size_t len, uint64_t *value)
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
 * Packed Array Reading
 * ============================================================================ */

size_t sh_pb_read_packed_varint_array(const uint8_t *buf, size_t len,
                                      uint64_t *out, size_t out_capacity)
{
    size_t count = 0;
    size_t offset = 0;
    while (offset < len && count < out_capacity) {
        int n = sh_pb_read_varint(buf + offset, len - offset, &out[count]);
        if (n == 0) break;
        count++;
        offset += n;
    }
    return count;
}

size_t sh_pb_read_packed_svarint_array(const uint8_t *buf, size_t len,
                                       int64_t *out, size_t out_capacity)
{
    size_t count = 0;
    size_t offset = 0;
    while (offset < len && count < out_capacity) {
        int n = sh_pb_read_svarint(buf + offset, len - offset, &out[count]);
        if (n == 0) break;
        count++;
        offset += n;
    }
    return count;
}

size_t sh_pb_count_packed_varint(const uint8_t *buf, size_t len)
{
    size_t count = 0;
    size_t offset = 0;
    while (offset < len) {
        uint64_t dummy;
        int n = sh_pb_read_varint(buf + offset, len - offset, &dummy);
        if (n == 0) break;
        count++;
        offset += n;
    }
    return count;
}

/* ============================================================================
 * Delta Decoding
 * ============================================================================ */

void sh_pb_delta_decode_i64(int64_t *arr, size_t count)
{
    if (count < 2) return;
    for (size_t i = 1; i < count; i++) {
        arr[i] += arr[i - 1];
    }
}

void sh_pb_delta_decode_u64(uint64_t *arr, size_t count)
{
    if (count < 2) return;
    for (size_t i = 1; i < count; i++) {
        arr[i] += arr[i - 1];
    }
}

/* ============================================================================
 * Varint Writing
 * ============================================================================ */

int sh_pb_write_varint(uint8_t *buf, size_t capacity, uint64_t value)
{
    int bytes = 0;

    while (value > 0x7F && (size_t)bytes < capacity) {
        buf[bytes++] = (uint8_t)(value & 0x7F) | 0x80;
        value >>= 7;
    }

    if ((size_t)bytes < capacity) {
        buf[bytes++] = (uint8_t)(value & 0x7F);
    } else {
        return 0;  /* Buffer too small */
    }

    return bytes;
}

int sh_pb_write_svarint(uint8_t *buf, size_t capacity, int64_t value)
{
    /* Zigzag encode: (value << 1) ^ (value >> 63) */
    uint64_t uval = (uint64_t)((value << 1) ^ (value >> 63));
    return sh_pb_write_varint(buf, capacity, uval);
}

int sh_pb_write_tag(uint8_t *buf, size_t capacity, uint32_t field, uint32_t wire_type)
{
    return sh_pb_write_varint(buf, capacity, ((uint64_t)field << 3) | wire_type);
}

int sh_pb_write_length(uint8_t *buf, size_t capacity, size_t length)
{
    return sh_pb_write_varint(buf, capacity, (uint64_t)length);
}

int sh_pb_write_string(uint8_t *buf, size_t capacity, const char *str, size_t len)
{
    int n = sh_pb_write_length(buf, capacity, len);
    if (n == 0 || (size_t)n + len > capacity) return 0;
    memcpy(buf + n, str, len);
    return n + (int)len;
}

int sh_pb_write_fixed32(uint8_t *buf, size_t capacity, uint32_t value)
{
    if (capacity < 4) return 0;
    buf[0] = (uint8_t)(value & 0xFF);
    buf[1] = (uint8_t)((value >> 8) & 0xFF);
    buf[2] = (uint8_t)((value >> 16) & 0xFF);
    buf[3] = (uint8_t)((value >> 24) & 0xFF);
    return 4;
}

int sh_pb_write_fixed64(uint8_t *buf, size_t capacity, uint64_t value)
{
    if (capacity < 8) return 0;
    buf[0] = (uint8_t)(value & 0xFF);
    buf[1] = (uint8_t)((value >> 8) & 0xFF);
    buf[2] = (uint8_t)((value >> 16) & 0xFF);
    buf[3] = (uint8_t)((value >> 24) & 0xFF);
    buf[4] = (uint8_t)((value >> 32) & 0xFF);
    buf[5] = (uint8_t)((value >> 40) & 0xFF);
    buf[6] = (uint8_t)((value >> 48) & 0xFF);
    buf[7] = (uint8_t)((value >> 56) & 0xFF);
    return 8;
}
