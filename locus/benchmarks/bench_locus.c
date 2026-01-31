/*
 * bench_locus.c - Locus benchmarks
 */

#include "locus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static void bench_search(LCIndex *index, const char *query, int iterations) {
    double start = get_time_ms();

    LCSearchOptions opts;
    lc_search_options_default(&opts);
    opts.limit = 10;

    for (int i = 0; i < iterations; i++) {
        LCSearchResult result;
        lc_search(index, query, &opts, &result);
        lc_search_result_free(&result);
    }

    double end = get_time_ms();
    double per_query = (end - start) / iterations;

    printf("  search(\"%s\"): %.3f ms/query (%d iterations)\n",
           query, per_query, iterations);
}

static void bench_autocomplete(LCIndex *index, const char *prefix, int iterations) {
    double start = get_time_ms();

    for (int i = 0; i < iterations; i++) {
        LCSearchResult result;
        lc_autocomplete(index, prefix, 10, &result);
        lc_search_result_free(&result);
    }

    double end = get_time_ms();
    double per_query = (end - start) / iterations;

    printf("  autocomplete(\"%s\"): %.3f ms/query (%d iterations)\n",
           prefix, per_query, iterations);
}

static void bench_reverse(LCIndex *index, double lat, double lon, int iterations) {
    double start = get_time_ms();

    SHCoord coord = {.lat = lat, .lon = lon};

    for (int i = 0; i < iterations; i++) {
        LCReverseResult result;
        lc_reverse(index, coord, NULL, &result);
        lc_reverse_result_free(&result);
    }

    double end = get_time_ms();
    double per_query = (end - start) / iterations;

    printf("  reverse(%.4f, %.4f): %.3f ms/query (%d iterations)\n",
           lat, lon, per_query, iterations);
}

int main(int argc, char **argv) {
    const char *pbf_file = argc > 1 ? argv[1] : "../data/monaco-latest.osm.pbf";

    printf("Locus Benchmarks\n");
    printf("================\n\n");

    /* Load index */
    printf("Loading %s...\n", pbf_file);
    double start = get_time_ms();

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

    double end = get_time_ms();
    printf("Loaded %u entities in %.1f ms (%.1f MB)\n\n",
           lc_index_entity_count(index),
           end - start,
           (double)lc_index_memory_usage(index) / (1024.0 * 1024.0));

    /* Run benchmarks */
    printf("Forward Search:\n");
    bench_search(index, "Monte Carlo", 1000);
    bench_search(index, "Casino", 1000);
    bench_search(index, "Monaco", 1000);

    printf("\nAutocomplete:\n");
    bench_autocomplete(index, "Mon", 1000);
    bench_autocomplete(index, "Monte", 1000);
    bench_autocomplete(index, "Ca", 1000);

    printf("\nReverse Geocoding:\n");
    bench_reverse(index, 43.7384, 7.4246, 1000);  /* Monaco center */
    bench_reverse(index, 43.7500, 7.4200, 1000);  /* Northern Monaco */

    printf("\nDone.\n");

    lc_index_free(index);
    return 0;
}
