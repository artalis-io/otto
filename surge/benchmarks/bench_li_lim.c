#include "surge.h"
#include "sg_parallel.h"

#include <ctype.h>
#include <glob.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    const char *path;
    const char *name;
} SGCaseFile;

typedef struct {
    const char *name;
    uint32_t vehicles;
    double distance;
} SGBKS;

/* SINTEF TOP Li & Lim PDPTW BKS for 100-task instances (hierarchical objective). */
static const SGBKS k_bks_100[] = {
    {"lc101", 10, 828.94}, {"lc102", 10, 828.94}, {"lc103", 9, 1035.35},
    {"lc104", 9, 860.01},  {"lc105", 10, 828.94}, {"lc106", 10, 828.94},
    {"lc107", 10, 828.94}, {"lc108", 10, 826.44}, {"lc109", 9, 1000.60},
    {"lc201", 3, 591.56},  {"lc202", 3, 591.56},  {"lc203", 3, 591.17},
    {"lc204", 3, 590.60},  {"lc205", 3, 588.88},  {"lc206", 3, 588.49},
    {"lc207", 3, 588.29},  {"lc208", 3, 588.32},
    {"lr101", 19, 1650.80}, {"lr102", 17, 1487.57}, {"lr103", 13, 1292.68},
    {"lr104", 9, 1013.39},  {"lr105", 14, 1377.11}, {"lr106", 12, 1252.62},
    {"lr107", 10, 1111.31}, {"lr108", 9, 968.97},   {"lr109", 11, 1208.96},
    {"lr110", 10, 1159.35}, {"lr111", 10, 1108.90}, {"lr112", 9, 1003.77},
    {"lr201", 4, 1253.23},  {"lr202", 3, 1197.67},  {"lr203", 3, 949.40},
    {"lr204", 2, 849.05},   {"lr205", 3, 1054.02},  {"lr206", 3, 931.63},
    {"lr207", 2, 903.06},   {"lr208", 2, 734.85},   {"lr209", 3, 930.59},
    {"lr210", 3, 964.22},   {"lr211", 2, 911.52},
    {"lrc101", 14, 1708.80}, {"lrc102", 12, 1558.07}, {"lrc103", 11, 1258.74},
    {"lrc104", 10, 1128.40}, {"lrc105", 13, 1637.62}, {"lrc106", 11, 1424.73},
    {"lrc107", 11, 1230.14}, {"lrc108", 10, 1147.43}, {"lrc201", 4, 1406.94},
    {"lrc202", 3, 1374.27},  {"lrc203", 3, 1089.07},  {"lrc204", 3, 818.66},
    {"lrc205", 4, 1302.20},  {"lrc206", 3, 1159.03},  {"lrc207", 3, 1062.05},
    {"lrc208", 3, 852.76},
};

static void sg_print_usage(const char *argv0) {
    printf("Usage: %s [options] [CASE ...]\n", argv0);
    printf("\n");
    printf("Options:\n");
    printf("  --dir <path>          Directory with Li & Lim .txt files (default: benchmarks/li_lim)\n");
    printf("  --iterations <n>      ALNS max iterations per case (default: 10000)\n");
    printf("  --time-limit <sec>    ALNS max wall time per case (default: 0 = unlimited)\n");
    printf("  --seed <n>            Deterministic seed (default: 42)\n");
    printf("  --non-deterministic   Use time-based random seed\n");
    printf("  --population          Use population-based parallel search\n");
    printf("  --threads <n>         Thread count for population mode (default: auto)\n");
    printf("  --generations <n>     Generation count for population mode (default: 3)\n");
    printf("  --telemetry           Print per-operator telemetry after each case\n");
    printf("  --help                Show this help\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s\n", argv0);
    printf("  %s LC101 LC102\n", argv0);
    printf("  %s --iterations 1500 --time-limit 3 LC101\n", argv0);
}

static const char *sg_basename(const char *path) {
    const char *slash;

    if (!path) {
        return "";
    }

    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void sg_normalize_case_name(const char *src, char *dst, size_t dst_size) {
    size_t j = 0;

    if (!dst || dst_size == 0) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    while (*src != '\0' && *src != '.' && j + 1 < dst_size) {
        unsigned char c = (unsigned char)*src;
        if (isalnum(c)) {
            dst[j++] = (char)toupper(c);
        }
        src++;
    }
    dst[j] = '\0';
}

static int sg_case_selected(const char *case_name, int filter_count, char **filters) {
    char normalized_case[64];
    int i;

    if (filter_count <= 0) {
        return 1;
    }

    sg_normalize_case_name(case_name, normalized_case, sizeof(normalized_case));
    for (i = 0; i < filter_count; i++) {
        char normalized_filter[64];
        sg_normalize_case_name(filters[i], normalized_filter, sizeof(normalized_filter));
        if (strcmp(normalized_case, normalized_filter) == 0) {
            return 1;
        }
    }

    return 0;
}

static int sg_compare_case_files(const void *lhs, const void *rhs) {
    const SGCaseFile *a = (const SGCaseFile *)lhs;
    const SGCaseFile *b = (const SGCaseFile *)rhs;
    return strcmp(a->name, b->name);
}

static int sg_case_key_from_name(const char *src, char *dst, size_t dst_size) {
    size_t j = 0;

    if (!src || !dst || dst_size < 6) {
        return 0;
    }
    while (*src != '\0' && *src != '.' && j + 1 < dst_size) {
        unsigned char c = (unsigned char)*src;
        if (isalnum(c)) {
            dst[j++] = (char)tolower(c);
        }
        src++;
    }
    dst[j] = '\0';
    return j >= 5;
}

static const SGBKS *sg_find_bks(const char *case_name) {
    char key[16];
    size_t i;

    if (!sg_case_key_from_name(case_name, key, sizeof(key))) {
        return NULL;
    }
    for (i = 0; i < sizeof(k_bks_100) / sizeof(k_bks_100[0]); i++) {
        if (strcmp(k_bks_100[i].name, key) == 0) {
            return &k_bks_100[i];
        }
    }
    return NULL;
}

static const char *sg_lexi_vs_bks(uint32_t vehicles, double distance, const SGBKS *bks) {
    if (!bks) {
        return "N/A";
    }
    if (vehicles < bks->vehicles) {
        return "betterV";
    }
    if (vehicles > bks->vehicles) {
        return "worseV";
    }
    if (distance + 1e-9 < bks->distance) {
        return "betterD";
    }
    if (distance > bks->distance + 1e-9) {
        return "worseD";
    }
    return "match";
}

static double sg_now_seconds(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static const char *sg_status_name(SGStatus status) {
    switch (status) {
        case SG_STATUS_OK:
            return "OK";
        case SG_STATUS_INVALID_ARG:
            return "INVALID_ARG";
        case SG_STATUS_OUT_OF_MEMORY:
            return "OUT_OF_MEMORY";
        case SG_STATUS_INFEASIBLE:
            return "INFEASIBLE";
        case SG_STATUS_LIMIT:
            return "LIMIT";
        case SG_STATUS_NOT_IMPLEMENTED:
            return "NOT_IMPLEMENTED";
        case SG_STATUS_ERROR:
        default:
            return "ERROR";
    }
}

int main(int argc, char **argv) {
    const char *cases_dir = "benchmarks/li_lim";
    int max_iterations = 10000;
    int max_time_seconds = 0;
    uint64_t seed = 42;
    int deterministic = 1;
    int show_telemetry = 0;
    int use_population = 0;
    uint32_t pop_threads = 0;
    uint32_t pop_generations = 3;
    int filter_start = argc;
    glob_t matches;
    SGCaseFile *cases = NULL;
    size_t i;
    int selected_count = 0;
    int solved_count = 0;
    int failed_count = 0;
    int compared_count = 0;
    int equal_vehicle_count = 0;
    int better_or_equal_lexi_count = 0;
    double sum_seconds = 0.0;
    double sum_distance = 0.0;
    double sum_vehicles = 0.0;
    double sum_unassigned = 0.0;
    double sum_vehicle_gap = 0.0;
    double sum_distance_gap = 0.0;
    char pattern[1024];

    memset(&matches, 0, sizeof(matches));

    for (i = 1; i < (size_t)argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            sg_print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--dir") == 0 && i + 1 < (size_t)argc) {
            cases_dir = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--iterations") == 0 && i + 1 < (size_t)argc) {
            max_iterations = atoi(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--time-limit") == 0 && i + 1 < (size_t)argc) {
            max_time_seconds = atoi(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < (size_t)argc) {
            seed = (uint64_t)strtoull(argv[++i], NULL, 10);
            continue;
        }
        if (strcmp(argv[i], "--non-deterministic") == 0) {
            deterministic = 0;
            continue;
        }
        if (strcmp(argv[i], "--population") == 0) {
            use_population = 1;
            continue;
        }
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < (size_t)argc) {
            pop_threads = (uint32_t)atoi(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--generations") == 0 && i + 1 < (size_t)argc) {
            pop_generations = (uint32_t)atoi(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--telemetry") == 0) {
            show_telemetry = 1;
            continue;
        }

        filter_start = (int)i;
        break;
    }

    if (max_iterations <= 0 || max_time_seconds < 0) {
        fprintf(stderr,
                "Invalid benchmark settings: iterations must be > 0 and time-limit must be >= 0\n");
        return 1;
    }

    if (snprintf(pattern, sizeof(pattern), "%s/*.txt", cases_dir) >= (int)sizeof(pattern)) {
        fprintf(stderr, "Case directory path is too long\n");
        return 1;
    }

    if (glob(pattern, 0, NULL, &matches) != 0 || matches.gl_pathc == 0) {
        fprintf(stderr, "No benchmark cases found under %s\n", cases_dir);
        globfree(&matches);
        return 1;
    }

    cases = (SGCaseFile *)calloc(matches.gl_pathc, sizeof(*cases));
    if (!cases) {
        fprintf(stderr, "Out of memory while preparing case list\n");
        globfree(&matches);
        return 1;
    }

    for (i = 0; i < matches.gl_pathc; i++) {
        cases[i].path = matches.gl_pathv[i];
        cases[i].name = sg_basename(matches.gl_pathv[i]);
    }
    qsort(cases, matches.gl_pathc, sizeof(*cases), sg_compare_case_files);

    printf("Surge Li & Lim PDPTW Benchmark\n");
    printf("  dir=%s\n", cases_dir);
    printf("  iterations=%d\n", max_iterations);
    printf("  time_limit=%d\n", max_time_seconds);
    if (deterministic) {
        printf("  deterministic=true seed=%" PRIu64 "\n", seed);
    } else {
        printf("  deterministic=false\n");
    }
    if (use_population) {
        printf("  population=true threads=%u generations=%u\n", pop_threads, pop_generations);
    }
    printf("\n");
    printf("%-14s %-9s %-8s %-6s %-6s %-10s %-5s %-10s %-8s %-8s\n",
           "case", "status", "sec", "req", "veh", "distance",
           "bksV", "bksD", "vehGap", "distGap%");

    for (i = 0; i < matches.gl_pathc; i++) {
        SGContext *ctx;
        SGConfig config;
        SGStatus status;
        SGStatus solve_status;
        double start;
        double elapsed;
        uint32_t request_count;
        uint32_t vehicles;
        uint32_t unassigned;
        double distance;
        const SGBKS *bks;
        int vehicle_gap = 0;
        double distance_gap_pct = 0.0;
        const char *bks_veh_str = "-";
        const char *bks_dist_str = "-";
        const char *veh_gap_str = "-";
        const char *dist_gap_str = "-";
        const char *lexi = "N/A";
        char bks_veh_buf[16];
        char bks_dist_buf[32];
        char veh_gap_buf[16];
        char dist_gap_buf[32];

        if (!sg_case_selected(cases[i].name, argc - filter_start, argv + filter_start)) {
            continue;
        }
        selected_count++;

        ctx = sg_create();
        if (!ctx) {
            printf("%-14s %-9s %-8s %-6s %-6s %-10s %-5s %-10s %-8s %-8s\n",
                   cases[i].name, "OUT_OF_MEMORY", "-", "-", "-", "-", "-", "-", "-", "-");
            failed_count++;
            continue;
        }

        sg_config_default(&config);
        config.max_iterations = max_iterations;
        config.max_time_seconds = max_time_seconds;
        config.seed = seed;
        config.deterministic = deterministic != 0;

        status = sg_set_config(ctx, &config);
        if (status != SG_STATUS_OK) {
            printf("%-14s %-9s %-8s %-6s %-6s %-10s %-5s %-10s %-8s %-8s\n",
                   cases[i].name, sg_status_name(status), "-", "-", "-", "-", "-", "-", "-", "-");
            sg_free(ctx);
            failed_count++;
            continue;
        }

        status = sg_load_li_lim_pdptw(ctx, cases[i].path);
        if (status != SG_STATUS_OK) {
            printf("%-14s %-9s %-8s %-6s %-6s %-10s %-5s %-10s %-8s %-8s\n",
                   cases[i].name, sg_status_name(status), "-", "-", "-", "-", "-", "-", "-", "-");
            sg_free(ctx);
            failed_count++;
            continue;
        }

        status = sg_validate_model(ctx);
        if (status != SG_STATUS_OK) {
            printf("%-14s %-9s %-8s %-6s %-6s %-10s %-5s %-10s %-8s %-8s\n",
                   cases[i].name, sg_status_name(status), "-", "-", "-", "-", "-", "-", "-", "-");
            sg_free(ctx);
            failed_count++;
            continue;
        }

        start = sg_now_seconds();
        if (use_population) {
            SGPopulationConfig pop_cfg;
            pop_cfg.num_threads = pop_threads;
            pop_cfg.population_size = 0;
            pop_cfg.num_generations = pop_generations;
            solve_status = sg_solve_population(ctx, &pop_cfg);
        } else {
            solve_status = sg_solve(ctx);
        }
        elapsed = sg_now_seconds() - start;

        request_count = sg_get_request_count(ctx);
        vehicles = sg_get_used_vehicle_count(ctx);
        unassigned = sg_get_unassigned(ctx);
        distance = sg_get_total_distance(ctx);
        bks = sg_find_bks(cases[i].name);

        if (bks) {
            vehicle_gap = (int)vehicles - (int)bks->vehicles;
            distance_gap_pct = (distance - bks->distance) * 100.0 / bks->distance;
            snprintf(bks_veh_buf, sizeof(bks_veh_buf), "%u", bks->vehicles);
            snprintf(bks_dist_buf, sizeof(bks_dist_buf), "%.2f", bks->distance);
            snprintf(veh_gap_buf, sizeof(veh_gap_buf), "%+d", vehicle_gap);
            snprintf(dist_gap_buf, sizeof(dist_gap_buf), "%+.1f", distance_gap_pct);
            bks_veh_str = bks_veh_buf;
            bks_dist_str = bks_dist_buf;
            veh_gap_str = veh_gap_buf;
            dist_gap_str = dist_gap_buf;
            lexi = sg_lexi_vs_bks(vehicles, distance, bks);
        }

        printf("%-14s %-9s %-8.3f %-6u %-6u %-10.2f %-5s %-10s %-8s %-8s\n",
               cases[i].name, sg_status_name(solve_status), elapsed, request_count,
               vehicles, distance, bks_veh_str, bks_dist_str, veh_gap_str, dist_gap_str);

        if (show_telemetry && (solve_status == SG_STATUS_OK || solve_status == SG_STATUS_LIMIT)) {
            uint32_t oi;
            uint32_t n_destroy = sg_get_destroy_operator_count(ctx);
            uint32_t n_repair = sg_get_repair_operator_count(ctx);
            for (oi = 0; oi < n_destroy; oi++) {
                SGOperatorStats os;
                if (sg_get_destroy_operator_stats(ctx, oi, &os) == SG_STATUS_OK) {
                    printf("  Destroy: %-20s sel=%-6" PRId64 " acc=%-6" PRId64
                           " imp=%-6" PRId64 " wt=%.2f sec=%.3f\n",
                           os.name, os.selected, os.accepted,
                           os.improvements, os.weight, os.total_seconds);
                }
            }
            for (oi = 0; oi < n_repair; oi++) {
                SGOperatorStats os;
                if (sg_get_repair_operator_stats(ctx, oi, &os) == SG_STATUS_OK) {
                    printf("  Repair:  %-20s sel=%-6" PRId64 " acc=%-6" PRId64
                           " imp=%-6" PRId64 " wt=%.2f sec=%.3f\n",
                           os.name, os.selected, os.accepted,
                           os.improvements, os.weight, os.total_seconds);
                }
            }
        }

        if (solve_status == SG_STATUS_OK || solve_status == SG_STATUS_LIMIT) {
            solved_count++;
            sum_seconds += elapsed;
            sum_distance += distance;
            sum_vehicles += (double)vehicles;
            sum_unassigned += (double)unassigned;
            if (bks) {
                compared_count++;
                sum_vehicle_gap += (double)vehicle_gap;
                sum_distance_gap += distance_gap_pct;
                if (vehicle_gap == 0) {
                    equal_vehicle_count++;
                }
                if (strcmp(lexi, "worseV") != 0 && strcmp(lexi, "worseD") != 0) {
                    better_or_equal_lexi_count++;
                }
            }
        } else {
            failed_count++;
        }

        sg_free(ctx);
    }

    printf("\n");
    if (selected_count == 0) {
        fprintf(stderr, "No cases matched the provided filter\n");
        free(cases);
        globfree(&matches);
        return 1;
    }

    printf("Summary: cases=%d solved=%d failed=%d\n", selected_count, solved_count, failed_count);
    if (solved_count > 0) {
        printf("Average solved: seconds=%.3f vehicles=%.2f unassigned=%.2f distance=%.2f\n",
               sum_seconds / (double)solved_count,
               sum_vehicles / (double)solved_count,
               sum_unassigned / (double)solved_count,
               sum_distance / (double)solved_count);
    }
    if (compared_count > 0) {
        printf("Against BKS: compared=%d equalVehicles=%d avgVehGap=%+.2f avgDistGap=%+.1f%% lexiNonWorse=%d\n",
               compared_count, equal_vehicle_count,
               sum_vehicle_gap / (double)compared_count,
               sum_distance_gap / (double)compared_count,
               better_or_equal_lexi_count);
    }

    free(cases);
    globfree(&matches);
    return failed_count == 0 ? 0 : 1;
}
