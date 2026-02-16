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
#include "shared.h"
#include "sh_protobuf.h"
#include "sh_inflate.h"
#include "sh_pbf.h"
#include "sh_pool.h"
#include "sh_hashmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#ifndef _WIN32
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

/* PBF field numbers - use shared definitions from sh_pbf.h */

/* ============================================================================
 * Parsing Constants
 * ============================================================================ */

/* Maximum elements in dense node parsing (per PrimitiveBlock)
 * OSM PBF spec: PrimitiveBlocks typically have ~8000 entities, but DenseNodes
 * can pack more. We use 100K as a safe upper bound (12x typical max). */
#define CT_MAX_DENSE_NODES       100000
#define CT_MAX_KEYS_VALS         500000

/* Maximum way node references */
#define CT_MAX_WAY_REFS          10000

/* Maximum relation members */
#define CT_MAX_RELATION_MEMBERS  10000

/* File size heuristics for hash table preallocation */
#define CT_BYTES_PER_NODE_ESTIMATE    40
#define CT_BYTES_PER_WAY_ESTIMATE     150
#define CT_COORDS_PER_WAY_ESTIMATE    12

/* Default arena and pool sizes (can be overridden via env vars) */
#define CT_DEFAULT_ARENA_SIZE         (128ULL * 1024 * 1024)
#define CT_MIN_COORD_POOL_CAPACITY    100000
#define CT_MIN_HASH_CAPACITY          65536

/* ============================================================================
 * Configuration
 * ============================================================================ */

static size_t parse_size_env(const char *name, size_t default_val)
{
    const char *val = getenv(name);
    if (!val) return default_val;

    char *end;
    unsigned long long n = strtoull(val, &end, 10);
    if (end == val) return default_val;

    /* Support K, M, G suffixes */
    switch (*end) {
        case 'k': case 'K': n *= 1024; break;
        case 'm': case 'M': n *= 1024 * 1024; break;
        case 'g': case 'G': n *= 1024ULL * 1024 * 1024; break;
    }
    return (size_t)n;
}

void ct_pbf_config_init(CTPBFConfig *config)
{
    if (!config) return;

    /* Initial coord capacity: 0 means auto-calculate from file size */
    config->initial_coord_capacity = parse_size_env("CARTA_INITIAL_COORDS", 0);
    config->arena_size = parse_size_env("CARTA_ARENA_SIZE", CT_DEFAULT_ARENA_SIZE);
    /* Memory limit: 0 means unlimited (default) */
    config->memory_limit = parse_size_env("CARTA_MEMORY_LIMIT", 0);
    config->progress_callback = NULL;
    config->progress_user_data = NULL;
    config->progress_interval = 10;  /* Report every 10 blobs by default */
}

/* ============================================================================
 * Memory Tracking
 * ============================================================================ */

/*
 * Check if allocation would exceed memory limit.
 * Returns 1 if OK to allocate, 0 if would exceed limit.
 */
static int check_memory_limit(CTPBFContext *ctx, size_t bytes)
{
    if (ctx->config.memory_limit == 0) return 1;  /* No limit */
    return (ctx->memory_used + bytes <= ctx->config.memory_limit);
}

/*
 * Track memory allocation.
 */
static void track_alloc(CTPBFContext *ctx, size_t bytes)
{
    ctx->memory_used += bytes;
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

static int classify_waterway(const char *value)
{
    /* Major rivers - Danube, Rhine, etc. */
    if (strcmp(value, "river") == 0 ||
        strcmp(value, "riverbank") == 0) {
        return CT_WATERWAY_RIVER;
    }
    /* Navigable canals */
    if (strcmp(value, "canal") == 0) {
        return CT_WATERWAY_CANAL;
    }
    /* Small streams */
    if (strcmp(value, "stream") == 0 ||
        strcmp(value, "brook") == 0 ||
        strcmp(value, "creek") == 0) {
        return CT_WATERWAY_STREAM;
    }
    /* Drainage */
    if (strcmp(value, "drain") == 0) {
        return CT_WATERWAY_DRAIN;
    }
    /* Ditches */
    if (strcmp(value, "ditch") == 0) {
        return CT_WATERWAY_DITCH;
    }
    return CT_WATERWAY_OTHER;
}

static int classify_railway(const char *value)
{
    /* Main rail lines */
    if (strcmp(value, "rail") == 0) {
        return CT_RAILWAY_RAIL;
    }
    /* Subway/metro */
    if (strcmp(value, "subway") == 0 ||
        strcmp(value, "metro") == 0) {
        return CT_RAILWAY_SUBWAY;
    }
    /* Trams and light rail */
    if (strcmp(value, "tram") == 0 ||
        strcmp(value, "light_rail") == 0) {
        return CT_RAILWAY_TRAM;
    }
    /* Narrow gauge */
    if (strcmp(value, "narrow_gauge") == 0) {
        return CT_RAILWAY_NARROW_GAUGE;
    }
    /* Heritage/preserved railways */
    if (strcmp(value, "preserved") == 0 ||
        strcmp(value, "heritage") == 0) {
        return CT_RAILWAY_PRESERVED;
    }
    /* Disused/abandoned */
    if (strcmp(value, "disused") == 0 ||
        strcmp(value, "abandoned") == 0) {
        return CT_RAILWAY_DISUSED;
    }
    return CT_RAILWAY_OTHER;
}

static int classify_landuse(const char *value)
{
    /* Forests and woods */
    if (strcmp(value, "forest") == 0) {
        return CT_LANDUSE_FOREST;
    }
    /* Residential areas */
    if (strcmp(value, "residential") == 0) {
        return CT_LANDUSE_RESIDENTIAL;
    }
    /* Commercial and retail */
    if (strcmp(value, "commercial") == 0 ||
        strcmp(value, "retail") == 0) {
        return CT_LANDUSE_COMMERCIAL;
    }
    /* Industrial */
    if (strcmp(value, "industrial") == 0) {
        return CT_LANDUSE_INDUSTRIAL;
    }
    /* Farmland and agricultural */
    if (strcmp(value, "farmland") == 0 ||
        strcmp(value, "meadow") == 0 ||
        strcmp(value, "farmyard") == 0 ||
        strcmp(value, "orchard") == 0 ||
        strcmp(value, "vineyard") == 0) {
        return CT_LANDUSE_FARMLAND;
    }
    /* Grass and village greens */
    if (strcmp(value, "grass") == 0 ||
        strcmp(value, "village_green") == 0 ||
        strcmp(value, "recreation_ground") == 0) {
        return CT_LANDUSE_GRASS;
    }
    /* Cemeteries */
    if (strcmp(value, "cemetery") == 0) {
        return CT_LANDUSE_CEMETERY;
    }
    /* Military */
    if (strcmp(value, "military") == 0) {
        return CT_LANDUSE_MILITARY;
    }
    return CT_LANDUSE_OTHER;
}

static int classify_natural(const char *value)
{
    /* Woods (natural=wood) treated as forest */
    if (strcmp(value, "wood") == 0) {
        return CT_LANDUSE_FOREST;
    }
    /* Grassland */
    if (strcmp(value, "grassland") == 0 ||
        strcmp(value, "heath") == 0 ||
        strcmp(value, "scrub") == 0) {
        return CT_LANDUSE_GRASS;
    }
    return CT_LANDUSE_OTHER;
}

static int classify_leisure(const char *value)
{
    /* Parks and nature reserves */
    if (strcmp(value, "park") == 0 ||
        strcmp(value, "nature_reserve") == 0 ||
        strcmp(value, "garden") == 0) {
        return CT_LANDUSE_PARK;
    }
    return CT_LANDUSE_OTHER;
}

/* ============================================================================
 * Place Classification (for labeled points)
 * ============================================================================ */

/*
 * Classify a place=* tag value and return min_zoom and priority.
 */
static CTPlaceType classify_place(const char *value, int *min_zoom, int *priority)
{
    if (strcmp(value, "country") == 0) {
        *min_zoom = 2; *priority = 100;
        return CT_PLACE_COUNTRY;
    }
    if (strcmp(value, "state") == 0) {
        *min_zoom = 4; *priority = 95;
        return CT_PLACE_STATE;
    }
    if (strcmp(value, "city") == 0) {
        *min_zoom = 6; *priority = 90;
        return CT_PLACE_CITY;
    }
    if (strcmp(value, "town") == 0) {
        *min_zoom = 9; *priority = 70;
        return CT_PLACE_TOWN;
    }
    if (strcmp(value, "village") == 0) {
        *min_zoom = 11; *priority = 50;
        return CT_PLACE_VILLAGE;
    }
    if (strcmp(value, "hamlet") == 0) {
        *min_zoom = 13; *priority = 30;
        return CT_PLACE_HAMLET;
    }
    if (strcmp(value, "suburb") == 0) {
        *min_zoom = 12; *priority = 40;
        return CT_PLACE_SUBURB;
    }
    if (strcmp(value, "neighbourhood") == 0 ||
        strcmp(value, "neighborhood") == 0) {
        *min_zoom = 14; *priority = 25;
        return CT_PLACE_NEIGHBOURHOOD;
    }
    if (strcmp(value, "locality") == 0) {
        *min_zoom = 14; *priority = 20;
        return CT_PLACE_LOCALITY;
    }
    if (strcmp(value, "island") == 0 ||
        strcmp(value, "islet") == 0) {
        *min_zoom = 8; *priority = 60;
        return CT_PLACE_ISLAND;
    }
    *min_zoom = 14; *priority = 10;
    return CT_PLACE_UNKNOWN;
}

/*
 * Add a labeled point to the context.
 */
static CTStatus add_labeled_point(CTPBFContext *ctx, int64_t id,
                                  double lat, double lon,
                                  CTPlaceType type, const char *name,
                                  int population, int min_zoom, int priority)
{
    if (!name || name[0] == '\0') {
        return CT_OK;  /* Skip unnamed places */
    }

    /* Grow array if needed */
    if (ctx->num_labeled_points >= ctx->labeled_points_capacity) {
        size_t new_cap = ctx->labeled_points_capacity ? ctx->labeled_points_capacity * 2 : 1024;
        CTLabeledPoint *new_pts = realloc(ctx->labeled_points, new_cap * sizeof(CTLabeledPoint));
        if (!new_pts) return CT_ERROR_OUT_OF_MEMORY;
        ctx->labeled_points = new_pts;
        ctx->labeled_points_capacity = new_cap;
    }

    CTLabeledPoint *pt = &ctx->labeled_points[ctx->num_labeled_points++];
    pt->id = id;
    pt->coord.lat = lat;
    pt->coord.lon = lon;
    pt->type = type;
    pt->name = strdup(name);
    if (!pt->name) {
        ctx->num_labeled_points--;
        return CT_ERROR_OUT_OF_MEMORY;
    }
    pt->population = population;
    pt->min_zoom = min_zoom;
    pt->priority = priority;

    /* Adjust priority based on population */
    if (population > 1000000) {
        pt->priority += 15;
        if (pt->min_zoom > 5) pt->min_zoom = 5;
    } else if (population > 500000) {
        pt->priority += 10;
        if (pt->min_zoom > 6) pt->min_zoom = 6;
    } else if (population > 100000) {
        pt->priority += 5;
        if (pt->min_zoom > 7) pt->min_zoom = 7;
    } else if (population > 50000) {
        pt->priority += 3;
    } else if (population > 10000) {
        pt->priority += 1;
    }

    return CT_OK;
}

static CTOSMFeatureClass classify_tags(const SHStringTable *st,
                                       const uint32_t *keys, const uint32_t *vals,
                                       int num_tags, int *feature_type, int *is_area,
                                       uint8_t *flags)
{
    *feature_type = 0;
    *is_area = 0;
    *flags = CT_FLAG_NONE;

    /* Track admin_level for boundary classification */
    int admin_level = CT_BOUNDARY_OTHER;
    int has_boundary = 0;

    /* Deferred feature class (set during tag scan, returned at end) */
    CTOSMFeatureClass feature_class = CT_OSM_UNKNOWN;
    const char *railway_val = NULL;

    /* First pass: check for area=yes, admin_level, bridge, tunnel, and classify */
    for (int i = 0; i < num_tags; i++) {
        const char *key = sh_string_table_get(st, keys[i]);
        const char *val = sh_string_table_get(st, vals[i]);

        /* Bridge/tunnel flags */
        if (strcmp(key, "bridge") == 0 && strcmp(val, "yes") == 0) {
            *flags |= CT_FLAG_BRIDGE;
        }
        if (strcmp(key, "tunnel") == 0 && strcmp(val, "yes") == 0) {
            *flags |= CT_FLAG_TUNNEL;
        }
        if (strcmp(key, "oneway") == 0 && strcmp(val, "yes") == 0) {
            *flags |= CT_FLAG_ONEWAY;
        }

        /* Area tag */
        if (strcmp(key, "area") == 0 && strcmp(val, "yes") == 0) {
            *is_area = 1;
        }

        /* Admin level */
        if (strcmp(key, "admin_level") == 0) {
            /* Parse admin_level with bounds check (OSM levels 1-12), default to 99 (OTHER) */
            int level = sh_parse_int(val, 99, 1, 12);
            /* Bucket to defined LOD levels (2, 4, 6, 8, 10) */
            if (level <= 2) admin_level = CT_BOUNDARY_COUNTRY;
            else if (level <= 4) admin_level = CT_BOUNDARY_STATE;
            else if (level <= 6) admin_level = CT_BOUNDARY_COUNTY;
            else if (level <= 8) admin_level = CT_BOUNDARY_CITY;
            else if (level <= 10) admin_level = CT_BOUNDARY_SUBURB;
            else admin_level = CT_BOUNDARY_OTHER;
        }
        if (strcmp(key, "boundary") == 0 && strcmp(val, "administrative") == 0) {
            has_boundary = 1;
        }

        /* Feature classification - continue to collect all tags for flags */
        if (strcmp(key, "highway") == 0 && feature_class == CT_OSM_UNKNOWN) {
            *feature_type = classify_highway(val);
            feature_class = CT_OSM_HIGHWAY;
        }
        if (strcmp(key, "waterway") == 0 && feature_class == CT_OSM_UNKNOWN) {
            /* riverbank is always an area (polygon) - use water body type */
            if (strcmp(val, "riverbank") == 0) {
                *feature_type = CT_WATER_RIVERBANK;
                *is_area = 1;
            } else {
                *feature_type = classify_waterway(val);
            }
            feature_class = CT_OSM_WATERWAY;
        }
        if (strcmp(key, "natural") == 0 && feature_class == CT_OSM_UNKNOWN) {
            if (strcmp(val, "water") == 0) {
                *feature_type = CT_WATER_BODY;  /* Distinct from linear waterways */
                *is_area = 1;
                feature_class = CT_OSM_WATER;
            } else {
                /* Woods and grassland go to landuse layer */
                *feature_type = classify_natural(val);
                *is_area = 1;
                feature_class = CT_OSM_NATURAL;
            }
        }
        if (strcmp(key, "building") == 0 && feature_class == CT_OSM_UNKNOWN) {
            *is_area = 1;
            feature_class = CT_OSM_BUILDING;
        }
        if (strcmp(key, "landuse") == 0 && feature_class == CT_OSM_UNKNOWN) {
            *feature_type = classify_landuse(val);
            *is_area = 1;
            feature_class = CT_OSM_LANDUSE;
        }
        if (strcmp(key, "leisure") == 0 && feature_class == CT_OSM_UNKNOWN) {
            *feature_type = classify_leisure(val);
            *is_area = 1;
            feature_class = CT_OSM_LANDUSE;  /* Parks go to landuse layer */
        }
        if (strcmp(key, "railway") == 0 && feature_class == CT_OSM_UNKNOWN) {
            railway_val = val;
            feature_class = CT_OSM_RAILWAY;
        }
    }

    /* Classify railway type if railway tag was found */
    if (feature_class == CT_OSM_RAILWAY && railway_val) {
        *feature_type = classify_railway(railway_val);
    }

    /* Handle administrative boundaries */
    if (feature_class == CT_OSM_UNKNOWN && has_boundary) {
        *feature_type = admin_level;
        feature_class = CT_OSM_BOUNDARY;
    }

    return feature_class;
}

/* ============================================================================
 * Hash Table Preallocation
 * ============================================================================ */

/*
 * Preallocate hash tables and coordinate pool based on file size heuristics.
 * This eliminates expensive rehashing and reallocation during parsing.
 *
 * Heuristics based on typical OSM PBF compression ratios:
 * - ~1 node per 50 bytes of compressed data
 * - ~1 way per 200 bytes of compressed data
 * - ~10 coordinates per way (average)
 * - Use 50% load factor for optimal hash performance
 */
static CTStatus preallocate_hash_tables(CTPBFContext *ctx, size_t file_size)
{
    /*
     * Growable data structures:
     * - node_map and way_map grow automatically in their insert functions
     * - coord_pool grows when full (see coord_pool_alloc helper)
     *
     * We estimate initial capacities from file size to reduce reallocation.
     */

    /* Estimate counts from file size */
    size_t estimated_nodes = file_size / CT_BYTES_PER_NODE_ESTIMATE;
    size_t estimated_ways = file_size / CT_BYTES_PER_WAY_ESTIMATE;
    size_t estimated_coords = estimated_ways * CT_COORDS_PER_WAY_ESTIMATE;

    /* Round up to power of 2 for efficient modulo (hash maps will grow if needed) */
    size_t node_cap = CT_MIN_HASH_CAPACITY;
    while (node_cap < estimated_nodes * 2 && node_cap < SIZE_MAX / 2) {
        node_cap *= 2;
    }

    size_t way_cap = CT_MIN_HASH_CAPACITY / 4;  /* Usually fewer ways than nodes */
    while (way_cap < estimated_ways * 2 && way_cap < SIZE_MAX / 2) {
        way_cap *= 2;
    }

    /* Allocate node_map using shared hashmap */
    ctx->node_map = sh_hashmap_i64_create(node_cap);
    if (!ctx->node_map) {
        return CT_ERROR_OUT_OF_MEMORY;
    }

    /* Allocate way_map using shared hashmap */
    ctx->way_map = sh_hashmap_i64_create(way_cap);
    if (!ctx->way_map) {
        sh_hashmap_i64_free(ctx->node_map);
        ctx->node_map = NULL;
        return CT_ERROR_OUT_OF_MEMORY;
    }

    /* Allocate coordinate pool (will grow if needed) */
    size_t coord_cap = ctx->config.initial_coord_capacity;
    if (coord_cap == 0) {
        /* Auto-calculate from file size estimate */
        coord_cap = estimated_coords;
    }
    if (coord_cap < CT_MIN_COORD_POOL_CAPACITY) {
        coord_cap = CT_MIN_COORD_POOL_CAPACITY;
    }

    ctx->coord_pool = malloc(sizeof(SHPool));
    if (!ctx->coord_pool) {
        return CT_ERROR_OUT_OF_MEMORY;
    }
    if (sh_pool_init(ctx->coord_pool, sizeof(CTCoord), coord_cap) != 0) {
        free(ctx->coord_pool);
        ctx->coord_pool = NULL;
        return CT_ERROR_OUT_OF_MEMORY;
    }

    return CT_OK;
}

/* ============================================================================
 * Node Map Wrappers (using shared hashmap)
 * ============================================================================ */

static CTStatus node_map_insert(CTPBFContext *ctx, int64_t id, size_t index)
{
    SHHashmapStatus status = sh_hashmap_i64_insert(ctx->node_map, id, index);
    return (status == SH_HASHMAP_OK) ? CT_OK : CT_ERROR_OUT_OF_MEMORY;
}

static size_t node_map_lookup(const CTPBFContext *ctx, int64_t id)
{
    return sh_hashmap_i64_lookup(ctx->node_map, id);
}

/* ============================================================================
 * Way Map Wrappers (using shared hashmap)
 * ============================================================================ */

static CTStatus way_map_insert(CTPBFContext *ctx, int64_t id, size_t index)
{
    SHHashmapStatus status = sh_hashmap_i64_insert(ctx->way_map, id, index);
    return (status == SH_HASHMAP_OK) ? CT_OK : CT_ERROR_OUT_OF_MEMORY;
}

/* ============================================================================
 * Coordinate Pool (Growable)
 * ============================================================================ */

/*
 * Allocate coordinates from pool, or fall back to malloc if pool is full.
 *
 * NOTE: We cannot grow the pool via realloc because existing way->coords
 * pointers would become invalid. Instead, we fall back to individual malloc
 * when the pool is exhausted. These malloc'd coords are freed individually
 * in ct_pbf_context_free (by checking if they're outside pool bounds).
 */
static CTCoord *coord_pool_alloc(CTPBFContext *ctx, size_t count)
{
    if (count == 0) return NULL;

    size_t alloc_size = count * sizeof(CTCoord);

    /* Check memory limit */
    if (!check_memory_limit(ctx, alloc_size)) {
        return NULL;
    }

    /* Try pool first */
    if (ctx->coord_pool) {
        size_t offset = sh_pool_alloc(ctx->coord_pool, count);
        if (offset != SH_POOL_INVALID) {
            track_alloc(ctx, alloc_size);
            return SH_POOL_PTR(ctx->coord_pool, CTCoord, offset);
        }
    }

    /* Pool full or unavailable - fall back to malloc */
    CTCoord *coords = malloc(alloc_size);
    if (coords) {
        track_alloc(ctx, alloc_size);
    }
    return coords;
}

/* ============================================================================
 * Role String Pool with Hash Lookup
 * ============================================================================ */

/*
 * Hash table for fast role string deduplication.
 * Uses djb2 hash with linear probing.
 * Role strings are few (<100 unique) so 256 buckets is plenty.
 *
 * THREAD SAFETY: Hash table is stored in CTPBFContext (not global static)
 * so each parsing context is independent.
 */
#define ROLE_HASH_SIZE 256

/* DJB2 hash function */
static uint32_t djb2_hash(const char *str)
{
    uint32_t hash = 5381;
    int c;
    while ((c = (unsigned char)*str++)) {
        hash = ((hash << 5) + hash) + c;  /* hash * 33 + c */
    }
    /* Ensure non-zero (0 means empty slot) */
    return hash ? hash : 1;
}

static void role_hash_reset(CTPBFContext *ctx)
{
    memset(ctx->role_hash, 0, sizeof(ctx->role_hash));
    ctx->role_hash_initialized = 1;
}

static uint32_t role_hash_lookup(const CTPBFContext *ctx, const char *role, uint32_t hash)
{
    (void)role;  /* Hash is pre-computed, role string only for debugging */
    uint32_t idx = hash % ROLE_HASH_SIZE;
    for (int i = 0; i < ROLE_HASH_SIZE; i++) {
        if (ctx->role_hash[idx].hash == 0) {
            return UINT32_MAX;  /* Not found */
        }
        if (ctx->role_hash[idx].hash == hash) {
            return ctx->role_hash[idx].role_idx;
        }
        idx = (idx + 1) % ROLE_HASH_SIZE;
    }
    return UINT32_MAX;  /* Not found, table full */
}

static void role_hash_insert(CTPBFContext *ctx, uint32_t hash, uint32_t role_idx)
{
    uint32_t idx = hash % ROLE_HASH_SIZE;
    for (int i = 0; i < ROLE_HASH_SIZE; i++) {
        if (ctx->role_hash[idx].hash == 0) {
            ctx->role_hash[idx].hash = hash;
            ctx->role_hash[idx].role_idx = role_idx;
            return;
        }
        idx = (idx + 1) % ROLE_HASH_SIZE;
    }
    /* Table full - shouldn't happen with 256 slots for ~50 roles */
}

static uint32_t add_role_string(CTPBFContext *ctx, const char *role)
{
    /* Empty role maps to index 0 */
    if (!role || role[0] == '\0') return 0;

    /* Initialize hash table if needed */
    if (!ctx->role_hash_initialized) {
        role_hash_reset(ctx);
    }

    /* Hash lookup for deduplication */
    uint32_t hash = djb2_hash(role);
    uint32_t existing_role_idx = role_hash_lookup(ctx, role, hash);
    if (existing_role_idx != UINT32_MAX && existing_role_idx > 0) {
        /* Convert 1-based role_idx to 0-based array index */
        uint32_t arr_idx = existing_role_idx - 1;
        /* Verify hash collision isn't a false positive */
        if (arr_idx < ctx->num_role_strings &&
            ctx->role_strings[arr_idx] &&
            strcmp(ctx->role_strings[arr_idx], role) == 0) {
            return existing_role_idx;
        }
        /* Hash collision with different string - fall through to add */
    }

    /* Add new role */
    if (ctx->num_role_strings >= ctx->role_strings_capacity) {
        size_t new_cap = ctx->role_strings_capacity ? ctx->role_strings_capacity * 2 : 64;
        char **new_strs = realloc(ctx->role_strings, new_cap * sizeof(char *));
        if (!new_strs) return 0;
        ctx->role_strings = new_strs;
        ctx->role_strings_capacity = new_cap;
    }

    uint32_t new_idx = (uint32_t)ctx->num_role_strings;
    ctx->role_strings[new_idx] = strdup(role);
    if (!ctx->role_strings[new_idx]) return 0;

    ctx->num_role_strings++;

    /* Add to hash table - use 1-based index to match ct_get_role_string */
    uint32_t role_idx = new_idx + 1;
    role_hash_insert(ctx, hash, role_idx);

    return role_idx;  /* 1-based: role 0 is reserved for empty, 1 = first real role */
}

/* ============================================================================
 * Context Management
 * ============================================================================ */

CTPBFContext *ct_pbf_context_create_with_config(const CTPBFConfig *config)
{
    CTPBFContext *ctx = calloc(1, sizeof(CTPBFContext));
    if (!ctx) return NULL;

    /* Apply configuration (use defaults if not specified) */
    CTPBFConfig default_config;
    if (!config) {
        ct_pbf_config_init(&default_config);
        config = &default_config;
    }

    ctx->config.initial_coord_capacity = config->initial_coord_capacity;  /* 0 = auto-calculate */
    ctx->config.arena_size = config->arena_size ? config->arena_size : CT_DEFAULT_ARENA_SIZE;
    ctx->config.memory_limit = config->memory_limit;  /* 0 = unlimited */
    ctx->memory_used = sizeof(CTPBFContext);  /* Track our own size */

    ctx->progress_callback = config->progress_callback;
    ctx->progress_user_data = config->progress_user_data;
    ctx->progress_interval = config->progress_interval ? config->progress_interval : 10;

    ctx->bbox.min_lat = 90;
    ctx->bbox.max_lat = -90;
    ctx->bbox.min_lon = 180;
    ctx->bbox.max_lon = -180;

    /* Initialize role string hash table for this context */
    role_hash_reset(ctx);

    /* Initialize boundary configuration with defaults */
    ctx->boundary_config.min_admin_level = 2;   /* Country borders */
    ctx->boundary_config.max_admin_level = 6;   /* Down to county level */
    ctx->boundary_config.include_protected_areas = 1;  /* Include national parks */

    /* Create arena for parsing temporaries */
    ctx->parse_arena = sh_arena_create(ctx->config.arena_size);
    if (!ctx->parse_arena) {
        free(ctx);
        return NULL;
    }

    /* Coordinate pool starts NULL; will be allocated in ct_pbf_parse_memory()
     * based on file size heuristics for optimal preallocation */
    ctx->coord_pool = NULL;

    return ctx;
}

CTPBFContext *ct_pbf_context_create(void)
{
    return ct_pbf_context_create_with_config(NULL);
}

void ct_pbf_context_free(CTPBFContext *ctx)
{
    if (!ctx) return;

    SAFE_FREE(ctx->nodes.ids);
    SAFE_FREE(ctx->nodes.coords);

    /* Free hash maps */
    sh_hashmap_i64_free(ctx->node_map);
    ctx->node_map = NULL;
    sh_hashmap_i64_free(ctx->way_map);
    ctx->way_map = NULL;

    if (ctx->mmap_base) {
        /* mmap'd context - ways point into allocated block, names are strdup'd */
        for (size_t i = 0; i < ctx->num_ways; i++) {
            SAFE_FREE(ctx->ways[i].name);
        }
        SAFE_FREE(ctx->ways);
        SAFE_FREE(ctx->mmap_coords);

        /* R-Tree nodes point into mmap, just free the struct */
        if (ctx->rtree && !ctx->rtree_is_mmap) {
            ct_rtree_free(ctx->rtree);
            ctx->rtree = NULL;
        } else if (ctx->rtree) {
            free(ctx->rtree);
            ctx->rtree = NULL;
        }

        /* Unmap the file */
        munmap(ctx->mmap_base, ctx->mmap_size);
        ctx->mmap_base = NULL;
    } else {
        /* Normal context - free everything */
        /* Determine pool boundaries for checking if coords are pool-allocated */
        char *pool_start = NULL;
        char *pool_end = NULL;
        if (ctx->coord_pool && ctx->coord_pool->data) {
            pool_start = (char *)ctx->coord_pool->data;
            pool_end = pool_start + (ctx->coord_pool->capacity * ctx->coord_pool->elem_size);
        }

        for (size_t i = 0; i < ctx->num_ways; i++) {
            /* Only free coords if they're NOT in the coordinate pool */
            if (ctx->ways[i].coords) {
                char *coord_ptr = (char *)ctx->ways[i].coords;
                int in_pool = (pool_start && coord_ptr >= pool_start && coord_ptr < pool_end);
                if (!in_pool) {
                    free(ctx->ways[i].coords);
                }
                ctx->ways[i].coords = NULL;
            }
            SAFE_FREE(ctx->ways[i].name);
        }
        SAFE_FREE(ctx->ways);

        ct_rtree_free(ctx->rtree);
        ctx->rtree = NULL;
    }

    /* Free relations */
    for (size_t i = 0; i < ctx->num_relations; i++) {
        SAFE_FREE(ctx->relations[i].members);
        SAFE_FREE(ctx->relations[i].name);
    }
    SAFE_FREE(ctx->relations);

    /* Free role strings */
    for (size_t i = 0; i < ctx->num_role_strings; i++) {
        SAFE_FREE(ctx->role_strings[i]);
    }
    SAFE_FREE(ctx->role_strings);

    /* Free assembled multipolygons */
    if (ctx->mmap_mp_coords || ctx->mmap_mp_rings) {
        /* mmap'd context - coords and rings are in bulk-allocated blocks */
        for (size_t i = 0; i < ctx->num_multipolygons; i++) {
            SAFE_FREE(ctx->multipolygons[i].name);
        }
        SAFE_FREE(ctx->multipolygons);
        SAFE_FREE(ctx->mmap_mp_coords);
        SAFE_FREE(ctx->mmap_mp_rings);

        /* Free multipolygon R-Tree (may point into mmap) */
        if (ctx->mp_rtree && !ctx->mp_rtree_is_mmap) {
            ct_rtree_free(ctx->mp_rtree);
        } else if (ctx->mp_rtree) {
            free(ctx->mp_rtree);
        }
        ctx->mp_rtree = NULL;
    } else {
        /* Normal context - free everything individually */
        for (size_t i = 0; i < ctx->num_multipolygons; i++) {
            for (int r = 0; r < ctx->multipolygons[i].num_rings; r++) {
                SAFE_FREE(ctx->multipolygons[i].rings[r].coords);
            }
            SAFE_FREE(ctx->multipolygons[i].rings);
            SAFE_FREE(ctx->multipolygons[i].name);
        }
        SAFE_FREE(ctx->multipolygons);

        /* Free multipolygon R-Tree */
        ct_rtree_free(ctx->mp_rtree);
        ctx->mp_rtree = NULL;
    }

    /* Free boundary relations */
    for (size_t i = 0; i < ctx->num_boundary_relations; i++) {
        SAFE_FREE(ctx->boundary_relations[i].members);
        SAFE_FREE(ctx->boundary_relations[i].name);
    }
    SAFE_FREE(ctx->boundary_relations);

    /* Free assembled boundaries */
    if (ctx->mmap_boundary_coords) {
        /* mmap'd context - coords in bulk-allocated block */
        for (size_t i = 0; i < ctx->num_boundaries; i++) {
            SAFE_FREE(ctx->boundaries[i].name);
        }
        SAFE_FREE(ctx->boundaries);
        SAFE_FREE(ctx->mmap_boundary_coords);

        /* Free boundary R-Tree (may point into mmap) */
        if (ctx->boundary_rtree && !ctx->boundary_rtree_is_mmap) {
            ct_rtree_free(ctx->boundary_rtree);
        } else if (ctx->boundary_rtree) {
            free(ctx->boundary_rtree);
        }
        ctx->boundary_rtree = NULL;
    } else {
        /* Normal context - free everything individually */
        for (size_t i = 0; i < ctx->num_boundaries; i++) {
            SAFE_FREE(ctx->boundaries[i].coords);
            SAFE_FREE(ctx->boundaries[i].name);
        }
        SAFE_FREE(ctx->boundaries);

        /* Free boundary R-Tree */
        ct_rtree_free(ctx->boundary_rtree);
        ctx->boundary_rtree = NULL;
    }

    /* Free labeled points */
    for (size_t i = 0; i < ctx->num_labeled_points; i++) {
        SAFE_FREE(ctx->labeled_points[i].name);
    }
    SAFE_FREE(ctx->labeled_points);

    /* Free parsing arena */
    sh_arena_free(ctx->parse_arena);
    ctx->parse_arena = NULL;

    /* Free coordinate pool */
    if (ctx->coord_pool) {
        sh_pool_free(ctx->coord_pool);
        free(ctx->coord_pool);
        ctx->coord_pool = NULL;
    }

    /* Free multipolygon assembly scratch buffers */
    SAFE_FREE(ctx->mp_scratch.outer_segs);
    SAFE_FREE(ctx->mp_scratch.inner_segs);

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
                                  const SHStringTable *st,
                                  int32_t granularity, int64_t lat_offset, int64_t lon_offset)
{
    int64_t *ids = NULL, *lats = NULL, *lons = NULL;
    uint64_t *keys_vals = NULL;
    size_t id_count = 0, lat_count = 0, lon_count = 0, kv_count = 0;
    size_t pos = 0;

    /* Temporary arrays for delta-encoded values (allocated from parsing arena) */
    size_t max_nodes = CT_MAX_DENSE_NODES;
    size_t max_kv = CT_MAX_KEYS_VALS;
    ids = sh_arena_alloc(ctx->parse_arena, max_nodes * sizeof(int64_t));
    lats = sh_arena_alloc(ctx->parse_arena, max_nodes * sizeof(int64_t));
    lons = sh_arena_alloc(ctx->parse_arena, max_nodes * sizeof(int64_t));
    keys_vals = sh_arena_alloc(ctx->parse_arena, max_kv * sizeof(uint64_t));
    if (!ids || !lats || !lons || !keys_vals) {
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
            } else if (field == SH_PBF_DENSE_KEYS_VALS) {
                /* Read keys_vals as unsigned varints */
                kv_count = sh_pb_read_packed_varint_array(data + pos, packed_len,
                                                         keys_vals, max_kv);
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
        while (new_cap < ctx->nodes.count + count) {
            if (new_cap > SIZE_MAX / 2) { goto error; }
            new_cap *= 2;
        }

        /* Realloc one at a time to avoid dangling pointer on partial failure */
        int64_t *new_ids = realloc(ctx->nodes.ids, new_cap * sizeof(int64_t));
        if (!new_ids) {
            goto error;
        }
        ctx->nodes.ids = new_ids;

        CTCoord *new_coords = realloc(ctx->nodes.coords, new_cap * sizeof(CTCoord));
        if (!new_coords) {
            goto error;
        }
        ctx->nodes.coords = new_coords;
        ctx->nodes.capacity = new_cap;
    }

    /* Process nodes and extract labeled points from tags */
    size_t kv_pos = 0;  /* Position in keys_vals array */

    for (size_t i = 0; i < count; i++) {
        double lat = (lat_offset + lats[i] * granularity) * 1e-9;
        double lon = (lon_offset + lons[i] * granularity) * 1e-9;

        size_t idx = ctx->nodes.count++;
        ctx->nodes.ids[idx] = ids[i];
        ctx->nodes.coords[idx].lat = lat;
        ctx->nodes.coords[idx].lon = lon;

        node_map_insert(ctx, ids[i], idx);

        /* Process tags for this node (if keys_vals available) */
        if (kv_pos < kv_count) {
            CTPlaceType place_type = CT_PLACE_UNKNOWN;
            const char *name = NULL;
            int population = 0;
            int min_zoom = 14;
            int priority = 10;
            int is_peak = 0;

            /* Parse key=value pairs until we hit 0 (separator) */
            while (kv_pos < kv_count && keys_vals[kv_pos] != 0) {
                uint32_t key_idx = keys_vals[kv_pos++];
                if (kv_pos >= kv_count) break;
                uint32_t val_idx = keys_vals[kv_pos++];

                const char *key = sh_string_table_get(st, key_idx);
                const char *val = sh_string_table_get(st, val_idx);

                if (strcmp(key, "place") == 0) {
                    place_type = classify_place(val, &min_zoom, &priority);
                } else if (strcmp(key, "name") == 0) {
                    name = val;
                } else if (strcmp(key, "population") == 0) {
                    /* Parse population with bounds check, default 0 */
                    population = sh_parse_int(val, 0, 0, 100000000);
                } else if (strcmp(key, "natural") == 0 && strcmp(val, "peak") == 0) {
                    is_peak = 1;
                }
            }

            /* Skip the 0 separator */
            if (kv_pos < kv_count && keys_vals[kv_pos] == 0) {
                kv_pos++;
            }

            /* Handle natural=peak as a place type */
            if (is_peak && place_type == CT_PLACE_UNKNOWN) {
                place_type = CT_PLACE_PEAK;
                min_zoom = 12;
                priority = 35;
            }

            /* Add labeled point if it has a place type and name */
            if (place_type != CT_PLACE_UNKNOWN && name) {
                CTStatus status = add_labeled_point(ctx, ids[i], lat, lon,
                                                    place_type, name,
                                                    population, min_zoom, priority);
                if (status != CT_OK) {
                    /* Non-fatal: just skip this point */
                }
            }
        }
    }

    ctx->total_nodes_parsed += count;

    /* No free needed - arena is reset after PrimitiveBlock */
    return CT_OK;

error:
    /* No free needed - arena is reset after PrimitiveBlock */
    return CT_ERROR_PARSE_ERROR;
}

/* Shared parsing buffers - allocated once per primitive group from the arena.
 * This avoids arena exhaustion when parsing many ways/relations while remaining
 * thread-safe (each context has its own arena). */
typedef struct {
    int64_t way_refs[CT_MAX_WAY_REFS];
    uint32_t way_keys[256];
    uint32_t way_vals[256];
    int64_t relation_memids[CT_MAX_RELATION_MEMBERS];
    uint32_t relation_role_sids[CT_MAX_RELATION_MEMBERS];
    uint32_t relation_types[CT_MAX_RELATION_MEMBERS];
    uint32_t relation_keys[256];
    uint32_t relation_vals[256];
} CTParseBuffers;

static CTStatus parse_way(CTPBFContext *ctx, const uint8_t *data, size_t len,
                          const SHStringTable *st, CTParseBuffers *bufs)
{
    int64_t id = 0;
    int64_t *refs = bufs->way_refs;
    uint32_t *keys = bufs->way_keys;
    uint32_t *vals = bufs->way_vals;
    size_t key_count = 0, val_count = 0, ref_count = 0;
    size_t pos = 0;
    size_t max_refs = CT_MAX_WAY_REFS;

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
    uint8_t flags = CT_FLAG_NONE;
    int num_tags = (int)(key_count < val_count ? key_count : val_count);
    CTOSMFeatureClass feature_class = classify_tags(st, keys, vals, num_tags,
                                                    &feature_type, &is_area, &flags);

    /*
     * NOTE: We store ALL ways, even unclassified ones (CT_OSM_UNKNOWN).
     * Multipolygon relations reference member ways that often have no tags -
     * the tags are on the relation itself. Unclassified ways are stored for
     * geometry lookup during multipolygon assembly, but won't be added to
     * the R-Tree spatial index (filtered in ct_rtree_build).
     */

    /* Resolve node references to coordinates
     * Allocate from pool if space available, otherwise malloc
     * (context_free handles both cases via pool bounds check)
     */
    CTCoord *coords = coord_pool_alloc(ctx, ref_count);
    if (!coords) goto skip_way;

    size_t coord_count = 0;
    for (size_t i = 0; i < ref_count; i++) {
        size_t idx = node_map_lookup(ctx, refs[i]);
        if (idx != SIZE_MAX && idx < ctx->nodes.count) {
            coords[coord_count++] = ctx->nodes.coords[idx];
        }
    }

    if (coord_count < 2) {
        /* Skip ways with < 2 coords. Memory waste is minor since
         * pool coords can't be freed individually and malloc'd coords
         * will be freed in context_free via pool bounds check */
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
            /* Pool coords cannot be individually freed */
            goto skip_way;
        }
        ctx->ways = new_ways;
        ctx->ways_capacity = new_cap;
    }

    size_t way_idx = ctx->num_ways;
    CTOSMWay *way = &ctx->ways[ctx->num_ways++];
    way->id = id;
    way->coords = coords;
    way->num_coords = (int)coord_count;
    way->feature_class = feature_class;
    way->feature_type = feature_type;
    way->is_area = is_area;
    way->flags = flags;
    way->name = NULL;

    /* Extract "name" tag for road/area labels */
    for (int i = 0; i < num_tags; i++) {
        const char *key = sh_string_table_get(st, keys[i]);
        if (strcmp(key, "name") == 0) {
            const char *val = sh_string_table_get(st, vals[i]);
            if (val && val[0]) {
                way->name = strdup(val);
                if (!way->name) break;  /* OOM - leave name as NULL */
            }
            break;
        }
    }

    /* Register in way_map for relation member lookup */
    way_map_insert(ctx, id, way_idx);

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

    /* Only count classified features (unclassified are geometry-only for relations) */
    if (feature_class != CT_OSM_UNKNOWN) {
        ctx->features_kept++;
    }

skip_way:
    /* No free needed - arena is reset after PrimitiveBlock */
    return CT_OK;
}

/*
 * Parse a Relation message.
 * Extracts member IDs, types, roles for multipolygon assembly.
 */
static CTStatus parse_relation(CTPBFContext *ctx, const uint8_t *data, size_t len,
                               const SHStringTable *st, CTParseBuffers *bufs)
{
    int64_t id = 0;
    int64_t *memids = bufs->relation_memids;
    uint32_t *role_sids = bufs->relation_role_sids;
    uint32_t *types = bufs->relation_types;
    uint32_t *keys = bufs->relation_keys;
    uint32_t *vals = bufs->relation_vals;
    size_t key_count = 0, val_count = 0;
    size_t role_count = 0, memid_count = 0, type_count = 0;
    size_t pos = 0;
    size_t max_members = CT_MAX_RELATION_MEMBERS;

    while (pos < len) {
        uint32_t field, wire;
        int n = sh_pb_read_tag(data + pos, len - pos, &field, &wire);
        if (n == 0) goto skip_relation;
        pos += n;

        if (field == SH_PBF_RELATION_ID && wire == SH_PB_WIRE_VARINT) {
            uint64_t val;
            n = sh_pb_read_varint(data + pos, len - pos, &val);
            if (n == 0) goto skip_relation;
            id = (int64_t)val;
            pos += n;
        } else if (field == SH_PBF_RELATION_KEYS && wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t packed_len;
            n = sh_pb_read_varint(data + pos, len - pos, &packed_len);
            if (n == 0) goto skip_relation;
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
        } else if (field == SH_PBF_RELATION_VALS && wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t packed_len;
            n = sh_pb_read_varint(data + pos, len - pos, &packed_len);
            if (n == 0) goto skip_relation;
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
        } else if (field == SH_PBF_RELATION_ROLES_SID && wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t packed_len;
            n = sh_pb_read_varint(data + pos, len - pos, &packed_len);
            if (n == 0) goto skip_relation;
            pos += n;

            const uint8_t *p = data + pos;
            size_t ppos = 0;
            while (ppos < packed_len && role_count < max_members) {
                uint64_t v;
                int k = sh_pb_read_varint(p + ppos, packed_len - ppos, &v);
                if (k == 0) break;
                role_sids[role_count++] = (uint32_t)v;
                ppos += k;
            }
            pos += packed_len;
        } else if (field == SH_PBF_RELATION_MEMIDS && wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t packed_len;
            n = sh_pb_read_varint(data + pos, len - pos, &packed_len);
            if (n == 0) goto skip_relation;
            pos += n;

            memid_count = sh_pb_read_packed_svarint_array(data + pos, packed_len,
                                                          memids, max_members);
            sh_pb_delta_decode_i64(memids, memid_count);
            pos += packed_len;
        } else if (field == SH_PBF_RELATION_TYPES && wire == SH_PB_WIRE_LENGTH_DELIM) {
            uint64_t packed_len;
            n = sh_pb_read_varint(data + pos, len - pos, &packed_len);
            if (n == 0) goto skip_relation;
            pos += n;

            const uint8_t *p = data + pos;
            size_t ppos = 0;
            while (ppos < packed_len && type_count < max_members) {
                uint64_t v;
                int k = sh_pb_read_varint(p + ppos, packed_len - ppos, &v);
                if (k == 0) break;
                types[type_count++] = (uint32_t)v;
                ppos += k;
            }
            pos += packed_len;
        } else {
            n = sh_pb_skip_field(data + pos, len - pos, wire);
            if (n == 0) goto skip_relation;
            pos += n;
        }
    }

    ctx->total_relations_parsed++;

    /* Check relation type and boundary tags */
    int is_multipolygon = 0;
    int is_boundary = 0;
    int is_protected_area = 0;
    int admin_level = -1;
    int num_tags = (int)(key_count < val_count ? key_count : val_count);
    CTOSMFeatureClass feature_class = CT_OSM_UNKNOWN;
    int feature_type = 0;

    for (int i = 0; i < num_tags; i++) {
        const char *key = sh_string_table_get(st, keys[i]);
        const char *val = sh_string_table_get(st, vals[i]);

        if (strcmp(key, "type") == 0) {
            if (strcmp(val, "multipolygon") == 0) {
                is_multipolygon = 1;
            } else if (strcmp(val, "boundary") == 0) {
                is_boundary = 1;
            }
        }
        if (strcmp(key, "boundary") == 0) {
            if (strcmp(val, "administrative") == 0) {
                is_boundary = 1;
            } else if (strcmp(val, "protected_area") == 0) {
                is_protected_area = 1;
            }
        }
        if (strcmp(key, "admin_level") == 0) {
            /* Parse admin_level with bounds check (OSM levels 1-12), default -1 (unset) */
            admin_level = sh_parse_int(val, -1, 1, 12);
        }
    }

    /* Handle boundary relations separately */
    if (is_boundary || is_protected_area) {
        /* Check config for which boundaries to extract */
        CTBoundaryConfig *cfg = &ctx->boundary_config;
        int keep_boundary = 0;

        if (is_protected_area && cfg->include_protected_areas) {
            keep_boundary = 1;
        } else if (is_boundary && admin_level >= 0) {
            if (admin_level >= cfg->min_admin_level &&
                admin_level <= cfg->max_admin_level) {
                keep_boundary = 1;
            }
        }

        if (keep_boundary) {
            /* Store boundary relation */
            if (ctx->num_boundary_relations >= ctx->boundary_relations_capacity) {
                size_t new_cap = ctx->boundary_relations_capacity ? ctx->boundary_relations_capacity * 2 : 100;
                CTOSMRelation *new_rels = realloc(ctx->boundary_relations, new_cap * sizeof(CTOSMRelation));
                if (!new_rels) goto skip_relation;
                ctx->boundary_relations = new_rels;
                ctx->boundary_relations_capacity = new_cap;
            }

            /* Build member list (only keep way members) */
            size_t num_members = memid_count;
            if (role_count < num_members) num_members = role_count;
            if (type_count < num_members) num_members = type_count;

            /* Count way members */
            size_t way_member_count = 0;
            for (size_t i = 0; i < num_members; i++) {
                if (types[i] == CT_MEMBER_WAY) way_member_count++;
            }

            if (way_member_count == 0) goto skip_relation;

            CTRelationMember *members = malloc(way_member_count * sizeof(CTRelationMember));
            if (!members) goto skip_relation;

            size_t j = 0;
            for (size_t i = 0; i < num_members; i++) {
                if (types[i] != CT_MEMBER_WAY) continue;
                members[j].ref = memids[i];
                members[j].type = CT_MEMBER_WAY;
                const char *role_str = sh_string_table_get(st, role_sids[i]);
                members[j].role_idx = add_role_string(ctx, role_str);
                j++;
            }

            CTOSMRelation *rel = &ctx->boundary_relations[ctx->num_boundary_relations++];
            rel->id = id;
            rel->members = members;
            rel->num_members = (int)way_member_count;
            rel->feature_class = CT_OSM_BOUNDARY;
            rel->feature_type = is_protected_area ? CT_BOUNDARY_TYPE_PROTECTED : CT_BOUNDARY_TYPE_ADMIN;
            rel->is_multipolygon = 0;
            rel->name = NULL;

            /* Extract name and store admin_level in feature_type for admin boundaries */
            if (!is_protected_area && admin_level >= 0) {
                /* Encode admin_level in the upper bits of feature_type */
                rel->feature_type = (admin_level << 8) | CT_BOUNDARY_TYPE_ADMIN;
            }

            for (int i = 0; i < num_tags; i++) {
                const char *key = sh_string_table_get(st, keys[i]);
                if (strcmp(key, "name") == 0) {
                    const char *val = sh_string_table_get(st, vals[i]);
                    if (val && val[0]) rel->name = strdup(val);
                    break;
                }
            }
        }
        goto skip_relation;  /* Don't also process as multipolygon */
    }

    /* Only keep multipolygon relations */
    if (!is_multipolygon) {
        goto skip_relation;
    }

    /* Classify the relation based on tags (water, landuse, etc.) */
    for (int i = 0; i < num_tags; i++) {
        const char *key = sh_string_table_get(st, keys[i]);
        const char *val = sh_string_table_get(st, vals[i]);

        if (strcmp(key, "natural") == 0) {
            if (strcmp(val, "water") == 0) {
                feature_class = CT_OSM_WATER;
                feature_type = CT_WATER_BODY;  /* Water body polygon */
            } else if (strcmp(val, "wood") == 0 || strcmp(val, "forest") == 0) {
                feature_class = CT_OSM_NATURAL;
                feature_type = CT_LANDUSE_FOREST;
            }
        } else if (strcmp(key, "landuse") == 0) {
            feature_class = CT_OSM_LANDUSE;
            feature_type = classify_landuse(val);
        } else if (strcmp(key, "water") == 0) {
            feature_class = CT_OSM_WATER;
            feature_type = CT_WATER_BODY;  /* All water=* tags are polygons */
        } else if (strcmp(key, "waterway") == 0) {
            feature_class = CT_OSM_WATERWAY;
            if (strcmp(val, "riverbank") == 0) {
                feature_type = CT_WATER_RIVERBANK;  /* Riverbank polygon */
            } else {
                feature_type = classify_waterway(val);
            }
        } else if (strcmp(key, "building") == 0) {
            /* Building multipolygons: buildings with courtyards/holes */
            feature_class = CT_OSM_BUILDING;
            feature_type = 0;  /* No sub-type for buildings */
        }
    }

    /* Skip relations without useful classification */
    if (feature_class == CT_OSM_UNKNOWN) {
        goto skip_relation;
    }

    /* Store the relation */
    if (ctx->num_relations >= ctx->relations_capacity) {
        size_t new_cap = ctx->relations_capacity ? ctx->relations_capacity * 2 : 100;
        CTOSMRelation *new_rels = realloc(ctx->relations, new_cap * sizeof(CTOSMRelation));
        if (!new_rels) goto skip_relation;
        ctx->relations = new_rels;
        ctx->relations_capacity = new_cap;
    }

    /* Build member list */
    size_t num_members = memid_count;
    if (role_count < num_members) num_members = role_count;
    if (type_count < num_members) num_members = type_count;

    CTRelationMember *members = malloc(num_members * sizeof(CTRelationMember));
    if (!members) goto skip_relation;

    for (size_t i = 0; i < num_members; i++) {
        members[i].ref = memids[i];
        members[i].type = (CTMemberType)types[i];

        /* Store role string index in our pool */
        const char *role_str = sh_string_table_get(st, role_sids[i]);
        members[i].role_idx = add_role_string(ctx, role_str);
    }

    CTOSMRelation *rel = &ctx->relations[ctx->num_relations++];
    rel->id = id;
    rel->members = members;
    rel->num_members = (int)num_members;
    rel->feature_class = feature_class;
    rel->feature_type = feature_type;
    rel->is_multipolygon = is_multipolygon;
    rel->name = NULL;

    /* Extract name if present */
    for (int i = 0; i < num_tags; i++) {
        const char *key = sh_string_table_get(st, keys[i]);
        if (strcmp(key, "name") == 0) {
            const char *val = sh_string_table_get(st, vals[i]);
            if (val && val[0]) rel->name = strdup(val);
            break;
        }
    }

skip_relation:
    /* No free needed - arena is reset after PrimitiveBlock */
    return CT_OK;
}

static CTStatus parse_primitive_group(CTPBFContext *ctx, const uint8_t *data, size_t len,
                                      const SHStringTable *st,
                                      int32_t granularity, int64_t lat_offset, int64_t lon_offset,
                                      CTParseBuffers *bufs)
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
                CTStatus status = parse_dense_nodes(ctx, data + pos, msg_len, st,
                                                    granularity, lat_offset, lon_offset);
                if (status != CT_OK) return status;
            } else if (field == SH_PBF_PRIMGROUP_WAYS) {
                CTStatus status = parse_way(ctx, data + pos, msg_len, st, bufs);
                if (status != CT_OK) return status;
            } else if (field == SH_PBF_PRIMGROUP_RELATIONS) {
                CTStatus status = parse_relation(ctx, data + pos, msg_len, st, bufs);
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
    /* Allocate shared parsing buffers from arena - reused for all ways/relations in this block */
    CTParseBuffers *bufs = sh_arena_alloc(ctx->parse_arena, sizeof(CTParseBuffers));
    if (!bufs) {
        return CT_ERROR_OUT_OF_MEMORY;
    }

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
                                                    granularity, lat_offset, lon_offset, bufs);
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

    /* Reset parsing arena after each PrimitiveBlock (temporaries no longer needed) */
    sh_arena_reset(ctx->parse_arena);

    sh_pbf_blob_free(&blob);
    return status;
}

/* Helper to report progress */
static void report_progress(CTPBFContext *ctx, const char *phase,
                            size_t current, size_t total)
{
    if (ctx->progress_callback) {
        ctx->progress_callback(phase, current, total, ctx->progress_user_data);
    }
}

CTStatus ct_pbf_parse_memory(CTPBFContext *ctx, const uint8_t *data, size_t size)
{
    /* Report start */
    report_progress(ctx, "parsing", 0, size);

    /* Preallocate hash tables based on file size to avoid rehashing */
    if (ctx->node_map == NULL) {
        CTStatus status = preallocate_hash_tables(ctx, size);
        if (status != CT_OK) {
            return status;
        }
    }

    size_t pos = 0;
    size_t blob_count = 0;
    size_t interval = ctx->progress_interval ? ctx->progress_interval : 10;

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
            blob_count++;

            /* Report progress periodically */
            if (blob_count % interval == 0) {
                report_progress(ctx, "parsing", pos, size);
            }
        }

        pos += data_size;
    }

    /* Report completion */
    report_progress(ctx, "parsing", size, size);

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

    report_progress(ctx, "indexing", 0, ctx->num_ways);

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

    report_progress(ctx, "indexing", ctx->num_ways, ctx->num_ways);

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
    /* Skip unclassified ways (geometry-only, kept for multipolygon assembly) */
    if (way->feature_class == CT_OSM_UNKNOWN) {
        return CT_OK;
    }

    /* Skip ways with no coordinates (prevents crash in MVT encoder) */
    if (way->num_coords == 0) {
        return CT_OK;
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
    f->layer = layer_from_osm_class(way->feature_class);
    f->feature_type = way->feature_type;
    f->flags = way->flags;
    f->area_sqm = way->area_sqm;
    f->length_m = way->length_m;

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

/*
 * Estimate area in square meters from lat/lon coordinates (Shoelace formula).
 */
static float estimate_ring_area_sqm(const CTCoord *coords, int count)
{
    if (count < 3) return 0.0f;

    double centroid_lat = 0;
    for (int i = 0; i < count; i++) {
        centroid_lat += coords[i].lat;
    }
    centroid_lat /= count;

    double lat_scale = 111320.0;
    double lon_scale = 111320.0 * cos(centroid_lat * 3.14159265358979 / 180.0);

    double area = 0.0;
    for (int i = 0; i < count; i++) {
        int j = (i + 1) % count;
        double x1 = coords[i].lon * lon_scale;
        double y1 = coords[i].lat * lat_scale;
        double x2 = coords[j].lon * lon_scale;
        double y2 = coords[j].lat * lat_scale;
        area += x1 * y2 - x2 * y1;
    }
    return (float)fabs(area / 2.0);
}

/*
 * Helper to add a multipolygon as feature(s).
 *
 * Landuse/natural: each outer ring becomes a separate simple polygon.
 * Inner rings (holes) are omitted — smaller landuse features render on top.
 *
 * Water: all rings kept together with even-odd fill (islands stay unfilled).
 */
static CTStatus add_multipolygon_as_feature(const CTAssembledMultipolygon *mp,
                                            CTFeature **features,
                                            size_t *count, size_t *capacity)
{
    if (mp->num_rings == 0) return CT_OK;

    /* Landuse/natural: emit each outer ring as a separate simple polygon */
    if (mp->feature_class == CT_OSM_LANDUSE || mp->feature_class == CT_OSM_NATURAL) {
        for (int r = 0; r < mp->num_rings; r++) {
            const CTMultipolygonRing *ring = &mp->rings[r];
            if (!ring->is_outer || ring->num_coords == 0) continue;

            /* Expand array if needed */
            if (*count >= *capacity) {
                *capacity *= 2;
                CTFeature *new_features = realloc(*features, *capacity * sizeof(CTFeature));
                if (!new_features) return CT_ERROR_OUT_OF_MEMORY;
                *features = new_features;
            }

            CTFeature *f = &(*features)[*count];
            memset(f, 0, sizeof(CTFeature));

            f->type = CT_GEOM_POLYGON;
            f->layer = layer_from_osm_class(mp->feature_class);
            f->feature_type = mp->feature_type;
            f->area_sqm = estimate_ring_area_sqm(ring->coords, ring->num_coords);
            f->length_m = 0;

            f->points = malloc(ring->num_coords * sizeof(CTTilePoint));
            if (!f->points) continue;  /* Skip this ring but continue */

            f->num_points = ring->num_coords;
            f->num_rings = 1;
            f->ring_ends = NULL;  /* Simple polygon */

            for (int j = 0; j < ring->num_coords; j++) {
                f->points[j].x = (int32_t)(ring->coords[j].lon * 1e7);
                f->points[j].y = (int32_t)(ring->coords[j].lat * 1e7);
            }

            (*count)++;
        }
        return CT_OK;
    }

    /* Water and other classes: keep multi-ring behavior (even-odd fill) */

    /* Expand array if needed */
    if (*count >= *capacity) {
        *capacity *= 2;
        CTFeature *new_features = realloc(*features, *capacity * sizeof(CTFeature));
        if (!new_features) {
            return CT_ERROR_OUT_OF_MEMORY;
        }
        *features = new_features;
    }

    /* Count total points across all rings */
    int total_points = 0;
    for (int r = 0; r < mp->num_rings; r++) {
        total_points += mp->rings[r].num_coords;
    }

    if (total_points == 0) return CT_OK;

    /* Create feature */
    CTFeature *f = &(*features)[*count];
    memset(f, 0, sizeof(CTFeature));

    f->type = CT_GEOM_POLYGON;
    f->layer = layer_from_osm_class(mp->feature_class);
    f->feature_type = mp->feature_type;
    f->area_sqm = mp->area_sqm;
    f->length_m = 0;  /* Multipolygons don't have length */

    /* Allocate points and ring_ends */
    f->points = malloc(total_points * sizeof(CTTilePoint));
    f->ring_ends = malloc(mp->num_rings * sizeof(int));
    if (!f->points || !f->ring_ends) {
        free(f->points);
        free(f->ring_ends);
        f->points = NULL;
        f->ring_ends = NULL;
        return CT_OK;  /* Skip this feature but continue */
    }

    /* Copy all ring coordinates and track ring boundaries */
    int point_idx = 0;
    for (int r = 0; r < mp->num_rings; r++) {
        const CTMultipolygonRing *ring = &mp->rings[r];
        for (int j = 0; j < ring->num_coords; j++) {
            /* Store as fixed-point for now, let caller convert to tile coords */
            f->points[point_idx].x = (int32_t)(ring->coords[j].lon * 1e7);
            f->points[point_idx].y = (int32_t)(ring->coords[j].lat * 1e7);
            point_idx++;
        }
        f->ring_ends[r] = point_idx;  /* End index (exclusive) of this ring */
    }

    f->num_points = total_points;
    f->num_rings = mp->num_rings;

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
    } else {
        /* Fallback: linear scan (O(n)) for ways */
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
    }

    /* Add multipolygon features */
    if (ctx->mp_rtree) {
        /* Use R-Tree for fast multipolygon lookup */
        size_t max_mp = ctx->num_multipolygons;
        uint32_t *mp_candidates = malloc(max_mp * sizeof(uint32_t));
        if (mp_candidates) {
            size_t num_mp = ct_rtree_query(ctx->mp_rtree, bbox, mp_candidates, max_mp);
            for (size_t i = 0; i < num_mp; i++) {
                uint32_t mp_idx = mp_candidates[i];
                if (mp_idx >= ctx->num_multipolygons) continue;

                const CTAssembledMultipolygon *mp = &ctx->multipolygons[mp_idx];
                CTStatus status = add_multipolygon_as_feature(mp, features, count, &capacity);
                if (status != CT_OK) {
                    free(mp_candidates);
                    free(*features);
                    *features = NULL;
                    *count = 0;
                    return status;
                }
            }
            free(mp_candidates);
        }
    } else {
        /* Fallback: linear scan */
        for (size_t i = 0; i < ctx->num_multipolygons; i++) {
            const CTAssembledMultipolygon *mp = &ctx->multipolygons[i];

            /* Quick bbox check */
            if (mp->bbox.max_lat < bbox.min_lat ||
                mp->bbox.min_lat > bbox.max_lat ||
                mp->bbox.max_lon < bbox.min_lon ||
                mp->bbox.min_lon > bbox.max_lon) {
                continue;
            }

            CTStatus status = add_multipolygon_as_feature(mp, features, count, &capacity);
            if (status != CT_OK) {
                free(*features);
                *features = NULL;
                *count = 0;
                return status;
            }
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
    /* Skip ways with no coordinates (prevents crash in MVT encoder) */
    if (way->num_coords == 0) {
        return CT_OK;
    }

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
    f->flags = way->flags;

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
    } else {
        /* Fallback: linear scan (O(n)) for ways */
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
    }

    /* Add multipolygon features with LOD filtering */
    if (ctx->mp_rtree) {
        /* Use R-Tree for fast multipolygon lookup */
        size_t max_mp = ctx->num_multipolygons;
        uint32_t *mp_candidates = malloc(max_mp * sizeof(uint32_t));
        if (mp_candidates) {
            size_t num_mp = ct_rtree_query(ctx->mp_rtree, bbox, mp_candidates, max_mp);
            for (size_t i = 0; i < num_mp; i++) {
                uint32_t mp_idx = mp_candidates[i];
                if (mp_idx >= ctx->num_multipolygons) continue;

                const CTAssembledMultipolygon *mp = &ctx->multipolygons[mp_idx];

                /* LOD filter for multipolygons */
                CTLayer layer = layer_from_osm_class(mp->feature_class);
                if (!ct_lod_is_visible(lod, layer, mp->feature_type,
                                       zoom, mp->area_sqm, 0)) {
                    continue;
                }

                CTStatus status = add_multipolygon_as_feature(mp, features, count, &capacity);
                if (status != CT_OK) {
                    free(mp_candidates);
                    free(*features);
                    *features = NULL;
                    *count = 0;
                    return status;
                }
            }
            free(mp_candidates);
        }
    } else {
        /* Fallback: linear scan for multipolygons */
        for (size_t i = 0; i < ctx->num_multipolygons; i++) {
            const CTAssembledMultipolygon *mp = &ctx->multipolygons[i];

            /* Quick bbox check */
            if (mp->bbox.max_lat < bbox.min_lat ||
                mp->bbox.min_lat > bbox.max_lat ||
                mp->bbox.max_lon < bbox.min_lon ||
                mp->bbox.min_lon > bbox.max_lon) {
                continue;
            }

            /* LOD filter for multipolygons */
            CTLayer layer = layer_from_osm_class(mp->feature_class);
            if (!ct_lod_is_visible(lod, layer, mp->feature_type,
                                   zoom, mp->area_sqm, 0)) {
                continue;
            }

            CTStatus status = add_multipolygon_as_feature(mp, features, count, &capacity);
            if (status != CT_OK) {
                free(*features);
                *features = NULL;
                *count = 0;
                return status;
            }
        }
    }

    return CT_OK;
}

/* ============================================================================
 * Labeled Points API
 * ============================================================================ */

CTStatus ct_pbf_get_tile_labels(const CTPBFContext *ctx, CTTileCoord coord,
                                const CTLabeledPoint ***points, size_t *count)
{
    if (!ctx || !points || !count) {
        if (points) *points = NULL;
        if (count) *count = 0;
        return CT_ERROR_INVALID_ARGUMENT;
    }

    *points = NULL;
    *count = 0;

    if (ctx->num_labeled_points == 0) {
        return CT_OK;
    }

    /* Get tile bounding box with buffer for labels near edges */
    CTBBox bbox = ct_tile_bounds(coord);

    /* Zoom-adaptive buffer: 25% of tile width so adjacent tiles see
     * labels near boundaries.  Replaces fixed 500m buffer that was
     * too large at high zoom and too small at low zoom. */
    double buf_lon = (bbox.max_lon - bbox.min_lon) * 0.25;
    double buf_lat = (bbox.max_lat - bbox.min_lat) * 0.25;
    bbox.min_lat -= buf_lat;
    bbox.max_lat += buf_lat;
    bbox.min_lon -= buf_lon;
    bbox.max_lon += buf_lon;

    /* Allocate result array (worst case: all points) */
    const CTLabeledPoint **result = malloc(ctx->num_labeled_points * sizeof(CTLabeledPoint *));
    if (!result) {
        return CT_ERROR_OUT_OF_MEMORY;
    }

    size_t result_count = 0;

    /* Linear scan - could use spatial index if we have many points */
    for (size_t i = 0; i < ctx->num_labeled_points; i++) {
        const CTLabeledPoint *pt = &ctx->labeled_points[i];

        /* Filter by zoom level */
        if (coord.z < pt->min_zoom) {
            continue;
        }

        /* Filter by bounding box */
        if (pt->coord.lat < bbox.min_lat || pt->coord.lat > bbox.max_lat ||
            pt->coord.lon < bbox.min_lon || pt->coord.lon > bbox.max_lon) {
            continue;
        }

        result[result_count++] = pt;
    }

    /* Shrink array to actual size */
    if (result_count == 0) {
        free(result);
        *points = NULL;
        *count = 0;
    } else if (result_count < ctx->num_labeled_points) {
        const CTLabeledPoint **shrunk = realloc(result, result_count * sizeof(CTLabeledPoint *));
        *points = shrunk ? shrunk : result;
        *count = result_count;
    } else {
        *points = result;
        *count = result_count;
    }

    return CT_OK;
}

size_t ct_pbf_get_label_count(const CTPBFContext *ctx)
{
    return ctx ? ctx->num_labeled_points : 0;
}

/* ============================================================================
 * Named Ways (for road/area labels)
 * ============================================================================ */

CTStatus ct_pbf_get_tile_named_ways(const CTPBFContext *ctx, CTTileCoord coord,
                                     const CTOSMWay ***ways, size_t *count)
{
    if (!ctx || !ways || !count) {
        if (ways) *ways = NULL;
        if (count) *count = 0;
        return CT_ERROR_INVALID_ARGUMENT;
    }

    *ways = NULL;
    *count = 0;

    if (ctx->num_ways == 0) {
        return CT_OK;
    }

    CTBBox bbox = ct_tile_bounds(coord);

    /* Zoom-adaptive buffer: 25% of tile width so adjacent tiles see
     * roads near boundaries (matches label point buffer). */
    double way_buf_lon = (bbox.max_lon - bbox.min_lon) * 0.25;
    double way_buf_lat = (bbox.max_lat - bbox.min_lat) * 0.25;
    bbox.min_lat -= way_buf_lat;
    bbox.max_lat += way_buf_lat;
    bbox.min_lon -= way_buf_lon;
    bbox.max_lon += way_buf_lon;

    /* Allocate result array */
    size_t capacity = 256;
    const CTOSMWay **result = malloc(capacity * sizeof(CTOSMWay *));
    if (!result) {
        return CT_ERROR_OUT_OF_MEMORY;
    }

    size_t result_count = 0;

    if (ctx->rtree) {
        /* R-tree query for candidate ways.
         * Cap candidates to avoid huge allocations (5.6M ways = 22MB).
         * A single tile rarely intersects more than a few thousand ways. */
        #define NAMED_WAY_MAX_CANDIDATES  4096
        #define NAMED_WAY_MAX_RESULTS      200

        uint32_t *candidates = malloc(NAMED_WAY_MAX_CANDIDATES * sizeof(uint32_t));
        if (!candidates) {
            free(result);
            return CT_ERROR_OUT_OF_MEMORY;
        }

        size_t num_candidates = ct_rtree_query(ctx->rtree, bbox, candidates,
                                                NAMED_WAY_MAX_CANDIDATES);

        for (size_t i = 0; i < num_candidates && result_count < NAMED_WAY_MAX_RESULTS; i++) {
            uint32_t way_idx = candidates[i];
            if (way_idx >= ctx->num_ways) continue;

            const CTOSMWay *way = &ctx->ways[way_idx];

            /* Only named highway ways */
            if (!way->name || way->name[0] == '\0') continue;
            if (way->feature_class != CT_OSM_HIGHWAY) continue;

            /* Grow if needed */
            if (result_count >= capacity) {
                capacity *= 2;
                const CTOSMWay **grown = realloc(result, capacity * sizeof(CTOSMWay *));
                if (!grown) { free(candidates); free(result); return CT_ERROR_OUT_OF_MEMORY; }
                result = grown;
            }

            result[result_count++] = way;
        }

        free(candidates);
    } else {
        /* Fallback: linear scan */
        for (size_t i = 0; i < ctx->num_ways; i++) {
            const CTOSMWay *way = &ctx->ways[i];

            if (!way->name || way->name[0] == '\0') continue;
            if (way->feature_class != CT_OSM_HIGHWAY) continue;

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

            if (result_count >= capacity) {
                capacity *= 2;
                const CTOSMWay **grown = realloc(result, capacity * sizeof(CTOSMWay *));
                if (!grown) { free(result); return CT_ERROR_OUT_OF_MEMORY; }
                result = grown;
            }

            result[result_count++] = way;
        }
    }

    if (result_count == 0) {
        free(result);
    } else {
        *ways = result;
    }
    *count = result_count;

    return CT_OK;
}
