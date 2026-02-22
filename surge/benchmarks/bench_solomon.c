#include "surge.h"

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

/* SINTEF TOP VRPTW 100-customer BKS (hierarchical objective). */
static const SGBKS k_bks[] = {
    {"c101", 10, 828.94}, {"c102", 10, 828.94}, {"c103", 10, 828.06},
    {"c104", 10, 824.78}, {"c105", 10, 828.94}, {"c106", 10, 828.94},
    {"c107", 10, 828.94}, {"c108", 10, 828.94}, {"c109", 10, 828.94},
    {"c201", 3, 591.56},  {"c202", 3, 591.56},  {"c203", 3, 591.17},
    {"c204", 3, 590.60},  {"c205", 3, 588.88},  {"c206", 3, 588.49},
    {"c207", 3, 588.29},  {"c208", 3, 588.32},

    {"r101", 19, 1650.80}, {"r102", 17, 1486.12}, {"r103", 13, 1292.68},
    {"r104", 9, 1007.31},  {"r105", 14, 1377.11}, {"r106", 12, 1252.03},
    {"r107", 10, 1104.66}, {"r108", 9, 960.88},   {"r109", 11, 1194.73},
    {"r110", 10, 1118.84}, {"r111", 10, 1096.72}, {"r112", 9, 982.14},
    {"r201", 4, 1252.37},  {"r202", 3, 1191.70},  {"r203", 3, 939.50},
    {"r204", 2, 825.52},   {"r205", 3, 994.43},   {"r206", 3, 906.14},
    {"r207", 2, 890.61},   {"r208", 2, 726.82},   {"r209", 3, 909.16},
    {"r210", 3, 939.37},   {"r211", 2, 885.71},

    {"rc101", 14, 1696.95}, {"rc102", 12, 1554.75}, {"rc103", 11, 1261.67},
    {"rc104", 10, 1135.48}, {"rc105", 13, 1629.44}, {"rc106", 11, 1424.73},
    {"rc107", 11, 1230.48}, {"rc108", 10, 1139.82}, {"rc201", 4, 1406.94},
    {"rc202", 3, 1365.65},  {"rc203", 3, 1049.62},  {"rc204", 3, 798.46},
    {"rc205", 4, 1297.65},  {"rc206", 3, 1146.32},  {"rc207", 3, 1061.14},
    {"rc208", 3, 828.14},
};

static void sg_print_usage(const char *argv0) {
    printf("Usage: %s [options] [CASE ...]\n", argv0);
    printf("\n");
    printf("Options:\n");
    printf("  --dir <path>          Directory with Solomon .txt files (default: benchmarks/solomon)\n");
    printf("  --iterations <n>      ALNS max iterations per case (default: 10000)\n");
    printf("  --time-limit <sec>    ALNS max wall time per case (default: 0 = unlimited)\n");
    printf("  --seed <n>            Deterministic seed (default: 42)\n");
    printf("  --non-deterministic   Use time-based random seed\n");
    printf("  --telemetry           Print per-operator telemetry after each case\n");
    printf("  --help                Show this help\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s\n", argv0);
    printf("  %s C101 R101 RC101\n", argv0);
    printf("  %s --iterations 1500 --time-limit 3 R101\n", argv0);
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

static int sg_case_key_from_name(const char *src, char *dst, size_t dst_size) {
    char token[64];
    size_t j = 0;

    if (!src || !dst || dst_size < 8) {
        return 0;
    }

    while (*src != '\0' && *src != '.' && j + 1 < sizeof(token)) {
        unsigned char c = (unsigned char)*src;
        if (isalnum(c)) {
            token[j++] = (char)toupper(c);
        }
        src++;
    }
    token[j] = '\0';

    if (j >= 5 && token[0] == 'R' && token[1] == 'C' && isdigit((unsigned char)token[2]) &&
        isdigit((unsigned char)token[3]) && isdigit((unsigned char)token[4])) {
        snprintf(dst, dst_size, "rc%c%c%c", token[2], token[3], token[4]);
        return 1;
    }
    if (j >= 4 && (token[0] == 'C' || token[0] == 'R') && isdigit((unsigned char)token[1]) &&
        isdigit((unsigned char)token[2]) && isdigit((unsigned char)token[3])) {
        snprintf(dst, dst_size, "%c%c%c%c", (char)tolower((unsigned char)token[0]),
                 token[1], token[2], token[3]);
        return 1;
    }

    return 0;
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

static const SGBKS *sg_find_bks(const char *case_name) {
    char key[16];
    size_t i;

    if (!sg_case_key_from_name(case_name, key, sizeof(key))) {
        return NULL;
    }

    for (i = 0; i < sizeof(k_bks) / sizeof(k_bks[0]); i++) {
        if (strcmp(k_bks[i].name, key) == 0) {
            return &k_bks[i];
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

int main(int argc, char **argv) {
    const char *cases_dir = "benchmarks/solomon";
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
    int equal_vehicle_count = 0;
    int better_or_equal_lexi_count = 0;
    double sum_seconds = 0.0;
    double sum_distance = 0.0;
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

    printf("Surge Solomon Benchmark (Lexicographic vs BKS)\n");
    printf("  dir=%s\n", cases_dir);
    printf("  iterations=%d\n", max_iterations);
    printf("  time_limit=%d\n", max_time_seconds);
    if (deterministic) {
        printf("  deterministic=true seed=%" PRIu64 "\n", seed);
    } else {
        printf("  deterministic=false\n");
    }
    printf("\n");
    printf("%-9s %-9s %-8s %-5s %-10s %-5s %-10s %-8s %-8s\n", "case", "status", "sec",
           "veh", "dist", "bksV", "bksD", "vehGap", "distGap%");

    for (i = 0; i < matches.gl_pathc; i++) {
        SGContext *ctx;
        SGConfig config;
        SGStatus status;
        SGStatus solve_status;
        double start;
        double elapsed;
        double distance;
        uint32_t vehicles;
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
            fprintf(stderr, "Failed to allocate SGContext for %s\n", cases[i].name);
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
            printf("%-9s %-9s %-8s %-5s %-10s %-5s %-10s %-8s %-8s\n", cases[i].name,
                   sg_status_name(status), "-", "-", "-", "-", "-", "-", "-");
            sg_free(ctx);
            failed_count++;
            continue;
        }

        status = sg_load_solomon_vrptw(ctx, cases[i].path);
        if (status != SG_STATUS_OK) {
            printf("%-9s %-9s %-8s %-5s %-10s %-5s %-10s %-8s %-8s\n", cases[i].name,
                   sg_status_name(status), "-", "-", "-", "-", "-", "-", "-");
            sg_free(ctx);
            failed_count++;
            continue;
        }

        status = sg_validate_model(ctx);
        if (status != SG_STATUS_OK) {
            printf("%-9s %-9s %-8s %-5s %-10s %-5s %-10s %-8s %-8s\n", cases[i].name,
                   sg_status_name(status), "-", "-", "-", "-", "-", "-", "-");
            sg_free(ctx);
            failed_count++;
            continue;
        }

        start = sg_now_seconds();
        solve_status = sg_solve(ctx);
        elapsed = sg_now_seconds() - start;

        distance = sg_get_total_distance(ctx);
        vehicles = sg_get_used_vehicle_count(ctx);
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

        printf("%-9s %-9s %-8.3f %-5u %-10.2f %-5s %-10s %-8s %-8s\n", cases[i].name,
               sg_status_name(solve_status), elapsed, vehicles, distance, bks_veh_str, bks_dist_str,
               veh_gap_str, dist_gap_str);

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
        printf("Average solved: seconds=%.3f distance=%.2f\n",
               sum_seconds / (double)solved_count,
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
