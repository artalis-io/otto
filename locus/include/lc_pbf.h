/*
 * lc_pbf.h - OSM PBF parsing for geocoding
 *
 * Extracts geocodable entities (places, streets, addresses, POIs)
 * from OpenStreetMap PBF files.
 */

#ifndef LC_PBF_H
#define LC_PBF_H

#include "lc_types.h"
#include <stdio.h>

/* ============================================================================
 * PBF Context
 * ============================================================================ */

typedef struct LCPBFContext LCPBFContext;

/* ============================================================================
 * Statistics
 * ============================================================================ */

typedef struct {
    uint64_t nodes_processed;
    uint64_t ways_processed;
    uint64_t relations_processed;

    uint32_t places_found;
    uint32_t streets_found;
    uint32_t addresses_found;
    uint32_t pois_found;
    uint32_t boundaries_found;

    double parse_time_ms;
} LCPBFStats;

/* ============================================================================
 * Parsing Options
 * ============================================================================ */

typedef struct {
    int include_pois;       /* Include amenity/shop/tourism POIs (default: 1) */
    int include_addresses;  /* Include addr:* nodes (default: 1) */
    int include_streets;    /* Include named highways (default: 1) */
    int include_boundaries; /* Include admin boundaries (default: 1) */
    int min_admin_level;    /* Minimum admin level to include (default: 2) */
    int max_admin_level;    /* Maximum admin level to include (default: 10) */
} LCPBFOptions;

/* ============================================================================
 * API Functions
 * ============================================================================ */

/* Initialize options with defaults */
void lc_pbf_default_options(LCPBFOptions *opts);

/* Create PBF context */
LCPBFContext *lc_pbf_context_create(void);

/* Free PBF context */
void lc_pbf_context_free(LCPBFContext *ctx);

/* Parse PBF file and extract entities */
LCStatus lc_pbf_parse_file(LCPBFContext *ctx, const char *filename,
                           const LCPBFOptions *opts);

/* Parse PBF from memory */
LCStatus lc_pbf_parse_memory(LCPBFContext *ctx, const uint8_t *data,
                             size_t size, const LCPBFOptions *opts);

/* Get parsing statistics */
void lc_pbf_get_stats(const LCPBFContext *ctx, LCPBFStats *stats);

/* Get entity store (transfers ownership to caller) */
LCEntityStore *lc_pbf_take_entities(LCPBFContext *ctx);

/* Get bounding box of all entities */
SHBBox lc_pbf_get_bounds(const LCPBFContext *ctx);

/* ============================================================================
 * Tag Classification Helpers
 * ============================================================================ */

/* Determine feature class from OSM tags */
LCFeatureClass lc_classify_place_tag(const char *value);
LCFeatureClass lc_classify_highway_tag(const char *value);
LCFeatureClass lc_classify_boundary_tag(int admin_level);
LCFeatureClass lc_classify_poi_tags(const char *amenity, const char *shop,
                                    const char *tourism, const char *leisure);

/* Check if highway type should have a name indexed */
int lc_highway_is_named(const char *highway);

#endif /* LC_PBF_H */
