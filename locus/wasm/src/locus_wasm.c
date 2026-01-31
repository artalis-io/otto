/**
 * Locus WASM Entry Points
 *
 * Provides browser-friendly API for the Locus geocoding library.
 */

#include <stdlib.h>
#include <string.h>
#include "locus.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define WASM_EXPORT
#endif

/* ============================================================================
 * Memory Management
 * ============================================================================ */

WASM_EXPORT
void *wasm_malloc(int size) {
    return malloc(size);
}

WASM_EXPORT
void wasm_free(void *ptr) {
    free(ptr);
}

/* ============================================================================
 * Version
 * ============================================================================ */

WASM_EXPORT
int wasm_version(void) {
    return LC_VERSION_MAJOR * 10000 + LC_VERSION_MINOR * 100 + LC_VERSION_PATCH;
}

/* ============================================================================
 * WASM Result Structure
 *
 * Wraps search/reverse results with index reference for entity lookup.
 * ============================================================================ */

typedef struct {
    const LCIndex *index;
    LCSearchResult search_result;
    uint32_t *entity_ids;   /* Array of entity IDs for reverse results */
    size_t count;
    int is_reverse;
} WasmResult;

/* ============================================================================
 * Index Management
 * ============================================================================ */

WASM_EXPORT
LCIndex *wasm_index_create(void) {
    return lc_index_create();
}

WASM_EXPORT
void wasm_index_free(LCIndex *index) {
    if (index) {
        lc_index_free(index);
    }
}

/**
 * Load index from PBF data in memory.
 * Returns 0 on success, error code otherwise.
 */
WASM_EXPORT
int wasm_index_load_pbf_memory(LCIndex *index, const uint8_t *data, size_t size) {
    if (!index || !data || size == 0) return -1;

    /* Create temporary file in memory using Emscripten's virtual filesystem */
#ifdef __EMSCRIPTEN__
    /* Write to virtual file */
    FILE *f = fopen("/tmp/temp.osm.pbf", "wb");
    if (!f) return -2;
    fwrite(data, 1, size, f);
    fclose(f);

    /* Load from virtual file */
    LCStatus status = lc_index_build_from_pbf(index, "/tmp/temp.osm.pbf", NULL);

    /* Remove virtual file */
    remove("/tmp/temp.osm.pbf");

    return (int)status;
#else
    /* Native: would need temp file or memory-mapped loading */
    (void)data;
    (void)size;
    return -3;
#endif
}

WASM_EXPORT
uint32_t wasm_index_entity_count(const LCIndex *index) {
    return index ? lc_index_entity_count(index) : 0;
}

WASM_EXPORT
size_t wasm_index_memory_usage(const LCIndex *index) {
    return index ? lc_index_memory_usage(index) : 0;
}

/* ============================================================================
 * Search
 * ============================================================================ */

/**
 * Forward geocoding search.
 *
 * @param index   Index pointer
 * @param query   Search query string (UTF-8)
 * @param limit   Max results
 * @param fuzzy   Enable fuzzy matching (1) or exact only (0)
 * @return WasmResult pointer or NULL on error
 */
WASM_EXPORT
WasmResult *wasm_search(LCIndex *index, const char *query, int limit, int fuzzy) {
    if (!index || !query) return NULL;

    WasmResult *result = calloc(1, sizeof(WasmResult));
    if (!result) return NULL;

    result->index = index;
    result->is_reverse = 0;

    LCSearchOptions opts;
    lc_search_options_default(&opts);
    opts.limit = limit > 0 ? limit : 10;
    opts.fuzzy = fuzzy;

    LCStatus status = lc_search(index, query, &opts, &result->search_result);
    if (status != LC_OK) {
        free(result);
        return NULL;
    }

    result->count = result->search_result.num_results;
    return result;
}

/**
 * Autocomplete search.
 *
 * @param index   Index pointer
 * @param prefix  Prefix string (UTF-8)
 * @param limit   Max results
 * @return WasmResult pointer or NULL on error
 */
WASM_EXPORT
WasmResult *wasm_autocomplete(LCIndex *index, const char *prefix, int limit) {
    if (!index || !prefix) return NULL;

    WasmResult *result = calloc(1, sizeof(WasmResult));
    if (!result) return NULL;

    result->index = index;
    result->is_reverse = 0;

    LCStatus status = lc_autocomplete(index, prefix, limit > 0 ? limit : 10, &result->search_result);
    if (status != LC_OK) {
        free(result);
        return NULL;
    }

    result->count = result->search_result.num_results;
    return result;
}

/**
 * Reverse geocoding.
 *
 * @param index   Index pointer
 * @param lat     Latitude
 * @param lon     Longitude
 * @param radius  Search radius in meters
 * @return WasmResult pointer or NULL on error
 */
WASM_EXPORT
WasmResult *wasm_reverse(LCIndex *index, double lat, double lon, double radius) {
    if (!index) return NULL;

    WasmResult *result = calloc(1, sizeof(WasmResult));
    if (!result) return NULL;

    result->index = index;
    result->is_reverse = 1;

    SHCoord coord = {.lat = lat, .lon = lon};

    LCReverseOptions opts;
    lc_reverse_options_default(&opts);
    opts.radius_m = radius > 0 ? radius : 100.0;

    LCReverseResult rev_result;
    LCStatus status = lc_reverse(index, coord, &opts, &rev_result);
    if (status != LC_OK) {
        free(result);
        return NULL;
    }

    /* Collect all non-NULL entities into entity_ids array */
    result->entity_ids = calloc(5, sizeof(uint32_t));
    if (!result->entity_ids) {
        lc_reverse_result_free(&rev_result);
        free(result);
        return NULL;
    }

    size_t idx = 0;
    if (rev_result.place) result->entity_ids[idx++] = rev_result.place->osm_id;
    if (rev_result.street) result->entity_ids[idx++] = rev_result.street->osm_id;
    if (rev_result.address) result->entity_ids[idx++] = rev_result.address->osm_id;
    if (rev_result.poi) result->entity_ids[idx++] = rev_result.poi->osm_id;

    result->count = idx;

    lc_reverse_result_free(&rev_result);
    return result;
}

/* ============================================================================
 * Result Access
 * ============================================================================ */

WASM_EXPORT
size_t wasm_result_count(const WasmResult *result) {
    return result ? result->count : 0;
}

WASM_EXPORT
uint32_t wasm_result_get_id(const WasmResult *result, size_t idx) {
    if (!result || idx >= result->count) return 0;

    if (result->is_reverse) {
        return result->entity_ids[idx];
    } else {
        return result->search_result.matches[idx].entity_id;
    }
}

WASM_EXPORT
double wasm_result_get_score(const WasmResult *result, size_t idx) {
    if (!result || result->is_reverse || idx >= result->count) return 0.0;
    return result->search_result.matches[idx].score;
}

/**
 * Get entity name from result.
 * Returns pointer to string in WASM memory (caller should not free).
 */
WASM_EXPORT
const char *wasm_result_get_name(const WasmResult *result, size_t idx) {
    if (!result || !result->index || idx >= result->count) return NULL;

    uint32_t entity_id;
    if (result->is_reverse) {
        entity_id = result->entity_ids[idx];
    } else {
        entity_id = result->search_result.matches[idx].entity_id;
    }

    const LCEntity *entity = lc_entity_store_get(result->index->entities, entity_id);
    return entity ? entity->name : NULL;
}

WASM_EXPORT
double wasm_result_get_lat(const WasmResult *result, size_t idx) {
    if (!result || !result->index || idx >= result->count) return 0.0;

    uint32_t entity_id;
    if (result->is_reverse) {
        entity_id = result->entity_ids[idx];
    } else {
        entity_id = result->search_result.matches[idx].entity_id;
    }

    const LCEntity *entity = lc_entity_store_get(result->index->entities, entity_id);
    return entity ? entity->coord.lat : 0.0;
}

WASM_EXPORT
double wasm_result_get_lon(const WasmResult *result, size_t idx) {
    if (!result || !result->index || idx >= result->count) return 0.0;

    uint32_t entity_id;
    if (result->is_reverse) {
        entity_id = result->entity_ids[idx];
    } else {
        entity_id = result->search_result.matches[idx].entity_id;
    }

    const LCEntity *entity = lc_entity_store_get(result->index->entities, entity_id);
    return entity ? entity->coord.lon : 0.0;
}

WASM_EXPORT
void wasm_result_free(WasmResult *result) {
    if (result) {
        if (!result->is_reverse) {
            lc_search_result_free(&result->search_result);
        }
        if (result->entity_ids) {
            free(result->entity_ids);
        }
        free(result);
    }
}

/* ============================================================================
 * Direct Entity Access
 * ============================================================================ */

WASM_EXPORT
const char *wasm_entity_get_name(const LCIndex *index, uint32_t entity_id) {
    if (!index || !index->entities) return NULL;
    const LCEntity *entity = lc_entity_store_get(index->entities, entity_id);
    return entity ? entity->name : NULL;
}

WASM_EXPORT
double wasm_entity_get_lat(const LCIndex *index, uint32_t entity_id) {
    if (!index || !index->entities) return 0.0;
    const LCEntity *entity = lc_entity_store_get(index->entities, entity_id);
    return entity ? entity->coord.lat : 0.0;
}

WASM_EXPORT
double wasm_entity_get_lon(const LCIndex *index, uint32_t entity_id) {
    if (!index || !index->entities) return 0.0;
    const LCEntity *entity = lc_entity_store_get(index->entities, entity_id);
    return entity ? entity->coord.lon : 0.0;
}
