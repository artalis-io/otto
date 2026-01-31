/*
 * search.c - Geocoding search tool with binary index support
 *
 * Usage:
 *   search <data-file> <query>       # Auto-detect PBF or .lcix
 *   search --save <pbf> <lcix>       # Convert PBF to binary index
 *   search --info <file>             # Show file info
 */

#include "locus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s <data-file> <query>       Search for locations\n", prog);
    fprintf(stderr, "  %s --save <pbf> <lcix>       Convert PBF to binary index\n", prog);
    fprintf(stderr, "  %s --info <file>             Show file info\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Data files:\n");
    fprintf(stderr, "  .osm.pbf  OpenStreetMap PBF file\n");
    fprintf(stderr, "  .lcix     Locus binary index (faster loading)\n");
}

static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int cmd_info(const char *path) {
    if (lc_is_binary_index(path)) {
        uint32_t version = lc_binary_version(path);
        printf("File: %s\n", path);
        printf("Type: Locus binary index (.lcix)\n");
        printf("Version: %u\n", version);

        /* Load to get more info */
        LCIndex *index = lc_index_load(path);
        if (index) {
            printf("Entities: %u\n", lc_index_entity_count(index));
            lc_index_free(index);
        }
    } else {
        printf("File: %s\n", path);
        printf("Type: OpenStreetMap PBF\n");
        printf("Note: Use --save to convert to binary index for faster loading\n");
    }
    return 0;
}

static int cmd_save(const char *pbf_path, const char *lcix_path) {
    printf("Loading PBF: %s\n", pbf_path);
    double start = get_time_ms();

    LCIndex *index = lc_index_create();
    if (!index) {
        fprintf(stderr, "Failed to create index\n");
        return 1;
    }

    LCStatus status = lc_index_build_from_pbf(index, pbf_path, NULL);
    if (status != LC_OK) {
        fprintf(stderr, "Failed to load PBF: %s\n", lc_status_string(status));
        lc_index_free(index);
        return 1;
    }

    double load_time = get_time_ms() - start;
    printf("Loaded %u entities in %.2f ms\n", lc_index_entity_count(index), load_time);

    printf("Saving binary index: %s\n", lcix_path);
    start = get_time_ms();

    status = lc_index_save(index, lcix_path);
    if (status != LC_OK) {
        fprintf(stderr, "Failed to save index: %s\n", lc_status_string(status));
        lc_index_free(index);
        return 1;
    }

    double save_time = get_time_ms() - start;
    printf("Saved in %.2f ms\n", save_time);

    lc_index_free(index);
    return 0;
}

static int cmd_search(const char *data_path, const char *query) {
    double start = get_time_ms();
    LCIndex *index = NULL;

    if (lc_is_binary_index(data_path)) {
        printf("Loading binary index: %s\n", data_path);
        index = lc_index_load(data_path);
        if (!index) {
            fprintf(stderr, "Failed to load binary index\n");
            return 1;
        }
    } else {
        printf("Loading PBF: %s\n", data_path);
        index = lc_index_create();
        if (!index) {
            fprintf(stderr, "Failed to create index\n");
            return 1;
        }

        LCStatus status = lc_index_build_from_pbf(index, data_path, NULL);
        if (status != LC_OK) {
            fprintf(stderr, "Failed to load PBF: %s\n", lc_status_string(status));
            lc_index_free(index);
            return 1;
        }
    }

    double load_time = get_time_ms() - start;
    printf("Loaded %u entities in %.2f ms\n\n", lc_index_entity_count(index), load_time);

    /* Search */
    LCSearchOptions opts;
    lc_search_options_default(&opts);
    opts.limit = 10;
    opts.fuzzy = 1;

    LCSearchResult result;
    LCStatus status = lc_search(index, query, &opts, &result);
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

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    /* --info command */
    if (strcmp(argv[1], "--info") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: --info requires a file path\n");
            return 1;
        }
        return cmd_info(argv[2]);
    }

    /* --save command */
    if (strcmp(argv[1], "--save") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: --save requires <pbf> and <lcix> paths\n");
            return 1;
        }
        return cmd_save(argv[2], argv[3]);
    }

    /* Search command */
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    return cmd_search(argv[1], argv[2]);
}
