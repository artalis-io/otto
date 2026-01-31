/*
 * sh_protobuf.h - Minimal Protocol Buffers encoding/decoding
 *
 * Implements just enough protobuf decoding/encoding to parse OSM PBF files
 * and generate MVT tiles. Supports varint, zigzag, length-delimited, and
 * fixed-width fields.
 *
 * Used by both velo (routing) and carta (tiles).
 */

#ifndef SH_PROTOBUF_H
#define SH_PROTOBUF_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Wire Types
 * ============================================================================ */

#define SH_PB_WIRE_VARINT       0
#define SH_PB_WIRE_FIXED64      1
#define SH_PB_WIRE_LENGTH_DELIM 2
#define SH_PB_WIRE_START_GROUP  3  /* Deprecated */
#define SH_PB_WIRE_END_GROUP    4  /* Deprecated */
#define SH_PB_WIRE_FIXED32      5

/* ============================================================================
 * Varint Reading
 *
 * All read functions return the number of bytes consumed, or 0 on error.
 * ============================================================================ */

/*
 * Read an unsigned varint from buffer.
 * Returns number of bytes consumed, or 0 on error.
 */
int sh_pb_read_varint(const uint8_t *buf, size_t len, uint64_t *value);

/*
 * Read a signed varint using zigzag encoding.
 * Returns number of bytes consumed, or 0 on error.
 */
int sh_pb_read_svarint(const uint8_t *buf, size_t len, int64_t *value);

/* ============================================================================
 * Field Tag Decoding
 * ============================================================================ */

/*
 * Read a field tag (field number + wire type).
 * Returns number of bytes consumed, or 0 on error.
 */
int sh_pb_read_tag(const uint8_t *buf, size_t len, uint32_t *field, uint32_t *wire_type);

/*
 * Skip a field based on its wire type.
 * Returns number of bytes to skip (excluding the tag), or 0 on error.
 */
int sh_pb_skip_field(const uint8_t *buf, size_t len, uint32_t wire_type);

/* ============================================================================
 * Fixed-Width Reading
 * ============================================================================ */

/*
 * Read a 32-bit little-endian value.
 * Returns 4 on success, 0 on error.
 */
int sh_pb_read_fixed32(const uint8_t *buf, size_t len, uint32_t *value);

/*
 * Read a 64-bit little-endian value.
 * Returns 8 on success, 0 on error.
 */
int sh_pb_read_fixed64(const uint8_t *buf, size_t len, uint64_t *value);

/* ============================================================================
 * Packed Array Reading (Direct to Array)
 * ============================================================================ */

/*
 * Read packed unsigned varints directly into array.
 * Returns number of values read.
 */
size_t sh_pb_read_packed_varint_array(const uint8_t *buf, size_t len,
                                      uint64_t *out, size_t out_capacity);

/*
 * Read packed signed varints directly into array (zigzag decoded).
 * Returns number of values read.
 */
size_t sh_pb_read_packed_svarint_array(const uint8_t *buf, size_t len,
                                       int64_t *out, size_t out_capacity);

/*
 * Count number of varints in a packed field.
 */
size_t sh_pb_count_packed_varint(const uint8_t *buf, size_t len);

/* ============================================================================
 * Delta Decoding (for DenseNodes)
 * ============================================================================ */

/*
 * Delta-decode an array of signed integers in place.
 * arr[i] becomes arr[i] + arr[i-1] (with arr[-1] = 0).
 */
void sh_pb_delta_decode_i64(int64_t *arr, size_t count);

/*
 * Delta-decode an array of unsigned integers in place.
 */
void sh_pb_delta_decode_u64(uint64_t *arr, size_t count);

/* ============================================================================
 * Varint Writing (for MVT encoding)
 * ============================================================================ */

/*
 * Write an unsigned varint to buffer.
 * Returns number of bytes written, or 0 if buffer too small.
 */
int sh_pb_write_varint(uint8_t *buf, size_t capacity, uint64_t value);

/*
 * Write a signed varint using zigzag encoding.
 * Returns number of bytes written, or 0 if buffer too small.
 */
int sh_pb_write_svarint(uint8_t *buf, size_t capacity, int64_t value);

/*
 * Write a field tag (field number + wire type).
 * Returns number of bytes written, or 0 if buffer too small.
 */
int sh_pb_write_tag(uint8_t *buf, size_t capacity, uint32_t field, uint32_t wire_type);

/*
 * Write a length prefix (for length-delimited fields).
 * Returns number of bytes written, or 0 if buffer too small.
 */
int sh_pb_write_length(uint8_t *buf, size_t capacity, size_t length);

/*
 * Write a string (length prefix + data).
 * Returns total bytes written, or 0 if buffer too small.
 */
int sh_pb_write_string(uint8_t *buf, size_t capacity, const char *str, size_t len);

/*
 * Write a 32-bit little-endian value.
 * Returns 4 on success, 0 if buffer too small.
 */
int sh_pb_write_fixed32(uint8_t *buf, size_t capacity, uint32_t value);

/*
 * Write a 64-bit little-endian value.
 * Returns 8 on success, 0 if buffer too small.
 */
int sh_pb_write_fixed64(uint8_t *buf, size_t capacity, uint64_t value);

#ifdef __cplusplus
}
#endif

#endif /* SH_PROTOBUF_H */
