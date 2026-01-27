/*
 * vl_pbf.c - OSM PBF file parser
 *
 * Parses OpenStreetMap Protocol Buffer Format files.
 * Extracts nodes and ways needed for routing.
 *
 * PBF Structure:
 *   [BlobHeader][Blob][BlobHeader][Blob]...
 *   BlobHeader: type (OSMHeader/OSMData), datasize
 *   Blob: raw or zlib-compressed data
 *
 * OSMData contains PrimitiveBlock with:
 *   - StringTable (for tag keys/values)
 *   - PrimitiveGroups containing:
 *     - DenseNodes (delta-encoded node IDs, lats, lons)
 *     - Ways (node references, tags)
 */

#include "vl_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef _WIN32
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

/* Forward declarations for protobuf functions */
int vl_pb_read_varint(const uint8_t *buf, size_t len, uint64_t *value);
int vl_pb_read_svarint(const uint8_t *buf, size_t len, int64_t *value);
int vl_pb_read_tag(const uint8_t *buf, size_t len, uint32_t *field, uint32_t *wire);
int vl_pb_skip_field(const uint8_t *buf, size_t len, uint32_t wire_type);
int vl_pb_read_fixed32(const uint8_t *buf, size_t len, uint32_t *value);
size_t vl_pb_read_packed_svarint_array(const uint8_t *buf, size_t len,
                                       int64_t *out, size_t out_capacity);
void vl_pb_delta_decode_i64(int64_t *arr, size_t count);

/* Forward declaration for inflate */
VLStatus vl_inflate(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t dst_len, size_t *actual_len);

/* ============================================================================
 * PBF Field Numbers (from fileformat.proto and osmformat.proto)
 * ============================================================================ */

/* BlobHeader */
#define PBF_BLOBHEADER_TYPE     1
#define PBF_BLOBHEADER_DATASIZE 3

/* Blob */
#define PBF_BLOB_RAW            1
#define PBF_BLOB_RAW_SIZE       2
#define PBF_BLOB_ZLIB_DATA      3

/* PrimitiveBlock */
#define PBF_PRIMBLOCK_STRINGTABLE   1
#define PBF_PRIMBLOCK_PRIMITIVEGROUP 2
#define PBF_PRIMBLOCK_GRANULARITY   17
#define PBF_PRIMBLOCK_LAT_OFFSET    19
#define PBF_PRIMBLOCK_LON_OFFSET    20

/* StringTable */
#define PBF_STRINGTABLE_S       1

/* PrimitiveGroup */
#define PBF_PRIMGROUP_NODES     1
#define PBF_PRIMGROUP_DENSE     2
#define PBF_PRIMGROUP_WAYS      3
#define PBF_PRIMGROUP_RELATIONS 4

/* DenseNodes */
#define PBF_DENSE_ID            1
#define PBF_DENSE_LAT           8
#define PBF_DENSE_LON           9
#define PBF_DENSE_KEYS_VALS     10

/* Way */
#define PBF_WAY_ID              1
#define PBF_WAY_KEYS            2
#define PBF_WAY_VALS            3
#define PBF_WAY_REFS            8

/* ============================================================================
 * String Table
 * ============================================================================ */

typedef struct {
    char **strings;
    size_t count;
    size_t capacity;
} VLStringTable;

static void string_table_init(VLStringTable *st)
{
    st->strings = NULL;
    st->count = 0;
    st->capacity = 0;
}

static void string_table_free(VLStringTable *st)
{
    for (size_t i = 0; i < st->count; i++) {
        free(st->strings[i]);
    }
    free(st->strings);
    string_table_init(st);
}

static VLStatus string_table_add(VLStringTable *st, const uint8_t *data, size_t len)
{
    if (st->count >= st->capacity) {
        size_t new_cap = st->capacity ? st->capacity * 2 : 256;
        char **new_strings = realloc(st->strings, new_cap * sizeof(char *));
        if (!new_strings) return VL_ERROR_OUT_OF_MEMORY;
        st->strings = new_strings;
        st->capacity = new_cap;
    }

    char *s = malloc(len + 1);
    if (!s) return VL_ERROR_OUT_OF_MEMORY;
    memcpy(s, data, len);
    s[len] = '\0';
    st->strings[st->count++] = s;
    return VL_OK;
}

static const char *string_table_get(const VLStringTable *st, size_t idx)
{
    if (idx >= st->count) return "";
    return st->strings[idx];
}

/* ============================================================================
 * Highway Type Detection
 * ============================================================================ */

/*
 * Check if a tag key/value pair indicates a routable highway.
 * Returns road type flags or 0 if not a highway.
 */
static int highway_type_from_string(const char *value)
{
    if (strcmp(value, "motorway") == 0 ||
        strcmp(value, "motorway_link") == 0) {
        return VL_EDGE_MOTORWAY;
    }
    if (strcmp(value, "trunk") == 0 ||
        strcmp(value, "trunk_link") == 0) {
        return VL_EDGE_TRUNK;
    }
    if (strcmp(value, "primary") == 0 ||
        strcmp(value, "primary_link") == 0) {
        return VL_EDGE_PRIMARY;
    }
    if (strcmp(value, "secondary") == 0 ||
        strcmp(value, "secondary_link") == 0) {
        return VL_EDGE_SECONDARY;
    }
    if (strcmp(value, "tertiary") == 0 ||
        strcmp(value, "tertiary_link") == 0) {
        return VL_EDGE_TERTIARY;
    }
    if (strcmp(value, "residential") == 0 ||
        strcmp(value, "living_street") == 0 ||
        strcmp(value, "unclassified") == 0) {
        return VL_EDGE_RESIDENTIAL;
    }
    if (strcmp(value, "service") == 0) {
        return VL_EDGE_SERVICE;
    }
    return 0;
}

/* ============================================================================
 * PBF Context Management
 * ============================================================================ */

VLPBFContext *vl_pbf_context_create(void)
{
    VLPBFContext *ctx = calloc(1, sizeof(VLPBFContext));
    if (!ctx) return NULL;
    return ctx;
}

void vl_pbf_context_free(VLPBFContext *ctx)
{
    if (!ctx) return;

    free(ctx->nodes);

    for (size_t i = 0; i < ctx->num_ways; i++) {
        free(ctx->ways[i].node_refs);
    }
    free(ctx->ways);

    free(ctx);
}

/* ============================================================================
 * Adding Nodes and Ways
 * ============================================================================ */

static VLStatus add_node(VLPBFContext *ctx, int64_t id, double lat, double lon)
{
    if (ctx->num_nodes >= ctx->nodes_capacity) {
        size_t new_cap = ctx->nodes_capacity ? ctx->nodes_capacity * 2 : 65536;
        VLOSMNode *new_nodes = realloc(ctx->nodes, new_cap * sizeof(VLOSMNode));
        if (!new_nodes) return VL_ERROR_OUT_OF_MEMORY;
        ctx->nodes = new_nodes;
        ctx->nodes_capacity = new_cap;
    }

    ctx->nodes[ctx->num_nodes].id = id;
    ctx->nodes[ctx->num_nodes].lat = lat;
    ctx->nodes[ctx->num_nodes].lon = lon;
    ctx->num_nodes++;
    ctx->total_nodes_parsed++;

    return VL_OK;
}

static VLStatus add_way(VLPBFContext *ctx, VLOSMWay *way)
{
    if (ctx->num_ways >= ctx->ways_capacity) {
        size_t new_cap = ctx->ways_capacity ? ctx->ways_capacity * 2 : 8192;
        VLOSMWay *new_ways = realloc(ctx->ways, new_cap * sizeof(VLOSMWay));
        if (!new_ways) return VL_ERROR_OUT_OF_MEMORY;
        ctx->ways = new_ways;
        ctx->ways_capacity = new_cap;
    }

    ctx->ways[ctx->num_ways] = *way;
    ctx->num_ways++;

    return VL_OK;
}

/* ============================================================================
 * Dense Nodes Parsing
 * ============================================================================ */

static VLStatus parse_dense_nodes(VLPBFContext *ctx, const uint8_t *data, size_t len,
                                  int32_t granularity, int64_t lat_offset, int64_t lon_offset)
{
    /* Temporary arrays for delta-encoded values */
    int64_t *ids = NULL;
    int64_t *lats = NULL;
    int64_t *lons = NULL;
    size_t count = 0;
    VLStatus status = VL_OK;

    size_t offset = 0;
    const uint8_t *id_data = NULL, *lat_data = NULL, *lon_data = NULL;
    size_t id_len = 0, lat_len = 0, lon_len = 0;

    /* First pass: find packed arrays */
    while (offset < len) {
        uint32_t field, wire;
        int n = vl_pb_read_tag(data + offset, len - offset, &field, &wire);
        if (n == 0) break;
        offset += n;

        if (wire == 2) {  /* Length-delimited */
            uint64_t field_len;
            n = vl_pb_read_varint(data + offset, len - offset, &field_len);
            if (n == 0) break;
            offset += n;

            switch (field) {
            case PBF_DENSE_ID:
                id_data = data + offset;
                id_len = (size_t)field_len;
                break;
            case PBF_DENSE_LAT:
                lat_data = data + offset;
                lat_len = (size_t)field_len;
                break;
            case PBF_DENSE_LON:
                lon_data = data + offset;
                lon_len = (size_t)field_len;
                break;
            }
            offset += (size_t)field_len;
        } else {
            n = vl_pb_skip_field(data + offset, len - offset, wire);
            if (n == 0) break;
            offset += n;
        }
    }

    if (!id_data || !lat_data || !lon_data) {
        return VL_OK;  /* No dense nodes in this group */
    }

    /* Count nodes (use ID array length) */
    count = 0;
    size_t tmp_off = 0;
    while (tmp_off < id_len) {
        int64_t dummy;
        int n = vl_pb_read_svarint(id_data + tmp_off, id_len - tmp_off, &dummy);
        if (n == 0) break;
        tmp_off += n;
        count++;
    }

    if (count == 0) return VL_OK;

    /* Allocate arrays */
    ids = malloc(count * sizeof(int64_t));
    lats = malloc(count * sizeof(int64_t));
    lons = malloc(count * sizeof(int64_t));

    if (!ids || !lats || !lons) {
        status = VL_ERROR_OUT_OF_MEMORY;
        goto cleanup;
    }

    /* Read packed arrays */
    vl_pb_read_packed_svarint_array(id_data, id_len, ids, count);
    vl_pb_read_packed_svarint_array(lat_data, lat_len, lats, count);
    vl_pb_read_packed_svarint_array(lon_data, lon_len, lons, count);

    /* Delta decode */
    vl_pb_delta_decode_i64(ids, count);
    vl_pb_delta_decode_i64(lats, count);
    vl_pb_delta_decode_i64(lons, count);

    /* Convert and add nodes */
    for (size_t i = 0; i < count; i++) {
        double lat = 1e-9 * (lat_offset + granularity * lats[i]);
        double lon = 1e-9 * (lon_offset + granularity * lons[i]);
        status = add_node(ctx, ids[i], lat, lon);
        if (status != VL_OK) goto cleanup;
    }

cleanup:
    free(ids);
    free(lats);
    free(lons);
    return status;
}

/* ============================================================================
 * Way Parsing
 * ============================================================================ */

static VLStatus parse_way(VLPBFContext *ctx, const uint8_t *data, size_t len,
                          const VLStringTable *st)
{
    VLOSMWay way = {0};
    const uint8_t *keys_data = NULL, *vals_data = NULL, *refs_data = NULL;
    size_t keys_len = 0, vals_len = 0, refs_len = 0;

    size_t offset = 0;
    while (offset < len) {
        uint32_t field, wire;
        int n = vl_pb_read_tag(data + offset, len - offset, &field, &wire);
        if (n == 0) break;
        offset += n;

        if (field == PBF_WAY_ID && wire == 0) {
            uint64_t id;
            n = vl_pb_read_varint(data + offset, len - offset, &id);
            if (n == 0) break;
            way.id = (int64_t)id;
            offset += n;
        } else if (wire == 2) {
            uint64_t field_len;
            n = vl_pb_read_varint(data + offset, len - offset, &field_len);
            if (n == 0) break;
            offset += n;

            switch (field) {
            case PBF_WAY_KEYS:
                keys_data = data + offset;
                keys_len = (size_t)field_len;
                break;
            case PBF_WAY_VALS:
                vals_data = data + offset;
                vals_len = (size_t)field_len;
                break;
            case PBF_WAY_REFS:
                refs_data = data + offset;
                refs_len = (size_t)field_len;
                break;
            }
            offset += (size_t)field_len;
        } else {
            n = vl_pb_skip_field(data + offset, len - offset, wire);
            if (n == 0) break;
            offset += n;
        }
    }

    ctx->total_ways_parsed++;

    /* Parse tags to find highway type and oneway */
    if (keys_data && vals_data) {
        size_t ki = 0, vi = 0;
        while (ki < keys_len && vi < vals_len) {
            uint64_t key_idx, val_idx;
            int nk = vl_pb_read_varint(keys_data + ki, keys_len - ki, &key_idx);
            int nv = vl_pb_read_varint(vals_data + vi, vals_len - vi, &val_idx);
            if (nk == 0 || nv == 0) break;
            ki += nk;
            vi += nv;

            const char *key = string_table_get(st, (size_t)key_idx);
            const char *val = string_table_get(st, (size_t)val_idx);

            if (strcmp(key, "highway") == 0) {
                way.highway_type = highway_type_from_string(val);
            } else if (strcmp(key, "oneway") == 0) {
                if (strcmp(val, "yes") == 0 || strcmp(val, "1") == 0 ||
                    strcmp(val, "true") == 0) {
                    way.oneway = 1;
                } else if (strcmp(val, "-1") == 0 || strcmp(val, "reverse") == 0) {
                    way.oneway = -1;
                }
            } else if (strcmp(key, "maxspeed") == 0) {
                way.max_speed = atoi(val);
            } else if (strcmp(key, "access") == 0) {
                /* General access restriction */
                if (strcmp(val, "no") == 0 || strcmp(val, "private") == 0) {
                    way.access_flags |= VL_ACCESS_NO_CAR | VL_ACCESS_NO_TRUCK |
                                        VL_ACCESS_NO_BIKE | VL_ACCESS_NO_FOOT;
                }
            } else if (strcmp(key, "motor_vehicle") == 0) {
                if (strcmp(val, "no") == 0 || strcmp(val, "private") == 0) {
                    way.access_flags |= VL_ACCESS_NO_CAR | VL_ACCESS_NO_TRUCK;
                }
            } else if (strcmp(key, "hgv") == 0) {
                if (strcmp(val, "no") == 0 || strcmp(val, "private") == 0) {
                    way.access_flags |= VL_ACCESS_NO_TRUCK;
                } else if (strcmp(val, "yes") == 0 || strcmp(val, "designated") == 0) {
                    way.access_flags &= ~VL_ACCESS_NO_TRUCK;  /* Allow trucks */
                }
            } else if (strcmp(key, "bicycle") == 0) {
                if (strcmp(val, "no") == 0 || strcmp(val, "private") == 0) {
                    way.access_flags |= VL_ACCESS_NO_BIKE;
                } else if (strcmp(val, "yes") == 0 || strcmp(val, "designated") == 0) {
                    way.access_flags &= ~VL_ACCESS_NO_BIKE;
                }
            } else if (strcmp(key, "foot") == 0) {
                if (strcmp(val, "no") == 0 || strcmp(val, "private") == 0) {
                    way.access_flags |= VL_ACCESS_NO_FOOT;
                } else if (strcmp(val, "yes") == 0 || strcmp(val, "designated") == 0) {
                    way.access_flags &= ~VL_ACCESS_NO_FOOT;
                }
            }
        }
    }

    /* Skip non-highway ways */
    if (way.highway_type == 0) {
        return VL_OK;
    }

    /* Parse node references */
    if (refs_data) {
        /* Count references */
        size_t count = 0;
        size_t tmp_off = 0;
        while (tmp_off < refs_len) {
            int64_t dummy;
            int n = vl_pb_read_svarint(refs_data + tmp_off, refs_len - tmp_off, &dummy);
            if (n == 0) break;
            tmp_off += n;
            count++;
        }

        if (count >= 2) {  /* Need at least 2 nodes for an edge */
            way.node_refs = malloc(count * sizeof(int64_t));
            if (!way.node_refs) return VL_ERROR_OUT_OF_MEMORY;

            vl_pb_read_packed_svarint_array(refs_data, refs_len, way.node_refs, count);
            vl_pb_delta_decode_i64(way.node_refs, count);

            way.num_refs = (int)count;
            ctx->highway_ways_kept++;

            return add_way(ctx, &way);
        }
    }

    return VL_OK;
}

/* ============================================================================
 * Primitive Group Parsing
 * ============================================================================ */

static VLStatus parse_primitive_group(VLPBFContext *ctx, const uint8_t *data, size_t len,
                                      const VLStringTable *st, int32_t granularity,
                                      int64_t lat_offset, int64_t lon_offset)
{
    size_t offset = 0;
    while (offset < len) {
        uint32_t field, wire;
        int n = vl_pb_read_tag(data + offset, len - offset, &field, &wire);
        if (n == 0) break;
        offset += n;

        if (wire == 2) {
            uint64_t field_len;
            n = vl_pb_read_varint(data + offset, len - offset, &field_len);
            if (n == 0) break;
            offset += n;

            VLStatus status = VL_OK;
            switch (field) {
            case PBF_PRIMGROUP_DENSE:
                status = parse_dense_nodes(ctx, data + offset, (size_t)field_len,
                                           granularity, lat_offset, lon_offset);
                break;
            case PBF_PRIMGROUP_WAYS:
                status = parse_way(ctx, data + offset, (size_t)field_len, st);
                break;
            }
            if (status != VL_OK) return status;
            offset += (size_t)field_len;
        } else {
            n = vl_pb_skip_field(data + offset, len - offset, wire);
            if (n == 0) break;
            offset += n;
        }
    }
    return VL_OK;
}

/* ============================================================================
 * Primitive Block Parsing
 * ============================================================================ */

static VLStatus parse_primitive_block(VLPBFContext *ctx, const uint8_t *data, size_t len)
{
    VLStringTable st;
    string_table_init(&st);

    int32_t granularity = 100;
    int64_t lat_offset = 0;
    int64_t lon_offset = 0;

    /* Temporary storage for primitive groups (parse after stringtable) */
    typedef struct {
        const uint8_t *data;
        size_t len;
    } GroupRef;
    GroupRef *groups = NULL;
    size_t num_groups = 0;
    size_t groups_cap = 0;

    VLStatus status = VL_OK;
    size_t offset = 0;

    while (offset < len) {
        uint32_t field, wire;
        int n = vl_pb_read_tag(data + offset, len - offset, &field, &wire);
        if (n == 0) break;
        offset += n;

        if (field == PBF_PRIMBLOCK_GRANULARITY && wire == 0) {
            uint64_t val;
            n = vl_pb_read_varint(data + offset, len - offset, &val);
            if (n == 0) break;
            granularity = (int32_t)val;
            offset += n;
        } else if (field == PBF_PRIMBLOCK_LAT_OFFSET && wire == 0) {
            int64_t val;
            n = vl_pb_read_svarint(data + offset, len - offset, &val);
            if (n == 0) break;
            lat_offset = val;
            offset += n;
        } else if (field == PBF_PRIMBLOCK_LON_OFFSET && wire == 0) {
            int64_t val;
            n = vl_pb_read_svarint(data + offset, len - offset, &val);
            if (n == 0) break;
            lon_offset = val;
            offset += n;
        } else if (wire == 2) {
            uint64_t field_len;
            n = vl_pb_read_varint(data + offset, len - offset, &field_len);
            if (n == 0) break;
            offset += n;

            if (field == PBF_PRIMBLOCK_STRINGTABLE) {
                /* Parse string table */
                size_t st_off = 0;
                while (st_off < (size_t)field_len) {
                    uint32_t sf, sw;
                    int sn = vl_pb_read_tag(data + offset + st_off,
                                            (size_t)field_len - st_off, &sf, &sw);
                    if (sn == 0) break;
                    st_off += sn;

                    if (sf == PBF_STRINGTABLE_S && sw == 2) {
                        uint64_t slen;
                        sn = vl_pb_read_varint(data + offset + st_off,
                                               (size_t)field_len - st_off, &slen);
                        if (sn == 0) break;
                        st_off += sn;

                        status = string_table_add(&st, data + offset + st_off, (size_t)slen);
                        if (status != VL_OK) goto cleanup;
                        st_off += (size_t)slen;
                    } else {
                        sn = vl_pb_skip_field(data + offset + st_off,
                                              (size_t)field_len - st_off, sw);
                        if (sn == 0) break;
                        st_off += sn;
                    }
                }
            } else if (field == PBF_PRIMBLOCK_PRIMITIVEGROUP) {
                /* Store group reference for later parsing */
                if (num_groups >= groups_cap) {
                    size_t new_cap = groups_cap ? groups_cap * 2 : 16;
                    GroupRef *new_groups = realloc(groups, new_cap * sizeof(GroupRef));
                    if (!new_groups) {
                        status = VL_ERROR_OUT_OF_MEMORY;
                        goto cleanup;
                    }
                    groups = new_groups;
                    groups_cap = new_cap;
                }
                groups[num_groups].data = data + offset;
                groups[num_groups].len = (size_t)field_len;
                num_groups++;
            }

            offset += (size_t)field_len;
        } else {
            n = vl_pb_skip_field(data + offset, len - offset, wire);
            if (n == 0) break;
            offset += n;
        }
    }

    /* Now parse primitive groups with complete string table */
    for (size_t i = 0; i < num_groups; i++) {
        status = parse_primitive_group(ctx, groups[i].data, groups[i].len,
                                       &st, granularity, lat_offset, lon_offset);
        if (status != VL_OK) goto cleanup;
    }

cleanup:
    free(groups);
    string_table_free(&st);
    return status;
}

/* ============================================================================
 * Blob Parsing
 * ============================================================================ */

static VLStatus parse_blob(VLPBFContext *ctx, const uint8_t *data, size_t len, int is_header)
{
    const uint8_t *raw_data = NULL;
    size_t raw_len = 0;
    const uint8_t *zlib_data = NULL;
    size_t zlib_len = 0;
    size_t raw_size = 0;

    size_t offset = 0;
    while (offset < len) {
        uint32_t field, wire;
        int n = vl_pb_read_tag(data + offset, len - offset, &field, &wire);
        if (n == 0) break;
        offset += n;

        if (field == PBF_BLOB_RAW_SIZE && wire == 0) {
            uint64_t val;
            n = vl_pb_read_varint(data + offset, len - offset, &val);
            if (n == 0) break;
            raw_size = (size_t)val;
            offset += n;
        } else if (wire == 2) {
            uint64_t field_len;
            n = vl_pb_read_varint(data + offset, len - offset, &field_len);
            if (n == 0) break;
            offset += n;

            if (field == PBF_BLOB_RAW) {
                raw_data = data + offset;
                raw_len = (size_t)field_len;
            } else if (field == PBF_BLOB_ZLIB_DATA) {
                zlib_data = data + offset;
                zlib_len = (size_t)field_len;
            }
            offset += (size_t)field_len;
        } else {
            n = vl_pb_skip_field(data + offset, len - offset, wire);
            if (n == 0) break;
            offset += n;
        }
    }

    /* Get decompressed data */
    uint8_t *block_data = NULL;
    size_t block_len = 0;
    int should_free = 0;

    if (raw_data) {
        block_data = (uint8_t *)raw_data;
        block_len = raw_len;
    } else if (zlib_data && raw_size > 0) {
        block_data = malloc(raw_size);
        if (!block_data) return VL_ERROR_OUT_OF_MEMORY;
        should_free = 1;

        VLStatus status = vl_inflate(zlib_data, zlib_len, block_data, raw_size, &block_len);
        if (status != VL_OK) {
            free(block_data);
            return status;
        }
    } else {
        return VL_ERROR_PARSE_ERROR;
    }

    VLStatus status = VL_OK;
    if (!is_header) {
        status = parse_primitive_block(ctx, block_data, block_len);
    }

    if (should_free) {
        free(block_data);
    }

    return status;
}

/* ============================================================================
 * Main PBF Parsing
 * ============================================================================ */

VLStatus vl_pbf_parse(VLPBFContext *ctx, const uint8_t *data, size_t len)
{
    if (!ctx || !data || len == 0) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    size_t offset = 0;

    while (offset < len) {
        /* Read BlobHeader size (4 bytes big-endian) */
        if (offset + 4 > len) break;

        uint32_t header_size =
            ((uint32_t)data[offset] << 24) |
            ((uint32_t)data[offset + 1] << 16) |
            ((uint32_t)data[offset + 2] << 8) |
            ((uint32_t)data[offset + 3]);
        offset += 4;

        if (offset + header_size > len) break;

        /* Parse BlobHeader */
        const uint8_t *header_data = data + offset;
        offset += header_size;

        char type[32] = {0};
        uint32_t datasize = 0;

        size_t h_off = 0;
        while (h_off < header_size) {
            uint32_t field, wire;
            int n = vl_pb_read_tag(header_data + h_off, header_size - h_off, &field, &wire);
            if (n == 0) break;
            h_off += n;

            if (field == PBF_BLOBHEADER_TYPE && wire == 2) {
                uint64_t slen;
                n = vl_pb_read_varint(header_data + h_off, header_size - h_off, &slen);
                if (n == 0) break;
                h_off += n;
                if (slen < sizeof(type)) {
                    memcpy(type, header_data + h_off, (size_t)slen);
                    type[slen] = '\0';
                }
                h_off += (size_t)slen;
            } else if (field == PBF_BLOBHEADER_DATASIZE && wire == 0) {
                uint64_t val;
                n = vl_pb_read_varint(header_data + h_off, header_size - h_off, &val);
                if (n == 0) break;
                datasize = (uint32_t)val;
                h_off += n;
            } else {
                n = vl_pb_skip_field(header_data + h_off, header_size - h_off, wire);
                if (n == 0) break;
                h_off += n;
            }
        }

        if (datasize == 0 || offset + datasize > len) break;

        /* Parse Blob */
        int is_header = (strcmp(type, "OSMHeader") == 0);
        VLStatus status = parse_blob(ctx, data + offset, datasize, is_header);
        if (status != VL_OK) return status;

        offset += datasize;
    }

    return VL_OK;
}

/* ============================================================================
 * File Loading
 * ============================================================================ */

#ifndef _WIN32
VLStatus vl_pbf_parse_file(VLPBFContext *ctx, const char *filename)
{
    if (!ctx || !filename) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        return VL_ERROR_FILE_NOT_FOUND;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return VL_ERROR_FILE_READ;
    }

    size_t len = (size_t)st.st_size;
    void *data = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (data == MAP_FAILED) {
        return VL_ERROR_OUT_OF_MEMORY;
    }

    VLStatus status = vl_pbf_parse(ctx, data, len);

    munmap(data, len);

    return status;
}
#else
/* Windows fallback: read entire file */
VLStatus vl_pbf_parse_file(VLPBFContext *ctx, const char *filename)
{
    if (!ctx || !filename) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    FILE *f = fopen(filename, "rb");
    if (!f) {
        return VL_ERROR_FILE_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0) {
        fclose(f);
        return VL_ERROR_FILE_READ;
    }

    uint8_t *data = malloc((size_t)len);
    if (!data) {
        fclose(f);
        return VL_ERROR_OUT_OF_MEMORY;
    }

    if (fread(data, 1, (size_t)len, f) != (size_t)len) {
        free(data);
        fclose(f);
        return VL_ERROR_FILE_READ;
    }

    fclose(f);

    VLStatus status = vl_pbf_parse(ctx, data, (size_t)len);

    free(data);

    return status;
}
#endif
