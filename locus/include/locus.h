/*
 * locus.h - Locus Geocoding Library
 *
 * Location Oriented Coordinate Unification System
 * Zero-dependency geocoding library for OSM data
 *
 * Features:
 * - Forward geocoding (text → coordinates)
 * - Reverse geocoding (coordinates → address)
 * - Autocomplete suggestions
 * - Fuzzy text matching
 *
 * Part of the OTTO platform GIS trifecta:
 * - Carta: Map tile generation
 * - Velo: Routing
 * - Locus: Geocoding
 */

#ifndef LOCUS_H
#define LOCUS_H

#include "lc_types.h"
#include "lc_pbf.h"

/* ============================================================================
 * Version
 * ============================================================================ */

#define LC_VERSION_MAJOR 0
#define LC_VERSION_MINOR 1
#define LC_VERSION_PATCH 0

const char *lc_version(void);

/* ============================================================================
 * Forward Declarations (for future phases)
 * ============================================================================ */

/* Search index (Phase 8) */
typedef struct LCIndex LCIndex;

/* Search result (Phase 6) */
typedef struct {
    LCEntity **results;
    double *scores;
    int num_results;
    int total_matches;      /* Total before limit applied */
} LCSearchResult;

/* Search options (Phase 6) */
typedef struct {
    int limit;              /* Max results (default: 10) */
    SHBBox *bounds;         /* Optional geographic filter */
    LCFeatureClass *filter; /* Optional class filter (NULL-terminated array) */
    const char *lang;       /* Preferred language (e.g., "en") */
    int fuzzy;              /* Enable fuzzy matching (default: 1) */
} LCSearchOptions;

/* Reverse result (Phase 7) */
typedef struct {
    LCAddress address;      /* Structured address */
    LCEntity *place;        /* Nearest named place */
    LCEntity *street;       /* Nearest street */
    LCEntity *poi;          /* Nearest POI (if requested) */
    LCEntity **hierarchy;   /* Admin hierarchy [country → city] */
    int hierarchy_depth;
    double distance_m;      /* Distance to nearest feature */
} LCReverseResult;

/* Reverse options (Phase 7) */
typedef struct {
    int include_poi;        /* Include nearest POI (default: 0) */
    double radius_m;        /* Search radius (default: 100.0) */
    const char *lang;       /* Preferred language */
} LCReverseOptions;

/* ============================================================================
 * Initialization (Phase 1 - Current)
 * ============================================================================ */

/* Load and build index from OSM PBF file */
LCEntityStore *lc_load_pbf(const char *filename, const LCPBFOptions *opts);

/* Get entity count */
uint32_t lc_entity_count(const LCEntityStore *store);

/* Get entity by index */
const LCEntity *lc_get_entity(const LCEntityStore *store, uint32_t index);

/* ============================================================================
 * Future API (stubs for now)
 * ============================================================================ */

/* Index building (Phase 8) */
/* LCIndex *lc_index_create(LCEntityStore *store); */
/* void lc_index_free(LCIndex *index); */
/* LCStatus lc_index_save(const LCIndex *index, const char *path); */
/* LCIndex *lc_index_load(const char *path); */

/* Forward geocoding (Phase 6) */
/* void lc_default_search_options(LCSearchOptions *opts); */
/* LCStatus lc_search(const LCIndex *index, const char *query, */
/*                    const LCSearchOptions *opts, LCSearchResult *result); */
/* LCStatus lc_autocomplete(const LCIndex *index, const char *prefix, */
/*                          int limit, LCSearchResult *result); */
/* void lc_free_search_result(LCSearchResult *result); */

/* Reverse geocoding (Phase 7) */
/* void lc_default_reverse_options(LCReverseOptions *opts); */
/* LCStatus lc_reverse(const LCIndex *index, SHCoord coord, */
/*                     const LCReverseOptions *opts, LCReverseResult *result); */
/* void lc_free_reverse_result(LCReverseResult *result); */

#endif /* LOCUS_H */
