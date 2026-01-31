/*
 * sh_pbf.h - OSM PBF low-level parsing utilities
 *
 * Provides common utilities for parsing OSM Protocol Buffer Format files:
 * - String table for tag key/value lookup
 * - PBF field number constants
 * - Blob header and data parsing
 *
 * Used by both velo (routing) and carta (tiles).
 */

#ifndef SH_PBF_H
#define SH_PBF_H

#include "sh_protobuf.h"
#include "sh_inflate.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * PBF Field Numbers (from fileformat.proto and osmformat.proto)
 * ============================================================================ */

/* BlobHeader */
#define SH_PBF_BLOBHEADER_TYPE     1
#define SH_PBF_BLOBHEADER_DATASIZE 3

/* Blob */
#define SH_PBF_BLOB_RAW            1
#define SH_PBF_BLOB_RAW_SIZE       2
#define SH_PBF_BLOB_ZLIB_DATA      3

/* PrimitiveBlock */
#define SH_PBF_PRIMBLOCK_STRINGTABLE   1
#define SH_PBF_PRIMBLOCK_PRIMITIVEGROUP 2
#define SH_PBF_PRIMBLOCK_GRANULARITY   17
#define SH_PBF_PRIMBLOCK_LAT_OFFSET    19
#define SH_PBF_PRIMBLOCK_LON_OFFSET    20

/* StringTable */
#define SH_PBF_STRINGTABLE_S       1

/* PrimitiveGroup */
#define SH_PBF_PRIMGROUP_NODES     1
#define SH_PBF_PRIMGROUP_DENSE     2
#define SH_PBF_PRIMGROUP_WAYS      3
#define SH_PBF_PRIMGROUP_RELATIONS 4

/* DenseNodes */
#define SH_PBF_DENSE_ID            1
#define SH_PBF_DENSE_LAT           8
#define SH_PBF_DENSE_LON           9
#define SH_PBF_DENSE_KEYS_VALS     10

/* Way */
#define SH_PBF_WAY_ID              1
#define SH_PBF_WAY_KEYS            2
#define SH_PBF_WAY_VALS            3
#define SH_PBF_WAY_REFS            8

/* ============================================================================
 * String Table
 *
 * Each PrimitiveBlock has a string table for tag key/value lookup.
 * ============================================================================ */

typedef struct {
    char **strings;
    size_t count;
    size_t capacity;
} SHStringTable;

/*
 * Initialize a string table to empty state.
 */
void sh_string_table_init(SHStringTable *st);

/*
 * Free all memory used by a string table.
 */
void sh_string_table_free(SHStringTable *st);

/*
 * Add a string to the string table (copies the data).
 * Returns SH_OK on success.
 */
SHStatus sh_string_table_add(SHStringTable *st, const uint8_t *data, size_t len);

/*
 * Get a string by index. Returns empty string if index out of bounds.
 */
const char *sh_string_table_get(const SHStringTable *st, size_t idx);

/*
 * Parse a StringTable message and populate the string table.
 * data/len point to the contents of the StringTable message.
 */
SHStatus sh_string_table_parse(SHStringTable *st, const uint8_t *data, size_t len);

/* ============================================================================
 * Blob Parsing
 * ============================================================================ */

/* Blob parsing result */
typedef struct {
    const uint8_t *data;     /* Decompressed data pointer */
    size_t len;              /* Decompressed data length */
    bool is_header;          /* True if this is an OSMHeader blob */
    uint8_t *decompressed;   /* Non-NULL if data was decompressed (caller frees) */
} SHPBFBlob;

/*
 * Parse a BlobHeader message.
 *
 * data/len: BlobHeader protobuf data
 * type_out: buffer for blob type string (e.g., "OSMHeader", "OSMData")
 * type_capacity: size of type_out buffer
 * datasize_out: receives the blob data size
 * consumed: receives number of bytes consumed
 *
 * Returns SH_OK on success.
 */
SHStatus sh_pbf_parse_blob_header(const uint8_t *data, size_t len,
                                  char *type_out, size_t type_capacity,
                                  uint32_t *datasize_out, size_t *consumed);

/*
 * Parse and decompress a Blob message.
 *
 * data/len: Blob protobuf data
 * out: receives blob contents
 *
 * On success, out->data points to decompressed data. If out->decompressed
 * is non-NULL, caller must free() it.
 *
 * Returns SH_OK on success.
 */
SHStatus sh_pbf_decompress_blob(const uint8_t *data, size_t len, SHPBFBlob *out);

/*
 * Free memory allocated by sh_pbf_decompress_blob().
 */
void sh_pbf_blob_free(SHPBFBlob *blob);

/* ============================================================================
 * PrimitiveBlock Parsing Helpers
 * ============================================================================ */

/* Block header info (granularity and offsets) */
typedef struct {
    int32_t granularity;     /* Coordinate granularity (default 100) */
    int64_t lat_offset;      /* Latitude offset (nanodegrees) */
    int64_t lon_offset;      /* Longitude offset (nanodegrees) */
} SHBlockHeader;

/*
 * Initialize block header with default values.
 */
void sh_block_header_init(SHBlockHeader *header);

/*
 * Parse block header fields (granularity, lat_offset, lon_offset) from
 * a PrimitiveBlock message. Only parses header fields, skips content.
 *
 * This is typically called before processing DenseNodes to get the
 * coordinate conversion parameters.
 */
SHStatus sh_pbf_parse_block_header(const uint8_t *data, size_t len,
                                   SHBlockHeader *header);

#ifdef __cplusplus
}
#endif

#endif /* SH_PBF_H */
