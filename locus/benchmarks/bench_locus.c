/*
 * bench_locus.c - Comprehensive Locus Benchmarks
 *
 * Profiles all major components:
 * - PBF parsing and binary index loading
 * - Index building (trie, ngram, spatial)
 * - Forward search (exact, prefix, fuzzy)
 * - Autocomplete
 * - Reverse geocoding
 * - Text normalization
 *
 * Usage:
 *   ./bench_locus                           # Monaco (default)
 *   ./bench_locus ../data/hungary.osm.pbf   # Hungary
 *   ./bench_locus --full ../data/hungary.osm.pbf  # Full benchmark suite
 */

#include "locus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <sys/resource.h>

/* ============================================================================
 * Timing Utilities
 * ============================================================================ */

typedef struct {
    double min;
    double max;
    double sum;
    double sum_sq;
    int count;
} BenchStats;

static void stats_init(BenchStats *s) {
    s->min = 1e9;
    s->max = 0;
    s->sum = 0;
    s->sum_sq = 0;
    s->count = 0;
}

static void stats_add(BenchStats *s, double value) {
    if (value < s->min) s->min = value;
    if (value > s->max) s->max = value;
    s->sum += value;
    s->sum_sq += value * value;
    s->count++;
}

static double stats_mean(const BenchStats *s) {
    return s->count > 0 ? s->sum / s->count : 0;
}

static double stats_stddev(const BenchStats *s) {
    if (s->count < 2) return 0;
    double mean = stats_mean(s);
    double variance = (s->sum_sq / s->count) - (mean * mean);
    return variance > 0 ? sqrt(variance) : 0;
}

static double get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1000.0;
}

static double get_time_ms(void) {
    return get_time_us() / 1000.0;
}

static size_t get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return (size_t)usage.ru_maxrss;
}

/* ============================================================================
 * Benchmark Results
 * ============================================================================ */

typedef struct {
    /* Loading */
    double pbf_load_ms;
    double binary_save_ms;
    double binary_load_ms;
    uint32_t entity_count;
    size_t memory_mb;

    /* Index building */
    double trie_build_ms;
    double ngram_build_ms;
    double spatial_build_ms;

    /* Search */
    BenchStats search_exact;
    BenchStats search_prefix;
    BenchStats search_fuzzy;
    BenchStats search_miss;

    /* Autocomplete */
    BenchStats autocomplete_short;   /* 1-2 chars */
    BenchStats autocomplete_medium;  /* 3-4 chars */
    BenchStats autocomplete_long;    /* 5+ chars */

    /* Reverse */
    BenchStats reverse_hit;
    BenchStats reverse_miss;

    /* Normalization */
    BenchStats normalize_ascii;
    BenchStats normalize_unicode;
} BenchResults;

static BenchResults results;

/* ============================================================================
 * Test Queries
 * ============================================================================ */

/* Queries for Monaco dataset */
static const char *monaco_exact_queries[] = {
    "Monte Carlo", "Casino", "Monaco", "Port Hercule",
    "La Condamine", "Fontvieille", "Larvotto", "Moneghetti"
};
#define MONACO_EXACT_COUNT (sizeof(monaco_exact_queries) / sizeof(monaco_exact_queries[0]))

static const char *monaco_prefix_queries[] = {
    "Mon", "Cas", "Port", "La ", "Fon"
};
#define MONACO_PREFIX_COUNT (sizeof(monaco_prefix_queries) / sizeof(monaco_prefix_queries[0]))

static const char *monaco_fuzzy_queries[] = {
    "Monte Karlo", "Kasino", "Monako", "Fontvieil"
};
#define MONACO_FUZZY_COUNT (sizeof(monaco_fuzzy_queries) / sizeof(monaco_fuzzy_queries[0]))

/* Queries for Hungary dataset */
static const char *hungary_exact_queries[] = {
    "Budapest", "Debrecen", "Szeged", "Miskolc", "Pécs",
    "Győr", "Nyíregyháza", "Kecskemét", "Székesfehérvár", "Szombathely",
    "Andrássy út", "Váci utca", "Margit híd", "Hősök tere"
};
#define HUNGARY_EXACT_COUNT (sizeof(hungary_exact_queries) / sizeof(hungary_exact_queries[0]))

static const char *hungary_prefix_queries[] = {
    "Bud", "Deb", "Sze", "Mis", "And", "Vác", "Hős"
};
#define HUNGARY_PREFIX_COUNT (sizeof(hungary_prefix_queries) / sizeof(hungary_prefix_queries[0]))

static const char *hungary_fuzzy_queries[] = {
    "Budapeszt", "Andrashy", "Vaci utca", "Hosok ter", "Margit hid"
};
#define HUNGARY_FUZZY_COUNT (sizeof(hungary_fuzzy_queries) / sizeof(hungary_fuzzy_queries[0]))

static const char *miss_queries[] = {
    "XYZNOTFOUND", "qwertyuiop", "12345abcde", "zzzzzzzzzz"
};
#define MISS_COUNT (sizeof(miss_queries) / sizeof(miss_queries[0]))

/* Normalization test strings */
static const char *normalize_ascii[] = {
    "hello world", "NEW YORK CITY", "Los Angeles", "Main Street"
};
#define NORMALIZE_ASCII_COUNT (sizeof(normalize_ascii) / sizeof(normalize_ascii[0]))

static const char *normalize_unicode[] = {
    "Székesfehérvár", "Großglockner", "Zürich", "Malmö",
    "Łódź", "Kraków", "Gdańsk", "Москва"
};
#define NORMALIZE_UNICODE_COUNT (sizeof(normalize_unicode) / sizeof(normalize_unicode[0]))

/* Reverse geocoding test points */
static const double monaco_coords[][2] = {
    {43.7384, 7.4246},   /* Center */
    {43.7500, 7.4200},   /* North */
    {43.7300, 7.4150},   /* South */
    {43.7400, 7.4350},   /* East */
};
#define MONACO_COORD_COUNT (sizeof(monaco_coords) / sizeof(monaco_coords[0]))

static const double hungary_coords[][2] = {
    {47.4979, 19.0402},  /* Budapest center */
    {47.5316, 21.6273},  /* Debrecen */
    {46.2530, 20.1414},  /* Szeged */
    {48.1035, 20.7784},  /* Miskolc */
    {46.0727, 18.2323},  /* Pécs */
    {47.6875, 17.6504},  /* Győr */
};
#define HUNGARY_COORD_COUNT (sizeof(hungary_coords) / sizeof(hungary_coords[0]))

static const double miss_coords[][2] = {
    {0.0, 0.0},          /* Null Island */
    {90.0, 0.0},         /* North Pole */
    {-90.0, 0.0},        /* South Pole */
};
#define MISS_COORD_COUNT (sizeof(miss_coords) / sizeof(miss_coords[0]))

/* ============================================================================
 * Benchmark Functions
 * ============================================================================ */

static void bench_normalization(int iterations) {
    printf("\n=== Text Normalization ===\n");

    char buffer[256];

    /* ASCII normalization */
    stats_init(&results.normalize_ascii);
    for (int iter = 0; iter < iterations; iter++) {
        for (size_t i = 0; i < NORMALIZE_ASCII_COUNT; i++) {
            double start = get_time_us();
            lc_normalize_to(normalize_ascii[i], buffer, sizeof(buffer));
            double elapsed = get_time_us() - start;
            stats_add(&results.normalize_ascii, elapsed);
        }
    }
    printf("  ASCII:   %.3f µs (min=%.3f, max=%.3f, stddev=%.3f)\n",
           stats_mean(&results.normalize_ascii),
           results.normalize_ascii.min,
           results.normalize_ascii.max,
           stats_stddev(&results.normalize_ascii));

    /* Unicode normalization */
    stats_init(&results.normalize_unicode);
    for (int iter = 0; iter < iterations; iter++) {
        for (size_t i = 0; i < NORMALIZE_UNICODE_COUNT; i++) {
            double start = get_time_us();
            lc_normalize_to(normalize_unicode[i], buffer, sizeof(buffer));
            double elapsed = get_time_us() - start;
            stats_add(&results.normalize_unicode, elapsed);
        }
    }
    printf("  Unicode: %.3f µs (min=%.3f, max=%.3f, stddev=%.3f)\n",
           stats_mean(&results.normalize_unicode),
           results.normalize_unicode.min,
           results.normalize_unicode.max,
           stats_stddev(&results.normalize_unicode));
}

static void bench_search(LCIndex *index, const char **queries, size_t query_count,
                         BenchStats *stats, const char *label, int iterations, int fuzzy) {
    LCSearchOptions opts;
    lc_search_options_default(&opts);
    opts.limit = 10;
    opts.fuzzy = fuzzy;

    stats_init(stats);

    for (int iter = 0; iter < iterations; iter++) {
        for (size_t i = 0; i < query_count; i++) {
            double start = get_time_us();
            LCSearchResult result;
            lc_search(index, queries[i], &opts, &result);
            double elapsed = get_time_us() - start;
            stats_add(stats, elapsed);
            lc_search_result_free(&result);
        }
    }

    printf("  %-12s %.3f µs (min=%.3f, max=%.3f, stddev=%.3f, n=%d)\n",
           label, stats_mean(stats), stats->min, stats->max,
           stats_stddev(stats), stats->count);
}

static void bench_autocomplete(LCIndex *index, const char **prefixes, size_t prefix_count,
                               BenchStats *stats, const char *label, int iterations) {
    stats_init(stats);

    for (int iter = 0; iter < iterations; iter++) {
        for (size_t i = 0; i < prefix_count; i++) {
            double start = get_time_us();
            LCSearchResult result;
            lc_autocomplete(index, prefixes[i], 10, &result);
            double elapsed = get_time_us() - start;
            stats_add(stats, elapsed);
            lc_search_result_free(&result);
        }
    }

    printf("  %-12s %.3f µs (min=%.3f, max=%.3f, stddev=%.3f, n=%d)\n",
           label, stats_mean(stats), stats->min, stats->max,
           stats_stddev(stats), stats->count);
}

static void bench_reverse(LCIndex *index, const double coords[][2], size_t coord_count,
                          BenchStats *stats, const char *label, int iterations) {
    stats_init(stats);

    for (int iter = 0; iter < iterations; iter++) {
        for (size_t i = 0; i < coord_count; i++) {
            SHCoord coord = {.lat = coords[i][0], .lon = coords[i][1]};
            double start = get_time_us();
            LCReverseResult result;
            lc_reverse(index, coord, NULL, &result);
            double elapsed = get_time_us() - start;
            stats_add(stats, elapsed);
            lc_reverse_result_free(&result);
        }
    }

    printf("  %-12s %.3f µs (min=%.3f, max=%.3f, stddev=%.3f, n=%d)\n",
           label, stats_mean(stats), stats->min, stats->max,
           stats_stddev(stats), stats->count);
}

static void bench_binary_index(LCIndex *index, const char *tmp_path) {
    printf("\n=== Binary Index I/O ===\n");

    /* Save */
    double start = get_time_ms();
    LCStatus status = lc_index_save(index, tmp_path);
    results.binary_save_ms = get_time_ms() - start;

    if (status == LC_OK) {
        printf("  Save:    %.2f ms\n", results.binary_save_ms);

        /* Get file size */
        FILE *f = fopen(tmp_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fclose(f);
            printf("  Size:    %.2f MB\n", size / (1024.0 * 1024.0));
        }

        /* Load */
        start = get_time_ms();
        LCIndex *loaded = lc_index_load(tmp_path);
        results.binary_load_ms = get_time_ms() - start;

        if (loaded) {
            printf("  Load:    %.2f ms (%.1fx vs PBF)\n",
                   results.binary_load_ms,
                   results.pbf_load_ms / results.binary_load_ms);
            lc_index_free(loaded);
        } else {
            printf("  Load:    FAILED\n");
        }

        /* Cleanup */
        remove(tmp_path);
    } else {
        printf("  Save:    FAILED\n");
    }
}

static void print_summary(int is_hungary) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║                         BENCHMARK SUMMARY                        ║\n");
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("║ Dataset:        %-48s ║\n", is_hungary ? "Hungary (~1.1M entities)" : "Monaco (2.7K entities)");
    printf("║ Entities:       %-48u ║\n", results.entity_count);
    printf("║ Memory:         %-45zu MB ║\n", results.memory_mb);
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("║ LOADING                                                          ║\n");
    printf("║   PBF parse:    %10.2f ms                                     ║\n", results.pbf_load_ms);
    printf("║   Binary save:  %10.2f ms                                     ║\n", results.binary_save_ms);
    printf("║   Binary load:  %10.2f ms                                     ║\n", results.binary_load_ms);
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("║ SEARCH (mean latency)                                            ║\n");
    printf("║   Exact match:  %10.3f µs                                     ║\n", stats_mean(&results.search_exact));
    printf("║   Prefix match: %10.3f µs                                     ║\n", stats_mean(&results.search_prefix));
    printf("║   Fuzzy match:  %10.3f µs                                     ║\n", stats_mean(&results.search_fuzzy));
    printf("║   Miss (no hit):%10.3f µs                                     ║\n", stats_mean(&results.search_miss));
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("║ AUTOCOMPLETE (mean latency)                                      ║\n");
    printf("║   Short prefix: %10.3f µs                                     ║\n", stats_mean(&results.autocomplete_short));
    printf("║   Medium prefix:%10.3f µs                                     ║\n", stats_mean(&results.autocomplete_medium));
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("║ REVERSE GEOCODE (mean latency)                                   ║\n");
    printf("║   Hit:          %10.3f µs                                     ║\n", stats_mean(&results.reverse_hit));
    printf("║   Miss:         %10.3f µs                                     ║\n", stats_mean(&results.reverse_miss));
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("║ NORMALIZATION (mean latency)                                     ║\n");
    printf("║   ASCII:        %10.3f µs                                     ║\n", stats_mean(&results.normalize_ascii));
    printf("║   Unicode:      %10.3f µs                                     ║\n", stats_mean(&results.normalize_unicode));
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char **argv) {
    int full_mode = 0;
    const char *pbf_file = "../data/monaco-latest.osm.pbf";

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--full") == 0) {
            full_mode = 1;
        } else if (argv[i][0] != '-') {
            pbf_file = argv[i];
        }
    }

    int is_hungary = strstr(pbf_file, "hungary") != NULL;
    int iterations = full_mode ? 1000 : (is_hungary ? 100 : 1000);

    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║                    LOCUS BENCHMARK SUITE                         ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n\n");

    printf("File:       %s\n", pbf_file);
    printf("Mode:       %s\n", full_mode ? "full" : "quick");
    printf("Iterations: %d\n", iterations);

    /* Warmup: Normalization benchmark (no index needed) */
    bench_normalization(iterations);

    /* Load PBF */
    printf("\n=== PBF Loading ===\n");
    size_t mem_before = get_memory_kb();
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

    results.pbf_load_ms = get_time_ms() - start;
    results.entity_count = lc_index_entity_count(index);
    results.memory_mb = (get_memory_kb() - mem_before) / 1024;

    printf("  Entities: %u\n", results.entity_count);
    printf("  Time:     %.2f ms (%.0f entities/sec)\n",
           results.pbf_load_ms,
           results.entity_count / (results.pbf_load_ms / 1000.0));
    printf("  Memory:   %zu MB\n", results.memory_mb);

    /* Binary index benchmark */
    bench_binary_index(index, "/tmp/bench_locus.lcix");

    /* Search benchmarks */
    printf("\n=== Forward Search ===\n");
    if (is_hungary) {
        bench_search(index, hungary_exact_queries, HUNGARY_EXACT_COUNT,
                     &results.search_exact, "Exact:", iterations, 0);
        bench_search(index, hungary_prefix_queries, HUNGARY_PREFIX_COUNT,
                     &results.search_prefix, "Prefix:", iterations, 0);
        bench_search(index, hungary_fuzzy_queries, HUNGARY_FUZZY_COUNT,
                     &results.search_fuzzy, "Fuzzy:", iterations, 1);
    } else {
        bench_search(index, monaco_exact_queries, MONACO_EXACT_COUNT,
                     &results.search_exact, "Exact:", iterations, 0);
        bench_search(index, monaco_prefix_queries, MONACO_PREFIX_COUNT,
                     &results.search_prefix, "Prefix:", iterations, 0);
        bench_search(index, monaco_fuzzy_queries, MONACO_FUZZY_COUNT,
                     &results.search_fuzzy, "Fuzzy:", iterations, 1);
    }
    bench_search(index, miss_queries, MISS_COUNT,
                 &results.search_miss, "Miss:", iterations, 1);

    /* Autocomplete benchmarks */
    printf("\n=== Autocomplete ===\n");
    const char *short_prefixes[] = {"M", "B", "S", "A", "P"};
    const char *medium_prefixes[] = {"Mon", "Bud", "Sze", "And", "Por"};

    bench_autocomplete(index, short_prefixes, 5,
                       &results.autocomplete_short, "1-char:", iterations);
    bench_autocomplete(index, medium_prefixes, 5,
                       &results.autocomplete_medium, "3-char:", iterations);

    /* Reverse geocoding benchmarks */
    printf("\n=== Reverse Geocoding ===\n");
    if (is_hungary) {
        bench_reverse(index, hungary_coords, HUNGARY_COORD_COUNT,
                      &results.reverse_hit, "Hit:", iterations);
    } else {
        bench_reverse(index, monaco_coords, MONACO_COORD_COUNT,
                      &results.reverse_hit, "Hit:", iterations);
    }
    bench_reverse(index, miss_coords, MISS_COORD_COUNT,
                  &results.reverse_miss, "Miss:", iterations);

    /* Summary */
    print_summary(is_hungary);

    /* Identify bottlenecks */
    printf("\n=== BOTTLENECK ANALYSIS ===\n");

    /* Loading */
    if (results.pbf_load_ms > 10000) {
        printf("⚠ PBF loading is slow (%.1fs). Consider:\n", results.pbf_load_ms / 1000);
        printf("  - Using binary index for production\n");
        printf("  - Parallelizing PBF parsing\n");
        printf("  - Memory-mapping the file\n");
    }

    /* Binary load */
    if (results.binary_load_ms > results.pbf_load_ms * 0.5) {
        printf("⚠ Binary index load is slow (%.1f%% of PBF). Consider:\n",
               100.0 * results.binary_load_ms / results.pbf_load_ms);
        printf("  - Serializing trie/spatial indexes\n");
        printf("  - Using mmap for index loading\n");
    }

    /* Search */
    if (stats_mean(&results.search_fuzzy) > 100) {
        printf("⚠ Fuzzy search is slow (%.1f µs). Consider:\n",
               stats_mean(&results.search_fuzzy));
        printf("  - Limiting n-gram candidate set\n");
        printf("  - Using BK-tree for edit distance\n");
        printf("  - Bloom filter for quick rejection\n");
    }

    /* Autocomplete */
    if (stats_mean(&results.autocomplete_short) > 50) {
        printf("⚠ Short-prefix autocomplete is slow (%.1f µs). Consider:\n",
               stats_mean(&results.autocomplete_short));
        printf("  - Caching common prefixes\n");
        printf("  - Limiting result set during traversal\n");
    }

    /* Reverse */
    if (stats_mean(&results.reverse_hit) > 50) {
        printf("⚠ Reverse geocoding is slow (%.1f µs). Consider:\n",
               stats_mean(&results.reverse_hit));
        printf("  - Reducing grid cell size\n");
        printf("  - Using R-tree for spatial index\n");
    }

    /* Memory */
    if (results.memory_mb > 500) {
        printf("⚠ Memory usage is high (%zu MB). Consider:\n", results.memory_mb);
        printf("  - String interning/deduplication\n");
        printf("  - Compact entity representation\n");
        printf("  - Memory-mapped indexes\n");
    }

    printf("\nDone.\n");

    lc_index_free(index);
    return 0;
}
