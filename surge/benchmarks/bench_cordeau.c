#include "surge.h"
#include "sh_args.h"
#include "sg_bench_utils.h"

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BKS_ENTRIES 128

static void sg_print_usage(const char *argv0) {
    printf("Usage: %s [options] [CASE ...]\n", argv0);
    printf("\n");
    printf("Options:\n");
    printf("  --dir <path>          Directory with Cordeau .txt files (default: benchmarks/cordeau)\n");
    printf("  --bks <path>          BKS CSV file (default: benchmarks/bks/cordeau_darp.csv)\n");
    printf("  --iterations <n>      ALNS max iterations per case (default: 10000)\n");
    printf("  --time-limit <sec>    ALNS max wall time per case (default: 0 = unlimited)\n");
    printf("  --seed <n>            Deterministic seed (default: 42)\n");
    printf("  --non-deterministic   Use time-based random seed\n");
    printf("  --telemetry           Print per-operator telemetry after each case\n");
    printf("  --output-csv <path>   Write results to CSV file\n");
    printf("  --help                Show this help\n");
}

int main(int argc, char **argv) {
    const char *cases_dir = "benchmarks/cordeau";
    const char *bks_path = "benchmarks/bks/cordeau_darp.csv";
    const char *output_csv_path = NULL;
    int max_iterations = 10000;
    int max_time_seconds = 0;
    uint64_t seed = 42;
    int deterministic = 1;
    int show_telemetry = 0;
    int filter_start = argc;

    SGBKSEntry bks_entries[MAX_BKS_ENTRIES];
    int bks_count = 0;
    SGBenchCase *cases = NULL;
    int case_count = 0;
    FILE *csv_fp = NULL;

    int i;
    int selected_count = 0;
    int solved_count = 0;
    int failed_count = 0;
    int compared_count = 0;
    double sum_seconds = 0.0;
    double sum_distance = 0.0;
    double sum_vehicles = 0.0;
    double sum_unassigned = 0.0;
    double sum_distance_gap = 0.0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) { sg_print_usage(argv[0]); return 0; }
        if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) { cases_dir = argv[++i]; continue; }
        if (strcmp(argv[i], "--bks") == 0 && i + 1 < argc) { bks_path = argv[++i]; continue; }
        if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) { max_iterations = sh_parse_int(argv[++i], 10000, 1, 1000000); continue; }
        if (strcmp(argv[i], "--time-limit") == 0 && i + 1 < argc) { max_time_seconds = sh_parse_int(argv[++i], 0, 0, 86400); continue; }
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) { seed = (uint64_t)strtoull(argv[++i], NULL, 10); continue; }
        if (strcmp(argv[i], "--non-deterministic") == 0) { deterministic = 0; continue; }
        if (strcmp(argv[i], "--telemetry") == 0) { show_telemetry = 1; continue; }
        if (strcmp(argv[i], "--output-csv") == 0 && i + 1 < argc) { output_csv_path = argv[++i]; continue; }
        filter_start = i;
        break;
    }

    /* Load BKS */
    bks_count = sg_load_bks_csv(bks_path, bks_entries, MAX_BKS_ENTRIES);
    if (bks_count < 0) {
        fprintf(stderr, "Warning: could not load BKS from %s\n", bks_path);
        bks_count = 0;
    }

    /* Collect cases */
    case_count = sg_collect_cases(cases_dir, 0, &cases);
    if (case_count == 0) {
        fprintf(stderr, "No benchmark cases found under %s\n", cases_dir);
        return 1;
    }
    qsort(cases, (size_t)case_count, sizeof(*cases), sg_compare_bench_cases);

    if (output_csv_path) {
        csv_fp = fopen(output_csv_path, "w");
        if (csv_fp) fprintf(csv_fp, "case,status,seconds,requests,vehicles,unassigned,distance,bks_vehicles,bks_distance,distance_gap_pct\n");
    }

    printf("Surge Cordeau DARP Benchmark\n");
    printf("  dir=%s  bks=%s (%d entries)\n", cases_dir, bks_path, bks_count);
    printf("  iterations=%d  seed=%" PRIu64 "\n", max_iterations, seed);
    printf("\n");
    printf("%-16s %-9s %-8s %-6s %-6s %-10s %-5s %-10s %-8s\n",
           "case", "status", "sec", "req", "veh", "distance",
           "bksV", "bksD", "distGap%");

    for (i = 0; i < case_count; i++) {
        SGContext *ctx;
        SGConfig config;
        SGStatus status, solve_status;
        double start, elapsed, distance;
        uint32_t request_count, vehicles, unassigned;
        char case_key[64];
        const SGBKSEntry *bks = NULL;
        char bks_veh_buf[16], bks_dist_buf[32], dist_gap_buf[32];
        const char *bks_veh_str = "-", *bks_dist_str = "-", *dist_gap_str = "-";

        if (!sg_bench_case_selected(cases[i].name, argc - filter_start, argv + filter_start)) continue;
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
            printf("%-16s %-9s\n", cases[i].name, sg_bench_status_name(status));
            sg_free(ctx); failed_count++; continue;
        }

        status = sg_load_cordeau_darp(ctx, cases[i].path);
        if (status != SG_STATUS_OK) {
            printf("%-16s %-9s (load)\n", cases[i].name, sg_bench_status_name(status));
            sg_free(ctx); failed_count++; continue;
        }

        status = sg_validate_model(ctx);
        if (status != SG_STATUS_OK) {
            printf("%-16s %-9s (validate)\n", cases[i].name, sg_bench_status_name(status));
            sg_free(ctx); failed_count++; continue;
        }

        start = sg_bench_now();
        solve_status = sg_solve(ctx);
        elapsed = sg_bench_now() - start;

        request_count = sg_get_request_count(ctx);
        vehicles = sg_get_used_vehicle_count(ctx);
        unassigned = sg_get_unassigned(ctx);
        distance = sg_get_total_distance(ctx);

        if (sg_bench_case_key(cases[i].name, case_key, sizeof(case_key))) {
            bks = sg_bks_find(bks_entries, bks_count, case_key);
        }

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

        printf("%-16s %-9s %-8.3f %-6u %-6u %-10.2f %-5s %-10s %-8s\n",
               cases[i].name, sg_bench_status_name(solve_status), elapsed,
               request_count, vehicles, distance, bks_veh_str, bks_dist_str, dist_gap_str);

        if (csv_fp) {
            fprintf(csv_fp, "%s,%s,%.3f,%u,%u,%u,%.2f,%s,%s,%s\n",
                    cases[i].name, sg_bench_status_name(solve_status), elapsed,
                    request_count, vehicles, unassigned, distance,
                    bks ? bks_veh_buf : "", bks ? bks_dist_buf : "",
                    bks ? dist_gap_buf : "");
        }

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
               sum_seconds / (double)solved_count, sum_vehicles / (double)solved_count,
               sum_unassigned / (double)solved_count, sum_distance / (double)solved_count);
    }
    if (compared_count > 0) {
        printf("Against BKS: compared=%d avgDistGap=%+.1f%%\n",
               compared_count, sum_distance_gap / (double)compared_count);
    }

    sg_free_bench_cases(cases, case_count);
    if (csv_fp) fclose(csv_fp);
    return failed_count == 0 ? 0 : 1;
}
