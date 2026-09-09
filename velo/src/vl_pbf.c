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
#include "sh_protobuf.h"
#include "sh_inflate.h"
#include "sh_pbf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#include "sh_pal.h"

/* PBF field numbers - use shared definitions from sh_pbf.h */

/* Maximum sizes to prevent unbounded memory growth from malformed PBF files */
#define VL_PBF_MAX_NODES     500000000  /* 500M nodes max (~12GB memory) */
#define VL_PBF_MAX_WAYS       50000000  /* 50M ways max */
#define VL_PBF_MAX_WAY_NODES      8192  /* Max nodes per way */

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
    if (ctx->num_nodes >= VL_PBF_MAX_NODES) {
        return VL_ERROR_OUT_OF_MEMORY;  /* Exceeded max node limit */
    }
    if (ctx->num_nodes >= ctx->nodes_capacity) {
        size_t new_cap = ctx->nodes_capacity ? ctx->nodes_capacity * 2 : 65536;
        /* Overflow check: ensure doubling didn't wrap */
        if (new_cap <= ctx->nodes_capacity) new_cap = VL_PBF_MAX_NODES;
        if (new_cap > VL_PBF_MAX_NODES) new_cap = VL_PBF_MAX_NODES;
        /* Integer overflow check for allocation size */
        if (new_cap > SIZE_MAX / sizeof(VLOSMNode)) {
            return VL_ERROR_OUT_OF_MEMORY;
        }
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
    if (ctx->num_ways >= VL_PBF_MAX_WAYS) {
        return VL_ERROR_OUT_OF_MEMORY;  /* Exceeded max way limit */
    }
    if (ctx->num_ways >= ctx->ways_capacity) {
        size_t new_cap = ctx->ways_capacity ? ctx->ways_capacity * 2 : 8192;
        /* Overflow check: ensure doubling didn't wrap */
        if (new_cap <= ctx->ways_capacity) new_cap = VL_PBF_MAX_WAYS;
        if (new_cap > VL_PBF_MAX_WAYS) new_cap = VL_PBF_MAX_WAYS;
        /* Integer overflow check for allocation size */
        if (new_cap > SIZE_MAX / sizeof(VLOSMWay)) {
            return VL_ERROR_OUT_OF_MEMORY;
        }
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
        int n = sh_pb_read_tag(data + offset, len - offset, &field, &wire);
        if (n == 0) break;
        offset += n;

        if (wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t field_len;
            n = sh_pb_read_varint(data + offset, len - offset, &field_len);
            if (n == 0) break;
            offset += n;

            switch (field) {
            case SH_PBF_DENSE_ID:
                id_data = data + offset;
                id_len = (size_t)field_len;
                break;
            case SH_PBF_DENSE_LAT:
                lat_data = data + offset;
                lat_len = (size_t)field_len;
                break;
            case SH_PBF_DENSE_LON:
                lon_data = data + offset;
                lon_len = (size_t)field_len;
                break;
            }
            offset += (size_t)field_len;
        } else {
            n = sh_pb_skip_field(data + offset, len - offset, wire);
            if (n == 0) break;
            offset += n;
        }
    }

    if (!id_data || !lat_data || !lon_data) {
        return VL_OK;  /* No dense nodes in this group */
    }

    /* Count nodes (use ID array length) */
    count = sh_pb_count_packed_varint(id_data, id_len);

    if (count == 0) return VL_OK;

    /* Integer overflow check for allocations */
    if (count > SIZE_MAX / sizeof(int64_t)) {
        return VL_ERROR_OUT_OF_MEMORY;
    }

    /* Allocate arrays */
    ids = malloc(count * sizeof(int64_t));
    lats = malloc(count * sizeof(int64_t));
    lons = malloc(count * sizeof(int64_t));

    if (!ids || !lats || !lons) {
        status = VL_ERROR_OUT_OF_MEMORY;
        goto cleanup;
    }

    /* Read packed arrays */
    sh_pb_read_packed_svarint_array(id_data, id_len, ids, count);
    sh_pb_read_packed_svarint_array(lat_data, lat_len, lats, count);
    sh_pb_read_packed_svarint_array(lon_data, lon_len, lons, count);

    /* Delta decode */
    sh_pb_delta_decode_i64(ids, count);
    sh_pb_delta_decode_i64(lats, count);
    sh_pb_delta_decode_i64(lons, count);

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
                          const SHStringTable *st)
{
    VLOSMWay way = {0};
    const uint8_t *keys_data = NULL, *vals_data = NULL, *refs_data = NULL;
    size_t keys_len = 0, vals_len = 0, refs_len = 0;

    size_t offset = 0;
    while (offset < len) {
        uint32_t field, wire;
        int n = sh_pb_read_tag(data + offset, len - offset, &field, &wire);
        if (n == 0) break;
        offset += n;

        if (field == SH_PBF_WAY_ID && wire == SH_PB_WIRE_VARINT) {
            uint64_t id;
            n = sh_pb_read_varint(data + offset, len - offset, &id);
            if (n == 0) break;
            way.id = (int64_t)id;
            offset += n;
        } else if (wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t field_len;
            n = sh_pb_read_varint(data + offset, len - offset, &field_len);
            if (n == 0) break;
            offset += n;

            switch (field) {
            case SH_PBF_WAY_KEYS:
                keys_data = data + offset;
                keys_len = (size_t)field_len;
                break;
            case SH_PBF_WAY_VALS:
                vals_data = data + offset;
                vals_len = (size_t)field_len;
                break;
            case SH_PBF_WAY_REFS:
                refs_data = data + offset;
                refs_len = (size_t)field_len;
                break;
            }
            offset += (size_t)field_len;
        } else {
            n = sh_pb_skip_field(data + offset, len - offset, wire);
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
            int nk = sh_pb_read_varint(keys_data + ki, keys_len - ki, &key_idx);
            int nv = sh_pb_read_varint(vals_data + vi, vals_len - vi, &val_idx);
            if (nk == 0 || nv == 0) break;
            ki += nk;
            vi += nv;

            const char *key = sh_string_table_get(st, (size_t)key_idx);
            const char *val = sh_string_table_get(st, (size_t)val_idx);

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
                long speed = strtol(val, NULL, 10);
                way.max_speed = (speed > 0 && speed <= 500) ? (int)speed : 0;
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
        size_t count = sh_pb_count_packed_varint(refs_data, refs_len);

        /* Validate count fits in int and doesn't exceed max */
        if (count >= 2 && count <= VL_PBF_MAX_WAY_NODES) {
            /* Integer overflow check (defensive, VL_PBF_MAX_WAY_NODES is small) */
            if (count > SIZE_MAX / sizeof(int64_t)) return VL_ERROR_OUT_OF_MEMORY;

            way.node_refs = malloc(count * sizeof(int64_t));
            if (!way.node_refs) return VL_ERROR_OUT_OF_MEMORY;

            sh_pb_read_packed_svarint_array(refs_data, refs_len, way.node_refs, count);
            sh_pb_delta_decode_i64(way.node_refs, count);

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
                                      const SHStringTable *st, int32_t granularity,
                                      int64_t lat_offset, int64_t lon_offset)
{
    size_t offset = 0;
    while (offset < len) {
        uint32_t field, wire;
        int n = sh_pb_read_tag(data + offset, len - offset, &field, &wire);
        if (n == 0) break;
        offset += n;

        if (wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t field_len;
            n = sh_pb_read_varint(data + offset, len - offset, &field_len);
            if (n == 0) break;
            offset += n;

            VLStatus status = VL_OK;
            switch (field) {
            case SH_PBF_PRIMGROUP_DENSE:
                status = parse_dense_nodes(ctx, data + offset, (size_t)field_len,
                                           granularity, lat_offset, lon_offset);
                break;
            case SH_PBF_PRIMGROUP_WAYS:
                status = parse_way(ctx, data + offset, (size_t)field_len, st);
                break;
            }
            if (status != VL_OK) return status;
            offset += (size_t)field_len;
        } else {
            n = sh_pb_skip_field(data + offset, len - offset, wire);
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
    SHStringTable st;
    sh_string_table_init(&st);

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
        int n = sh_pb_read_tag(data + offset, len - offset, &field, &wire);
        if (n == 0) break;
        offset += n;

        if (field == SH_PBF_PRIMBLOCK_GRANULARITY && wire == SH_PB_WIRE_VARINT) {
            uint64_t val;
            n = sh_pb_read_varint(data + offset, len - offset, &val);
            if (n == 0) break;
            granularity = (int32_t)val;
            offset += n;
        } else if (field == SH_PBF_PRIMBLOCK_LAT_OFFSET && wire == SH_PB_WIRE_VARINT) {
            int64_t val;
            n = sh_pb_read_svarint(data + offset, len - offset, &val);
            if (n == 0) break;
            lat_offset = val;
            offset += n;
        } else if (field == SH_PBF_PRIMBLOCK_LON_OFFSET && wire == SH_PB_WIRE_VARINT) {
            int64_t val;
            n = sh_pb_read_svarint(data + offset, len - offset, &val);
            if (n == 0) break;
            lon_offset = val;
            offset += n;
        } else if (wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t field_len;
            n = sh_pb_read_varint(data + offset, len - offset, &field_len);
            if (n == 0) break;
            offset += n;

            if (field == SH_PBF_PRIMBLOCK_STRINGTABLE) {
                /* Parse string table using shared function */
                SHStatus sh_status = sh_string_table_parse(&st, data + offset, (size_t)field_len);
                if (sh_status != SH_OK) {
                    status = VL_ERROR_OUT_OF_MEMORY;
                    goto cleanup;
                }
            } else if (field == SH_PBF_PRIMBLOCK_PRIMITIVEGROUP) {
                /* Store group reference for later parsing */
                if (num_groups >= groups_cap) {
                    size_t new_cap = groups_cap ? groups_cap * 2 : 16;
                    /* Integer overflow check for allocation size */
                    if (new_cap > SIZE_MAX / sizeof(GroupRef)) {
                        status = VL_ERROR_OUT_OF_MEMORY;
                        goto cleanup;
                    }
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
            n = sh_pb_skip_field(data + offset, len - offset, wire);
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
    sh_string_table_free(&st);
    return status;
}

/* ============================================================================
 * Blob Parsing
 * ============================================================================ */

static VLStatus parse_blob(VLPBFContext *ctx, const uint8_t *data, size_t len, int is_header)
{
    SHPBFBlob blob;
    SHStatus sh_status = sh_pbf_decompress_blob(data, len, &blob);
    if (sh_status != SH_OK) {
        return VL_ERROR_PARSE_ERROR;
    }

    VLStatus status = VL_OK;
    if (!is_header) {
        status = parse_primitive_block(ctx, blob.data, blob.len);
    }

    sh_pbf_blob_free(&blob);
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

        /* Parse BlobHeader using shared function */
        char type[32] = {0};
        uint32_t datasize = 0;
        SHStatus sh_status = sh_pbf_parse_blob_header(data + offset, header_size,
                                                       type, sizeof(type),
                                                       &datasize, NULL);
        if (sh_status != SH_OK) {
            return VL_ERROR_PARSE_ERROR;
        }
        offset += header_size;

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

VLStatus vl_pbf_parse_file(VLPBFContext *ctx, const char *filename)
{
    if (!ctx || !filename) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    /* One mapped path on every platform. This used to be two: mmap here and a
     * read-the-whole-file fallback on Windows, which mattered because these are
     * OSM extracts -- hundreds of megabytes for a country, more for a continent.
     * Reading one into the heap costs its full size in resident memory where
     * mapping costs almost nothing. */
    ShFileMap map;
    if (sh_map_file_readonly(filename, &map) != 0) {
        /* The PAL reports one failure for missing, empty and unmappable alike.
         * Only the message differs to a user, so distinguish the common case
         * here, off the path that matters. */
        FILE *probe = fopen(filename, "rb");
        if (!probe) return VL_ERROR_FILE_NOT_FOUND;
        fclose(probe);
        return VL_ERROR_FILE_READ;
    }

    sh_map_advise_sequential(&map);

    VLStatus status = vl_pbf_parse(ctx, map.data, map.size);

    sh_unmap_file(&map);

    return status;
}
