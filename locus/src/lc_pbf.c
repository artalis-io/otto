/*
 * lc_pbf.c - OSM PBF parsing for geocoding
 *
 * Extracts geocodable entities from OSM PBF files using the shared library.
 */

#include "lc_pbf.h"
#include "sh_protobuf.h"
#include "sh_inflate.h"
#include "sh_pbf.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ============================================================================
 * PBF Field Numbers (OSM-specific)
 * ============================================================================ */

/* PrimitiveBlock fields */
#define PBF_PRIMITIVEBLOCK_STRINGTABLE  1
#define PBF_PRIMITIVEBLOCK_PRIMITIVEGROUP 2
#define PBF_PRIMITIVEBLOCK_GRANULARITY  17
#define PBF_PRIMITIVEBLOCK_LAT_OFFSET   19
#define PBF_PRIMITIVEBLOCK_LON_OFFSET   20

/* PrimitiveGroup fields */
#define PBF_PRIMITIVEGROUP_NODES        1
#define PBF_PRIMITIVEGROUP_DENSE        2
#define PBF_PRIMITIVEGROUP_WAYS         3
#define PBF_PRIMITIVEGROUP_RELATIONS    4

/* DenseNodes fields */
#define PBF_DENSENODES_ID               1
#define PBF_DENSENODES_DENSEINFO        5
#define PBF_DENSENODES_LAT              8
#define PBF_DENSENODES_LON              9
#define PBF_DENSENODES_KEYS_VALS        10

/* Way fields */
#define PBF_WAY_ID                      1
#define PBF_WAY_KEYS                    2
#define PBF_WAY_VALS                    3
#define PBF_WAY_INFO                    4
#define PBF_WAY_REFS                    8

/* Relation fields */
#define PBF_RELATION_ID                 1
#define PBF_RELATION_KEYS               2
#define PBF_RELATION_VALS               3
#define PBF_RELATION_INFO               4
#define PBF_RELATION_ROLES_SID          8
#define PBF_RELATION_MEMIDS             9
#define PBF_RELATION_TYPES              10

/* ============================================================================
 * Node Coordinate Cache (Hash Table)
 *
 * OSM ways only store node ID references, not coordinates. To compute
 * centroids for streets, we cache all node coordinates during parsing.
 * ============================================================================ */

typedef struct {
    int64_t id;         /* Node ID (0 = empty slot) */
    int32_t lat_fixed;  /* Latitude as fixed point (1e-7 degrees) */
    int32_t lon_fixed;  /* Longitude as fixed point (1e-7 degrees) */
} LCNodeEntry;

typedef struct {
    LCNodeEntry *entries;
    size_t capacity;
    size_t count;
} LCNodeCache;

static LCNodeCache *node_cache_create(size_t initial_capacity)
{
    LCNodeCache *cache = calloc(1, sizeof(LCNodeCache));
    if (!cache) return NULL;

    /* Round up to power of 2 for efficient modulo */
    size_t cap = 1;
    while (cap < initial_capacity) cap *= 2;

    cache->entries = calloc(cap, sizeof(LCNodeEntry));
    if (!cache->entries) {
        free(cache);
        return NULL;
    }
    cache->capacity = cap;
    cache->count = 0;
    return cache;
}

static void node_cache_free(LCNodeCache *cache)
{
    if (cache) {
        free(cache->entries);
        free(cache);
    }
}

/* FNV-1a hash for node IDs */
static size_t node_hash(int64_t id, size_t capacity)
{
    uint64_t h = 14695981039346656037ULL;
    h ^= (uint64_t)id;
    h *= 1099511628211ULL;
    return (size_t)(h & (capacity - 1));  /* capacity is power of 2 */
}

static int node_cache_insert(LCNodeCache *cache, int64_t id, double lat, double lon)
{
    /* Grow if load factor > 0.7 */
    if (cache->count * 10 > cache->capacity * 7) {
        size_t new_cap = cache->capacity * 2;
        LCNodeEntry *new_entries = calloc(new_cap, sizeof(LCNodeEntry));
        if (!new_entries) return 0;

        /* Rehash all entries */
        for (size_t i = 0; i < cache->capacity; i++) {
            if (cache->entries[i].id != 0) {
                size_t idx = node_hash(cache->entries[i].id, new_cap);
                while (new_entries[idx].id != 0) {
                    idx = (idx + 1) & (new_cap - 1);
                }
                new_entries[idx] = cache->entries[i];
            }
        }
        free(cache->entries);
        cache->entries = new_entries;
        cache->capacity = new_cap;
    }

    /* Insert with linear probing */
    size_t idx = node_hash(id, cache->capacity);
    while (cache->entries[idx].id != 0) {
        if (cache->entries[idx].id == id) return 1;  /* Already exists */
        idx = (idx + 1) & (cache->capacity - 1);
    }

    cache->entries[idx].id = id;
    cache->entries[idx].lat_fixed = (int32_t)(lat * 1e7);
    cache->entries[idx].lon_fixed = (int32_t)(lon * 1e7);
    cache->count++;
    return 1;
}

static int node_cache_lookup(const LCNodeCache *cache, int64_t id, double *lat, double *lon)
{
    if (!cache || cache->count == 0) return 0;

    size_t idx = node_hash(id, cache->capacity);
    size_t start = idx;
    while (cache->entries[idx].id != 0) {
        if (cache->entries[idx].id == id) {
            *lat = cache->entries[idx].lat_fixed * 1e-7;
            *lon = cache->entries[idx].lon_fixed * 1e-7;
            return 1;
        }
        idx = (idx + 1) & (cache->capacity - 1);
        if (idx == start) break;  /* Full circle */
    }
    return 0;
}

/* ============================================================================
 * Context Structure
 * ============================================================================ */

struct LCPBFContext {
    LCEntityStore *entities;
    LCNodeCache *node_cache;
    SHBBox bounds;
    LCPBFStats stats;
    LCPBFOptions opts;
    clock_t start_time;
};

/* ============================================================================
 * Default Options
 * ============================================================================ */

void lc_pbf_default_options(LCPBFOptions *opts)
{
    if (!opts) return;
    opts->include_pois = 1;
    opts->include_addresses = 1;
    opts->include_streets = 1;
    opts->include_boundaries = 1;
    opts->min_admin_level = 2;
    opts->max_admin_level = 10;
}

/* ============================================================================
 * Tag Classification
 * ============================================================================ */

LCFeatureClass lc_classify_place_tag(const char *value)
{
    if (!value) return LC_CLASS_UNKNOWN;

    if (strcmp(value, "country") == 0) return LC_CLASS_COUNTRY;
    if (strcmp(value, "state") == 0) return LC_CLASS_STATE;
    if (strcmp(value, "region") == 0) return LC_CLASS_STATE;
    if (strcmp(value, "province") == 0) return LC_CLASS_STATE;
    if (strcmp(value, "county") == 0) return LC_CLASS_COUNTY;
    if (strcmp(value, "city") == 0) return LC_CLASS_CITY;
    if (strcmp(value, "town") == 0) return LC_CLASS_TOWN;
    if (strcmp(value, "village") == 0) return LC_CLASS_VILLAGE;
    if (strcmp(value, "suburb") == 0) return LC_CLASS_SUBURB;
    if (strcmp(value, "neighbourhood") == 0) return LC_CLASS_NEIGHBOURHOOD;
    if (strcmp(value, "neighborhood") == 0) return LC_CLASS_NEIGHBOURHOOD;
    if (strcmp(value, "quarter") == 0) return LC_CLASS_NEIGHBOURHOOD;
    if (strcmp(value, "hamlet") == 0) return LC_CLASS_HAMLET;
    if (strcmp(value, "locality") == 0) return LC_CLASS_LOCALITY;
    if (strcmp(value, "isolated_dwelling") == 0) return LC_CLASS_LOCALITY;

    return LC_CLASS_OTHER;
}

LCFeatureClass lc_classify_highway_tag(const char *value)
{
    if (!value) return LC_CLASS_UNKNOWN;

    /* Only streets that should be geocodable */
    if (strcmp(value, "residential") == 0) return LC_CLASS_STREET;
    if (strcmp(value, "primary") == 0) return LC_CLASS_STREET;
    if (strcmp(value, "secondary") == 0) return LC_CLASS_STREET;
    if (strcmp(value, "tertiary") == 0) return LC_CLASS_STREET;
    if (strcmp(value, "unclassified") == 0) return LC_CLASS_STREET;
    if (strcmp(value, "living_street") == 0) return LC_CLASS_STREET;
    if (strcmp(value, "pedestrian") == 0) return LC_CLASS_STREET;
    if (strcmp(value, "trunk") == 0) return LC_CLASS_STREET;
    if (strcmp(value, "motorway") == 0) return LC_CLASS_STREET;

    return LC_CLASS_UNKNOWN;
}

LCFeatureClass lc_classify_boundary_tag(int admin_level)
{
    switch (admin_level) {
        case 2: return LC_CLASS_COUNTRY;
        case 3:
        case 4: return LC_CLASS_STATE;
        case 5:
        case 6: return LC_CLASS_COUNTY;
        case 7:
        case 8: return LC_CLASS_CITY;
        case 9:
        case 10: return LC_CLASS_SUBURB;
        default: return LC_CLASS_OTHER;
    }
}

LCFeatureClass lc_classify_poi_tags(const char *amenity, const char *shop,
                                    const char *tourism, const char *leisure)
{
    if (amenity || shop || tourism || leisure) {
        return LC_CLASS_POI;
    }
    return LC_CLASS_UNKNOWN;
}

int lc_highway_is_named(const char *highway)
{
    if (!highway) return 0;

    /* These highway types typically have names */
    return (strcmp(highway, "residential") == 0 ||
            strcmp(highway, "primary") == 0 ||
            strcmp(highway, "secondary") == 0 ||
            strcmp(highway, "tertiary") == 0 ||
            strcmp(highway, "unclassified") == 0 ||
            strcmp(highway, "living_street") == 0 ||
            strcmp(highway, "pedestrian") == 0 ||
            strcmp(highway, "trunk") == 0 ||
            strcmp(highway, "motorway") == 0 ||
            strcmp(highway, "primary_link") == 0 ||
            strcmp(highway, "secondary_link") == 0 ||
            strcmp(highway, "tertiary_link") == 0 ||
            strcmp(highway, "trunk_link") == 0 ||
            strcmp(highway, "motorway_link") == 0);
}

/* ============================================================================
 * Context Management
 * ============================================================================ */

LCPBFContext *lc_pbf_context_create(void)
{
    LCPBFContext *ctx = calloc(1, sizeof(LCPBFContext));
    if (!ctx) return NULL;

    ctx->entities = lc_entity_store_create(100000);
    if (!ctx->entities) {
        free(ctx);
        return NULL;
    }

    /* Create node cache for way coordinate lookups
     * Initial size of 1M entries, will grow as needed */
    ctx->node_cache = node_cache_create(1024 * 1024);
    if (!ctx->node_cache) {
        lc_entity_store_free(ctx->entities);
        free(ctx);
        return NULL;
    }

    sh_bbox_init(&ctx->bounds);
    lc_pbf_default_options(&ctx->opts);

    return ctx;
}

void lc_pbf_context_free(LCPBFContext *ctx)
{
    if (!ctx) return;
    lc_entity_store_free(ctx->entities);
    node_cache_free(ctx->node_cache);
    free(ctx);
}

void lc_pbf_get_stats(const LCPBFContext *ctx, LCPBFStats *stats)
{
    if (!ctx || !stats) return;
    *stats = ctx->stats;
}

LCEntityStore *lc_pbf_take_entities(LCPBFContext *ctx)
{
    if (!ctx) return NULL;
    LCEntityStore *store = ctx->entities;
    ctx->entities = NULL;
    return store;
}

SHBBox lc_pbf_get_bounds(const LCPBFContext *ctx)
{
    SHBBox empty;
    sh_bbox_init(&empty);
    if (!ctx) return empty;
    return ctx->bounds;
}

/* ============================================================================
 * Tag Parsing Helpers
 * ============================================================================ */

typedef struct {
    const char *name;
    const char *name_en;
    const char *alt_name;
    const char *place;
    const char *highway;
    const char *boundary;
    const char *amenity;
    const char *shop;
    const char *tourism;
    const char *leisure;
    const char *natural;
    const char *waterway;
    const char *admin_level;
    const char *population;
    const char *addr_housenumber;
    const char *addr_street;
    const char *addr_city;
    const char *addr_postcode;
    const char *addr_state;
    const char *addr_country;
} ParsedTags;

static void parse_tags(const SHStringTable *st, const uint64_t *keys,
                       const uint64_t *vals, size_t count, ParsedTags *tags)
{
    memset(tags, 0, sizeof(ParsedTags));

    for (size_t i = 0; i < count; i++) {
        const char *key = sh_string_table_get(st, (size_t)keys[i]);
        const char *val = sh_string_table_get(st, (size_t)vals[i]);
        if (!key || !val) continue;

        if (strcmp(key, "name") == 0) tags->name = val;
        else if (strcmp(key, "name:en") == 0) tags->name_en = val;
        else if (strcmp(key, "alt_name") == 0) tags->alt_name = val;
        else if (strcmp(key, "place") == 0) tags->place = val;
        else if (strcmp(key, "highway") == 0) tags->highway = val;
        else if (strcmp(key, "boundary") == 0) tags->boundary = val;
        else if (strcmp(key, "amenity") == 0) tags->amenity = val;
        else if (strcmp(key, "shop") == 0) tags->shop = val;
        else if (strcmp(key, "tourism") == 0) tags->tourism = val;
        else if (strcmp(key, "leisure") == 0) tags->leisure = val;
        else if (strcmp(key, "natural") == 0) tags->natural = val;
        else if (strcmp(key, "waterway") == 0) tags->waterway = val;
        else if (strcmp(key, "admin_level") == 0) tags->admin_level = val;
        else if (strcmp(key, "population") == 0) tags->population = val;
        else if (strcmp(key, "addr:housenumber") == 0) tags->addr_housenumber = val;
        else if (strcmp(key, "addr:street") == 0) tags->addr_street = val;
        else if (strcmp(key, "addr:city") == 0) tags->addr_city = val;
        else if (strcmp(key, "addr:postcode") == 0) tags->addr_postcode = val;
        else if (strcmp(key, "addr:state") == 0) tags->addr_state = val;
        else if (strcmp(key, "addr:country") == 0) tags->addr_country = val;
    }
}

static char *dup_string(LCEntityStore *store, const char *s)
{
    if (!s || !s[0]) return NULL;
    return lc_entity_store_intern(store, s, strlen(s));
}

static void add_entity_from_tags(LCPBFContext *ctx, uint64_t osm_id,
                                 LCEntityType type, const ParsedTags *tags,
                                 double lat, double lon)
{
    /* Skip if no name and not an address */
    if (!tags->name && !tags->addr_housenumber) return;

    LCEntity entity;
    lc_entity_init(&entity);

    entity.osm_id = osm_id;
    entity.type = type;
    entity.centroid.lat = lat;
    entity.centroid.lon = lon;
    entity.bbox.min_lat = lat;
    entity.bbox.max_lat = lat;
    entity.bbox.min_lon = lon;
    entity.bbox.max_lon = lon;

    /* Determine feature class */
    if (tags->place) {
        entity.fclass = lc_classify_place_tag(tags->place);
        if (entity.fclass == LC_CLASS_UNKNOWN) entity.fclass = LC_CLASS_OTHER;
        ctx->stats.places_found++;
    } else if (tags->boundary && strcmp(tags->boundary, "administrative") == 0) {
        if (!ctx->opts.include_boundaries) return;
        int level = tags->admin_level ? atoi(tags->admin_level) : 0;
        if (level < ctx->opts.min_admin_level || level > ctx->opts.max_admin_level) return;
        entity.fclass = lc_classify_boundary_tag(level);
        entity.admin_level = (int8_t)level;
        ctx->stats.boundaries_found++;
    } else if (tags->highway && tags->name) {
        if (!ctx->opts.include_streets) return;
        entity.fclass = lc_classify_highway_tag(tags->highway);
        if (entity.fclass == LC_CLASS_UNKNOWN) return;
        ctx->stats.streets_found++;
    } else if (tags->addr_housenumber && tags->addr_street) {
        if (!ctx->opts.include_addresses) return;
        entity.fclass = LC_CLASS_ADDRESS;
        ctx->stats.addresses_found++;
    } else if (tags->amenity || tags->shop || tags->tourism || tags->leisure) {
        if (!ctx->opts.include_pois) return;
        if (!tags->name) return;  /* POIs need names */
        entity.fclass = LC_CLASS_POI;
        entity.poi_type = dup_string(ctx->entities,
            tags->amenity ? tags->amenity :
            tags->shop ? tags->shop :
            tags->tourism ? tags->tourism : tags->leisure);
        ctx->stats.pois_found++;
    } else if (tags->natural && strcmp(tags->natural, "water") == 0 && tags->name) {
        entity.fclass = LC_CLASS_WATER;
    } else if (tags->waterway && tags->name) {
        entity.fclass = LC_CLASS_WATER;
    } else {
        /* Has name but unknown type */
        if (tags->name) {
            entity.fclass = LC_CLASS_OTHER;
        } else {
            return;
        }
    }

    /* Copy name */
    entity.name = dup_string(ctx->entities, tags->name);

    /* Alternative names */
    int alt_count = 0;
    if (tags->name_en && tags->name && strcmp(tags->name_en, tags->name) != 0) alt_count++;
    if (tags->alt_name) alt_count++;

    if (alt_count > 0) {
        entity.alt_names = calloc(alt_count, sizeof(char *));
        if (entity.alt_names) {
            int idx = 0;
            if (tags->name_en && tags->name && strcmp(tags->name_en, tags->name) != 0) {
                entity.alt_names[idx++] = dup_string(ctx->entities, tags->name_en);
            }
            if (tags->alt_name) {
                entity.alt_names[idx++] = dup_string(ctx->entities, tags->alt_name);
            }
            entity.num_alt_names = (uint16_t)idx;
        }
    }

    /* Population */
    if (tags->population) {
        entity.population = atoi(tags->population);
    }

    /* Address components */
    if (tags->addr_housenumber) {
        entity.address.housenumber = dup_string(ctx->entities, tags->addr_housenumber);
    }
    if (tags->addr_street) {
        entity.address.street = dup_string(ctx->entities, tags->addr_street);
    }
    if (tags->addr_city) {
        entity.address.city = dup_string(ctx->entities, tags->addr_city);
    }
    if (tags->addr_postcode) {
        entity.address.postcode = dup_string(ctx->entities, tags->addr_postcode);
    }
    if (tags->addr_state) {
        entity.address.state = dup_string(ctx->entities, tags->addr_state);
    }
    if (tags->addr_country) {
        entity.address.country = dup_string(ctx->entities, tags->addr_country);
    }

    /* Update bounds */
    sh_bbox_expand(&ctx->bounds, entity.centroid);

    /* Add to store */
    lc_entity_store_add(ctx->entities, &entity);
}

/* ============================================================================
 * DenseNodes Parsing
 * ============================================================================ */

static void parse_dense_nodes(LCPBFContext *ctx, const uint8_t *data, size_t len,
                              const SHStringTable *st, const SHBlockHeader *hdr)
{
    const uint8_t *ptr = data;
    const uint8_t *end = data + len;

    /* Arrays for dense data */
    int64_t *ids = NULL;
    int64_t *lats = NULL;
    int64_t *lons = NULL;
    uint64_t *keys_vals = NULL;  /* Unsigned - string table indices */
    size_t id_count = 0, lat_count = 0, lon_count = 0, kv_count = 0;

    /* First pass: find field sizes */
    const uint8_t *p = ptr;
    while (p < end) {
        uint32_t field, wire;
        int n = sh_pb_read_tag(p, end - p, &field, &wire);
        if (n <= 0) break;
        p += n;

        if (wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t field_len;
            n = sh_pb_read_varint(p, end - p, &field_len);
            if (n <= 0) break;
            p += n;

            if (field == PBF_DENSENODES_ID) {
                id_count = sh_pb_count_packed_varint(p, field_len);
            } else if (field == PBF_DENSENODES_LAT) {
                lat_count = sh_pb_count_packed_varint(p, field_len);
            } else if (field == PBF_DENSENODES_LON) {
                lon_count = sh_pb_count_packed_varint(p, field_len);
            } else if (field == PBF_DENSENODES_KEYS_VALS) {
                kv_count = sh_pb_count_packed_varint(p, field_len);
            }
            p += field_len;
        } else {
            n = sh_pb_skip_field(p, end - p, wire);
            if (n <= 0) break;
            p += n;
        }
    }

    if (id_count == 0 || lat_count == 0 || lon_count == 0) return;
    if (id_count != lat_count || id_count != lon_count) return;

    /* Allocate arrays */
    ids = malloc(id_count * sizeof(int64_t));
    lats = malloc(lat_count * sizeof(int64_t));
    lons = malloc(lon_count * sizeof(int64_t));
    keys_vals = kv_count > 0 ? malloc(kv_count * sizeof(uint64_t)) : NULL;

    if (!ids || !lats || !lons || (kv_count > 0 && !keys_vals)) {
        free(ids); free(lats); free(lons); free(keys_vals);
        return;
    }

    /* Second pass: read data */
    p = ptr;
    while (p < end) {
        uint32_t field, wire;
        int n = sh_pb_read_tag(p, end - p, &field, &wire);
        if (n <= 0) break;
        p += n;

        if (wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t field_len;
            n = sh_pb_read_varint(p, end - p, &field_len);
            if (n <= 0) break;
            p += n;

            if (field == PBF_DENSENODES_ID) {
                sh_pb_read_packed_svarint_array(p, field_len, ids, id_count);
                sh_pb_delta_decode_i64(ids, id_count);
            } else if (field == PBF_DENSENODES_LAT) {
                sh_pb_read_packed_svarint_array(p, field_len, lats, lat_count);
                sh_pb_delta_decode_i64(lats, lat_count);
            } else if (field == PBF_DENSENODES_LON) {
                sh_pb_read_packed_svarint_array(p, field_len, lons, lon_count);
                sh_pb_delta_decode_i64(lons, lon_count);
            } else if (field == PBF_DENSENODES_KEYS_VALS && keys_vals) {
                /* keys_vals uses unsigned varints (string table indices) */
                sh_pb_read_packed_varint_array(p, field_len, keys_vals, kv_count);
            }
            p += field_len;
        } else {
            n = sh_pb_skip_field(p, end - p, wire);
            if (n <= 0) break;
            p += n;
        }
    }

    /* Process nodes */
    size_t kv_idx = 0;
    for (size_t i = 0; i < id_count; i++) {
        ctx->stats.nodes_processed++;

        double lat = (hdr->lat_offset + (lats[i] * hdr->granularity)) * 1e-9;
        double lon = (hdr->lon_offset + (lons[i] * hdr->granularity)) * 1e-9;

        /* Cache ALL node coordinates for way centroid calculation */
        node_cache_insert(ctx->node_cache, ids[i], lat, lon);

        /* Collect tags for this node (keys_vals uses unsigned string table indices) */
        uint64_t node_keys[64], node_vals[64];
        size_t tag_count = 0;

        if (keys_vals && kv_idx < kv_count) {
            while (kv_idx < kv_count && keys_vals[kv_idx] != 0) {
                if (tag_count < 64 && kv_idx + 1 < kv_count) {
                    node_keys[tag_count] = keys_vals[kv_idx];
                    node_vals[tag_count] = keys_vals[kv_idx + 1];
                    tag_count++;
                }
                kv_idx += 2;
            }
            if (kv_idx < kv_count) kv_idx++;  /* Skip the 0 delimiter */
        }

        if (tag_count > 0) {
            ParsedTags tags;
            parse_tags(st, node_keys, node_vals, tag_count, &tags);
            add_entity_from_tags(ctx, (uint64_t)ids[i], LC_ENTITY_NODE, &tags, lat, lon);
        }
    }

    free(ids);
    free(lats);
    free(lons);
    free(keys_vals);
}

/* ============================================================================
 * Way Parsing
 * ============================================================================ */

static void parse_way(LCPBFContext *ctx, const uint8_t *data, size_t len,
                      const SHStringTable *st, const SHBlockHeader *hdr)
{
    (void)hdr;

    const uint8_t *ptr = data;
    const uint8_t *end = data + len;

    uint64_t way_id = 0;
    uint64_t keys[256], vals[256];  /* Unsigned - string table indices */
    size_t key_count = 0, val_count = 0;

    /* Node refs for centroid calculation */
    int64_t *refs = NULL;
    size_t ref_count = 0;

    /* First pass: get sizes */
    const uint8_t *p = ptr;
    while (p < end) {
        uint32_t field, wire;
        int n = sh_pb_read_tag(p, end - p, &field, &wire);
        if (n <= 0) break;
        p += n;

        if (wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t field_len;
            n = sh_pb_read_varint(p, end - p, &field_len);
            if (n <= 0) break;
            p += n;
            if (field == PBF_WAY_REFS) {
                ref_count = sh_pb_count_packed_varint(p, field_len);
            }
            p += field_len;
        } else if (wire == SH_PB_WIRE_VARINT) {
            uint64_t v;
            n = sh_pb_read_varint(p, end - p, &v);
            if (n <= 0) break;
            p += n;
        } else {
            n = sh_pb_skip_field(p, end - p, wire);
            if (n <= 0) break;
            p += n;
        }
    }

    /* Allocate refs array if needed */
    if (ref_count > 0) {
        refs = malloc(ref_count * sizeof(int64_t));
    }

    /* Second pass: read data */
    p = ptr;
    while (p < end) {
        uint32_t field, wire;
        int n = sh_pb_read_tag(p, end - p, &field, &wire);
        if (n <= 0) break;
        p += n;

        if (field == PBF_WAY_ID && wire == SH_PB_WIRE_VARINT) {
            uint64_t v;
            n = sh_pb_read_varint(p, end - p, &v);
            if (n <= 0) break;
            p += n;
            way_id = v;
        } else if (field == PBF_WAY_KEYS && wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t field_len;
            n = sh_pb_read_varint(p, end - p, &field_len);
            if (n <= 0) break;
            p += n;
            key_count = sh_pb_read_packed_varint_array(p, field_len, keys, 256);
            p += field_len;
        } else if (field == PBF_WAY_VALS && wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t field_len;
            n = sh_pb_read_varint(p, end - p, &field_len);
            if (n <= 0) break;
            p += n;
            val_count = sh_pb_read_packed_varint_array(p, field_len, vals, 256);
            p += field_len;
        } else if (field == PBF_WAY_REFS && wire == SH_PB_WIRE_LENGTH_DELIM && refs) {
            uint64_t field_len;
            n = sh_pb_read_varint(p, end - p, &field_len);
            if (n <= 0) break;
            p += n;
            sh_pb_read_packed_svarint_array(p, field_len, refs, ref_count);
            sh_pb_delta_decode_i64(refs, ref_count);  /* Refs are delta-encoded */
            p += field_len;
        } else {
            n = sh_pb_skip_field(p, end - p, wire);
            if (n <= 0) break;
            p += n;
        }
    }

    ctx->stats.ways_processed++;

    if (key_count > 0 && key_count == val_count) {
        ParsedTags tags;
        parse_tags(st, keys, vals, key_count, &tags);

        /* Compute centroid from node refs */
        double lat = 0.0, lon = 0.0;
        int found_coords = 0;

        if (refs && ref_count > 0 && ctx->node_cache) {
            double sum_lat = 0.0, sum_lon = 0.0;
            size_t coord_count = 0;

            for (size_t i = 0; i < ref_count; i++) {
                double node_lat, node_lon;
                if (node_cache_lookup(ctx->node_cache, refs[i], &node_lat, &node_lon)) {
                    sum_lat += node_lat;
                    sum_lon += node_lon;
                    coord_count++;
                }
            }

            if (coord_count > 0) {
                lat = sum_lat / coord_count;
                lon = sum_lon / coord_count;
                found_coords = 1;
            }
        }

        if (tags.name || (tags.addr_housenumber && tags.addr_street)) {
            add_entity_from_tags(ctx, way_id, LC_ENTITY_WAY, &tags, lat, lon);
        }

        (void)found_coords;  /* Suppress unused warning */
    }

    free(refs);
}

/* ============================================================================
 * Relation Parsing
 * ============================================================================ */

static void parse_relation(LCPBFContext *ctx, const uint8_t *data, size_t len,
                           const SHStringTable *st, const SHBlockHeader *hdr)
{
    (void)hdr;

    const uint8_t *ptr = data;
    const uint8_t *end = data + len;

    uint64_t rel_id = 0;
    uint64_t keys[256], vals[256];  /* Unsigned - string table indices */
    size_t key_count = 0, val_count = 0;

    while (ptr < end) {
        uint32_t field, wire;
        int n = sh_pb_read_tag(ptr, end - ptr, &field, &wire);
        if (n <= 0) break;
        ptr += n;

        if (field == PBF_RELATION_ID && wire == SH_PB_WIRE_VARINT) {
            uint64_t v;
            n = sh_pb_read_varint(ptr, end - ptr, &v);
            if (n <= 0) break;
            ptr += n;
            rel_id = v;
        } else if (field == PBF_RELATION_KEYS && wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t field_len;
            n = sh_pb_read_varint(ptr, end - ptr, &field_len);
            if (n <= 0) break;
            ptr += n;
            key_count = sh_pb_read_packed_varint_array(ptr, field_len, keys, 256);
            ptr += field_len;
        } else if (field == PBF_RELATION_VALS && wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t field_len;
            n = sh_pb_read_varint(ptr, end - ptr, &field_len);
            if (n <= 0) break;
            ptr += n;
            val_count = sh_pb_read_packed_varint_array(ptr, field_len, vals, 256);
            ptr += field_len;
        } else {
            n = sh_pb_skip_field(ptr, end - ptr, wire);
            if (n <= 0) break;
            ptr += n;
        }
    }

    ctx->stats.relations_processed++;

    if (key_count > 0 && key_count == val_count) {
        ParsedTags tags;
        parse_tags(st, keys, vals, key_count, &tags);

        /* Relations typically need member geometry - use 0,0 for now */
        /* TODO: Compute centroid from relation members */
        if (tags.name || tags.boundary) {
            add_entity_from_tags(ctx, rel_id, LC_ENTITY_RELATION, &tags, 0.0, 0.0);
        }
    }
}

/* ============================================================================
 * PrimitiveGroup Parsing
 * ============================================================================ */

static void parse_primitive_group(LCPBFContext *ctx, const uint8_t *data, size_t len,
                                  const SHStringTable *st, const SHBlockHeader *hdr)
{
    const uint8_t *ptr = data;
    const uint8_t *end = data + len;

    while (ptr < end) {
        uint32_t field, wire;
        int n = sh_pb_read_tag(ptr, end - ptr, &field, &wire);
        if (n <= 0) break;
        ptr += n;

        if (wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t field_len;
            n = sh_pb_read_varint(ptr, end - ptr, &field_len);
            if (n <= 0) break;
            ptr += n;

            if (field == PBF_PRIMITIVEGROUP_DENSE) {
                parse_dense_nodes(ctx, ptr, field_len, st, hdr);
            } else if (field == PBF_PRIMITIVEGROUP_WAYS) {
                parse_way(ctx, ptr, field_len, st, hdr);
            } else if (field == PBF_PRIMITIVEGROUP_RELATIONS) {
                parse_relation(ctx, ptr, field_len, st, hdr);
            }
            ptr += field_len;
        } else {
            n = sh_pb_skip_field(ptr, end - ptr, wire);
            if (n <= 0) break;
            ptr += n;
        }
    }
}

/* ============================================================================
 * PrimitiveBlock Parsing
 * ============================================================================ */

static void parse_primitive_block(LCPBFContext *ctx, const uint8_t *data, size_t len)
{
    const uint8_t *ptr = data;
    const uint8_t *end = data + len;

    SHStringTable st;
    sh_string_table_init(&st);

    SHBlockHeader hdr;
    sh_block_header_init(&hdr);

    /* First pass: get string table and header info */
    const uint8_t *p = ptr;
    while (p < end) {
        uint32_t field, wire;
        int n = sh_pb_read_tag(p, end - p, &field, &wire);
        if (n <= 0) break;
        p += n;

        if (wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t field_len;
            n = sh_pb_read_varint(p, end - p, &field_len);
            if (n <= 0) break;
            p += n;

            if (field == PBF_PRIMITIVEBLOCK_STRINGTABLE) {
                sh_string_table_parse(&st, p, field_len);
            }
            p += field_len;
        } else if (wire == SH_PB_WIRE_VARINT) {
            uint64_t v;
            n = sh_pb_read_varint(p, end - p, &v);
            if (n <= 0) break;
            p += n;

            if (field == PBF_PRIMITIVEBLOCK_GRANULARITY) {
                hdr.granularity = (int32_t)v;
            } else if (field == PBF_PRIMITIVEBLOCK_LAT_OFFSET) {
                hdr.lat_offset = (int64_t)v;
            } else if (field == PBF_PRIMITIVEBLOCK_LON_OFFSET) {
                hdr.lon_offset = (int64_t)v;
            }
        } else {
            n = sh_pb_skip_field(p, end - p, wire);
            if (n <= 0) break;
            p += n;
        }
    }

    /* Second pass: parse primitive groups */
    p = ptr;
    while (p < end) {
        uint32_t field, wire;
        int n = sh_pb_read_tag(p, end - p, &field, &wire);
        if (n <= 0) break;
        p += n;

        if (wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t field_len;
            n = sh_pb_read_varint(p, end - p, &field_len);
            if (n <= 0) break;
            p += n;

            if (field == PBF_PRIMITIVEBLOCK_PRIMITIVEGROUP) {
                parse_primitive_group(ctx, p, field_len, &st, &hdr);
            }
            p += field_len;
        } else {
            n = sh_pb_skip_field(p, end - p, wire);
            if (n <= 0) break;
            p += n;
        }
    }

    sh_string_table_free(&st);
}

/* ============================================================================
 * File Parsing
 * ============================================================================ */

LCStatus lc_pbf_parse_memory(LCPBFContext *ctx, const uint8_t *data,
                             size_t size, const LCPBFOptions *opts)
{
    if (!ctx || !data || size == 0) return LC_ERROR_INVALID_PARAM;

    if (opts) {
        ctx->opts = *opts;
    }

    ctx->start_time = clock();

    const uint8_t *ptr = data;
    const uint8_t *end = data + size;

    while (ptr < end) {
        /* Read blob header length (4 bytes big-endian) */
        if (ptr + 4 > end) break;
        uint32_t header_len = ((uint32_t)ptr[0] << 24) |
                              ((uint32_t)ptr[1] << 16) |
                              ((uint32_t)ptr[2] << 8) |
                              ((uint32_t)ptr[3]);
        ptr += 4;

        if (ptr + header_len > end) break;

        /* Parse blob header */
        char type[32] = {0};
        uint32_t data_size = 0;
        size_t consumed;
        SHStatus status = sh_pbf_parse_blob_header(ptr, header_len, type, sizeof(type),
                                                   &data_size, &consumed);
        if (status != SH_OK) break;
        ptr += header_len;

        if (ptr + data_size > end) break;

        /* Parse blob data */
        SHPBFBlob blob;
        status = sh_pbf_decompress_blob(ptr, data_size, &blob);
        if (status != SH_OK) {
            ptr += data_size;
            continue;
        }

        /* Process OSMData blocks */
        if (strcmp(type, "OSMData") == 0) {
            parse_primitive_block(ctx, blob.data, blob.len);
        }

        sh_pbf_blob_free(&blob);
        ptr += data_size;
    }

    clock_t end_time = clock();
    ctx->stats.parse_time_ms = (double)(end_time - ctx->start_time) * 1000.0 / CLOCKS_PER_SEC;

    return LC_OK;
}

LCStatus lc_pbf_parse_file(LCPBFContext *ctx, const char *filename,
                           const LCPBFOptions *opts)
{
    if (!ctx || !filename) return LC_ERROR_INVALID_PARAM;

    FILE *f = fopen(filename, "rb");
    if (!f) return LC_ERROR_FILE_NOT_FOUND;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(f);
        return LC_ERROR_PARSE_FAILED;
    }

    uint8_t *data = malloc((size_t)file_size);
    if (!data) {
        fclose(f);
        return LC_ERROR_OUT_OF_MEMORY;
    }

    size_t read = fread(data, 1, (size_t)file_size, f);
    fclose(f);

    if (read != (size_t)file_size) {
        free(data);
        return LC_ERROR_PARSE_FAILED;
    }

    LCStatus status = lc_pbf_parse_memory(ctx, data, (size_t)file_size, opts);
    free(data);

    return status;
}
