/*
 * bench_tune.c - Hyperparameter tuner for Surge VRP solver
 *
 * Evaluates configurations across a representative instance set,
 * using tiered parameter search (coarse grid -> refinement).
 * Results are written as JSON for analysis.
 *
 * Build: make bench-tune
 * Usage: bench_tune --tier 0 --iterations 2500 --json
 *        bench_tune --all-tiers --threads 4
 *        bench_tune --baseline
 */
#include "surge.h"
#include "sg_bench_utils.h"
#include "sh_args.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Representative Instance Set ---- */

typedef struct {
    const char *path;
    const char *name;
    int loader; /* 0=solomon, 1=li_lim */
    uint32_t bks_vehicles;
    double bks_distance;
} TuneInstance;

/* Default representative set (18 instances) —
   covers all Solomon + Li-Lim class types at 100-customer scale */
static const TuneInstance k_representative[] = {
    /* Solomon VRPTW — 12 instances covering all 6 classes */
    {"benchmarks/solomon/C101.txt", "C101", 0, 10, 828.94},
    {"benchmarks/solomon/C104.txt", "C104", 0, 10, 824.78},
    {"benchmarks/solomon/C201.txt", "C201", 0, 3, 591.56},
    {"benchmarks/solomon/C204.txt", "C204", 0, 3, 590.60},
    {"benchmarks/solomon/R101.txt", "R101", 0, 19, 1645.79},
    {"benchmarks/solomon/R104.txt", "R104", 0, 9, 971.50},
    {"benchmarks/solomon/R108.txt", "R108", 0, 9, 960.88},
    {"benchmarks/solomon/R201.txt", "R201", 0, 4, 1252.37},
    {"benchmarks/solomon/R204.txt", "R204", 0, 2, 825.52},
    {"benchmarks/solomon/RC101.txt","RC101", 0, 14, 1696.94},
    {"benchmarks/solomon/RC108.txt","RC108", 0, 11, 1139.82},
    {"benchmarks/solomon/RC201.txt","RC201", 0, 4, 1406.94},
    /* Li-Lim PDPTW — 6 instances */
    {"benchmarks/li_lim/lc101.txt", "LC101", 1, 10, 828.94},
    {"benchmarks/li_lim/lc104.txt", "LC104", 1, 10, 824.78},
    {"benchmarks/li_lim/lr101.txt", "LR101", 1, 19, 1650.80},
    {"benchmarks/li_lim/lr108.txt", "LR108", 1, 9, 968.97},
    {"benchmarks/li_lim/lrc101.txt","LRC101", 1, 14, 1708.80},
    {"benchmarks/li_lim/lrc104.txt","LRC104", 1, 11, 1144.21},
};
#define NUM_REPRESENTATIVE (int)(sizeof(k_representative) / sizeof(k_representative[0]))

/* ---- Grid Generation ---- */

static void log_grid(double lo, double hi, int n, double *out) {
    double log_lo = log(lo);
    double log_hi = log(hi);
    int i;
    if (n <= 1) { out[0] = sqrt(lo * hi); return; }
    for (i = 0; i < n; i++) {
        double t = (double)i / (double)(n - 1);
        out[i] = exp(log_lo + t * (log_hi - log_lo));
    }
}

static void lin_grid(double lo, double hi, int n, double *out) {
    int i;
    if (n <= 1) { out[0] = (lo + hi) / 2.0; return; }
    for (i = 0; i < n; i++) {
        double t = (double)i / (double)(n - 1);
        out[i] = lo + t * (hi - lo);
    }
}

/* ---- Evaluation ---- */

typedef struct {
    SGTuneParams params;
    double avg_vehicle_gap;
    double avg_distance_gap_pct;
    double worst_vehicle_gap;
    double composite_score;      /* lower = better */
    double total_time_seconds;
    int valid;
} TuneResult;

static double evaluate_instance(const TuneInstance *inst, const SGTuneParams *params,
                                int max_iterations, uint64_t seed,
                                double *out_vgap, double *out_dgap) {
    SGContext *ctx = sg_create();
    SGConfig cfg;
    SGStatus status;
    double wall_start, wall_end;
    uint32_t vehicles;
    double distance;

    if (!ctx) {
        *out_vgap = 100.0;
        *out_dgap = 100.0;
        return 0.0;
    }

    sg_config_default(&cfg);
    cfg.max_iterations = max_iterations;
    cfg.seed = seed;
    cfg.deterministic = true;
    cfg.require_bound_requests_at_solve = true;
    sg_set_config(ctx, &cfg);

    if (params) {
        sg_set_tune_params(ctx, params);
    }

    /* Load instance */
    if (inst->loader == 0) {
        status = sg_load_solomon_vrptw(ctx, inst->path);
    } else {
        status = sg_load_li_lim_pdptw(ctx, inst->path);
    }

    if (status != SG_STATUS_OK) {
        sg_free(ctx);
        *out_vgap = 100.0;
        *out_dgap = 100.0;
        return 0.0;
    }

    wall_start = sg_bench_now();
    status = sg_solve(ctx);
    wall_end = sg_bench_now();

    if (status != SG_STATUS_OK && status != SG_STATUS_LIMIT) {
        sg_free(ctx);
        *out_vgap = 100.0;
        *out_dgap = 100.0;
        return wall_end - wall_start;
    }

    vehicles = sg_get_used_vehicle_count(ctx);
    distance = sg_get_total_distance(ctx);

    /* Vehicle gap (integer: positive = worse, negative = better) */
    *out_vgap = (double)vehicles - (double)inst->bks_vehicles;

    /* Distance gap % (only meaningful when vehicles match) */
    if (vehicles == inst->bks_vehicles && inst->bks_distance > 0.0) {
        *out_dgap = 100.0 * (distance - inst->bks_distance) / inst->bks_distance;
    } else if (vehicles < inst->bks_vehicles) {
        *out_dgap = 0.0; /* Better vehicle count, distance gap irrelevant */
    } else {
        *out_dgap = 50.0; /* Worse vehicle count, large penalty */
    }

    sg_free(ctx);
    return wall_end - wall_start;
}

static void evaluate_config(const TuneInstance *instances, int num_instances,
                            const SGTuneParams *params, int max_iterations,
                            uint64_t seed, TuneResult *result) {
    double sum_vgap = 0.0, sum_dgap = 0.0;
    double worst_vgap = -1e9;
    double total_time = 0.0;
    int i;

    result->params = *params;
    result->valid = 1;

    for (i = 0; i < num_instances; i++) {
        double vgap, dgap;
        double t = evaluate_instance(&instances[i], params, max_iterations, seed,
                                     &vgap, &dgap);
        sum_vgap += vgap;
        sum_dgap += dgap;
        if (vgap > worst_vgap) worst_vgap = vgap;
        total_time += t;
    }

    result->avg_vehicle_gap = sum_vgap / num_instances;
    result->avg_distance_gap_pct = sum_dgap / num_instances;
    result->worst_vehicle_gap = worst_vgap;
    result->total_time_seconds = total_time;

    /* Composite score: heavily penalize vehicle excess, reward distance quality */
    result->composite_score = 100.0 * result->avg_vehicle_gap
                            + result->avg_distance_gap_pct
                            + 50.0 * (worst_vgap > 0.0 ? worst_vgap : 0.0);
}

/* ---- Parallel Evaluation ---- */

typedef struct {
    const TuneInstance *instances;
    int num_instances;
    int max_iterations;
    uint64_t seed;
    SGTuneParams *configs;   /* array of configurations */
    TuneResult *results;     /* output array */
    int total_configs;
    volatile int next_config; /* atomic counter */
    pthread_mutex_t mutex;
} TuneWorkContext;

static void *tune_worker(void *arg) {
    TuneWorkContext *wctx = (TuneWorkContext *)arg;

    for (;;) {
        int idx;
        pthread_mutex_lock(&wctx->mutex);
        idx = wctx->next_config++;
        pthread_mutex_unlock(&wctx->mutex);

        if (idx >= wctx->total_configs) break;

        evaluate_config(wctx->instances, wctx->num_instances,
                        &wctx->configs[idx], wctx->max_iterations,
                        wctx->seed, &wctx->results[idx]);

        /* Progress indicator */
        fprintf(stderr, "\r  [%d/%d] configs evaluated", idx + 1, wctx->total_configs);
    }
    return NULL;
}

static void evaluate_configs_parallel(const TuneInstance *instances, int num_instances,
                                      SGTuneParams *configs, int num_configs,
                                      int max_iterations, uint64_t seed,
                                      int num_threads, TuneResult *results) {
    TuneWorkContext wctx;
    pthread_t *threads;
    int i;

    wctx.instances = instances;
    wctx.num_instances = num_instances;
    wctx.max_iterations = max_iterations;
    wctx.seed = seed;
    wctx.configs = configs;
    wctx.results = results;
    wctx.total_configs = num_configs;
    wctx.next_config = 0;
    pthread_mutex_init(&wctx.mutex, NULL);

    threads = (pthread_t *)calloc((size_t)num_threads, sizeof(*threads));
    if (!threads) {
        /* Fallback to single-threaded */
        for (i = 0; i < num_configs; i++) {
            evaluate_config(instances, num_instances, &configs[i],
                            max_iterations, seed, &results[i]);
            fprintf(stderr, "\r  [%d/%d] configs evaluated", i + 1, num_configs);
        }
        return;
    }

    for (i = 0; i < num_threads; i++) {
        pthread_create(&threads[i], NULL, tune_worker, &wctx);
    }
    for (i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    fprintf(stderr, "\n");

    free(threads);
    pthread_mutex_destroy(&wctx.mutex);
}

/* ---- Result Sorting ---- */

static int compare_results(const void *a, const void *b) {
    const TuneResult *ra = (const TuneResult *)a;
    const TuneResult *rb = (const TuneResult *)b;
    if (ra->composite_score < rb->composite_score) return -1;
    if (ra->composite_score > rb->composite_score) return 1;
    return 0;
}

/* ---- JSON Output ---- */

static void print_tune_params_json(FILE *fp, const SGTuneParams *p) {
    fprintf(fp, "{");
    const char *sep = "";
#define JP_D(name, fmt) do { \
    if (p->name != SG_TUNE_SENTINEL_D) { \
        fprintf(fp, "%s\"" #name "\":" fmt, sep, p->name); sep = ","; \
    } } while(0)
#define JP_I(name) do { \
    if (p->name != SG_TUNE_SENTINEL_I) { \
        fprintf(fp, "%s\"" #name "\":%d", sep, p->name); sep = ","; \
    } } while(0)
    JP_D(phase1_fraction, "%.4f");
    JP_I(phase15_iters);
    JP_D(sa_accept_pct, "%.6f");
    JP_D(p1_final_temp_ratio, "%.6f");
    JP_D(p2_final_temp_ratio, "%.6f");
    JP_D(pen_target_start, "%.4f");
    JP_D(pen_target_end, "%.4f");
    JP_D(pen_tolerance, "%.4f");
    JP_D(pen_increase, "%.4f");
    JP_D(pen_decrease, "%.4f");
    JP_D(pen_p15_target, "%.4f");
    JP_D(pen_p15_tolerance, "%.4f");
    JP_D(pen_p15_increase, "%.4f");
    JP_D(pen_p15_decrease, "%.4f");
    JP_D(reaction_factor, "%.4f");
    JP_D(reward_best, "%.2f");
    JP_D(reward_better, "%.2f");
    JP_D(reward_accepted, "%.2f");
    JP_I(segment_size);
    JP_D(worst_randomness, "%.2f");
    JP_D(shaw_randomness, "%.2f");
    JP_D(route_cluster_randomness, "%.2f");
    JP_D(time_cluster_randomness, "%.2f");
    JP_D(pd_shaw_randomness, "%.2f");
    JP_D(route_shaw_randomness, "%.2f");
    JP_I(string_l_max);
#undef JP_D
#undef JP_I
    fprintf(fp, "}");
}

static void print_result_json(FILE *fp, const TuneResult *r, int rank) {
    fprintf(fp, "    {\n");
    fprintf(fp, "      \"rank\": %d,\n", rank);
    fprintf(fp, "      \"composite_score\": %.4f,\n", r->composite_score);
    fprintf(fp, "      \"avg_vehicle_gap\": %.4f,\n", r->avg_vehicle_gap);
    fprintf(fp, "      \"avg_distance_gap_pct\": %.4f,\n", r->avg_distance_gap_pct);
    fprintf(fp, "      \"worst_vehicle_gap\": %.1f,\n", r->worst_vehicle_gap);
    fprintf(fp, "      \"total_time_s\": %.2f,\n", r->total_time_seconds);
    fprintf(fp, "      \"params\": ");
    print_tune_params_json(fp, &r->params);
    fprintf(fp, "\n    }");
}

/* ---- Tier Definitions ---- */

#define MAX_CONFIGS 4096

/*
 * Tier 0: Phase budget split (phase1_fraction x phase15_iters)
 */
static int generate_tier0(SGTuneParams *configs, const SGTuneParams *base) {
    double p1f_grid[5];
    int p15_grid[] = {100, 250, 500, 1000, 2000};
    int count = 0;
    int i, j;

    lin_grid(0.40, 0.80, 5, p1f_grid);

    for (i = 0; i < 5 && count < MAX_CONFIGS; i++) {
        for (j = 0; j < 5 && count < MAX_CONFIGS; j++) {
            configs[count] = *base;
            configs[count].phase1_fraction = p1f_grid[i];
            configs[count].phase15_iters = p15_grid[j];
            count++;
        }
    }
    return count;
}

/*
 * Tier 1: SA temperature (sa_accept_pct, p1_final_temp_ratio, p2_final_temp_ratio)
 */
static int generate_tier1(SGTuneParams *configs, const SGTuneParams *base) {
    double sa_grid[4], p1f_grid[5], p2f_grid[4];
    int count = 0;
    int i, j, k;

    log_grid(0.01, 0.20, 4, sa_grid);
    log_grid(0.005, 0.20, 5, p1f_grid);
    log_grid(0.0001, 0.05, 4, p2f_grid);

    /* Latin-hypercube-like: ~80 combos */
    for (i = 0; i < 4 && count < MAX_CONFIGS; i++) {
        for (j = 0; j < 5 && count < MAX_CONFIGS; j++) {
            for (k = 0; k < 4 && count < MAX_CONFIGS; k++) {
                configs[count] = *base;
                configs[count].sa_accept_pct = sa_grid[i];
                configs[count].p1_final_temp_ratio = p1f_grid[j];
                configs[count].p2_final_temp_ratio = p2f_grid[k];
                count++;
            }
        }
    }
    return count;
}

/*
 * Tier 2: Penalty weights
 */
static int generate_tier2(SGTuneParams *configs, const SGTuneParams *base) {
    double ts_grid[4], te_grid[4], tol_grid[3], inc_grid[3], dec_grid[3];
    int count = 0;
    int a, b, c, d, e;

    lin_grid(0.10, 0.50, 4, ts_grid);
    lin_grid(0.05, 0.30, 4, te_grid);
    lin_grid(0.02, 0.15, 3, tol_grid);
    log_grid(1.05, 2.0, 3, inc_grid);
    lin_grid(0.50, 0.95, 3, dec_grid);

    /* Structured subsample: ~100 combos */
    for (a = 0; a < 4 && count < MAX_CONFIGS; a++) {
        for (b = 0; b < 4 && count < MAX_CONFIGS; b++) {
            if (te_grid[b] >= ts_grid[a]) continue; /* end < start required */
            for (c = 0; c < 3 && count < MAX_CONFIGS; c++) {
                for (d = 0; d < 3 && count < MAX_CONFIGS; d++) {
                    for (e = 0; e < 3 && count < MAX_CONFIGS; e++) {
                        configs[count] = *base;
                        configs[count].pen_target_start = ts_grid[a];
                        configs[count].pen_target_end = te_grid[b];
                        configs[count].pen_tolerance = tol_grid[c];
                        configs[count].pen_increase = inc_grid[d];
                        configs[count].pen_decrease = dec_grid[e];
                        count++;
                    }
                }
            }
        }
    }
    return count;
}

/*
 * Tier 3: ALNS reward weights
 */
static int generate_tier3(SGTuneParams *configs, const SGTuneParams *base) {
    double rf_grid[4], rb_grid[4], rbt_grid[4], ra_grid[4];
    int seg_grid[] = {50, 100, 200, 400};
    int count = 0;
    int i, j, k, l;

    log_grid(0.01, 0.5, 4, rf_grid);
    log_grid(3.0, 50.0, 4, rb_grid);
    log_grid(1.0, 20.0, 4, rbt_grid);
    log_grid(0.5, 10.0, 4, ra_grid);

    /* Sample ~80 combos with segment_size rotation */
    for (i = 0; i < 4 && count < MAX_CONFIGS; i++) {
        for (j = 0; j < 4 && count < MAX_CONFIGS; j++) {
            for (k = 0; k < 4 && count < MAX_CONFIGS; k++) {
                for (l = 0; l < 4 && count < MAX_CONFIGS; l++) {
                    configs[count] = *base;
                    configs[count].reaction_factor = rf_grid[i];
                    configs[count].reward_best = rb_grid[j];
                    configs[count].reward_better = rbt_grid[k];
                    configs[count].reward_accepted = ra_grid[l];
                    /* Rotate segment_size based on combo index */
                    configs[count].segment_size = seg_grid[count % 4];
                    count++;
                }
            }
        }
    }
    return count;
}

/*
 * Tier 4: Destruction sizing (worst_randomness, shaw_randomness, string_l_max)
 */
static int generate_tier4(SGTuneParams *configs, const SGTuneParams *base) {
    double wr_grid[4], sr_grid[4];
    int sl_grid[] = {4, 8, 14, 20};
    int count = 0;
    int i, j, k;

    log_grid(1.0, 10.0, 4, wr_grid);
    log_grid(1.0, 10.0, 4, sr_grid);

    for (i = 0; i < 4 && count < MAX_CONFIGS; i++) {
        for (j = 0; j < 4 && count < MAX_CONFIGS; j++) {
            for (k = 0; k < 4 && count < MAX_CONFIGS; k++) {
                configs[count] = *base;
                configs[count].worst_randomness = wr_grid[i];
                configs[count].shaw_randomness = sr_grid[j];
                configs[count].string_l_max = sl_grid[k];
                count++;
            }
        }
    }
    return count;
}

/*
 * Tier 5: Extended randomness (route_cluster, time_cluster, pd_shaw, route_shaw)
 */
static int generate_tier5(SGTuneParams *configs, const SGTuneParams *base) {
    double rc_grid[3], tc_grid[3], pd_grid[3], rs_grid[3];
    int count = 0;
    int a, b, c, d;

    log_grid(1.0, 10.0, 3, rc_grid);
    log_grid(1.0, 10.0, 3, tc_grid);
    log_grid(1.0, 10.0, 3, pd_grid);
    log_grid(1.0, 10.0, 3, rs_grid);

    for (a = 0; a < 3 && count < MAX_CONFIGS; a++) {
        for (b = 0; b < 3 && count < MAX_CONFIGS; b++) {
            for (c = 0; c < 3 && count < MAX_CONFIGS; c++) {
                for (d = 0; d < 3 && count < MAX_CONFIGS; d++) {
                    configs[count] = *base;
                    configs[count].route_cluster_randomness = rc_grid[a];
                    configs[count].time_cluster_randomness = tc_grid[b];
                    configs[count].pd_shaw_randomness = pd_grid[c];
                    configs[count].route_shaw_randomness = rs_grid[d];
                    count++;
                }
            }
        }
    }
    return count;
}

/* ---- Multi-Seed Verification ---- */

static void verify_top_results(const TuneInstance *instances, int num_instances,
                               TuneResult *results, int top_n,
                               int max_iterations, int num_threads) {
    uint64_t seeds[] = {42, 123, 456, 789};
    int num_seeds = (int)(sizeof(seeds) / sizeof(seeds[0]));
    int i, s;

    fprintf(stderr, "\nVerifying top %d configurations with %d seeds...\n",
            top_n, num_seeds);

    for (i = 0; i < top_n; i++) {
        double total_score = 0.0;
        for (s = 0; s < num_seeds; s++) {
            TuneResult verify;
            evaluate_config(instances, num_instances, &results[i].params,
                            max_iterations, seeds[s], &verify);
            total_score += verify.composite_score;
        }
        /* Replace score with multi-seed average */
        results[i].composite_score = total_score / num_seeds;
    }

    /* Re-sort after verification */
    qsort(results, (size_t)top_n, sizeof(TuneResult), compare_results);
    (void)num_threads;
}

/* ---- CLI ---- */

static void print_usage(const char *argv0) {
    printf("Usage: %s [options]\n\n", argv0);
    printf("Hyperparameter tuner for Surge VRP solver.\n\n");
    printf("Options:\n");
    printf("  --tier <0-5>          Tune specific tier (default: 0)\n");
    printf("  --all-tiers           Tune all tiers sequentially (carry best forward)\n");
    printf("  --iterations <n>      ALNS iterations per instance (default: 2500)\n");
    printf("  --seed <n>            Base seed (default: 42)\n");
    printf("  --threads <n>         Parallel config evaluations (default: 4)\n");
    printf("  --top <n>             Report top N configs (default: 10)\n");
    printf("  --verify <n>          Multi-seed verify top N (default: 3)\n");
    printf("  --json                Output JSON results to stdout\n");
    printf("  --baseline            Run defaults first for comparison\n");
    printf("  --solomon-dir <p>     Path to Solomon instances (overrides representative set)\n");
    printf("  --li-lim-dir <p>      Path to Li-Lim instances (overrides representative set)\n");
    printf("  --help                Show this help\n");
    printf("\nTiers (tuned in order of impact):\n");
    printf("  0: Phase budget split (phase1_fraction, phase15_iters)    25 configs\n");
    printf("  1: SA temperature (sa_accept_pct, final_temp_ratios)     80 configs\n");
    printf("  2: Penalty weights (targets, tolerance, inc/dec)        ~100 configs\n");
    printf("  3: ALNS rewards (reaction, reward_best/better/accepted) ~256 configs\n");
    printf("  4: Destruction sizing (worst/shaw randomness, string_l)   64 configs\n");
    printf("  5: Extended randomness (route_cluster, time, pd, route)   81 configs\n");
}

int main(int argc, char **argv) {
    int tier = 0;
    int all_tiers = 0;
    int max_iterations = 2500;
    uint64_t seed = 42;
    int num_threads = 4;
    int top_n = 10;
    int verify_n = 3;
    int json_output = 0;
    int run_baseline = 0;
    int i;

    SGTuneParams base_params;
    SGTuneParams *configs = NULL;
    TuneResult *results = NULL;
    TuneResult baseline = {0};
    int num_configs = 0;

    /* Parse CLI */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--tier") == 0 && i + 1 < argc) {
            tier = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--all-tiers") == 0) {
            all_tiers = 1;
        } else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            max_iterations = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            num_threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
            top_n = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--verify") == 0 && i + 1 < argc) {
            verify_n = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--json") == 0) {
            json_output = 1;
        } else if (strcmp(argv[i], "--baseline") == 0) {
            run_baseline = 1;
        } else {
            fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (tier < 0 || tier > 5) {
        fprintf(stderr, "Error: tier must be 0-5\n");
        return 1;
    }
    if (num_threads < 1) num_threads = 1;
    if (top_n < 1) top_n = 1;
    if (verify_n < 0) verify_n = 0;

    configs = (SGTuneParams *)calloc(MAX_CONFIGS, sizeof(*configs));
    results = (TuneResult *)calloc(MAX_CONFIGS, sizeof(*results));
    if (!configs || !results) {
        fprintf(stderr, "Error: out of memory\n");
        free(configs);
        free(results);
        return 1;
    }

    /* Initialize base params to all sentinels (= use defaults) */
    sg_tune_params_default(&base_params);

    /* Baseline evaluation */
    if (run_baseline) {
        fprintf(stderr, "Evaluating baseline (default params)...\n");
        evaluate_config(k_representative, NUM_REPRESENTATIVE, &base_params,
                        max_iterations, seed, &baseline);
        fprintf(stderr, "  Baseline score: %.4f (V-gap: %.2f, D-gap: %.2f%%, worst-V: %.0f)\n",
                baseline.composite_score, baseline.avg_vehicle_gap,
                baseline.avg_distance_gap_pct, baseline.worst_vehicle_gap);
    }

    if (all_tiers) {
        /* Run all tiers sequentially, carrying best forward */
        int t;
        for (t = 0; t <= 5; t++) {
            fprintf(stderr, "\n=== Tier %d ===\n", t);

            switch (t) {
                case 0: num_configs = generate_tier0(configs, &base_params); break;
                case 1: num_configs = generate_tier1(configs, &base_params); break;
                case 2: num_configs = generate_tier2(configs, &base_params); break;
                case 3: num_configs = generate_tier3(configs, &base_params); break;
                case 4: num_configs = generate_tier4(configs, &base_params); break;
                case 5: num_configs = generate_tier5(configs, &base_params); break;
                default: num_configs = 0; break;
            }

            fprintf(stderr, "  %d configurations to evaluate\n", num_configs);
            evaluate_configs_parallel(k_representative, NUM_REPRESENTATIVE,
                                     configs, num_configs, max_iterations,
                                     seed, num_threads, results);

            qsort(results, (size_t)num_configs, sizeof(TuneResult), compare_results);

            /* Carry best forward as base for next tier */
            base_params = results[0].params;
            fprintf(stderr, "  Best score: %.4f (V-gap: %.2f, D-gap: %.2f%%)\n",
                    results[0].composite_score, results[0].avg_vehicle_gap,
                    results[0].avg_distance_gap_pct);
        }

        /* Verify final top results */
        if (verify_n > 0 && verify_n <= num_configs) {
            verify_top_results(k_representative, NUM_REPRESENTATIVE,
                               results, verify_n, max_iterations, num_threads);
        }

    } else {
        /* Single tier */
        fprintf(stderr, "=== Tier %d ===\n", tier);

        switch (tier) {
            case 0: num_configs = generate_tier0(configs, &base_params); break;
            case 1: num_configs = generate_tier1(configs, &base_params); break;
            case 2: num_configs = generate_tier2(configs, &base_params); break;
            case 3: num_configs = generate_tier3(configs, &base_params); break;
            case 4: num_configs = generate_tier4(configs, &base_params); break;
            case 5: num_configs = generate_tier5(configs, &base_params); break;
            default: num_configs = 0; break;
        }

        fprintf(stderr, "%d configurations to evaluate\n", num_configs);
        evaluate_configs_parallel(k_representative, NUM_REPRESENTATIVE,
                                 configs, num_configs, max_iterations,
                                 seed, num_threads, results);

        qsort(results, (size_t)num_configs, sizeof(TuneResult), compare_results);

        /* Multi-seed verification */
        if (verify_n > 0 && verify_n <= num_configs) {
            verify_top_results(k_representative, NUM_REPRESENTATIVE,
                               results, verify_n, max_iterations, num_threads);
        }
    }

    /* Output results */
    if (json_output) {
        int n = top_n < num_configs ? top_n : num_configs;
        printf("{\n");
        printf("  \"tier\": %d,\n", all_tiers ? -1 : tier);
        printf("  \"iterations\": %d,\n", max_iterations);
        printf("  \"seed\": %llu,\n", (unsigned long long)seed);
        printf("  \"num_configs_evaluated\": %d,\n", num_configs);
        printf("  \"num_instances\": %d,\n", NUM_REPRESENTATIVE);
        if (run_baseline) {
            printf("  \"baseline\": {\n");
            printf("    \"composite_score\": %.4f,\n", baseline.composite_score);
            printf("    \"avg_vehicle_gap\": %.4f,\n", baseline.avg_vehicle_gap);
            printf("    \"avg_distance_gap_pct\": %.4f,\n", baseline.avg_distance_gap_pct);
            printf("    \"worst_vehicle_gap\": %.1f,\n", baseline.worst_vehicle_gap);
            printf("    \"total_time_s\": %.2f\n", baseline.total_time_seconds);
            printf("  },\n");
        }
        printf("  \"results\": [\n");
        for (i = 0; i < n; i++) {
            print_result_json(stdout, &results[i], i + 1);
            if (i + 1 < n) printf(",");
            printf("\n");
        }
        printf("  ]\n");
        printf("}\n");
    } else {
        /* Table output */
        int n = top_n < num_configs ? top_n : num_configs;
        printf("\n");
        if (run_baseline) {
            printf("Baseline: score=%.4f  V-gap=%.2f  D-gap=%.2f%%  worst-V=%.0f  time=%.1fs\n\n",
                   baseline.composite_score, baseline.avg_vehicle_gap,
                   baseline.avg_distance_gap_pct, baseline.worst_vehicle_gap,
                   baseline.total_time_seconds);
        }
        printf("Top %d configurations (tier %d, %d evaluated):\n", n,
               all_tiers ? -1 : tier, num_configs);
        printf("%-4s  %-10s  %-8s  %-10s  %-8s  %-8s\n",
               "Rank", "Score", "V-Gap", "D-Gap(%)", "W-V", "Time(s)");
        printf("----  ----------  --------  ----------  --------  --------\n");
        for (i = 0; i < n; i++) {
            printf("%-4d  %-10.4f  %-8.2f  %-10.2f  %-8.0f  %-8.1f\n",
                   i + 1,
                   results[i].composite_score,
                   results[i].avg_vehicle_gap,
                   results[i].avg_distance_gap_pct,
                   results[i].worst_vehicle_gap,
                   results[i].total_time_seconds);
        }
    }

    free(configs);
    free(results);
    return 0;
}
