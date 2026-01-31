/*
 * sh_pbf.c - OSM PBF low-level parsing utilities
 *
 * Provides common utilities for parsing OSM Protocol Buffer Format files:
 * - String table for tag key/value lookup
 * - Blob header and data parsing
 */

#include "sh_pbf.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * String Table
 * ============================================================================ */

void sh_string_table_init(SHStringTable *st)
{
    st->strings = NULL;
    st->count = 0;
    st->capacity = 0;
}

void sh_string_table_free(SHStringTable *st)
{
    for (size_t i = 0; i < st->count; i++) {
        free(st->strings[i]);
    }
    free(st->strings);
    sh_string_table_init(st);
}

SHStatus sh_string_table_add(SHStringTable *st, const uint8_t *data, size_t len)
{
    if (st->count >= st->capacity) {
        size_t new_cap = st->capacity ? st->capacity * 2 : 256;
        char **new_strings = realloc(st->strings, new_cap * sizeof(char *));
        if (!new_strings) return SH_ERROR_OUT_OF_MEMORY;
        st->strings = new_strings;
        st->capacity = new_cap;
    }

    char *s = malloc(len + 1);
    if (!s) return SH_ERROR_OUT_OF_MEMORY;
    memcpy(s, data, len);
    s[len] = '\0';
    st->strings[st->count++] = s;
    return SH_OK;
}

const char *sh_string_table_get(const SHStringTable *st, size_t idx)
{
    if (idx >= st->count) return "";
    return st->strings[idx];
}

SHStatus sh_string_table_parse(SHStringTable *st, const uint8_t *data, size_t len)
{
    size_t pos = 0;

    while (pos < len) {
        uint32_t field, wire;
        int n = sh_pb_read_tag(data + pos, len - pos, &field, &wire);
        if (n == 0) return SH_ERROR_INVALID_PARAM;
        pos += n;

        if (field == SH_PBF_STRINGTABLE_S && wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t slen;
            n = sh_pb_read_varint(data + pos, len - pos, &slen);
            if (n == 0) return SH_ERROR_INVALID_PARAM;
            pos += n;

            SHStatus status = sh_string_table_add(st, data + pos, (size_t)slen);
            if (status != SH_OK) return status;
            pos += slen;
        } else {
            n = sh_pb_skip_field(data + pos, len - pos, wire);
            if (n == 0) return SH_ERROR_INVALID_PARAM;
            pos += n;
        }
    }

    return SH_OK;
}

/* ============================================================================
 * Blob Parsing
 * ============================================================================ */

SHStatus sh_pbf_parse_blob_header(const uint8_t *data, size_t len,
                                  char *type_out, size_t type_capacity,
                                  uint32_t *datasize_out, size_t *consumed)
{
    if (!data || !type_out || !datasize_out || !consumed) {
        return SH_ERROR_INVALID_PARAM;
    }

    type_out[0] = '\0';
    *datasize_out = 0;
    *consumed = len;

    size_t pos = 0;
    while (pos < len) {
        uint32_t field, wire;
        int n = sh_pb_read_tag(data + pos, len - pos, &field, &wire);
        if (n == 0) break;
        pos += n;

        if (field == SH_PBF_BLOBHEADER_TYPE && wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t slen;
            n = sh_pb_read_varint(data + pos, len - pos, &slen);
            if (n == 0) break;
            pos += n;

            if (slen < type_capacity) {
                memcpy(type_out, data + pos, (size_t)slen);
                type_out[slen] = '\0';
            }
            pos += (size_t)slen;
        } else if (field == SH_PBF_BLOBHEADER_DATASIZE && wire == SH_PB_WIRE_VARINT) {
            uint64_t val;
            n = sh_pb_read_varint(data + pos, len - pos, &val);
            if (n == 0) break;
            *datasize_out = (uint32_t)val;
            pos += n;
        } else {
            n = sh_pb_skip_field(data + pos, len - pos, wire);
            if (n == 0) break;
            pos += n;
        }
    }

    return SH_OK;
}

SHStatus sh_pbf_decompress_blob(const uint8_t *data, size_t len, SHPBFBlob *out)
{
    if (!data || !out) {
        return SH_ERROR_INVALID_PARAM;
    }

    memset(out, 0, sizeof(*out));

    const uint8_t *raw_data = NULL;
    size_t raw_len = 0;
    const uint8_t *zlib_data = NULL;
    size_t zlib_len = 0;
    size_t raw_size = 0;

    size_t pos = 0;
    while (pos < len) {
        uint32_t field, wire;
        int n = sh_pb_read_tag(data + pos, len - pos, &field, &wire);
        if (n == 0) break;
        pos += n;

        if (field == SH_PBF_BLOB_RAW_SIZE && wire == SH_PB_WIRE_VARINT) {
            uint64_t val;
            n = sh_pb_read_varint(data + pos, len - pos, &val);
            if (n == 0) break;
            raw_size = (size_t)val;
            pos += n;
        } else if (wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t field_len;
            n = sh_pb_read_varint(data + pos, len - pos, &field_len);
            if (n == 0) break;
            pos += n;

            if (field == SH_PBF_BLOB_RAW) {
                raw_data = data + pos;
                raw_len = (size_t)field_len;
            } else if (field == SH_PBF_BLOB_ZLIB_DATA) {
                zlib_data = data + pos;
                zlib_len = (size_t)field_len;
            }
            pos += (size_t)field_len;
        } else {
            n = sh_pb_skip_field(data + pos, len - pos, wire);
            if (n == 0) break;
            pos += n;
        }
    }

    /* Get decompressed data */
    if (raw_data) {
        /* Uncompressed blob */
        out->data = raw_data;
        out->len = raw_len;
        out->decompressed = NULL;
    } else if (zlib_data && raw_size > 0) {
        /* Compressed blob - decompress */
        out->decompressed = malloc(raw_size);
        if (!out->decompressed) return SH_ERROR_OUT_OF_MEMORY;

        SHStatus status = sh_inflate(zlib_data, zlib_len,
                                     out->decompressed, raw_size, &out->len);
        if (status != SH_OK) {
            free(out->decompressed);
            out->decompressed = NULL;
            return status;
        }
        out->data = out->decompressed;
    } else {
        return SH_ERROR_INVALID_PARAM;
    }

    return SH_OK;
}

void sh_pbf_blob_free(SHPBFBlob *blob)
{
    if (blob && blob->decompressed) {
        free(blob->decompressed);
        blob->decompressed = NULL;
        blob->data = NULL;
        blob->len = 0;
    }
}

/* ============================================================================
 * PrimitiveBlock Parsing Helpers
 * ============================================================================ */

void sh_block_header_init(SHBlockHeader *header)
{
    header->granularity = 100;  /* Default OSM granularity */
    header->lat_offset = 0;
    header->lon_offset = 0;
}

SHStatus sh_pbf_parse_block_header(const uint8_t *data, size_t len,
                                   SHBlockHeader *header)
{
    if (!data || !header) {
        return SH_ERROR_INVALID_PARAM;
    }

    sh_block_header_init(header);

    size_t pos = 0;
    while (pos < len) {
        uint32_t field, wire;
        int n = sh_pb_read_tag(data + pos, len - pos, &field, &wire);
        if (n == 0) break;
        pos += n;

        if (field == SH_PBF_PRIMBLOCK_GRANULARITY && wire == SH_PB_WIRE_VARINT) {
            uint64_t val;
            n = sh_pb_read_varint(data + pos, len - pos, &val);
            if (n == 0) break;
            header->granularity = (int32_t)val;
            pos += n;
        } else if (field == SH_PBF_PRIMBLOCK_LAT_OFFSET && wire == SH_PB_WIRE_VARINT) {
            int64_t val;
            n = sh_pb_read_svarint(data + pos, len - pos, &val);
            if (n == 0) break;
            header->lat_offset = val;
            pos += n;
        } else if (field == SH_PBF_PRIMBLOCK_LON_OFFSET && wire == SH_PB_WIRE_VARINT) {
            int64_t val;
            n = sh_pb_read_svarint(data + pos, len - pos, &val);
            if (n == 0) break;
            header->lon_offset = val;
            pos += n;
        } else {
            n = sh_pb_skip_field(data + pos, len - pos, wire);
            if (n == 0) break;
            pos += n;
        }
    }

    return SH_OK;
}
