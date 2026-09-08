/*
 * bench_tune.c - Hyperparameter tuner for Surge VRP solver
 *
 * Evaluates configurations across a representative instance set,
 * using tiered parameter search (coarse grid -> refinement).
 * Results are written as JSON for analysis.
 *
 * Checkpoint/resume: evaluations are written to a JSONL checkpoint
 * file as they complete. On restart with --checkpoint <file>, already-
 * evaluated configs are skipped. The JSONL is also ML-ready training
 * data: each line has full params + per-instance scores.
 *
 * Build: make bench-tune
 * Usage: bench_tune --tier 0 --iterations 2500 --json
 *        bench_tune --all-tiers --threads 4 --checkpoint tune.jsonl
 *        bench_tune --baseline
 */
#include "surge.h"
#include "../src/sg_profile_matrix.h"
#include "sg_bench_utils.h"
#include "sh_args.h"
#include "sh_json.h"
#include "sh_arena.h"

#include <math.h>
#include "sh_pal.h"
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

#define MAX_TUNE_INSTANCES 64

typedef struct {
    SGTuneParams params;
    double avg_vehicle_gap;
    double avg_distance_gap_pct;
    double worst_vehicle_gap;
    double composite_score;      /* lower = better */
    double total_time_seconds;
    int valid;
    /* Per-instance breakdown (for ML training data in checkpoint) */
    int num_instances;
    double inst_vgap[MAX_TUNE_INSTANCES];
    double inst_dgap[MAX_TUNE_INSTANCES];
} TuneResult;

static double evaluate_instance(const TuneInstance *inst, const SGTuneParams *params,
                                int max_iterations, int max_time_seconds,
                                uint64_t seed,
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
    cfg.max_time_seconds = max_time_seconds;
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
                            int max_time_seconds, uint64_t seed,
                            TuneResult *result) {
    double sum_vgap = 0.0, sum_dgap = 0.0;
    double worst_vgap = -1e9;
    double total_time = 0.0;
    int i;

    result->params = *params;
    result->valid = 1;
    result->num_instances = num_instances < MAX_TUNE_INSTANCES ? num_instances : MAX_TUNE_INSTANCES;

    for (i = 0; i < num_instances; i++) {
        double vgap, dgap;
        double t = evaluate_instance(&instances[i], params, max_iterations,
                                     max_time_seconds, seed, &vgap, &dgap);
        if (i < MAX_TUNE_INSTANCES) {
            result->inst_vgap[i] = vgap;
            result->inst_dgap[i] = dgap;
        }
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

/* Forward declarations */
static void print_tune_params_json(FILE *fp, const SGTuneParams *p);

/* ---- Checkpoint (JSONL) ---- */

/*
 * Checkpoint format: one JSON object per line (JSONL).
 *
 * Line types:
 *   {"type":"header","iterations":2500,"seed":42}
 *   {"type":"baseline","score":123.4,"avg_vgap":0.5,"avg_dgap":12.3,...}
 *   {"type":"result","tier":0,"idx":3,"score":110.2,...,"params":{...},"instances":[...]}
 *   {"type":"tier_done","tier":0,"best_params":{...}}
 *
 * The "instances" array in result lines contains per-instance {vgap, dgap}
 * pairs — this is ML training data for surrogate model fitting.
 */

typedef struct {
    FILE *fp;                  /* Append handle (NULL = no checkpoint) */
    ShMutex mutex;             /* Protects fp writes */
    /* Loaded state from existing checkpoint */
    int tier_done[7];          /* 1 = tier fully completed */
    SGTuneParams tier_best[7]; /* Best params from each completed tier */
    int has_baseline;
    TuneResult baseline;
    /* Cached results: flat array of (tier, idx, result) triples */
    int *cached_tier;
    int *cached_idx;
    TuneResult *cached_results;
    int cached_count;
    int cached_cap;
} CheckpointState;

static void cp_init(CheckpointState *cp) {
    memset(cp, 0, sizeof(*cp));
    sh_mutex_init(&cp->mutex);
    for (int i = 0; i < 7; i++)
        sg_tune_params_default(&cp->tier_best[i]);
}

static void cp_free(CheckpointState *cp) {
    if (cp->fp) fclose(cp->fp);
    free(cp->cached_tier);
    free(cp->cached_idx);
    free(cp->cached_results);
    sh_mutex_destroy(&cp->mutex);
}

/* Write one JSONL line for a result (thread-safe) */
static void cp_write_result(CheckpointState *cp, int tier, int idx,
                            const TuneResult *r, const TuneInstance *instances) {
    if (!cp->fp) return;
    sh_mutex_lock(&cp->mutex);

    fprintf(cp->fp, "{\"type\":\"result\",\"tier\":%d,\"idx\":%d,"
            "\"score\":%.6f,\"avg_vgap\":%.6f,\"avg_dgap\":%.6f,"
            "\"worst_vgap\":%.2f,\"time_s\":%.3f,\"params\":",
            tier, idx, r->composite_score, r->avg_vehicle_gap,
            r->avg_distance_gap_pct, r->worst_vehicle_gap, r->total_time_seconds);
    print_tune_params_json(cp->fp, &r->params);
    fprintf(cp->fp, ",\"instances\":[");
    for (int i = 0; i < r->num_instances; i++) {
        if (i > 0) fprintf(cp->fp, ",");
        fprintf(cp->fp, "{\"name\":\"%s\",\"vgap\":%.4f,\"dgap\":%.4f}",
                instances[i].name, r->inst_vgap[i], r->inst_dgap[i]);
    }
    fprintf(cp->fp, "]}\n");
    fflush(cp->fp);

    sh_mutex_unlock(&cp->mutex);
}

static void cp_write_baseline(CheckpointState *cp, const TuneResult *r) {
    if (!cp->fp) return;
    fprintf(cp->fp, "{\"type\":\"baseline\",\"score\":%.6f,"
            "\"avg_vgap\":%.6f,\"avg_dgap\":%.6f,\"worst_vgap\":%.2f,"
            "\"time_s\":%.3f}\n",
            r->composite_score, r->avg_vehicle_gap,
            r->avg_distance_gap_pct, r->worst_vehicle_gap, r->total_time_seconds);
    fflush(cp->fp);
}

static void cp_write_tier_done(CheckpointState *cp, int tier, const SGTuneParams *best) {
    if (!cp->fp) return;
    fprintf(cp->fp, "{\"type\":\"tier_done\",\"tier\":%d,\"best_params\":", tier);
    print_tune_params_json(cp->fp, best);
    fprintf(cp->fp, "}\n");
    fflush(cp->fp);
}

static void cp_write_header(CheckpointState *cp, int iterations, uint64_t seed) {
    if (!cp->fp) return;
    fprintf(cp->fp, "{\"type\":\"header\",\"iterations\":%d,\"seed\":%llu}\n",
            iterations, (unsigned long long)seed);
    fflush(cp->fp);
}

/* Parse SGTuneParams from a ShJsonValue object */
static void cp_parse_params(ShJsonValue *obj, SGTuneParams *p) {
    sg_tune_params_default(p);
    if (!obj) return;
#define CP_D(name) do { \
    ShJsonValue *v = sh_json_get(obj, #name); \
    if (v) p->name = sh_json_as_double(v, SG_TUNE_SENTINEL_D); \
    } while(0)
#define CP_I(name) do { \
    ShJsonValue *v = sh_json_get(obj, #name); \
    if (v) p->name = sh_json_as_int(v, SG_TUNE_SENTINEL_I); \
    } while(0)
    CP_D(phase1_fraction); CP_I(phase15_iters);
    CP_D(sa_accept_pct); CP_D(p1_final_temp_ratio); CP_D(p2_final_temp_ratio);
    CP_D(pen_target_start); CP_D(pen_target_end); CP_D(pen_tolerance);
    CP_D(pen_increase); CP_D(pen_decrease);
    CP_D(pen_p15_target); CP_D(pen_p15_tolerance); CP_D(pen_p15_increase); CP_D(pen_p15_decrease);
    CP_D(reaction_factor); CP_D(reward_best); CP_D(reward_better); CP_D(reward_accepted);
    CP_I(segment_size);
    CP_D(worst_randomness); CP_D(shaw_randomness); CP_D(route_cluster_randomness);
    CP_D(time_cluster_randomness); CP_D(pd_shaw_randomness); CP_D(route_shaw_randomness);
    CP_I(string_l_max);
    CP_I(neighbor_k);
    CP_D(gen_reheat_ratio); CP_D(gen_cooling_stretch);
#undef CP_D
#undef CP_I
}

/* Add a cached result to the checkpoint state */
static void cp_cache_add(CheckpointState *cp, int tier, int idx, const TuneResult *r) {
    if (cp->cached_count >= cp->cached_cap) {
        int new_cap = cp->cached_cap ? cp->cached_cap * 2 : 256;
        cp->cached_tier = realloc(cp->cached_tier, (size_t)new_cap * sizeof(int));
        cp->cached_idx = realloc(cp->cached_idx, (size_t)new_cap * sizeof(int));
        cp->cached_results = realloc(cp->cached_results, (size_t)new_cap * sizeof(TuneResult));
        cp->cached_cap = new_cap;
    }
    cp->cached_tier[cp->cached_count] = tier;
    cp->cached_idx[cp->cached_count] = idx;
    cp->cached_results[cp->cached_count] = *r;
    cp->cached_count++;
}

/* Load existing checkpoint file. Returns number of entries loaded. */
static int cp_load(CheckpointState *cp, const char *path) {
    FILE *f = fopen(path, "r");
    char line[16384];
    int loaded = 0;

    if (!f) return 0;

    while (fgets(line, (int)sizeof(line), f)) {
        size_t len = strlen(line);
        if (len < 2) continue;

        SHArena *arena = sh_arena_create(len * 4 + 4096);
        if (!arena) continue;

        ShJsonValue *root = NULL;
        if (sh_json_parse(line, len, arena, &root) != SH_JSON_OK || !root) {
            sh_arena_free(arena);
            continue;
        }

        const char *type = sh_json_as_string(sh_json_get(root, "type"), "");

        if (strcmp(type, "result") == 0) {
            TuneResult r;
            memset(&r, 0, sizeof(r));
            int tier = sh_json_as_int(sh_json_get(root, "tier"), -1);
            int idx = sh_json_as_int(sh_json_get(root, "idx"), -1);
            r.composite_score = sh_json_as_double(sh_json_get(root, "score"), 1e9);
            r.avg_vehicle_gap = sh_json_as_double(sh_json_get(root, "avg_vgap"), 100.0);
            r.avg_distance_gap_pct = sh_json_as_double(sh_json_get(root, "avg_dgap"), 100.0);
            r.worst_vehicle_gap = sh_json_as_double(sh_json_get(root, "worst_vgap"), 100.0);
            r.total_time_seconds = sh_json_as_double(sh_json_get(root, "time_s"), 0.0);
            r.valid = 1;
            cp_parse_params(sh_json_get(root, "params"), &r.params);

            /* Parse per-instance data */
            ShJsonValue *inst_arr = sh_json_get(root, "instances");
            r.num_instances = (int)sh_json_array_len(inst_arr);
            if (r.num_instances > MAX_TUNE_INSTANCES) r.num_instances = MAX_TUNE_INSTANCES;
            for (int i = 0; i < r.num_instances; i++) {
                ShJsonValue *item = sh_json_array_get(inst_arr, (size_t)i);
                r.inst_vgap[i] = sh_json_as_double(sh_json_get(item, "vgap"), 0.0);
                r.inst_dgap[i] = sh_json_as_double(sh_json_get(item, "dgap"), 0.0);
            }

            if (tier >= 0 && idx >= 0) {
                cp_cache_add(cp, tier, idx, &r);
                loaded++;
            }

        } else if (strcmp(type, "baseline") == 0) {
            cp->has_baseline = 1;
            memset(&cp->baseline, 0, sizeof(cp->baseline));
            cp->baseline.composite_score = sh_json_as_double(sh_json_get(root, "score"), 1e9);
            cp->baseline.avg_vehicle_gap = sh_json_as_double(sh_json_get(root, "avg_vgap"), 0.0);
            cp->baseline.avg_distance_gap_pct = sh_json_as_double(sh_json_get(root, "avg_dgap"), 0.0);
            cp->baseline.worst_vehicle_gap = sh_json_as_double(sh_json_get(root, "worst_vgap"), 0.0);
            cp->baseline.total_time_seconds = sh_json_as_double(sh_json_get(root, "time_s"), 0.0);
            cp->baseline.valid = 1;

        } else if (strcmp(type, "tier_done") == 0) {
            int tier = sh_json_as_int(sh_json_get(root, "tier"), -1);
            if (tier >= 0 && tier < 7) {
                cp->tier_done[tier] = 1;
                cp_parse_params(sh_json_get(root, "best_params"), &cp->tier_best[tier]);
            }
        }

        sh_arena_free(arena);
    }

    fclose(f);
    return loaded;
}

/* Pre-fill results array with cached checkpoint data for a given tier.
 * Returns the number of configs that were pre-filled (i.e., can be skipped). */
static int cp_prefill(const CheckpointState *cp, int tier,
                      TuneResult *results, int num_configs) {
    int filled = 0;
    for (int c = 0; c < cp->cached_count; c++) {
        if (cp->cached_tier[c] == tier) {
            int idx = cp->cached_idx[c];
            if (idx >= 0 && idx < num_configs && !results[idx].valid) {
                results[idx] = cp->cached_results[c];
                filled++;
            }
        }
    }
    return filled;
}

/* ---- Parallel Evaluation ---- */

typedef struct {
    const TuneInstance *instances;
    int num_instances;
    int max_iterations;
    int max_time_seconds;
    uint64_t seed;
    SGTuneParams *configs;   /* array of configurations */
    TuneResult *results;     /* output array */
    int total_configs;
    volatile int next_config; /* work counter */
    int done_count;           /* completed counter (cached + evaluated) */
    int cached_count;         /* pre-filled from checkpoint */
    int current_tier;
    CheckpointState *checkpoint;
    ShMutex mutex;
} TuneWorkContext;

static void *tune_worker(void *arg) {
    TuneWorkContext *wctx = (TuneWorkContext *)arg;

    for (;;) {
        int idx;
        sh_mutex_lock(&wctx->mutex);
        idx = wctx->next_config++;
        sh_mutex_unlock(&wctx->mutex);

        if (idx >= wctx->total_configs) break;

        /* Skip if already loaded from checkpoint */
        if (wctx->results[idx].valid) {
            sh_mutex_lock(&wctx->mutex);
            wctx->done_count++;
            sh_mutex_unlock(&wctx->mutex);
            continue;
        }

        evaluate_config(wctx->instances, wctx->num_instances,
                        &wctx->configs[idx], wctx->max_iterations,
                        wctx->max_time_seconds, wctx->seed,
                        &wctx->results[idx]);

        /* Write checkpoint line */
        if (wctx->checkpoint) {
            cp_write_result(wctx->checkpoint, wctx->current_tier, idx,
                            &wctx->results[idx], wctx->instances);
        }

        sh_mutex_lock(&wctx->mutex);
        wctx->done_count++;
        if (wctx->cached_count > 0) {
            fprintf(stderr, "\r  [%d/%d] evaluated (%d cached)",
                    wctx->done_count, wctx->total_configs, wctx->cached_count);
        } else {
            fprintf(stderr, "\r  [%d/%d] configs evaluated",
                    wctx->done_count, wctx->total_configs);
        }
        sh_mutex_unlock(&wctx->mutex);
    }
    return NULL;
}

static void evaluate_configs_parallel(const TuneInstance *instances, int num_instances,
                                      SGTuneParams *configs, int num_configs,
                                      int max_iterations, int max_time_seconds,
                                      uint64_t seed,
                                      int num_threads, TuneResult *results,
                                      int current_tier, CheckpointState *checkpoint) {
    TuneWorkContext wctx;
    ShThread *threads;
    int i;
    int cached = 0;

    /* Pre-fill from checkpoint */
    if (checkpoint) {
        cached = cp_prefill(checkpoint, current_tier, results, num_configs);
        if (cached > 0) {
            fprintf(stderr, "  %d/%d configs restored from checkpoint\n",
                    cached, num_configs);
        }
        if (cached >= num_configs) {
            fprintf(stderr, "  All configs cached, skipping evaluation\n");
            return;
        }
    }

    wctx.instances = instances;
    wctx.num_instances = num_instances;
    wctx.max_iterations = max_iterations;
    wctx.max_time_seconds = max_time_seconds;
    wctx.seed = seed;
    wctx.configs = configs;
    wctx.results = results;
    wctx.total_configs = num_configs;
    wctx.next_config = 0;
    wctx.done_count = 0;
    wctx.cached_count = cached;
    wctx.current_tier = current_tier;
    wctx.checkpoint = checkpoint;
    sh_mutex_init(&wctx.mutex);

    threads = (ShThread *)calloc((size_t)num_threads, sizeof(*threads));
    if (!threads) {
        /* Fallback to single-threaded */
        for (i = 0; i < num_configs; i++) {
            if (results[i].valid) continue; /* Skip cached */
            evaluate_config(instances, num_instances, &configs[i],
                            max_iterations, max_time_seconds, seed,
                            &results[i]);
            if (checkpoint) {
                cp_write_result(checkpoint, current_tier, i,
                                &results[i], instances);
            }
            fprintf(stderr, "\r  [%d/%d] configs evaluated", i + 1, num_configs);
        }
        fprintf(stderr, "\n");
        return;
    }

    for (i = 0; i < num_threads; i++) {
        sh_thread_create(&threads[i], tune_worker, &wctx);
    }
    for (i = 0; i < num_threads; i++) {
        sh_thread_join(&threads[i], NULL);
    }
    fprintf(stderr, "\n");

    free(threads);
    sh_mutex_destroy(&wctx.mutex);
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
    JP_I(neighbor_k);
    JP_D(gen_reheat_ratio, "%.4f");
    JP_D(gen_cooling_stretch, "%.4f");
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

/*
 * Tier 6: Neighbor pruning k (insertion repair speedup vs quality tradeoff)
 */
static int generate_tier6(SGTuneParams *configs, const SGTuneParams *base) {
    int k_grid[] = {5, 8, 10, 15, 20, 25, 30, 40, 50};
    int n = (int)(sizeof(k_grid) / sizeof(k_grid[0]));
    int count = 0;
    int i;

    for (i = 0; i < n && count < MAX_CONFIGS; i++) {
        configs[count] = *base;
        configs[count].neighbor_k = k_grid[i];
        count++;
    }
    return count;
}

/* ---- Multi-Seed Verification ---- */

static void verify_top_results(const TuneInstance *instances, int num_instances,
                               TuneResult *results, int top_n,
                               int max_iterations, int max_time_seconds,
                               int num_threads) {
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
                            max_iterations, max_time_seconds, seeds[s],
                            &verify);
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
    printf("  --tier <0-6>          Tune specific tier (default: 0)\n");
    printf("  --all-tiers           Tune all tiers sequentially (carry best forward)\n");
    printf("  --iterations <n>      ALNS iterations per instance (default: 2500)\n");
    printf("  --time-limit <sec>    Wall-clock time limit per instance (0=none, default: 0)\n");
    printf("  --seed <n>            Base seed (default: 42)\n");
    printf("  --threads <n>         Parallel config evaluations (default: 4)\n");
    printf("  --top <n>             Report top N configs (default: 10)\n");
    printf("  --verify <n>          Multi-seed verify top N (default: 3)\n");
    printf("  --json                Output JSON results to stdout\n");
    printf("  --baseline            Run defaults first for comparison\n");
    printf("  --checkpoint <file>   Checkpoint file for resume (default: surge_tune.jsonl)\n");
    printf("  --no-checkpoint       Disable checkpointing\n");
    printf("  --dir <path>          Instance directory (overrides representative set)\n");
    printf("  --bks <csv>           BKS CSV file for custom instances\n");
    printf("  --size <n>            Filter instances by size (e.g., 400 for GH-400)\n");
    printf("  --loader <type>       Instance format: solomon (default) or li_lim\n");
    printf("  --profile <0-3>       Profile to tune (0=REALTIME..3=BEST)\n");
    printf("  --scale-size <N>      Request count for scale column selection\n");
    printf("  --max-instances <N>   Limit to N evenly-spaced instances (0=all)\n");
    printf("  --help                Show this help\n");
    printf("\nCheckpoint/Resume:\n");
    printf("  Results are saved to a JSONL checkpoint file as they complete.\n");
    printf("  On restart, already-evaluated configs are skipped automatically.\n");
    printf("  The JSONL file is also ML-ready training data for surrogate models.\n");
    printf("\nCustom Instance Sets:\n");
    printf("  --dir benchmarks/gehring_homberger --bks benchmarks/bks/gehring_homberger.csv --size 400\n");
    printf("  --dir benchmarks/li_lim_extended --bks benchmarks/bks/li_lim_extended.csv --size 400 --loader li_lim\n");
    printf("\nTiers (tuned in order of impact):\n");
    printf("  0: Phase budget split (phase1_fraction, phase15_iters)    25 configs\n");
    printf("  1: SA temperature (sa_accept_pct, final_temp_ratios)     80 configs\n");
    printf("  2: Penalty weights (targets, tolerance, inc/dec)        ~100 configs\n");
    printf("  3: ALNS rewards (reaction, reward_best/better/accepted) ~256 configs\n");
    printf("  4: Destruction sizing (worst/shaw randomness, string_l)   64 configs\n");
    printf("  5: Extended randomness (route_cluster, time, pd, route)   81 configs\n");
    printf("  6: Neighbor pruning k (insertion repair speed vs quality)   9 configs\n");
}

int main(int argc, char **argv) {
    int tier = 0;
    int all_tiers = 0;
    int max_iterations = 2500;
    int max_time_seconds = 0;
    uint64_t seed = 42;
    int num_threads = 4;
    int top_n = 10;
    int verify_n = 3;
    int json_output = 0;
    int run_baseline = 0;
    const char *checkpoint_path = "surge_tune.jsonl";
    int use_checkpoint = 1;
    const char *custom_dir = NULL;
    const char *bks_path = NULL;
    int custom_size = 0;
    int custom_loader = 0; /* 0=solomon, 1=li_lim */
    int profile_idx = -1;  /* -1 = not set; 0-3 = REALTIME..BEST */
    int scale_size = 0;    /* request count for scale column (0 = not set) */
    int max_instances = 0; /* 0 = no limit; >0 = evenly sample N instances */
    int i;

    SGTuneParams base_params;
    SGTuneParams *configs = NULL;
    TuneResult *results = NULL;
    TuneResult baseline = {0};
    int num_configs = 0;
    CheckpointState cp;

    /* Instance set (defaults to k_representative, overridden by --dir) */
    const TuneInstance *instances = k_representative;
    int num_instances = NUM_REPRESENTATIVE;
    TuneInstance *dynamic_instances = NULL;
    SGBenchCase *dyn_cases = NULL;
    int dyn_case_count = 0;

    cp_init(&cp);

    /* Parse CLI */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--tier") == 0 && i + 1 < argc) {
            tier = sh_parse_int(argv[++i], 0, 0, 6);
        } else if (strcmp(argv[i], "--all-tiers") == 0) {
            all_tiers = 1;
        } else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            max_iterations = sh_parse_int(argv[++i], 2500, 1, 1000000);
        } else if (strcmp(argv[i], "--time-limit") == 0 && i + 1 < argc) {
            max_time_seconds = sh_parse_int(argv[++i], 0, 0, 86400);
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = (uint64_t)strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            num_threads = sh_parse_int(argv[++i], 1, 1, 256);
        } else if (strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
            top_n = sh_parse_int(argv[++i], 10, 1, 10000);
        } else if (strcmp(argv[i], "--verify") == 0 && i + 1 < argc) {
            verify_n = sh_parse_int(argv[++i], 3, 0, 100);
        } else if (strcmp(argv[i], "--json") == 0) {
            json_output = 1;
        } else if (strcmp(argv[i], "--baseline") == 0) {
            run_baseline = 1;
        } else if (strcmp(argv[i], "--checkpoint") == 0 && i + 1 < argc) {
            checkpoint_path = argv[++i];
        } else if (strcmp(argv[i], "--no-checkpoint") == 0) {
            use_checkpoint = 0;
        } else if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            custom_dir = argv[++i];
        } else if (strcmp(argv[i], "--bks") == 0 && i + 1 < argc) {
            bks_path = argv[++i];
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            custom_size = sh_parse_int(argv[++i], 0, 0, 10000);
        } else if (strcmp(argv[i], "--loader") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "li_lim") == 0 || strcmp(argv[i], "pdptw") == 0)
                custom_loader = 1;
            else
                custom_loader = 0;
        } else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            profile_idx = sh_parse_int(argv[++i], -1, 0, 3);
        } else if (strcmp(argv[i], "--scale-size") == 0 && i + 1 < argc) {
            scale_size = sh_parse_int(argv[++i], 0, 1, 100000);
        } else if (strcmp(argv[i], "--max-instances") == 0 && i + 1 < argc) {
            max_instances = sh_parse_int(argv[++i], 0, 0, MAX_TUNE_INSTANCES);
        } else {
            fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (tier < 0 || tier > 6) {
        fprintf(stderr, "Error: tier must be 0-6\n");
        return 1;
    }
    if (num_threads < 1) num_threads = 1;
    if (top_n < 1) top_n = 1;
    if (verify_n < 0) verify_n = 0;

    /* Build custom instance set from --dir if specified */
    if (custom_dir) {
        SGBKSEntry bks_entries[2048];
        int num_bks = 0;
        int c;

        dyn_case_count = sg_collect_cases(custom_dir, custom_size, &dyn_cases);
        if (dyn_case_count <= 0) {
            fprintf(stderr, "Error: no instances found in %s", custom_dir);
            if (custom_size > 0) fprintf(stderr, " (size=%d)", custom_size);
            fprintf(stderr, "\n");
            cp_free(&cp);
            return 1;
        }

        qsort(dyn_cases, (size_t)dyn_case_count, sizeof(*dyn_cases),
              sg_compare_bench_cases);

        if (bks_path) {
            num_bks = sg_load_bks_csv(bks_path, bks_entries, 2048);
            if (num_bks < 0) {
                fprintf(stderr, "Warning: cannot load BKS from %s\n", bks_path);
                num_bks = 0;
            }
        }

        dynamic_instances = (TuneInstance *)calloc(
            (size_t)(dyn_case_count < MAX_TUNE_INSTANCES ? dyn_case_count : MAX_TUNE_INSTANCES),
            sizeof(TuneInstance));
        if (!dynamic_instances) {
            fprintf(stderr, "Error: out of memory\n");
            sg_free_bench_cases(dyn_cases, dyn_case_count);
            cp_free(&cp);
            return 1;
        }

        num_instances = 0;
        for (c = 0; c < dyn_case_count && num_instances < MAX_TUNE_INSTANCES; c++) {
            char key[64];
            const SGBKSEntry *bks;
            sg_bench_case_key(dyn_cases[c].name, key, sizeof(key));
            bks = sg_bks_find(bks_entries, num_bks, key);

            dynamic_instances[num_instances].path = dyn_cases[c].path;
            dynamic_instances[num_instances].name = dyn_cases[c].name;
            dynamic_instances[num_instances].loader = custom_loader;
            dynamic_instances[num_instances].bks_vehicles = bks ? bks->vehicles : 0;
            dynamic_instances[num_instances].bks_distance = bks ? bks->distance : 0.0;
            num_instances++;
        }
        instances = dynamic_instances;

        /* Subsample instances if --max-instances is set */
        if (max_instances > 0 && num_instances > max_instances) {
            int stride = num_instances / max_instances;
            int dst = 0;
            for (c = 0; dst < max_instances && c < num_instances; c += stride) {
                dynamic_instances[dst] = dynamic_instances[c];
                dst++;
            }
            fprintf(stderr, "Subsampled %d -> %d instances (stride=%d)\n",
                    num_instances, dst, stride);
            num_instances = dst;
        }

        fprintf(stderr, "Loaded %d instances from %s", num_instances, custom_dir);
        if (custom_size > 0) fprintf(stderr, " (size=%d)", custom_size);
        fprintf(stderr, "\n");
        if (num_bks > 0) {
            int matched = 0;
            for (c = 0; c < num_instances; c++) {
                if (instances[c].bks_vehicles > 0) matched++;
            }
            fprintf(stderr, "  %d/%d instances matched BKS entries\n",
                    matched, num_instances);
        }
    }

    /* Load existing checkpoint */
    if (use_checkpoint) {
        int loaded = cp_load(&cp, checkpoint_path);
        if (loaded > 0) {
            fprintf(stderr, "Loaded %d results from checkpoint: %s\n",
                    loaded, checkpoint_path);
            int td = 0;
            for (i = 0; i < 7; i++) if (cp.tier_done[i]) td++;
            if (td > 0) fprintf(stderr, "  %d tier(s) fully completed\n", td);
        }
        /* Open for appending */
        cp.fp = fopen(checkpoint_path, "a");
        if (!cp.fp) {
            fprintf(stderr, "Warning: cannot open checkpoint file for writing: %s\n",
                    checkpoint_path);
        } else if (loaded == 0) {
            /* New file — write header */
            cp_write_header(&cp, max_iterations, seed);
        }
    }

    configs = (SGTuneParams *)calloc(MAX_CONFIGS, sizeof(*configs));
    results = (TuneResult *)calloc(MAX_CONFIGS, sizeof(*results));
    if (!configs || !results) {
        fprintf(stderr, "Error: out of memory\n");
        free(configs);
        free(results);
        free(dynamic_instances);
        sg_free_bench_cases(dyn_cases, dyn_case_count);
        cp_free(&cp);
        return 1;
    }

    /* Initialize base params to all sentinels (= use defaults) */
    sg_tune_params_default(&base_params);

    /* Profile mode: load matrix cell as baseline and fix iteration/time budget */
    if (profile_idx >= 0) {
        SGScale scale;
        const SGProfileCell *cell;

        if (scale_size > 0) {
            scale = sg_scale_from_count((uint32_t)scale_size);
        } else if (custom_size > 0) {
            scale = sg_scale_from_count((uint32_t)custom_size);
        } else {
            scale = SG_SCALE_SMALL;
        }

        cell = &k_profile_matrix[profile_idx][scale];
        base_params = cell->tune;
        max_iterations = cell->max_iterations;
        max_time_seconds = cell->max_time_seconds;

        fprintf(stderr, "Profile mode: %s x %s (iters=%d, time=%ds)\n",
                (const char *[]){"REALTIME","FAST","NEAR_OPTIMAL","BEST"}[profile_idx],
                (const char *[]){"SMALL","MEDIUM","LARGE","XLARGE","MASSIVE"}[scale],
                max_iterations, max_time_seconds);
    }

    if (max_time_seconds > 0) {
        fprintf(stderr, "Time limit: %d seconds per instance\n", max_time_seconds);
    }
    fprintf(stderr, "Instance set: %d instances, %d iterations/instance\n",
            num_instances, max_iterations);

    /* Baseline evaluation */
    if (run_baseline) {
        if (cp.has_baseline) {
            baseline = cp.baseline;
            fprintf(stderr, "Baseline restored from checkpoint: score=%.4f\n",
                    baseline.composite_score);
        } else {
            fprintf(stderr, "Evaluating baseline (default params)...\n");
            evaluate_config(instances, num_instances, &base_params,
                            max_iterations, max_time_seconds, seed, &baseline);
            cp_write_baseline(&cp, &baseline);
        }
        fprintf(stderr, "  Baseline score: %.4f (V-gap: %.2f, D-gap: %.2f%%, worst-V: %.0f)\n",
                baseline.composite_score, baseline.avg_vehicle_gap,
                baseline.avg_distance_gap_pct, baseline.worst_vehicle_gap);
    }

    if (all_tiers) {
        /* Run all tiers sequentially, carrying best forward */
        int t;

        /* Restore base_params from completed tiers in checkpoint */
        for (t = 0; t < 7 && cp.tier_done[t]; t++) {
            base_params = cp.tier_best[t];
            fprintf(stderr, "\n=== Tier %d === (completed, restored from checkpoint)\n", t);
        }

        for (; t <= 6; t++) {
            fprintf(stderr, "\n=== Tier %d ===\n", t);

            /* Clear results for this tier */
            memset(results, 0, MAX_CONFIGS * sizeof(*results));

            switch (t) {
                case 0: num_configs = generate_tier0(configs, &base_params); break;
                case 1: num_configs = generate_tier1(configs, &base_params); break;
                case 2: num_configs = generate_tier2(configs, &base_params); break;
                case 3: num_configs = generate_tier3(configs, &base_params); break;
                case 4: num_configs = generate_tier4(configs, &base_params); break;
                case 5: num_configs = generate_tier5(configs, &base_params); break;
                case 6: num_configs = generate_tier6(configs, &base_params); break;
                default: num_configs = 0; break;
            }

            fprintf(stderr, "  %d configurations to evaluate\n", num_configs);
            evaluate_configs_parallel(instances, num_instances,
                                     configs, num_configs, max_iterations,
                                     max_time_seconds, seed, num_threads,
                                     results, t, use_checkpoint ? &cp : NULL);

            qsort(results, (size_t)num_configs, sizeof(TuneResult), compare_results);

            /* Carry best forward as base for next tier */
            base_params = results[0].params;
            fprintf(stderr, "  Best score: %.4f (V-gap: %.2f, D-gap: %.2f%%)\n",
                    results[0].composite_score, results[0].avg_vehicle_gap,
                    results[0].avg_distance_gap_pct);

            /* Mark tier as done in checkpoint */
            cp_write_tier_done(&cp, t, &base_params);
        }

        /* Verify final top results */
        if (verify_n > 0 && verify_n <= num_configs) {
            verify_top_results(instances, num_instances,
                               results, verify_n, max_iterations,
                               max_time_seconds, num_threads);
        }

    } else {
        /* Single tier */
        fprintf(stderr, "=== Tier %d ===\n", tier);

        /* Clear results */
        memset(results, 0, MAX_CONFIGS * sizeof(*results));

        switch (tier) {
            case 0: num_configs = generate_tier0(configs, &base_params); break;
            case 1: num_configs = generate_tier1(configs, &base_params); break;
            case 2: num_configs = generate_tier2(configs, &base_params); break;
            case 3: num_configs = generate_tier3(configs, &base_params); break;
            case 4: num_configs = generate_tier4(configs, &base_params); break;
            case 5: num_configs = generate_tier5(configs, &base_params); break;
            case 6: num_configs = generate_tier6(configs, &base_params); break;
            default: num_configs = 0; break;
        }

        fprintf(stderr, "%d configurations to evaluate\n", num_configs);
        evaluate_configs_parallel(instances, num_instances,
                                 configs, num_configs, max_iterations,
                                 max_time_seconds, seed, num_threads,
                                 results, tier, use_checkpoint ? &cp : NULL);

        qsort(results, (size_t)num_configs, sizeof(TuneResult), compare_results);

        /* Multi-seed verification */
        if (verify_n > 0 && verify_n <= num_configs) {
            verify_top_results(instances, num_instances,
                               results, verify_n, max_iterations,
                               max_time_seconds, num_threads);
        }
    }

    /* Output results */
    if (json_output) {
        int n = top_n < num_configs ? top_n : num_configs;
        printf("{\n");
        printf("  \"tier\": %d,\n", all_tiers ? -1 : tier);
        printf("  \"iterations\": %d,\n", max_iterations);
        printf("  \"time_limit\": %d,\n", max_time_seconds);
        printf("  \"seed\": %llu,\n", (unsigned long long)seed);
        printf("  \"num_configs_evaluated\": %d,\n", num_configs);
        printf("  \"num_instances\": %d,\n", num_instances);
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
    free(dynamic_instances);
    sg_free_bench_cases(dyn_cases, dyn_case_count);
    cp_free(&cp);
    return 0;
}
