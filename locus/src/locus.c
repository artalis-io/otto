/*
 * locus.c - Locus Geocoding Library main API
 *
 * Location Oriented Coordinate Unification System
 */

#include "locus.h"
#include <stdio.h>

/* ============================================================================
 * Version
 * ============================================================================ */

static const char VERSION[] = "0.1.0";

const char *lc_version(void)
{
    return VERSION;
}

/* ============================================================================
 * High-Level API
 * ============================================================================ */

LCEntityStore *lc_load_pbf(const char *filename, const LCPBFOptions *opts)
{
    if (!filename) return NULL;

    LCPBFContext *ctx = lc_pbf_context_create();
    if (!ctx) return NULL;

    LCStatus status = lc_pbf_parse_file(ctx, filename, opts);
    if (status != LC_OK) {
        fprintf(stderr, "locus: Failed to parse PBF: %s\n", lc_status_string(status));
        lc_pbf_context_free(ctx);
        return NULL;
    }

    /* Print stats */
    LCPBFStats stats;
    lc_pbf_get_stats(ctx, &stats);
    fprintf(stderr, "locus: Parsed %lu nodes, %lu ways, %lu relations (%.1f ms)\n",
            (unsigned long)stats.nodes_processed,
            (unsigned long)stats.ways_processed,
            (unsigned long)stats.relations_processed,
            stats.parse_time_ms);
    fprintf(stderr, "locus: Found %u places, %u streets, %u addresses, %u POIs, %u boundaries\n",
            stats.places_found, stats.streets_found, stats.addresses_found,
            stats.pois_found, stats.boundaries_found);

    LCEntityStore *store = lc_pbf_take_entities(ctx);
    lc_pbf_context_free(ctx);

    if (store) {
        fprintf(stderr, "locus: Total entities: %u\n", store->count);
    }

    return store;
}

uint32_t lc_entity_count(const LCEntityStore *store)
{
    if (!store) return 0;
    return store->count;
}

const LCEntity *lc_get_entity(const LCEntityStore *store, uint32_t index)
{
    if (!store || index >= store->count) return NULL;
    return &store->entities[index];
}
