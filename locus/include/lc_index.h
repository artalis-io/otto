/*
 * lc_index.h - Unified Geocoding Index
 *
 * Combines all index structures for complete geocoding functionality.
 */

#ifndef LC_INDEX_H
#define LC_INDEX_H

#include <stdint.h>
#include <stddef.h>
#include "lc_types.h"
#include "lc_trie.h"
#include "lc_ngram.h"
#include "lc_spatial.h"
#include "lc_pbf.h"
#include "sh_geo.h"

/* Forward declaration for mmap index */
typedef struct LCMmapIndex LCMmapIndex;

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Index Structure
 * ============================================================================ */

typedef struct {
    LCEntityStore *entities;    /* Entity data (NULL for v4 mmap) */
    LCTrie *trie;               /* Text prefix index */
    LCNgramIndex *ngrams;       /* Fuzzy text index (NULL for v4 mmap) */
    LCSpatialGrid *grid;        /* Spatial index */

    SHBBox bounds;              /* Geographic bounds */
    uint32_t num_entities;      /* Total entities */
    size_t memory_used;         /* Total memory usage */

    void *mmap_ctx;             /* mmap context for v3 (deprecated) */
    LCMmapIndex *mmap_idx;      /* Zero-copy mmap index for v4 */
} LCIndex;

/* ============================================================================
 * Search Options
 * ============================================================================ */

typedef struct {
    int limit;                  /* Max results (default: 10) */
    SHBBox *bounds;             /* Optional geographic filter */
    LCFeatureClass *filter;     /* Optional class filter (NULL-terminated) */
    const char *lang;           /* Preferred language (e.g., "en") */
    int fuzzy;                  /* Enable fuzzy matching (default: 1) */
    float fuzzy_threshold;      /* Min similarity for fuzzy (default: 0.3) */
} LCSearchOptions;

/* ============================================================================
 * Search Result
 * ============================================================================ */

typedef struct {
    uint32_t entity_id;
    double score;
} LCSearchMatch;

typedef struct {
    LCSearchMatch *matches;
    size_t num_results;
    size_t total_matches;       /* Before limit */
    double query_time_ms;
} LCSearchResult;

/* ============================================================================
 * Reverse Options
 * ============================================================================ */

typedef struct {
    int include_poi;            /* Include nearest POI (default: 0) */
    double radius_m;            /* Search radius (default: 100.0) */
    int max_results;            /* Max results (default: 5) */
} LCReverseOptions;

/* ============================================================================
 * Reverse Result
 * ============================================================================ */

typedef struct {
    LCEntity *place;            /* Nearest named place */
    LCEntity *street;           /* Nearest street */
    LCEntity *address;          /* Nearest address */
    LCEntity *poi;              /* Nearest POI (if requested) */
    LCEntity **hierarchy;       /* Admin hierarchy [country → city] */
    int hierarchy_depth;
    double distance_m;          /* Distance to nearest feature */
} LCReverseResult;

/* ============================================================================
 * Index API
 * ============================================================================ */

/*
 * Create an empty index.
 */
LCIndex *lc_index_create(void);

/*
 * Free index and all data.
 */
void lc_index_free(LCIndex *index);

/*
 * Build index from entity store.
 * Takes ownership of the entity store.
 */
LCStatus lc_index_build(LCIndex *index, LCEntityStore *store);

/*
 * Build index from PBF file.
 */
LCStatus lc_index_build_from_pbf(LCIndex *index, const char *path, const LCPBFOptions *opts);

/*
 * Get index statistics.
 */
uint32_t lc_index_entity_count(const LCIndex *index);
size_t lc_index_memory_usage(const LCIndex *index);
SHBBox lc_index_bounds(const LCIndex *index);

/* ============================================================================
 * Search API
 * ============================================================================ */

/*
 * Set default search options.
 */
void lc_search_options_default(LCSearchOptions *opts);

/*
 * Forward geocoding: search for entities matching a query.
 */
LCStatus lc_search(const LCIndex *index, const char *query,
                   const LCSearchOptions *opts, LCSearchResult *result);

/*
 * Autocomplete: find entities starting with prefix.
 */
LCStatus lc_autocomplete(const LCIndex *index, const char *prefix,
                         int limit, LCSearchResult *result);

/*
 * Free search result.
 */
void lc_search_result_free(LCSearchResult *result);

/*
 * Get entity from search match.
 */
const LCEntity *lc_search_get_entity(const LCIndex *index, const LCSearchMatch *match);

/* ============================================================================
 * Reverse Geocoding API
 * ============================================================================ */

/*
 * Set default reverse options.
 */
void lc_reverse_options_default(LCReverseOptions *opts);

/*
 * Reverse geocoding: find entities near a coordinate.
 */
LCStatus lc_reverse(const LCIndex *index, SHCoord coord,
                    const LCReverseOptions *opts, LCReverseResult *result);

/*
 * Free reverse result.
 */
void lc_reverse_result_free(LCReverseResult *result);

/*
 * Format address as string.
 */
size_t lc_format_address(const LCReverseResult *result, char *buffer, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* LC_INDEX_H */
