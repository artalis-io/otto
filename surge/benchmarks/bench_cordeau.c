#include "surge.h"
#include "sh_args.h"

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

/* BKS table for Cordeau a-series (approximate, from literature). */
static const SGBKS k_bks_cordeau[] = {
    {"a1",  3,  190.02},
    {"a2",  3,  301.34},
    {"a3",  4,  532.08},
    {"a4",  4,  541.09},
    {"a5",  5,  636.97},
    {"a6",  6,  793.29},
    {"a7",  7,  291.71},
    {"a8",  7,  487.16},
    {"a9",  8,  660.98},
    {"a10", 8,  878.06},
    {"a11", 8, 1004.82},
    {"a12", 9, 1116.38},
    {"a13", 9, 1356.03},
};

static void sg_print_usage(const char *argv0) {
    printf("Usage: %s [options] [CASE ...]\n", argv0);
    printf("\n");
    printf("Options:\n");
    printf("  --dir <path>          Directory with Cordeau .txt files (default: benchmarks/cordeau)\n");
    printf("  --iterations <n>      ALNS max iterations per case (default: 10000)\n");
    printf("  --time-limit <sec>    ALNS max wall time per case (default: 0 = unlimited)\n");
    printf("  --seed <n>            Deterministic seed (default: 42)\n");
    printf("  --non-deterministic   Use time-based random seed\n");
    printf("  --telemetry           Print per-operator telemetry after each case\n");
    printf("  --help                Show this help\n");
}

static const char *sg_basename(const char *path) {
    const char *slash;
    if (!path) return "";
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void sg_normalize_case_name(const char *src, char *dst, size_t dst_size) {
    size_t j = 0;
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    while (*src != '\0' && *src != '.' && j + 1 < dst_size) {
        unsigned char c = (unsigned char)*src;
        if (isalnum(c)) dst[j++] = (char)toupper(c);
        src++;
    }
    dst[j] = '\0';
}

static int sg_case_selected(const char *case_name, int filter_count, char **filters) {
    char normalized_case[64];
    int i;
    if (filter_count <= 0) return 1;
    sg_normalize_case_name(case_name, normalized_case, sizeof(normalized_case));
    for (i = 0; i < filter_count; i++) {
        char normalized_filter[64];
        sg_normalize_case_name(filters[i], normalized_filter, sizeof(normalized_filter));
        if (strcmp(normalized_case, normalized_filter) == 0) return 1;
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
    if (!src || !dst || dst_size < 2) return 0;
    while (*src != '\0' && *src != '.' && j + 1 < dst_size) {
        unsigned char c = (unsigned char)*src;
        if (isalnum(c)) dst[j++] = (char)tolower(c);
        src++;
    }
    dst[j] = '\0';
    return j >= 2;
}

static const SGBKS *sg_find_bks(const char *case_name) {
    char key[16];
    size_t i;
    if (!sg_case_key_from_name(case_name, key, sizeof(key))) return NULL;
    for (i = 0; i < sizeof(k_bks_cordeau) / sizeof(k_bks_cordeau[0]); i++) {
        if (strcmp(k_bks_cordeau[i].name, key) == 0) return &k_bks_cordeau[i];
    }
    return NULL;
}

static double sg_now_seconds(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static const char *sg_status_name(SGStatus status) {
    switch (status) {
        case SG_STATUS_OK: return "OK";
        case SG_STATUS_INVALID_ARG: return "INVALID_ARG";
        case SG_STATUS_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
        case SG_STATUS_INFEASIBLE: return "INFEASIBLE";
        case SG_STATUS_LIMIT: return "LIMIT";
        case SG_STATUS_NOT_IMPLEMENTED: return "NOT_IMPLEMENTED";
        case SG_STATUS_ERROR:
        default: return "ERROR";
    }
}

int main(int argc, char **argv) {
    const char *cases_dir = "benchmarks/cordeau";
    int max_iterations = 10000;
    int max_time_seconds = 0;
    uint64_t seed = 42;
    int deterministic = 1;
    int show_telemetry = 0;
    int filter_start = argc;
    glob_t matches;
    SGCaseFile *cases = NULL;
    size_t i;
    int selected_count = 0;
    int solved_count = 0;
    int failed_count = 0;
    int compared_count = 0;
    double sum_seconds = 0.0;
    double sum_distance = 0.0;
    double sum_vehicles = 0.0;
    double sum_unassigned = 0.0;
    double sum_distance_gap = 0.0;
    char pattern[1024];

    memset(&matches, 0, sizeof(matches));

    for (i = 1; i < (size_t)argc; i++) {
        if (strcmp(argv[i], "--help") == 0) { sg_print_usage(argv[0]); return 0; }
        if (strcmp(argv[i], "--dir") == 0 && i + 1 < (size_t)argc) { cases_dir = argv[++i]; continue; }
        if (strcmp(argv[i], "--iterations") == 0 && i + 1 < (size_t)argc) { max_iterations = sh_parse_int(argv[++i], 10000, 1, 1000000); continue; }
        if (strcmp(argv[i], "--time-limit") == 0 && i + 1 < (size_t)argc) { max_time_seconds = sh_parse_int(argv[++i], 0, 0, 86400); continue; }
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < (size_t)argc) { seed = (uint64_t)strtoull(argv[++i], NULL, 10); continue; }
        if (strcmp(argv[i], "--non-deterministic") == 0) { deterministic = 0; continue; }
        if (strcmp(argv[i], "--telemetry") == 0) { show_telemetry = 1; continue; }
        filter_start = (int)i;
        break;
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
        globfree(&matches);
        return 1;
    }

    for (i = 0; i < matches.gl_pathc; i++) {
        cases[i].path = matches.gl_pathv[i];
        cases[i].name = sg_basename(matches.gl_pathv[i]);
    }
    qsort(cases, matches.gl_pathc, sizeof(*cases), sg_compare_case_files);

    printf("Surge Cordeau DARP Benchmark\n");
    printf("  dir=%s iterations=%d seed=%" PRIu64 "\n", cases_dir, max_iterations, seed);
    printf("\n");
    printf("%-14s %-9s %-8s %-6s %-6s %-10s %-5s %-10s %-8s\n",
           "case", "status", "sec", "req", "veh", "distance",
           "bksV", "bksD", "distGap%");

    for (i = 0; i < matches.gl_pathc; i++) {
        SGContext *ctx;
        SGConfig config;
        SGStatus status;
        SGStatus solve_status;
        double start, elapsed;
        uint32_t request_count, vehicles, unassigned;
        double distance;
        const SGBKS *bks;
        char bks_veh_buf[16], bks_dist_buf[32], dist_gap_buf[32];
        const char *bks_veh_str = "-", *bks_dist_str = "-", *dist_gap_str = "-";

        if (!sg_case_selected(cases[i].name, argc - filter_start, argv + filter_start)) continue;
        selected_count++;

        ctx = sg_create();
        if (!ctx) { failed_count++; continue; }

        sg_config_default(&config);
        config.max_iterations = max_iterations;
        config.max_time_seconds = max_time_seconds;
        config.seed = seed;
        config.deterministic = deterministic != 0;

        status = sg_set_config(ctx, &config);
        if (status != SG_STATUS_OK) {
            printf("%-14s %-9s\n", cases[i].name, sg_status_name(status));
            sg_free(ctx); failed_count++; continue;
        }

        status = sg_load_cordeau_darp(ctx, cases[i].path);
        if (status != SG_STATUS_OK) {
            printf("%-14s %-9s (load)\n", cases[i].name, sg_status_name(status));
            sg_free(ctx); failed_count++; continue;
        }

        status = sg_validate_model(ctx);
        if (status != SG_STATUS_OK) {
            printf("%-14s %-9s (validate)\n", cases[i].name, sg_status_name(status));
            sg_free(ctx); failed_count++; continue;
        }

        start = sg_now_seconds();
        solve_status = sg_solve(ctx);
        elapsed = sg_now_seconds() - start;

        request_count = sg_get_request_count(ctx);
        vehicles = sg_get_used_vehicle_count(ctx);
        unassigned = sg_get_unassigned(ctx);
        distance = sg_get_total_distance(ctx);
        bks = sg_find_bks(cases[i].name);

        if (bks) {
            double gap = (distance - bks->distance) * 100.0 / bks->distance;
            snprintf(bks_veh_buf, sizeof(bks_veh_buf), "%u", bks->vehicles);
            snprintf(bks_dist_buf, sizeof(bks_dist_buf), "%.2f", bks->distance);
            snprintf(dist_gap_buf, sizeof(dist_gap_buf), "%+.1f", gap);
            bks_veh_str = bks_veh_buf;
            bks_dist_str = bks_dist_buf;
            dist_gap_str = dist_gap_buf;
            compared_count++;
            sum_distance_gap += gap;
        }

        printf("%-14s %-9s %-8.3f %-6u %-6u %-10.2f %-5s %-10s %-8s\n",
               cases[i].name, sg_status_name(solve_status), elapsed, request_count,
               vehicles, distance, bks_veh_str, bks_dist_str, dist_gap_str);

        if (show_telemetry && (solve_status == SG_STATUS_OK || solve_status == SG_STATUS_LIMIT)) {
            uint32_t oi;
            uint32_t n_destroy = sg_get_destroy_operator_count(ctx);
            uint32_t n_repair = sg_get_repair_operator_count(ctx);
            for (oi = 0; oi < n_destroy; oi++) {
                SGOperatorStats os;
                if (sg_get_destroy_operator_stats(ctx, oi, &os) == SG_STATUS_OK) {
                    printf("  Destroy: %-20s sel=%-6" PRId64 " acc=%-6" PRId64
                           " imp=%-6" PRId64 " wt=%.2f\n",
                           os.name, os.selected, os.accepted, os.improvements, os.weight);
                }
            }
            for (oi = 0; oi < n_repair; oi++) {
                SGOperatorStats os;
                if (sg_get_repair_operator_stats(ctx, oi, &os) == SG_STATUS_OK) {
                    printf("  Repair:  %-20s sel=%-6" PRId64 " acc=%-6" PRId64
                           " imp=%-6" PRId64 " wt=%.2f\n",
                           os.name, os.selected, os.accepted, os.improvements, os.weight);
                }
            }
        }

        if (solve_status == SG_STATUS_OK || solve_status == SG_STATUS_LIMIT) {
            solved_count++;
            sum_seconds += elapsed;
            sum_distance += distance;
            sum_vehicles += (double)vehicles;
            sum_unassigned += (double)unassigned;
        } else {
            failed_count++;
        }

        sg_free(ctx);
    }

    printf("\nSummary: cases=%d solved=%d failed=%d\n", selected_count, solved_count, failed_count);
    if (solved_count > 0) {
        printf("Average: seconds=%.3f vehicles=%.2f unassigned=%.2f distance=%.2f\n",
               sum_seconds / (double)solved_count,
               sum_vehicles / (double)solved_count,
               sum_unassigned / (double)solved_count,
               sum_distance / (double)solved_count);
    }
    if (compared_count > 0) {
        printf("Against BKS: compared=%d avgDistGap=%+.1f%%\n",
               compared_count, sum_distance_gap / (double)compared_count);
    }

    free(cases);
    globfree(&matches);
    return failed_count == 0 ? 0 : 1;
}
