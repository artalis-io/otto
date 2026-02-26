#include "surge.h"
#include "sg_parallel.h"
#include "sh_args.h"
#include "sg_bench_utils.h"

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BKS_ENTRIES 512

static void sg_print_usage(const char *argv0) {
    printf("Usage: %s [options] [CASE ...]\n", argv0);
    printf("\n");
    printf("Options:\n");
    printf("  --dir <path>          Directory with Li & Lim .txt files (default: benchmarks/li_lim)\n");
    printf("  --bks <path>          BKS CSV file (default: benchmarks/bks/li_lim_100.csv)\n");
    printf("  --size <n>            Only run instances in subdirectory <n> (e.g., 200)\n");
    printf("  --iterations <n>      ALNS max iterations per case (default: 10000)\n");
    printf("  --time-limit <sec>    ALNS max wall time per case (default: 0 = unlimited)\n");
    printf("  --seed <n>            Deterministic seed (default: 42)\n");
    printf("  --non-deterministic   Use time-based random seed\n");
    printf("  --population          Use population-based parallel search\n");
    printf("  --threads <n>         Thread count for population mode (default: auto)\n");
    printf("  --generations <n>     Generation count for population mode (default: 3)\n");
    printf("  --telemetry           Print per-operator telemetry after each case\n");
    printf("  --output-csv <path>   Write results to CSV file\n");
    printf("  --help                Show this help\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s\n", argv0);
    printf("  %s LC101 LC102\n", argv0);
    printf("  %s --dir benchmarks/li_lim_extended --bks benchmarks/bks/li_lim_extended.csv --size 200\n", argv0);
}

int main(int argc, char **argv) {
    const char *cases_dir = "benchmarks/li_lim";
    const char *bks_path = "benchmarks/bks/li_lim_100.csv";
    const char *output_csv_path = NULL;
    int max_iterations = 10000;
    int max_time_seconds = 0;
    uint64_t seed = 42;
    int deterministic = 1;
    int show_telemetry = 0;
    int use_population = 0;
    uint32_t pop_threads = 0;
    uint32_t pop_generations = 3;
    int size_filter = 0;
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
    int equal_vehicle_count = 0;
    int better_or_equal_lexi_count = 0;
    double sum_seconds = 0.0;
    double sum_distance = 0.0;
    double sum_vehicles = 0.0;
    double sum_unassigned = 0.0;
    double sum_vehicle_gap = 0.0;
    double sum_distance_gap = 0.0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) { sg_print_usage(argv[0]); return 0; }
        if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) { cases_dir = argv[++i]; continue; }
        if (strcmp(argv[i], "--bks") == 0 && i + 1 < argc) { bks_path = argv[++i]; continue; }
        if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) { size_filter = sh_parse_int(argv[++i], 0, 0, 100000); continue; }
        if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) { max_iterations = sh_parse_int(argv[++i], 10000, 1, 1000000); continue; }
        if (strcmp(argv[i], "--time-limit") == 0 && i + 1 < argc) { max_time_seconds = sh_parse_int(argv[++i], 0, 0, 86400); continue; }
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) { seed = (uint64_t)strtoull(argv[++i], NULL, 10); continue; }
        if (strcmp(argv[i], "--non-deterministic") == 0) { deterministic = 0; continue; }
        if (strcmp(argv[i], "--population") == 0) { use_population = 1; continue; }
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) { pop_threads = (uint32_t)sh_parse_int(argv[++i], 0, 0, 256); continue; }
        if (strcmp(argv[i], "--generations") == 0 && i + 1 < argc) { pop_generations = (uint32_t)sh_parse_int(argv[++i], 3, 1, 10000); continue; }
        if (strcmp(argv[i], "--telemetry") == 0) { show_telemetry = 1; continue; }
        if (strcmp(argv[i], "--output-csv") == 0 && i + 1 < argc) { output_csv_path = argv[++i]; continue; }
        filter_start = i;
        break;
    }

    if (max_iterations <= 0 || max_time_seconds < 0) {
        fprintf(stderr, "Invalid settings: iterations > 0, time-limit >= 0\n");
        return 1;
    }

    /* Load BKS */
    bks_count = sg_load_bks_csv(bks_path, bks_entries, MAX_BKS_ENTRIES);
    if (bks_count < 0) {
        fprintf(stderr, "Warning: could not load BKS from %s\n", bks_path);
        bks_count = 0;
    }

    /* Collect cases */
    case_count = sg_collect_cases(cases_dir, size_filter, &cases);
    if (case_count == 0) {
        fprintf(stderr, "No benchmark cases found under %s\n", cases_dir);
        return 1;
    }
    qsort(cases, (size_t)case_count, sizeof(*cases), sg_compare_bench_cases);

    /* Open CSV output */
    if (output_csv_path) {
        csv_fp = fopen(output_csv_path, "w");
        if (csv_fp) {
            fprintf(csv_fp, "case,status,seconds,requests,vehicles,unassigned,distance,bks_vehicles,bks_distance,vehicle_gap,distance_gap_pct\n");
        }
    }

    printf("Surge Li & Lim PDPTW Benchmark\n");
    printf("  dir=%s  bks=%s (%d entries)\n", cases_dir, bks_path, bks_count);
    printf("  iterations=%d  time_limit=%d\n", max_iterations, max_time_seconds);
    if (deterministic) printf("  deterministic=true seed=%" PRIu64 "\n", seed);
    else printf("  deterministic=false\n");
    if (use_population) printf("  population=true threads=%u generations=%u\n", pop_threads, pop_generations);
    if (size_filter > 0) printf("  size_filter=%d\n", size_filter);
    printf("\n");
    printf("%-16s %-9s %-8s %-6s %-6s %-10s %-5s %-10s %-8s %-8s\n",
           "case", "status", "sec", "req", "veh", "distance",
           "bksV", "bksD", "vehGap", "distGap%");

    for (i = 0; i < case_count; i++) {
        SGContext *ctx;
        SGConfig config;
        SGStatus status, solve_status;
        double start, elapsed, distance;
        uint32_t request_count, vehicles, unassigned;
        char case_key[64];
        const SGBKSEntry *bks = NULL;
        int vehicle_gap = 0;
        double distance_gap_pct = 0.0;
        const char *bks_veh_str = "-", *bks_dist_str = "-";
        const char *veh_gap_str = "-", *dist_gap_str = "-";
        char bks_veh_buf[16], bks_dist_buf[32], veh_gap_buf[16], dist_gap_buf[32];

        if (!sg_bench_case_selected(cases[i].name, argc - filter_start, argv + filter_start)) continue;
        selected_count++;

        ctx = sg_create();
        if (!ctx) { printf("%-16s %-9s\n", cases[i].name, "OUT_OF_MEMORY"); failed_count++; continue; }

        sg_config_default(&config);
        config.max_iterations = max_iterations;
        config.max_time_seconds = max_time_seconds;
        config.seed = seed;
        config.deterministic = deterministic != 0;

        status = sg_set_config(ctx, &config);
        if (status != SG_STATUS_OK) { printf("%-16s %-9s\n", cases[i].name, sg_bench_status_name(status)); sg_free(ctx); failed_count++; continue; }

        status = sg_load_li_lim_pdptw(ctx, cases[i].path);
        if (status != SG_STATUS_OK) { printf("%-16s %-9s\n", cases[i].name, sg_bench_status_name(status)); sg_free(ctx); failed_count++; continue; }

        status = sg_validate_model(ctx);
        if (status != SG_STATUS_OK) { printf("%-16s %-9s\n", cases[i].name, sg_bench_status_name(status)); sg_free(ctx); failed_count++; continue; }

        start = sg_bench_now();
        if (use_population) {
            SGPopulationConfig pop_cfg;
            pop_cfg.num_threads = pop_threads;
            pop_cfg.population_size = 0;
            pop_cfg.num_generations = pop_generations;
            solve_status = sg_solve_population(ctx, &pop_cfg);
        } else {
            solve_status = sg_solve(ctx);
        }
        elapsed = sg_bench_now() - start;

        request_count = sg_get_request_count(ctx);
        vehicles = sg_get_used_vehicle_count(ctx);
        unassigned = sg_get_unassigned(ctx);
        distance = sg_get_total_distance(ctx);

        if (sg_bench_case_key(cases[i].name, case_key, sizeof(case_key))) {
            bks = sg_bks_find(bks_entries, bks_count, case_key);
        }

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
        }

        printf("%-16s %-9s %-8.3f %-6u %-6u %-10.2f %-5s %-10s %-8s %-8s\n",
               cases[i].name, sg_bench_status_name(solve_status), elapsed,
               request_count, vehicles, distance, bks_veh_str, bks_dist_str,
               veh_gap_str, dist_gap_str);

        if (csv_fp) {
            fprintf(csv_fp, "%s,%s,%.3f,%u,%u,%u,%.2f,%s,%s,%s,%s\n",
                    cases[i].name, sg_bench_status_name(solve_status), elapsed,
                    request_count, vehicles, unassigned, distance,
                    bks ? bks_veh_buf : "", bks ? bks_dist_buf : "",
                    bks ? veh_gap_buf : "", bks ? dist_gap_buf : "");
        }

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
                if (vehicle_gap == 0) equal_vehicle_count++;
                const char *lexi = sg_bench_lexi_vs_bks(vehicles, distance, bks);
                if (strcmp(lexi, "worseV") != 0 && strcmp(lexi, "worseD") != 0) better_or_equal_lexi_count++;
            }
        } else {
            failed_count++;
        }

        sg_free(ctx);
    }

    printf("\n");
    if (selected_count == 0) { fprintf(stderr, "No cases matched\n"); sg_free_bench_cases(cases, case_count); if (csv_fp) fclose(csv_fp); return 1; }

    printf("Summary: cases=%d solved=%d failed=%d\n", selected_count, solved_count, failed_count);
    if (solved_count > 0) {
        printf("Average solved: seconds=%.3f vehicles=%.2f unassigned=%.2f distance=%.2f\n",
               sum_seconds / (double)solved_count, sum_vehicles / (double)solved_count,
               sum_unassigned / (double)solved_count, sum_distance / (double)solved_count);
    }
    if (compared_count > 0) {
        printf("Against BKS: compared=%d equalVehicles=%d avgVehGap=%+.2f avgDistGap=%+.1f%% lexiNonWorse=%d\n",
               compared_count, equal_vehicle_count,
               sum_vehicle_gap / (double)compared_count,
               sum_distance_gap / (double)compared_count,
               better_or_equal_lexi_count);
    }

    sg_free_bench_cases(cases, case_count);
    if (csv_fp) fclose(csv_fp);
    return failed_count == 0 ? 0 : 1;
}
