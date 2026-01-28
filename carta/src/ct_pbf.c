/*
 * ct_pbf.c - OSM PBF file parser for map features
 *
 * Parses OpenStreetMap Protocol Buffer Format files.
 * Extracts nodes and ways needed for map rendering.
 */

#include "ct_pbf.h"
#include "ct_tile.h"
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

/* Forward declarations */
int ct_pb_read_varint(const uint8_t *buf, size_t len, uint64_t *value);
int ct_pb_read_svarint(const uint8_t *buf, size_t len, int64_t *value);
int ct_pb_read_tag(const uint8_t *buf, size_t len, uint32_t *field, uint32_t *wire);
int ct_pb_skip_field(const uint8_t *buf, size_t len, uint32_t wire_type);
int ct_pb_read_fixed32(const uint8_t *buf, size_t len, uint32_t *value);
size_t ct_pb_read_packed_svarint_array(const uint8_t *buf, size_t len,
                                       int64_t *out, size_t out_capacity);
void ct_pb_delta_decode_i64(int64_t *arr, size_t count);
CTStatus ct_inflate(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t dst_len, size_t *actual_len);

/* ============================================================================
 * PBF Field Numbers
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
#define PBF_PRIMGROUP_DENSE     2
#define PBF_PRIMGROUP_WAYS      3

/* DenseNodes */
#define PBF_DENSE_ID            1
#define PBF_DENSE_LAT           8
#define PBF_DENSE_LON           9

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
} CTStringTable;

static void string_table_init(CTStringTable *st)
{
    st->strings = NULL;
    st->count = 0;
    st->capacity = 0;
}

static void string_table_free(CTStringTable *st)
{
    for (size_t i = 0; i < st->count; i++) {
        free(st->strings[i]);
    }
    free(st->strings);
    string_table_init(st);
}

static CTStatus string_table_add(CTStringTable *st, const uint8_t *data, size_t len)
{
    if (st->count >= st->capacity) {
        size_t new_cap = st->capacity ? st->capacity * 2 : 256;
        char **new_strings = realloc(st->strings, new_cap * sizeof(char *));
        if (!new_strings) return CT_ERROR_OUT_OF_MEMORY;
        st->strings = new_strings;
        st->capacity = new_cap;
    }

    char *s = malloc(len + 1);
    if (!s) return CT_ERROR_OUT_OF_MEMORY;
    memcpy(s, data, len);
    s[len] = '\0';
    st->strings[st->count++] = s;
    return CT_OK;
}

static const char *string_table_get(const CTStringTable *st, size_t idx)
{
    if (idx >= st->count) return "";
    return st->strings[idx];
}

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

static CTOSMFeatureClass classify_tags(const CTStringTable *st,
                                       const uint32_t *keys, const uint32_t *vals,
                                       int num_tags, int *feature_type, int *is_area)
{
    *feature_type = 0;
    *is_area = 0;

    for (int i = 0; i < num_tags; i++) {
        const char *key = string_table_get(st, keys[i]);
        const char *val = string_table_get(st, vals[i]);

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

    for (size_t i = 0; i < ctx->num_ways; i++) {
        free(ctx->ways[i].coords);
        free(ctx->ways[i].name);
    }
    free(ctx->ways);

    /* TODO: free rtree */

    free(ctx);
}

/* ============================================================================
 * PBF Parsing
 * ============================================================================ */

static CTStatus parse_string_table(const uint8_t *data, size_t len,
                                   CTStringTable *st)
{
    size_t pos = 0;

    while (pos < len) {
        uint32_t field, wire;
        int n = ct_pb_read_tag(data + pos, len - pos, &field, &wire);
        if (n < 0) return CT_ERROR_PARSE_ERROR;
        pos += n;

        if (field == PBF_STRINGTABLE_S && wire == 2) {
            uint64_t slen;
            n = ct_pb_read_varint(data + pos, len - pos, &slen);
            if (n < 0) return CT_ERROR_PARSE_ERROR;
            pos += n;

            CTStatus status = string_table_add(st, data + pos, (size_t)slen);
            if (status != CT_OK) return status;
            pos += slen;
        } else {
            n = ct_pb_skip_field(data + pos, len - pos, wire);
            if (n < 0) return CT_ERROR_PARSE_ERROR;
            pos += n;
        }
    }

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
        int n = ct_pb_read_tag(data + pos, len - pos, &field, &wire);
        if (n < 0) goto error;
        pos += n;

        if (wire == 2) {
            uint64_t packed_len;
            n = ct_pb_read_varint(data + pos, len - pos, &packed_len);
            if (n < 0) goto error;
            pos += n;

            if (field == PBF_DENSE_ID) {
                id_count = ct_pb_read_packed_svarint_array(data + pos, packed_len,
                                                          ids, max_nodes);
            } else if (field == PBF_DENSE_LAT) {
                lat_count = ct_pb_read_packed_svarint_array(data + pos, packed_len,
                                                           lats, max_nodes);
            } else if (field == PBF_DENSE_LON) {
                lon_count = ct_pb_read_packed_svarint_array(data + pos, packed_len,
                                                           lons, max_nodes);
            }
            pos += packed_len;
        } else {
            n = ct_pb_skip_field(data + pos, len - pos, wire);
            if (n < 0) goto error;
            pos += n;
        }
    }

    /* Delta decode */
    ct_pb_delta_decode_i64(ids, id_count);
    ct_pb_delta_decode_i64(lats, lat_count);
    ct_pb_delta_decode_i64(lons, lon_count);

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
                          const CTStringTable *st)
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
        int n = ct_pb_read_tag(data + pos, len - pos, &field, &wire);
        if (n < 0) goto skip_way;
        pos += n;

        if (field == PBF_WAY_ID && wire == 0) {
            uint64_t val;
            n = ct_pb_read_varint(data + pos, len - pos, &val);
            if (n < 0) goto skip_way;
            id = (int64_t)val;
            pos += n;
        } else if (field == PBF_WAY_KEYS && wire == 2) {
            uint64_t packed_len;
            n = ct_pb_read_varint(data + pos, len - pos, &packed_len);
            if (n < 0) goto skip_way;
            pos += n;

            const uint8_t *p = data + pos;
            size_t ppos = 0;
            while (ppos < packed_len && key_count < 256) {
                uint64_t v;
                int k = ct_pb_read_varint(p + ppos, packed_len - ppos, &v);
                if (k < 0) break;
                keys[key_count++] = (uint32_t)v;
                ppos += k;
            }
            pos += packed_len;
        } else if (field == PBF_WAY_VALS && wire == 2) {
            uint64_t packed_len;
            n = ct_pb_read_varint(data + pos, len - pos, &packed_len);
            if (n < 0) goto skip_way;
            pos += n;

            const uint8_t *p = data + pos;
            size_t ppos = 0;
            while (ppos < packed_len && val_count < 256) {
                uint64_t v;
                int k = ct_pb_read_varint(p + ppos, packed_len - ppos, &v);
                if (k < 0) break;
                vals[val_count++] = (uint32_t)v;
                ppos += k;
            }
            pos += packed_len;
        } else if (field == PBF_WAY_REFS && wire == 2) {
            uint64_t packed_len;
            n = ct_pb_read_varint(data + pos, len - pos, &packed_len);
            if (n < 0) goto skip_way;
            pos += n;

            ref_count = ct_pb_read_packed_svarint_array(data + pos, packed_len,
                                                        refs, max_refs);
            ct_pb_delta_decode_i64(refs, ref_count);
            pos += packed_len;
        } else {
            n = ct_pb_skip_field(data + pos, len - pos, wire);
            if (n < 0) goto skip_way;
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
                                      const CTStringTable *st,
                                      int32_t granularity, int64_t lat_offset, int64_t lon_offset)
{
    size_t pos = 0;

    while (pos < len) {
        uint32_t field, wire;
        int n = ct_pb_read_tag(data + pos, len - pos, &field, &wire);
        if (n < 0) return CT_ERROR_PARSE_ERROR;
        pos += n;

        if (wire == 2) {
            uint64_t msg_len;
            n = ct_pb_read_varint(data + pos, len - pos, &msg_len);
            if (n < 0) return CT_ERROR_PARSE_ERROR;
            pos += n;

            if (field == PBF_PRIMGROUP_DENSE) {
                CTStatus status = parse_dense_nodes(ctx, data + pos, msg_len,
                                                    granularity, lat_offset, lon_offset);
                if (status != CT_OK) return status;
            } else if (field == PBF_PRIMGROUP_WAYS) {
                CTStatus status = parse_way(ctx, data + pos, msg_len, st);
                if (status != CT_OK) return status;
            }
            pos += msg_len;
        } else {
            n = ct_pb_skip_field(data + pos, len - pos, wire);
            if (n < 0) return CT_ERROR_PARSE_ERROR;
            pos += n;
        }
    }

    return CT_OK;
}

static CTStatus parse_primitive_block(CTPBFContext *ctx, const uint8_t *data, size_t len)
{
    CTStringTable st;
    string_table_init(&st);

    int32_t granularity = 100;
    int64_t lat_offset = 0;
    int64_t lon_offset = 0;

    /* First pass: parse header and string table */
    size_t pos = 0;
    while (pos < len) {
        uint32_t field, wire;
        int n = ct_pb_read_tag(data + pos, len - pos, &field, &wire);
        if (n < 0) {
            string_table_free(&st);
            return CT_ERROR_PARSE_ERROR;
        }
        pos += n;

        if (field == PBF_PRIMBLOCK_STRINGTABLE && wire == 2) {
            uint64_t msg_len;
            n = ct_pb_read_varint(data + pos, len - pos, &msg_len);
            if (n < 0) {
                string_table_free(&st);
                return CT_ERROR_PARSE_ERROR;
            }
            pos += n;

            CTStatus status = parse_string_table(data + pos, msg_len, &st);
            if (status != CT_OK) {
                string_table_free(&st);
                return status;
            }
            pos += msg_len;
        } else if (field == PBF_PRIMBLOCK_GRANULARITY && wire == 0) {
            uint64_t val;
            n = ct_pb_read_varint(data + pos, len - pos, &val);
            if (n < 0) {
                string_table_free(&st);
                return CT_ERROR_PARSE_ERROR;
            }
            granularity = (int32_t)val;
            pos += n;
        } else if (field == PBF_PRIMBLOCK_LAT_OFFSET && wire == 0) {
            int64_t val;
            n = ct_pb_read_svarint(data + pos, len - pos, &val);
            if (n < 0) {
                string_table_free(&st);
                return CT_ERROR_PARSE_ERROR;
            }
            lat_offset = val;
            pos += n;
        } else if (field == PBF_PRIMBLOCK_LON_OFFSET && wire == 0) {
            int64_t val;
            n = ct_pb_read_svarint(data + pos, len - pos, &val);
            if (n < 0) {
                string_table_free(&st);
                return CT_ERROR_PARSE_ERROR;
            }
            lon_offset = val;
            pos += n;
        } else {
            n = ct_pb_skip_field(data + pos, len - pos, wire);
            if (n < 0) {
                string_table_free(&st);
                return CT_ERROR_PARSE_ERROR;
            }
            pos += n;
        }
    }

    /* Second pass: parse primitive groups */
    pos = 0;
    while (pos < len) {
        uint32_t field, wire;
        int n = ct_pb_read_tag(data + pos, len - pos, &field, &wire);
        if (n < 0) {
            string_table_free(&st);
            return CT_ERROR_PARSE_ERROR;
        }
        pos += n;

        if (field == PBF_PRIMBLOCK_PRIMITIVEGROUP && wire == 2) {
            uint64_t msg_len;
            n = ct_pb_read_varint(data + pos, len - pos, &msg_len);
            if (n < 0) {
                string_table_free(&st);
                return CT_ERROR_PARSE_ERROR;
            }
            pos += n;

            CTStatus status = parse_primitive_group(ctx, data + pos, msg_len, &st,
                                                    granularity, lat_offset, lon_offset);
            if (status != CT_OK) {
                string_table_free(&st);
                return status;
            }
            pos += msg_len;
        } else {
            n = ct_pb_skip_field(data + pos, len - pos, wire);
            if (n < 0) {
                string_table_free(&st);
                return CT_ERROR_PARSE_ERROR;
            }
            pos += n;
        }
    }

    string_table_free(&st);
    return CT_OK;
}

static CTStatus parse_blob(CTPBFContext *ctx, const uint8_t *data, size_t len)
{
    const uint8_t *raw_data = NULL;
    size_t raw_size = 0;
    const uint8_t *zlib_data = NULL;
    size_t zlib_size = 0;
    uint8_t *decompressed = NULL;

    size_t pos = 0;
    while (pos < len) {
        uint32_t field, wire;
        int n = ct_pb_read_tag(data + pos, len - pos, &field, &wire);
        if (n < 0) return CT_ERROR_PARSE_ERROR;
        pos += n;

        if (field == PBF_BLOB_RAW && wire == 2) {
            uint64_t msg_len;
            n = ct_pb_read_varint(data + pos, len - pos, &msg_len);
            if (n < 0) return CT_ERROR_PARSE_ERROR;
            pos += n;
            raw_data = data + pos;
            raw_size = msg_len;
            pos += msg_len;
        } else if (field == PBF_BLOB_RAW_SIZE && wire == 0) {
            uint64_t val;
            n = ct_pb_read_varint(data + pos, len - pos, &val);
            if (n < 0) return CT_ERROR_PARSE_ERROR;
            raw_size = val;
            pos += n;
        } else if (field == PBF_BLOB_ZLIB_DATA && wire == 2) {
            uint64_t msg_len;
            n = ct_pb_read_varint(data + pos, len - pos, &msg_len);
            if (n < 0) return CT_ERROR_PARSE_ERROR;
            pos += n;
            zlib_data = data + pos;
            zlib_size = msg_len;
            pos += msg_len;
        } else {
            n = ct_pb_skip_field(data + pos, len - pos, wire);
            if (n < 0) return CT_ERROR_PARSE_ERROR;
            pos += n;
        }
    }

    if (zlib_data && raw_size > 0) {
        decompressed = malloc(raw_size);
        if (!decompressed) return CT_ERROR_OUT_OF_MEMORY;

        size_t actual_size;
        CTStatus status = ct_inflate(zlib_data, zlib_size,
                                     decompressed, raw_size, &actual_size);
        if (status != CT_OK) {
            free(decompressed);
            return status;
        }

        status = parse_primitive_block(ctx, decompressed, actual_size);
        free(decompressed);
        return status;
    } else if (raw_data) {
        return parse_primitive_block(ctx, raw_data, raw_size);
    }

    return CT_OK;
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

        /* Parse blob header */
        char type[32] = "";
        uint32_t data_size = 0;

        size_t hpos = 0;
        while (hpos < header_len) {
            uint32_t field, wire;
            int n = ct_pb_read_tag(data + pos + hpos, header_len - hpos, &field, &wire);
            if (n < 0) return CT_ERROR_PARSE_ERROR;
            hpos += n;

            if (field == PBF_BLOBHEADER_TYPE && wire == 2) {
                uint64_t slen;
                n = ct_pb_read_varint(data + pos + hpos, header_len - hpos, &slen);
                if (n < 0) return CT_ERROR_PARSE_ERROR;
                hpos += n;
                if (slen < sizeof(type)) {
                    memcpy(type, data + pos + hpos, slen);
                    type[slen] = '\0';
                }
                hpos += slen;
            } else if (field == PBF_BLOBHEADER_DATASIZE && wire == 0) {
                uint64_t val;
                n = ct_pb_read_varint(data + pos + hpos, header_len - hpos, &val);
                if (n < 0) return CT_ERROR_PARSE_ERROR;
                data_size = (uint32_t)val;
                hpos += n;
            } else {
                n = ct_pb_skip_field(data + pos + hpos, header_len - hpos, wire);
                if (n < 0) return CT_ERROR_PARSE_ERROR;
                hpos += n;
            }
        }
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
    /* TODO: Build R-tree for fast spatial queries */
    /* For now, we'll do brute-force queries */
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

CTStatus ct_pbf_get_bbox_features(const CTPBFContext *ctx, CTBBox bbox,
                                  CTFeature **features, size_t *count)
{
    if (!ctx || !features || !count) return CT_ERROR_INVALID_ARGUMENT;

    /* Allocate output array */
    size_t capacity = 1024;
    *features = malloc(capacity * sizeof(CTFeature));
    if (!*features) return CT_ERROR_OUT_OF_MEMORY;
    *count = 0;

    /* Iterate through ways and check bbox intersection */
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

        /* Expand array if needed */
        if (*count >= capacity) {
            capacity *= 2;
            CTFeature *new_features = realloc(*features, capacity * sizeof(CTFeature));
            if (!new_features) {
                free(*features);
                *features = NULL;
                *count = 0;
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

        /* Convert coordinates to tile space */
        /* This will be done by the caller (tile generator) for now */
        f->points = malloc(way->num_coords * sizeof(CTTilePoint));
        if (!f->points) continue;

        f->num_points = way->num_coords;
        for (int j = 0; j < way->num_coords; j++) {
            /* Store as fixed-point for now, let caller convert to tile coords */
            f->points[j].x = (int32_t)(way->coords[j].lon * 1e7);
            f->points[j].y = (int32_t)(way->coords[j].lat * 1e7);
        }

        (*count)++;
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
