/*
 * ct_pbf.c - OSM PBF file parser for map features
 *
 * Parses OpenStreetMap Protocol Buffer Format files.
 * Extracts nodes and ways needed for map rendering.
 */

#include "ct_pbf.h"
#include "ct_tile.h"
#include "ct_lod.h"
#include "ct_rtree.h"
#include "sh_protobuf.h"
#include "sh_inflate.h"
#include "sh_pbf.h"
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

/* PBF field numbers - use shared definitions from sh_pbf.h */

/* ============================================================================
 * Feature Classification
 * ============================================================================ */

static int classify_highway(const char *value)
{
    if (strcmp(value, "motorway") == 0 ||
        strcmp(value, "motorway_link") == 0) {
        return CT_ROAD_MOTORWAY;
    }
    if (strcmp(value, "trunk") == 0 ||
        strcmp(value, "trunk_link") == 0) {
        return CT_ROAD_TRUNK;
    }
    if (strcmp(value, "primary") == 0 ||
        strcmp(value, "primary_link") == 0) {
        return CT_ROAD_PRIMARY;
    }
    if (strcmp(value, "secondary") == 0 ||
        strcmp(value, "secondary_link") == 0) {
        return CT_ROAD_SECONDARY;
    }
    if (strcmp(value, "tertiary") == 0 ||
        strcmp(value, "tertiary_link") == 0) {
        return CT_ROAD_TERTIARY;
    }
    if (strcmp(value, "residential") == 0 ||
        strcmp(value, "living_street") == 0 ||
        strcmp(value, "unclassified") == 0) {
        return CT_ROAD_RESIDENTIAL;
    }
    if (strcmp(value, "service") == 0 ||
        strcmp(value, "track") == 0 ||
        strcmp(value, "path") == 0) {
        return CT_ROAD_SERVICE;
    }
    return CT_ROAD_OTHER;
}

static CTOSMFeatureClass classify_tags(const SHStringTable *st,
                                       const uint32_t *keys, const uint32_t *vals,
                                       int num_tags, int *feature_type, int *is_area)
{
    *feature_type = 0;
    *is_area = 0;

    for (int i = 0; i < num_tags; i++) {
        const char *key = sh_string_table_get(st, keys[i]);
        const char *val = sh_string_table_get(st, vals[i]);

        if (strcmp(key, "highway") == 0) {
            *feature_type = classify_highway(val);
            return CT_OSM_HIGHWAY;
        }
        if (strcmp(key, "waterway") == 0) {
            return CT_OSM_WATERWAY;
        }
        if (strcmp(key, "natural") == 0) {
            if (strcmp(val, "water") == 0) {
                *is_area = 1;
                return CT_OSM_WATER;
            }
            return CT_OSM_NATURAL;
        }
        if (strcmp(key, "building") == 0) {
            *is_area = 1;
            return CT_OSM_BUILDING;
        }
        if (strcmp(key, "landuse") == 0) {
            *is_area = 1;
            return CT_OSM_LANDUSE;
        }
        if (strcmp(key, "railway") == 0) {
            return CT_OSM_RAILWAY;
        }
        if (strcmp(key, "area") == 0 && strcmp(val, "yes") == 0) {
            *is_area = 1;
        }
    }

    return CT_OSM_UNKNOWN;
}

/* ============================================================================
 * Node Map (Hash Table)
 * ============================================================================ */

static uint64_t hash_id(int64_t id)
{
    uint64_t x = (uint64_t)id;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return x;
}

static CTStatus node_map_insert(CTPBFContext *ctx, int64_t id, uint32_t index)
{
    if (ctx->node_map.count >= ctx->node_map.capacity * 3 / 4) {
        size_t new_cap = ctx->node_map.capacity ? ctx->node_map.capacity * 2 : 65536;
        int64_t *new_keys = calloc(new_cap, sizeof(int64_t));
        uint32_t *new_vals = malloc(new_cap * sizeof(uint32_t));
        if (!new_keys || !new_vals) {
            free(new_keys);
            free(new_vals);
            return CT_ERROR_OUT_OF_MEMORY;
        }

        /* Rehash */
        for (size_t i = 0; i < ctx->node_map.capacity; i++) {
            if (ctx->node_map.keys[i] != 0) {
                uint64_t h = hash_id(ctx->node_map.keys[i]) % new_cap;
                while (new_keys[h] != 0) {
                    h = (h + 1) % new_cap;
                }
                new_keys[h] = ctx->node_map.keys[i];
                new_vals[h] = ctx->node_map.values[i];
            }
        }

        free(ctx->node_map.keys);
        free(ctx->node_map.values);
        ctx->node_map.keys = new_keys;
        ctx->node_map.values = new_vals;
        ctx->node_map.capacity = new_cap;
    }

    uint64_t h = hash_id(id) % ctx->node_map.capacity;
    while (ctx->node_map.keys[h] != 0) {
        h = (h + 1) % ctx->node_map.capacity;
    }
    ctx->node_map.keys[h] = id;
    ctx->node_map.values[h] = index;
    ctx->node_map.count++;
    return CT_OK;
}

static uint32_t node_map_lookup(const CTPBFContext *ctx, int64_t id)
{
    if (ctx->node_map.capacity == 0) return UINT32_MAX;

    uint64_t h = hash_id(id) % ctx->node_map.capacity;
    size_t start = h;

    while (ctx->node_map.keys[h] != 0) {
        if (ctx->node_map.keys[h] == id) {
            return ctx->node_map.values[h];
        }
        h = (h + 1) % ctx->node_map.capacity;
        if (h == start) break;
    }

    return UINT32_MAX;
}

/* ============================================================================
 * Context Management
 * ============================================================================ */

CTPBFContext *ct_pbf_context_create(void)
{
    CTPBFContext *ctx = calloc(1, sizeof(CTPBFContext));
    if (!ctx) return NULL;

    ctx->bbox.min_lat = 90;
    ctx->bbox.max_lat = -90;
    ctx->bbox.min_lon = 180;
    ctx->bbox.max_lon = -180;

    return ctx;
}

void ct_pbf_context_free(CTPBFContext *ctx)
{
    if (!ctx) return;

    free(ctx->nodes.ids);
    free(ctx->nodes.coords);
    free(ctx->node_map.keys);
    free(ctx->node_map.values);

    if (ctx->mmap_base) {
        /* mmap'd context - ways point into allocated block, names are strdup'd */
        for (size_t i = 0; i < ctx->num_ways; i++) {
            free(ctx->ways[i].name);
        }
        free(ctx->ways);
        free(ctx->mmap_coords);

        /* R-Tree nodes point into mmap, just free the struct */
        if (ctx->rtree && !ctx->rtree_is_mmap) {
            ct_rtree_free(ctx->rtree);
        } else if (ctx->rtree) {
            free(ctx->rtree);
        }

        /* Unmap the file */
        munmap(ctx->mmap_base, ctx->mmap_size);
    } else {
        /* Normal context - free everything */
        for (size_t i = 0; i < ctx->num_ways; i++) {
            free(ctx->ways[i].coords);
            free(ctx->ways[i].name);
        }
        free(ctx->ways);

        ct_rtree_free(ctx->rtree);
    }

    free(ctx);
}

/* ============================================================================
 * PBF Parsing
 * ============================================================================ */

static CTStatus parse_string_table(const uint8_t *data, size_t len,
                                   SHStringTable *st)
{
    SHStatus status = sh_string_table_parse(st, data, len);
    if (status != SH_OK) return CT_ERROR_PARSE_ERROR;
    return CT_OK;
}

static CTStatus parse_dense_nodes(CTPBFContext *ctx, const uint8_t *data, size_t len,
                                  int32_t granularity, int64_t lat_offset, int64_t lon_offset)
{
    int64_t *ids = NULL, *lats = NULL, *lons = NULL;
    size_t id_count = 0, lat_count = 0, lon_count = 0;
    size_t pos = 0;

    /* Temporary arrays for delta-encoded values */
    size_t max_nodes = 1000000;
    ids = malloc(max_nodes * sizeof(int64_t));
    lats = malloc(max_nodes * sizeof(int64_t));
    lons = malloc(max_nodes * sizeof(int64_t));
    if (!ids || !lats || !lons) {
        free(ids); free(lats); free(lons);
        return CT_ERROR_OUT_OF_MEMORY;
    }

    while (pos < len) {
        uint32_t field, wire;
        int n = sh_pb_read_tag(data + pos, len - pos, &field, &wire);
        if (n == 0) goto error;
        pos += n;

        if (wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t packed_len;
            n = sh_pb_read_varint(data + pos, len - pos, &packed_len);
            if (n == 0) goto error;
            pos += n;

            if (field == SH_PBF_DENSE_ID) {
                id_count = sh_pb_read_packed_svarint_array(data + pos, packed_len,
                                                          ids, max_nodes);
            } else if (field == SH_PBF_DENSE_LAT) {
                lat_count = sh_pb_read_packed_svarint_array(data + pos, packed_len,
                                                           lats, max_nodes);
            } else if (field == SH_PBF_DENSE_LON) {
                lon_count = sh_pb_read_packed_svarint_array(data + pos, packed_len,
                                                           lons, max_nodes);
            }
            pos += packed_len;
        } else {
            n = sh_pb_skip_field(data + pos, len - pos, wire);
            if (n == 0) goto error;
            pos += n;
        }
    }

    /* Delta decode */
    sh_pb_delta_decode_i64(ids, id_count);
    sh_pb_delta_decode_i64(lats, lat_count);
    sh_pb_delta_decode_i64(lons, lon_count);

    /* Store nodes */
    size_t count = id_count;
    if (lat_count < count) count = lat_count;
    if (lon_count < count) count = lon_count;

    if (ctx->nodes.count + count > ctx->nodes.capacity) {
        size_t new_cap = ctx->nodes.capacity ? ctx->nodes.capacity * 2 : 100000;
        while (new_cap < ctx->nodes.count + count) new_cap *= 2;

        int64_t *new_ids = realloc(ctx->nodes.ids, new_cap * sizeof(int64_t));
        CTCoord *new_coords = realloc(ctx->nodes.coords, new_cap * sizeof(CTCoord));
        if (!new_ids || !new_coords) {
            free(new_ids); free(new_coords);
            goto error;
        }
        ctx->nodes.ids = new_ids;
        ctx->nodes.coords = new_coords;
        ctx->nodes.capacity = new_cap;
    }

    for (size_t i = 0; i < count; i++) {
        double lat = (lat_offset + lats[i] * granularity) * 1e-9;
        double lon = (lon_offset + lons[i] * granularity) * 1e-9;

        size_t idx = ctx->nodes.count++;
        ctx->nodes.ids[idx] = ids[i];
        ctx->nodes.coords[idx].lat = lat;
        ctx->nodes.coords[idx].lon = lon;

        /* Note: bbox is updated when features are kept, not for all nodes */

        node_map_insert(ctx, ids[i], (uint32_t)idx);
    }

    ctx->total_nodes_parsed += count;

    free(ids); free(lats); free(lons);
    return CT_OK;

error:
    free(ids); free(lats); free(lons);
    return CT_ERROR_PARSE_ERROR;
}

static CTStatus parse_way(CTPBFContext *ctx, const uint8_t *data, size_t len,
                          const SHStringTable *st)
{
    int64_t id = 0;
    uint32_t *keys = NULL, *vals = NULL;
    int64_t *refs = NULL;
    size_t key_count = 0, val_count = 0, ref_count = 0;
    size_t pos = 0;

    size_t max_refs = 10000;
    refs = malloc(max_refs * sizeof(int64_t));
    keys = malloc(256 * sizeof(uint32_t));
    vals = malloc(256 * sizeof(uint32_t));
    if (!refs || !keys || !vals) {
        free(refs); free(keys); free(vals);
        return CT_ERROR_OUT_OF_MEMORY;
    }

    while (pos < len) {
        uint32_t field, wire;
        int n = sh_pb_read_tag(data + pos, len - pos, &field, &wire);
        if (n == 0) goto skip_way;
        pos += n;

        if (field == SH_PBF_WAY_ID && wire == SH_PB_WIRE_VARINT) {
            uint64_t val;
            n = sh_pb_read_varint(data + pos, len - pos, &val);
            if (n == 0) goto skip_way;
            id = (int64_t)val;
            pos += n;
        } else if (field == SH_PBF_WAY_KEYS && wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t packed_len;
            n = sh_pb_read_varint(data + pos, len - pos, &packed_len);
            if (n == 0) goto skip_way;
            pos += n;

            const uint8_t *p = data + pos;
            size_t ppos = 0;
            while (ppos < packed_len && key_count < 256) {
                uint64_t v;
                int k = sh_pb_read_varint(p + ppos, packed_len - ppos, &v);
                if (k == 0) break;
                keys[key_count++] = (uint32_t)v;
                ppos += k;
            }
            pos += packed_len;
        } else if (field == SH_PBF_WAY_VALS && wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t packed_len;
            n = sh_pb_read_varint(data + pos, len - pos, &packed_len);
            if (n == 0) goto skip_way;
            pos += n;

            const uint8_t *p = data + pos;
            size_t ppos = 0;
            while (ppos < packed_len && val_count < 256) {
                uint64_t v;
                int k = sh_pb_read_varint(p + ppos, packed_len - ppos, &v);
                if (k == 0) break;
                vals[val_count++] = (uint32_t)v;
                ppos += k;
            }
            pos += packed_len;
        } else if (field == SH_PBF_WAY_REFS && wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t packed_len;
            n = sh_pb_read_varint(data + pos, len - pos, &packed_len);
            if (n == 0) goto skip_way;
            pos += n;

            ref_count = sh_pb_read_packed_svarint_array(data + pos, packed_len,
                                                        refs, max_refs);
            sh_pb_delta_decode_i64(refs, ref_count);
            pos += packed_len;
        } else {
            n = sh_pb_skip_field(data + pos, len - pos, wire);
            if (n == 0) goto skip_way;
            pos += n;
        }
    }

    ctx->total_ways_parsed++;

    /* Classify the way */
    int feature_type = 0;
    int is_area = 0;
    int num_tags = (int)(key_count < val_count ? key_count : val_count);
    CTOSMFeatureClass feature_class = classify_tags(st, keys, vals, num_tags,
                                                    &feature_type, &is_area);

    if (feature_class == CT_OSM_UNKNOWN) {
        goto skip_way;
    }

    /* Resolve node references to coordinates */
    CTCoord *coords = malloc(ref_count * sizeof(CTCoord));
    if (!coords) goto skip_way;

    size_t coord_count = 0;
    for (size_t i = 0; i < ref_count; i++) {
        uint32_t idx = node_map_lookup(ctx, refs[i]);
        if (idx != UINT32_MAX && idx < ctx->nodes.count) {
            coords[coord_count++] = ctx->nodes.coords[idx];
        }
    }

    if (coord_count < 2) {
        free(coords);
        goto skip_way;
    }

    /* Check if polygon is closed */
    if (coord_count >= 4 &&
        coords[0].lat == coords[coord_count - 1].lat &&
        coords[0].lon == coords[coord_count - 1].lon) {
        is_area = 1;
    }

    /* Store the way */
    if (ctx->num_ways >= ctx->ways_capacity) {
        size_t new_cap = ctx->ways_capacity ? ctx->ways_capacity * 2 : 1000;
        CTOSMWay *new_ways = realloc(ctx->ways, new_cap * sizeof(CTOSMWay));
        if (!new_ways) {
            free(coords);
            goto skip_way;
        }
        ctx->ways = new_ways;
        ctx->ways_capacity = new_cap;
    }

    CTOSMWay *way = &ctx->ways[ctx->num_ways++];
    way->id = id;
    way->coords = coords;
    way->num_coords = (int)coord_count;
    way->feature_class = feature_class;
    way->feature_type = feature_type;
    way->is_area = is_area;
    way->name = NULL;

    /* Calculate area/length for LOD filtering */
    if (is_area) {
        way->area_sqm = ct_lod_estimate_area(coords, (int)coord_count);
        way->length_m = 0;
    } else {
        way->area_sqm = 0;
        way->length_m = ct_lod_estimate_length(coords, (int)coord_count);
    }

    /* min_zoom will be calculated when querying with LOD config */
    way->min_zoom = 0;

    /* Update bbox from this feature's coordinates */
    for (size_t i = 0; i < coord_count; i++) {
        if (coords[i].lat < ctx->bbox.min_lat) ctx->bbox.min_lat = coords[i].lat;
        if (coords[i].lat > ctx->bbox.max_lat) ctx->bbox.max_lat = coords[i].lat;
        if (coords[i].lon < ctx->bbox.min_lon) ctx->bbox.min_lon = coords[i].lon;
        if (coords[i].lon > ctx->bbox.max_lon) ctx->bbox.max_lon = coords[i].lon;
    }

    ctx->features_kept++;

skip_way:
    free(refs);
    free(keys);
    free(vals);
    return CT_OK;
}

static CTStatus parse_primitive_group(CTPBFContext *ctx, const uint8_t *data, size_t len,
                                      const SHStringTable *st,
                                      int32_t granularity, int64_t lat_offset, int64_t lon_offset)
{
    size_t pos = 0;

    while (pos < len) {
        uint32_t field, wire;
        int n = sh_pb_read_tag(data + pos, len - pos, &field, &wire);
        if (n == 0) return CT_ERROR_PARSE_ERROR;
        pos += n;

        if (wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t msg_len;
            n = sh_pb_read_varint(data + pos, len - pos, &msg_len);
            if (n == 0) return CT_ERROR_PARSE_ERROR;
            pos += n;

            if (field == SH_PBF_PRIMGROUP_DENSE) {
                CTStatus status = parse_dense_nodes(ctx, data + pos, msg_len,
                                                    granularity, lat_offset, lon_offset);
                if (status != CT_OK) return status;
            } else if (field == SH_PBF_PRIMGROUP_WAYS) {
                CTStatus status = parse_way(ctx, data + pos, msg_len, st);
                if (status != CT_OK) return status;
            }
            pos += msg_len;
        } else {
            n = sh_pb_skip_field(data + pos, len - pos, wire);
            if (n == 0) return CT_ERROR_PARSE_ERROR;
            pos += n;
        }
    }

    return CT_OK;
}

static CTStatus parse_primitive_block(CTPBFContext *ctx, const uint8_t *data, size_t len)
{
    SHStringTable st;
    sh_string_table_init(&st);

    int32_t granularity = 100;
    int64_t lat_offset = 0;
    int64_t lon_offset = 0;

    /* First pass: parse header and string table */
    size_t pos = 0;
    while (pos < len) {
        uint32_t field, wire;
        int n = sh_pb_read_tag(data + pos, len - pos, &field, &wire);
        if (n == 0) {
            sh_string_table_free(&st);
            return CT_ERROR_PARSE_ERROR;
        }
        pos += n;

        if (field == SH_PBF_PRIMBLOCK_STRINGTABLE && wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t msg_len;
            n = sh_pb_read_varint(data + pos, len - pos, &msg_len);
            if (n == 0) {
                sh_string_table_free(&st);
                return CT_ERROR_PARSE_ERROR;
            }
            pos += n;

            CTStatus status = parse_string_table(data + pos, msg_len, &st);
            if (status != CT_OK) {
                sh_string_table_free(&st);
                return status;
            }
            pos += msg_len;
        } else if (field == SH_PBF_PRIMBLOCK_GRANULARITY && wire == SH_PB_WIRE_VARINT) {
            uint64_t val;
            n = sh_pb_read_varint(data + pos, len - pos, &val);
            if (n == 0) {
                sh_string_table_free(&st);
                return CT_ERROR_PARSE_ERROR;
            }
            granularity = (int32_t)val;
            pos += n;
        } else if (field == SH_PBF_PRIMBLOCK_LAT_OFFSET && wire == SH_PB_WIRE_VARINT) {
            int64_t val;
            n = sh_pb_read_svarint(data + pos, len - pos, &val);
            if (n == 0) {
                sh_string_table_free(&st);
                return CT_ERROR_PARSE_ERROR;
            }
            lat_offset = val;
            pos += n;
        } else if (field == SH_PBF_PRIMBLOCK_LON_OFFSET && wire == SH_PB_WIRE_VARINT) {
            int64_t val;
            n = sh_pb_read_svarint(data + pos, len - pos, &val);
            if (n == 0) {
                sh_string_table_free(&st);
                return CT_ERROR_PARSE_ERROR;
            }
            lon_offset = val;
            pos += n;
        } else {
            n = sh_pb_skip_field(data + pos, len - pos, wire);
            if (n == 0) {
                sh_string_table_free(&st);
                return CT_ERROR_PARSE_ERROR;
            }
            pos += n;
        }
    }

    /* Second pass: parse primitive groups */
    pos = 0;
    while (pos < len) {
        uint32_t field, wire;
        int n = sh_pb_read_tag(data + pos, len - pos, &field, &wire);
        if (n == 0) {
            sh_string_table_free(&st);
            return CT_ERROR_PARSE_ERROR;
        }
        pos += n;

        if (field == SH_PBF_PRIMBLOCK_PRIMITIVEGROUP && wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t msg_len;
            n = sh_pb_read_varint(data + pos, len - pos, &msg_len);
            if (n == 0) {
                sh_string_table_free(&st);
                return CT_ERROR_PARSE_ERROR;
            }
            pos += n;

            CTStatus status = parse_primitive_group(ctx, data + pos, msg_len, &st,
                                                    granularity, lat_offset, lon_offset);
            if (status != CT_OK) {
                sh_string_table_free(&st);
                return status;
            }
            pos += msg_len;
        } else {
            n = sh_pb_skip_field(data + pos, len - pos, wire);
            if (n == 0) {
                sh_string_table_free(&st);
                return CT_ERROR_PARSE_ERROR;
            }
            pos += n;
        }
    }

    sh_string_table_free(&st);
    return CT_OK;
}

static CTStatus parse_blob(CTPBFContext *ctx, const uint8_t *data, size_t len)
{
    SHPBFBlob blob;
    SHStatus sh_status = sh_pbf_decompress_blob(data, len, &blob);
    if (sh_status != SH_OK) {
        return CT_ERROR_PARSE_ERROR;
    }

    CTStatus status = parse_primitive_block(ctx, blob.data, blob.len);
    sh_pbf_blob_free(&blob);
    return status;
}

CTStatus ct_pbf_parse_memory(CTPBFContext *ctx, const uint8_t *data, size_t size)
{
    size_t pos = 0;

    while (pos < size) {
        /* Read blob header length (4 bytes big-endian) */
        if (pos + 4 > size) break;
        uint32_t header_len = ((uint32_t)data[pos] << 24) |
                              ((uint32_t)data[pos + 1] << 16) |
                              ((uint32_t)data[pos + 2] << 8) |
                              (uint32_t)data[pos + 3];
        pos += 4;

        if (pos + header_len > size) return CT_ERROR_PARSE_ERROR;

        /* Parse blob header using shared function */
        char type[32] = "";
        uint32_t data_size = 0;
        size_t consumed;
        SHStatus sh_status = sh_pbf_parse_blob_header(data + pos, header_len,
                                                       type, sizeof(type),
                                                       &data_size, &consumed);
        if (sh_status != SH_OK) return CT_ERROR_PARSE_ERROR;
        pos += header_len;

        if (pos + data_size > size) return CT_ERROR_PARSE_ERROR;

        /* Parse blob data (only OSMData blobs) */
        if (strcmp(type, "OSMData") == 0) {
            CTStatus status = parse_blob(ctx, data + pos, data_size);
            if (status != CT_OK) return status;
        }

        pos += data_size;
    }

    return CT_OK;
}

CTStatus ct_pbf_parse_file(CTPBFContext *ctx, const char *filename)
{
#ifdef _WIN32
    FILE *f = fopen(filename, "rb");
    if (!f) return CT_ERROR_FILE_NOT_FOUND;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = malloc(size);
    if (!data) {
        fclose(f);
        return CT_ERROR_OUT_OF_MEMORY;
    }

    if (fread(data, 1, size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return CT_ERROR_FILE_READ;
    }
    fclose(f);

    CTStatus status = ct_pbf_parse_memory(ctx, data, size);
    free(data);
    return status;
#else
    int fd = open(filename, O_RDONLY);
    if (fd < 0) return CT_ERROR_FILE_NOT_FOUND;

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return CT_ERROR_FILE_READ;
    }

    void *map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (map == MAP_FAILED) return CT_ERROR_FILE_READ;

    CTStatus status = ct_pbf_parse_memory(ctx, map, st.st_size);
    munmap(map, st.st_size);
    return status;
#endif
}

/* ============================================================================
 * Spatial Index
 * ============================================================================ */

CTStatus ct_pbf_build_index(CTPBFContext *ctx)
{
    if (!ctx || ctx->num_ways == 0) return CT_OK;

    /* Free existing index if any */
    if (ctx->rtree) {
        ct_rtree_free(ctx->rtree);
        ctx->rtree = NULL;
    }

    /* Build R-Tree from ways */
    ctx->rtree = ct_rtree_build(ctx->ways, ctx->num_ways, ctx->bbox);
    if (!ctx->rtree) {
        return CT_ERROR_OUT_OF_MEMORY;
    }

    return CT_OK;
}

/* ============================================================================
 * Feature Extraction
 * ============================================================================ */

static CTLayer layer_from_osm_class(CTOSMFeatureClass cls)
{
    switch (cls) {
        case CT_OSM_HIGHWAY:   return CT_LAYER_ROADS;
        case CT_OSM_WATER:
        case CT_OSM_WATERWAY:  return CT_LAYER_WATER;
        case CT_OSM_BUILDING:  return CT_LAYER_BUILDINGS;
        case CT_OSM_LANDUSE:
        case CT_OSM_NATURAL:   return CT_LAYER_LANDUSE;
        case CT_OSM_RAILWAY:   return CT_LAYER_RAILWAYS;
        case CT_OSM_BOUNDARY:  return CT_LAYER_BOUNDARIES;
        default:               return CT_LAYER_BACKGROUND;
    }
}

CTStatus ct_pbf_get_tile_features(const CTPBFContext *ctx, CTTileCoord tile,
                                  CTFeature **features, size_t *count)
{
    CTBBox bbox = ct_tile_bounds(tile);
    return ct_pbf_get_bbox_features(ctx, bbox, features, count);
}

/*
 * Helper to add a single way as a feature to the output array.
 */
static CTStatus add_way_as_feature(const CTOSMWay *way, CTFeature **features,
                                   size_t *count, size_t *capacity)
{
    /* Expand array if needed */
    if (*count >= *capacity) {
        *capacity *= 2;
        CTFeature *new_features = realloc(*features, *capacity * sizeof(CTFeature));
        if (!new_features) {
            return CT_ERROR_OUT_OF_MEMORY;
        }
        *features = new_features;
    }

    /* Create feature */
    CTFeature *f = &(*features)[*count];
    memset(f, 0, sizeof(CTFeature));

    f->type = way->is_area ? CT_GEOM_POLYGON : CT_GEOM_LINESTRING;
    f->layer = layer_from_osm_class(way->feature_class);
    f->feature_type = way->feature_type;

    /* Allocate and copy coordinates */
    f->points = malloc(way->num_coords * sizeof(CTTilePoint));
    if (!f->points) return CT_OK;  /* Skip this feature but continue */

    f->num_points = way->num_coords;
    for (int j = 0; j < way->num_coords; j++) {
        /* Store as fixed-point for now, let caller convert to tile coords */
        f->points[j].x = (int32_t)(way->coords[j].lon * 1e7);
        f->points[j].y = (int32_t)(way->coords[j].lat * 1e7);
    }

    (*count)++;
    return CT_OK;
}

CTStatus ct_pbf_get_bbox_features(const CTPBFContext *ctx, CTBBox bbox,
                                  CTFeature **features, size_t *count)
{
    if (!ctx || !features || !count) return CT_ERROR_INVALID_ARGUMENT;

    /* Allocate output array */
    size_t capacity = 1024;
    *features = malloc(capacity * sizeof(CTFeature));
    if (!*features) return CT_ERROR_OUT_OF_MEMORY;
    *count = 0;

    /* Use R-Tree if available (O(log n) query) */
    if (ctx->rtree) {
        /* Query R-Tree for candidate ways */
        size_t max_candidates = ctx->num_ways;
        uint32_t *candidates = malloc(max_candidates * sizeof(uint32_t));
        if (!candidates) {
            free(*features);
            *features = NULL;
            return CT_ERROR_OUT_OF_MEMORY;
        }

        size_t num_candidates = ct_rtree_query(ctx->rtree, bbox, candidates, max_candidates);

        /* Convert candidates to features - R-Tree already filtered by bbox */
        for (size_t i = 0; i < num_candidates; i++) {
            uint32_t way_idx = candidates[i];
            if (way_idx >= ctx->num_ways) continue;

            const CTOSMWay *way = &ctx->ways[way_idx];

            /* Trust R-Tree bbox filter - skip redundant per-point checks */
            CTStatus status = add_way_as_feature(way, features, count, &capacity);
            if (status != CT_OK) {
                free(candidates);
                free(*features);
                *features = NULL;
                *count = 0;
                return status;
            }
        }

        free(candidates);
        return CT_OK;
    }

    /* Fallback: linear scan (O(n)) */
    for (size_t i = 0; i < ctx->num_ways; i++) {
        const CTOSMWay *way = &ctx->ways[i];

        /* Quick bbox check */
        int intersects = 0;
        for (int j = 0; j < way->num_coords; j++) {
            if (way->coords[j].lat >= bbox.min_lat &&
                way->coords[j].lat <= bbox.max_lat &&
                way->coords[j].lon >= bbox.min_lon &&
                way->coords[j].lon <= bbox.max_lon) {
                intersects = 1;
                break;
            }
        }

        if (!intersects) continue;

        CTStatus status = add_way_as_feature(way, features, count, &capacity);
        if (status != CT_OK) {
            free(*features);
            *features = NULL;
            *count = 0;
            return status;
        }
    }

    return CT_OK;
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

void ct_pbf_stats(const CTPBFContext *ctx,
                  size_t *total_nodes,
                  size_t *total_ways,
                  size_t *features_kept,
                  CTBBox *bbox)
{
    if (total_nodes) *total_nodes = ctx->total_nodes_parsed;
    if (total_ways) *total_ways = ctx->total_ways_parsed;
    if (features_kept) *features_kept = ctx->features_kept;
    if (bbox) *bbox = ctx->bbox;
}

/* ============================================================================
 * LOD-Aware Feature Extraction
 * ============================================================================ */

/*
 * Helper to add a way with LOD filtering.
 */
static CTStatus add_way_with_lod(const CTOSMWay *way, const struct CTLODConfig *lod,
                                 int zoom, CTFeature **features,
                                 size_t *count, size_t *capacity)
{
    /* LOD filter: check if visible at this zoom level */
    CTLayer layer = layer_from_osm_class(way->feature_class);
    if (!ct_lod_is_visible(lod, layer, way->feature_type,
                           zoom, way->area_sqm, way->length_m)) {
        return CT_OK;  /* Skip but not an error */
    }

    /* Expand array if needed */
    if (*count >= *capacity) {
        *capacity *= 2;
        CTFeature *new_features = realloc(*features, *capacity * sizeof(CTFeature));
        if (!new_features) {
            return CT_ERROR_OUT_OF_MEMORY;
        }
        *features = new_features;
    }

    /* Create feature */
    CTFeature *f = &(*features)[*count];
    memset(f, 0, sizeof(CTFeature));

    f->type = way->is_area ? CT_GEOM_POLYGON : CT_GEOM_LINESTRING;
    f->layer = layer;
    f->feature_type = way->feature_type;

    /* Allocate and copy coordinates */
    f->points = malloc(way->num_coords * sizeof(CTTilePoint));
    if (!f->points) return CT_OK;  /* Skip this feature but continue */

    f->num_points = way->num_coords;
    for (int j = 0; j < way->num_coords; j++) {
        f->points[j].x = (int32_t)(way->coords[j].lon * 1e7);
        f->points[j].y = (int32_t)(way->coords[j].lat * 1e7);
    }

    (*count)++;
    return CT_OK;
}

CTStatus ct_pbf_get_tile_features_lod(const CTPBFContext *ctx, CTTileCoord coord,
                                      const struct CTLODConfig *lod,
                                      CTFeature **features, size_t *count)
{
    if (!ctx || !features || !count) return CT_ERROR_INVALID_ARGUMENT;

    CTBBox bbox = ct_tile_bounds(coord);
    int zoom = coord.z;

    /* Allocate output array */
    size_t capacity = 1024;
    *features = malloc(capacity * sizeof(CTFeature));
    if (!*features) return CT_ERROR_OUT_OF_MEMORY;
    *count = 0;

    /* Use R-Tree if available (O(log n) query) */
    if (ctx->rtree) {
        /* Query R-Tree for candidate ways */
        size_t max_candidates = ctx->num_ways;
        uint32_t *candidates = malloc(max_candidates * sizeof(uint32_t));
        if (!candidates) {
            free(*features);
            *features = NULL;
            return CT_ERROR_OUT_OF_MEMORY;
        }

        size_t num_candidates = ct_rtree_query(ctx->rtree, bbox, candidates, max_candidates);

        /* Convert candidates to features with LOD filtering - R-Tree already filtered by bbox */
        for (size_t i = 0; i < num_candidates; i++) {
            uint32_t way_idx = candidates[i];
            if (way_idx >= ctx->num_ways) continue;

            const CTOSMWay *way = &ctx->ways[way_idx];

            /* Trust R-Tree bbox filter - skip redundant per-point checks */
            CTStatus status = add_way_with_lod(way, lod, zoom, features, count, &capacity);
            if (status != CT_OK) {
                free(candidates);
                free(*features);
                *features = NULL;
                *count = 0;
                return status;
            }
        }

        free(candidates);
        return CT_OK;
    }

    /* Fallback: linear scan (O(n)) */
    for (size_t i = 0; i < ctx->num_ways; i++) {
        const CTOSMWay *way = &ctx->ways[i];

        /* Quick bbox check first */
        int intersects = 0;
        for (int j = 0; j < way->num_coords; j++) {
            if (way->coords[j].lat >= bbox.min_lat &&
                way->coords[j].lat <= bbox.max_lat &&
                way->coords[j].lon >= bbox.min_lon &&
                way->coords[j].lon <= bbox.max_lon) {
                intersects = 1;
                break;
            }
        }
        if (!intersects) continue;

        CTStatus status = add_way_with_lod(way, lod, zoom, features, count, &capacity);
        if (status != CT_OK) {
            free(*features);
            *features = NULL;
            *count = 0;
            return status;
        }
    }

    return CT_OK;
}
