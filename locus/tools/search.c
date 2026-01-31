/*
 * search.c - Simple geocoding search tool
 */

#include "locus.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <pbf-file> <query>\n", argv[0]);
        return 1;
    }

    const char *pbf_file = argv[1];
    const char *query = argv[2];

    printf("Loading %s...\n", pbf_file);

    LCIndex *index = lc_index_create();
    if (!index) {
        fprintf(stderr, "Failed to create index\n");
        return 1;
    }

    LCStatus status = lc_index_build_from_pbf(index, pbf_file, NULL);
    if (status != LC_OK) {
        fprintf(stderr, "Failed to load PBF: %s\n", lc_status_string(status));
        lc_index_free(index);
        return 1;
    }

    printf("Loaded %u entities\n\n", lc_index_entity_count(index));

    /* Search */
    LCSearchOptions opts;
    lc_search_options_default(&opts);
    opts.limit = 10;
    opts.fuzzy = 1;

    LCSearchResult result;
    status = lc_search(index, query, &opts, &result);
    if (status != LC_OK) {
        fprintf(stderr, "Search failed: %s\n", lc_status_string(status));
        lc_index_free(index);
        return 1;
    }

    printf("Search results for \"%s\":\n", query);
    printf("Found %zu results (%.3f ms)\n\n", result.num_results, result.query_time_ms);

    for (size_t i = 0; i < result.num_results; i++) {
        const LCEntity *entity = lc_search_get_entity(index, &result.matches[i]);
        if (entity) {
            printf("%zu. %s\n", i + 1, entity->name);
            printf("   Coordinates: %.6f, %.6f\n", entity->centroid.lat, entity->centroid.lon);
            printf("   Class: %s, Score: %.3f\n\n",
                   lc_class_string(entity->fclass),
                   result.matches[i].score);
        }
    }

    lc_search_result_free(&result);
    lc_index_free(index);
    return 0;
}
