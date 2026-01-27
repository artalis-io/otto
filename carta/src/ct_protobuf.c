/*
 * ct_protobuf.c - Minimal protobuf read/write implementation
 *
 * Read functions for parsing PBF files.
 * Write functions for generating MVT files.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ============================================================================
 * Protobuf Wire Types
 * ============================================================================ */

#define PB_WIRE_VARINT  0
#define PB_WIRE_FIXED64 1
#define PB_WIRE_LENGTH  2
#define PB_WIRE_FIXED32 5

/* ============================================================================
 * Varint Reading
 * ============================================================================ */

int ct_pb_read_varint(const uint8_t *buf, size_t len, uint64_t *value)
{
    *value = 0;
    int shift = 0;
    size_t i = 0;

    while (i < len && i < 10) {
        uint8_t b = buf[i];
        *value |= ((uint64_t)(b & 0x7F)) << shift;
        i++;
        if (!(b & 0x80)) {
            return (int)i;
        }
        shift += 7;
    }

    return -1;  /* Invalid varint */
}

int ct_pb_read_svarint(const uint8_t *buf, size_t len, int64_t *value)
{
    uint64_t uval;
    int bytes = ct_pb_read_varint(buf, len, &uval);
    if (bytes < 0) return -1;

    /* Zigzag decode */
    *value = (int64_t)((uval >> 1) ^ -(int64_t)(uval & 1));
    return bytes;
}

int ct_pb_read_tag(const uint8_t *buf, size_t len, uint32_t *field, uint32_t *wire)
{
    uint64_t val;
    int bytes = ct_pb_read_varint(buf, len, &val);
    if (bytes < 0) return -1;

    *field = (uint32_t)(val >> 3);
    *wire = (uint32_t)(val & 0x07);
    return bytes;
}

int ct_pb_skip_field(const uint8_t *buf, size_t len, uint32_t wire_type)
{
    switch (wire_type) {
        case PB_WIRE_VARINT: {
            uint64_t dummy;
            return ct_pb_read_varint(buf, len, &dummy);
        }
        case PB_WIRE_FIXED64:
            return len >= 8 ? 8 : -1;
        case PB_WIRE_LENGTH: {
            uint64_t length;
            int n = ct_pb_read_varint(buf, len, &length);
            if (n < 0 || (size_t)n + length > len) return -1;
            return n + (int)length;
        }
        case PB_WIRE_FIXED32:
            return len >= 4 ? 4 : -1;
        default:
            return -1;
    }
}

int ct_pb_read_fixed32(const uint8_t *buf, size_t len, uint32_t *value)
{
    if (len < 4) return -1;
    *value = (uint32_t)buf[0] |
             ((uint32_t)buf[1] << 8) |
             ((uint32_t)buf[2] << 16) |
             ((uint32_t)buf[3] << 24);
    return 4;
}

/* ============================================================================
 * Packed Array Reading
 * ============================================================================ */

size_t ct_pb_read_packed_svarint_array(const uint8_t *buf, size_t len,
                                       int64_t *out, size_t out_capacity)
{
    size_t count = 0;
    size_t pos = 0;

    while (pos < len && count < out_capacity) {
        int n = ct_pb_read_svarint(buf + pos, len - pos, &out[count]);
        if (n < 0) break;
        pos += n;
        count++;
    }

    return count;
}

void ct_pb_delta_decode_i64(int64_t *arr, size_t count)
{
    if (count < 2) return;

    for (size_t i = 1; i < count; i++) {
        arr[i] += arr[i - 1];
    }
}

/* ============================================================================
 * Varint Writing
 * ============================================================================ */

int ct_pb_write_varint(uint8_t *buf, size_t capacity, uint64_t value)
{
    int bytes = 0;

    while (value > 0x7F && (size_t)bytes < capacity) {
        buf[bytes++] = (uint8_t)(value & 0x7F) | 0x80;
        value >>= 7;
    }

    if ((size_t)bytes < capacity) {
        buf[bytes++] = (uint8_t)(value & 0x7F);
    }

    return bytes;
}

int ct_pb_write_svarint(uint8_t *buf, size_t capacity, int64_t value)
{
    /* Zigzag encode */
    uint64_t uval = (uint64_t)((value << 1) ^ (value >> 63));
    return ct_pb_write_varint(buf, capacity, uval);
}

int ct_pb_write_tag(uint8_t *buf, size_t capacity, uint32_t field, uint32_t wire)
{
    return ct_pb_write_varint(buf, capacity, ((uint64_t)field << 3) | wire);
}

int ct_pb_write_length(uint8_t *buf, size_t capacity, size_t length)
{
    return ct_pb_write_varint(buf, capacity, (uint64_t)length);
}

int ct_pb_write_string(uint8_t *buf, size_t capacity, const char *str, size_t len)
{
    int n = ct_pb_write_length(buf, capacity, len);
    if (n < 0 || (size_t)n + len > capacity) return -1;
    memcpy(buf + n, str, len);
    return n + (int)len;
}

int ct_pb_write_fixed32(uint8_t *buf, size_t capacity, uint32_t value)
{
    if (capacity < 4) return -1;
    buf[0] = (uint8_t)(value & 0xFF);
    buf[1] = (uint8_t)((value >> 8) & 0xFF);
    buf[2] = (uint8_t)((value >> 16) & 0xFF);
    buf[3] = (uint8_t)((value >> 24) & 0xFF);
    return 4;
}
