/*
 * lc_index.c - Unified Geocoding Index
 *
 * Combines entity store, trie, n-gram, and spatial indexes for geocoding.
 */

#include "lc_index.h"
#include "lc_pbf.h"
#include "lc_normalize.h"
#include "lc_serialize.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <sys/mman.h>
#include <unistd.h>

/* Minimal mmap context for cleanup (first 3 fields same as LCMmapContextV3) */
typedef struct {
    void *map_base;
    size_t map_size;
    int fd;
} LCMmapContextBase;

/* ============================================================================
 * Index Management
 * ============================================================================ */

LCIndex *lc_index_create(void)
{
    LCIndex *index = calloc(1, sizeof(LCIndex));
    if (!index) return NULL;

    index->trie = lc_trie_create();
    index->ngrams = lc_ngram_create();

    if (!index->trie || !index->ngrams) {
        lc_index_free(index);
        return NULL;
    }

    sh_bbox_init(&index->bounds);
    index->memory_used = sizeof(LCIndex);

    return index;
}

void lc_index_free(LCIndex *index)
{
    if (!index) return;

    /* Handle mmap'd index specially */
    if (index->mmap_ctx) {
        LCMmapContextBase *ctx = (LCMmapContextBase *)index->mmap_ctx;

        /* Free alt_names arrays (these are allocated, not mmap'd) */
        if (index->entities) {
            for (uint32_t i = 0; i < index->entities->count; i++) {
                free(index->entities->entities[i].alt_names);
            }
            free(index->entities->entities);
            free(index->entities);
        }

        /* Unmap and close */
        if (ctx->map_base && ctx->map_base != MAP_FAILED) {
            munmap(ctx->map_base, ctx->map_size);
        }
        if (ctx->fd >= 0) {
            close(ctx->fd);
        }
        free(ctx);
    } else {
        /* Regular free */
        lc_entity_store_free(index->entities);
    }

    lc_trie_free(index->trie);
    lc_ngram_free(index->ngrams);
    lc_grid_free(index->grid);
    free(index);
}

LCStatus lc_index_build(LCIndex *index, LCEntityStore *store)
{
    if (!index || !store) return LC_ERROR_INVALID_PARAM;

    index->entities = store;
    index->num_entities = store->count;

    /* Compute bounds from entities */
    sh_bbox_init(&index->bounds);
    for (uint32_t i = 0; i < store->count; i++) {
        const LCEntity *e = &store->entities[i];
        if (e->centroid.lat != 0 || e->centroid.lon != 0) {
            sh_bbox_expand(&index->bounds, e->centroid);
        }
    }

    /* Ensure bounds have some size (add small buffer if degenerate) */
    if (index->bounds.max_lat <= index->bounds.min_lat) {
        index->bounds.min_lat -= 0.1;
        index->bounds.max_lat += 0.1;
    }
    if (index->bounds.max_lon <= index->bounds.min_lon) {
        index->bounds.min_lon -= 0.1;
        index->bounds.max_lon += 0.1;
    }

    /* Create spatial grid */
    if (index->bounds.max_lat > index->bounds.min_lat &&
        index->bounds.max_lon > index->bounds.min_lon) {
        index->grid = lc_grid_create(index->bounds, LC_GRID_DEFAULT_CELL_SIZE);
    }

    /* Index each entity */
    for (uint32_t i = 0; i < store->count; i++) {
        const LCEntity *e = &store->entities[i];

        /* Index name in trie and n-gram */
        if (e->name && e->name[0]) {
            char *normalized = lc_normalize(e->name);
            if (normalized) {
                lc_trie_insert(index->trie, normalized, i);
                lc_ngram_index_name(index->ngrams, normalized, i);
                free(normalized);
            }

            /* Index alternative names */
            for (uint16_t j = 0; j < e->num_alt_names; j++) {
                if (e->alt_names[j]) {
                    normalized = lc_normalize(e->alt_names[j]);
                    if (normalized) {
                        lc_trie_insert(index->trie, normalized, i);
                        lc_ngram_index_name(index->ngrams, normalized, i);
                        free(normalized);
                    }
                }
            }
        }

        /* Index in spatial grid */
        if (index->grid && (e->centroid.lat != 0 || e->centroid.lon != 0)) {
            lc_grid_insert(index->grid, e->centroid, i);
        }
    }

    /* Finalize n-gram index */
    lc_ngram_build(index->ngrams);

    /* Update memory usage */
    index->memory_used = sizeof(LCIndex);
    if (index->entities) {
        /* Rough estimate */
        index->memory_used += store->count * sizeof(LCEntity) + store->string_pool_size;
    }
    index->memory_used += lc_trie_memory_usage(index->trie);
    index->memory_used += lc_ngram_memory_usage(index->ngrams);
    if (index->grid) {
        index->memory_used += lc_grid_memory_usage(index->grid);
    }

    return LC_OK;
}

LCStatus lc_index_build_from_pbf(LCIndex *index, const char *path, const LCPBFOptions *opts)
{
    if (!index || !path) return LC_ERROR_INVALID_PARAM;

    LCPBFContext *ctx = lc_pbf_context_create();
    if (!ctx) return LC_ERROR_OUT_OF_MEMORY;

    LCStatus status = lc_pbf_parse_file(ctx, path, opts);
    if (status != LC_OK) {
        lc_pbf_context_free(ctx);
        return status;
    }

    LCEntityStore *store = lc_pbf_take_entities(ctx);
    lc_pbf_context_free(ctx);

    if (!store) return LC_ERROR_OUT_OF_MEMORY;

    return lc_index_build(index, store);
}

uint32_t lc_index_entity_count(const LCIndex *index)
{
    return index ? index->num_entities : 0;
}

size_t lc_index_memory_usage(const LCIndex *index)
{
    return index ? index->memory_used : 0;
}

SHBBox lc_index_bounds(const LCIndex *index)
{
    SHBBox empty;
    sh_bbox_init(&empty);
    return index ? index->bounds : empty;
}

/* ============================================================================
 * Search Options
 * ============================================================================ */

void lc_search_options_default(LCSearchOptions *opts)
{
    if (!opts) return;
    opts->limit = 10;
    opts->bounds = NULL;
    opts->filter = NULL;
    opts->lang = NULL;
    opts->fuzzy = 1;
    opts->fuzzy_threshold = 0.3f;
}

/* ============================================================================
 * Result Scoring
 * ============================================================================ */

/* Score factors */
#define SCORE_EXACT_MATCH   1.0
#define SCORE_PREFIX_MATCH  0.8
#define SCORE_FUZZY_BASE    0.5

static double score_importance(LCFeatureClass fclass)
{
    switch (fclass) {
        case LC_CLASS_COUNTRY: return 0.30;
        case LC_CLASS_STATE: return 0.25;
        case LC_CLASS_COUNTY: return 0.20;
        case LC_CLASS_CITY: return 0.15;
        case LC_CLASS_TOWN: return 0.12;
        case LC_CLASS_VILLAGE: return 0.10;
        case LC_CLASS_SUBURB: return 0.08;
        case LC_CLASS_NEIGHBOURHOOD: return 0.05;
        case LC_CLASS_STREET: return 0.10;
        case LC_CLASS_ADDRESS: return 0.05;
        case LC_CLASS_POI: return 0.08;
        default: return 0.02;
    }
}

static double score_population(int population)
{
    if (population <= 0) return 0;
    /* log10(population) * 0.05, capped */
    double pop_score = 0.05 * log10((double)population);
    return pop_score > 0.25 ? 0.25 : pop_score;
}

static double compute_score(const LCEntity *entity, double match_score)
{
    double score = match_score;
    score += score_importance(entity->fclass);
    score += score_population(entity->population);

    /* Slight penalty for long names */
    if (entity->name) {
        size_t len = strlen(entity->name);
        score -= 0.002 * (double)len;
    }

    return score;
}

/* ============================================================================
 * Forward Search
 * ============================================================================ */

static int match_filter(const LCEntity *entity, LCFeatureClass *filter)
{
    if (!filter) return 1;  /* No filter */

    for (int i = 0; filter[i] != LC_CLASS_UNKNOWN; i++) {
        if (entity->fclass == filter[i]) return 1;
    }
    return 0;
}

static int in_bounds(const LCEntity *entity, SHBBox *bounds)
{
    if (!bounds) return 1;  /* No bounds filter */

    return (entity->centroid.lat >= bounds->min_lat &&
            entity->centroid.lat <= bounds->max_lat &&
            entity->centroid.lon >= bounds->min_lon &&
            entity->centroid.lon <= bounds->max_lon);
}

/* Comparator for sorting results by score descending */
static int match_compare(const void *a, const void *b)
{
    const LCSearchMatch *ma = (const LCSearchMatch *)a;
    const LCSearchMatch *mb = (const LCSearchMatch *)b;
    if (ma->score > mb->score) return -1;
    if (ma->score < mb->score) return 1;
    return 0;
}

LCStatus lc_search(const LCIndex *index, const char *query,
                   const LCSearchOptions *opts, LCSearchResult *result)
{
    if (!index || !query || !result) return LC_ERROR_INVALID_PARAM;

    clock_t start = clock();

    memset(result, 0, sizeof(LCSearchResult));

    LCSearchOptions default_opts;
    if (!opts) {
        lc_search_options_default(&default_opts);
        opts = &default_opts;
    }

    /* Normalize query */
    char *normalized = lc_normalize(query);
    if (!normalized || !normalized[0]) {
        free(normalized);
        return LC_OK;
    }

    /* Allocate result buffer */
    size_t capacity = opts->limit * 4;  /* Extra space for filtering */
    LCSearchMatch *matches = calloc(capacity, sizeof(LCSearchMatch));
    if (!matches) {
        free(normalized);
        return LC_ERROR_OUT_OF_MEMORY;
    }

    size_t count = 0;

    /* Step 1: Exact match search */
    uint32_t exact_results[64];
    size_t exact_count;
    if (index->mmap_ctx) {
        exact_count = lc_mmap_trie_search_exact(index->mmap_ctx, normalized, 64, exact_results);
    } else {
        exact_count = lc_trie_search_exact(index->trie, normalized, 64, exact_results);
    }

    for (size_t i = 0; i < exact_count && count < capacity; i++) {
        uint32_t eid = exact_results[i];
        if (eid >= index->num_entities) continue;

        const LCEntity *e = &index->entities->entities[eid];
        if (!match_filter(e, opts->filter)) continue;
        if (!in_bounds(e, opts->bounds)) continue;

        matches[count].entity_id = eid;
        matches[count].score = compute_score(e, SCORE_EXACT_MATCH);
        count++;
    }

    /* Step 2: Prefix search (if exact didn't find enough) */
    if (count < (size_t)opts->limit) {
        uint32_t prefix_results[128];
        size_t prefix_count;
        if (index->mmap_ctx) {
            prefix_count = lc_mmap_trie_search_prefix(index->mmap_ctx, normalized, 128, prefix_results);
        } else {
            prefix_count = lc_trie_search_prefix(index->trie, normalized, 128, prefix_results);
        }

        for (size_t i = 0; i < prefix_count && count < capacity; i++) {
            uint32_t eid = prefix_results[i];
            if (eid >= index->num_entities) continue;

            /* Check for duplicates */
            int found = 0;
            for (size_t j = 0; j < count; j++) {
                if (matches[j].entity_id == eid) {
                    found = 1;
                    break;
                }
            }
            if (found) continue;

            const LCEntity *e = &index->entities->entities[eid];
            if (!match_filter(e, opts->filter)) continue;
            if (!in_bounds(e, opts->bounds)) continue;

            matches[count].entity_id = eid;
            matches[count].score = compute_score(e, SCORE_PREFIX_MATCH);
            count++;
        }
    }

    /* Step 3: Fuzzy search (if enabled and still need more) */
    if (opts->fuzzy && count < (size_t)opts->limit) {
        LCFuzzyMatch fuzzy_results[128];
        size_t fuzzy_count = lc_ngram_search(index->ngrams, normalized,
                                             opts->fuzzy_threshold, 128, fuzzy_results);

        for (size_t i = 0; i < fuzzy_count && count < capacity; i++) {
            uint32_t eid = fuzzy_results[i].entity_id;
            if (eid >= index->num_entities) continue;

            /* Check for duplicates */
            int found = 0;
            for (size_t j = 0; j < count; j++) {
                if (matches[j].entity_id == eid) {
                    found = 1;
                    break;
                }
            }
            if (found) continue;

            const LCEntity *e = &index->entities->entities[eid];
            if (!match_filter(e, opts->filter)) continue;
            if (!in_bounds(e, opts->bounds)) continue;

            double fuzzy_score = SCORE_FUZZY_BASE + 0.3 * fuzzy_results[i].score;
            matches[count].entity_id = eid;
            matches[count].score = compute_score(e, fuzzy_score);
            count++;
        }
    }

    /* Sort by score */
    if (count > 1) {
        qsort(matches, count, sizeof(LCSearchMatch), match_compare);
    }

    /* Trim to limit */
    result->total_matches = count;
    result->num_results = count > (size_t)opts->limit ? (size_t)opts->limit : count;
    result->matches = matches;

    clock_t end = clock();
    result->query_time_ms = (double)(end - start) * 1000.0 / CLOCKS_PER_SEC;

    free(normalized);
    return LC_OK;
}

LCStatus lc_autocomplete(const LCIndex *index, const char *prefix,
                         int limit, LCSearchResult *result)
{
    LCSearchOptions opts;
    lc_search_options_default(&opts);
    opts.limit = limit;
    opts.fuzzy = 0;  /* Disable fuzzy for autocomplete */

    return lc_search(index, prefix, &opts, result);
}

void lc_search_result_free(LCSearchResult *result)
{
    if (!result) return;
    free(result->matches);
    memset(result, 0, sizeof(LCSearchResult));
}

const LCEntity *lc_search_get_entity(const LCIndex *index, const LCSearchMatch *match)
{
    if (!index || !match || !index->entities) return NULL;
    if (match->entity_id >= index->num_entities) return NULL;
    return &index->entities->entities[match->entity_id];
}

/* ============================================================================
 * Reverse Geocoding
 * ============================================================================ */

void lc_reverse_options_default(LCReverseOptions *opts)
{
    if (!opts) return;
    opts->include_poi = 0;
    opts->radius_m = 100.0;
    opts->max_results = 5;
}

LCStatus lc_reverse(const LCIndex *index, SHCoord coord,
                    const LCReverseOptions *opts, LCReverseResult *result)
{
    if (!index || !result) return LC_ERROR_INVALID_PARAM;

    memset(result, 0, sizeof(LCReverseResult));

    LCReverseOptions default_opts;
    if (!opts) {
        lc_reverse_options_default(&default_opts);
        opts = &default_opts;
    }

    if (!index->entities) {
        return LC_OK;  /* No entity store */
    }

    /* Check for spatial index (regular or mmap'd) */
    if (!index->grid && !index->mmap_ctx) {
        return LC_OK;  /* No spatial index */
    }

    /* Find nearest entities */
    LCNearestResult nearest[32];
    size_t nearest_count;
    if (index->mmap_ctx) {
        nearest_count = lc_mmap_grid_find_nearest(index->mmap_ctx, index->entities,
                                                   coord, 32, nearest);
    } else {
        nearest_count = lc_grid_find_nearest(index->grid, index->entities,
                                              coord, 32, nearest);
    }

    if (nearest_count == 0) return LC_OK;

    result->distance_m = nearest[0].distance_m;

    /* Categorize results */
    for (size_t i = 0; i < nearest_count; i++) {
        uint32_t eid = nearest[i].entity_id;
        if (eid >= index->num_entities) continue;

        LCEntity *e = &index->entities->entities[eid];

        switch (e->fclass) {
            case LC_CLASS_STREET:
                if (!result->street) result->street = e;
                break;

            case LC_CLASS_ADDRESS:
                if (!result->address) result->address = e;
                break;

            case LC_CLASS_POI:
                if (opts->include_poi && !result->poi) result->poi = e;
                break;

            case LC_CLASS_CITY:
            case LC_CLASS_TOWN:
            case LC_CLASS_VILLAGE:
            case LC_CLASS_SUBURB:
            case LC_CLASS_NEIGHBOURHOOD:
                if (!result->place) result->place = e;
                break;

            default:
                break;
        }
    }

    /* Build admin hierarchy (simplified - just find nearby admin boundaries) */
    /* TODO: Implement proper point-in-polygon for admin boundaries */

    return LC_OK;
}

void lc_reverse_result_free(LCReverseResult *result)
{
    if (!result) return;
    free(result->hierarchy);
    memset(result, 0, sizeof(LCReverseResult));
}

size_t lc_format_address(const LCReverseResult *result, char *buffer, size_t size)
{
    if (!result || !buffer || size == 0) return 0;

    size_t offset = 0;

    /* Format: [house] street, place */
    if (result->address && result->address->address.housenumber) {
        int n = snprintf(buffer + offset, size - offset, "%s ",
                        result->address->address.housenumber);
        if (n > 0) offset += (size_t)n;
    }

    if (result->street && result->street->name) {
        int n = snprintf(buffer + offset, size - offset, "%s",
                        result->street->name);
        if (n > 0) offset += (size_t)n;
    } else if (result->address && result->address->address.street) {
        int n = snprintf(buffer + offset, size - offset, "%s",
                        result->address->address.street);
        if (n > 0) offset += (size_t)n;
    }

    if (result->place && result->place->name) {
        if (offset > 0) {
            int n = snprintf(buffer + offset, size - offset, ", %s",
                            result->place->name);
            if (n > 0) offset += (size_t)n;
        } else {
            int n = snprintf(buffer + offset, size - offset, "%s",
                            result->place->name);
            if (n > 0) offset += (size_t)n;
        }
    }

    return offset;
}
