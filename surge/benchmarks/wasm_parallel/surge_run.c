/* surge_run.c — one-shot single-threaded Surge solve for the WASM
 * parallel-portfolio benchmark. Loads one Li & Lim PDPTW instance, solves ONCE
 * with a time/iteration budget + seed (sg_solve, never the thread pool), and
 * prints one machine-parsable KEY=VALUE line. Built both native (sanity) and to
 * WASM (emcc); K of these run concurrently to reproduce sg_solve_parallel(K).
 *
 * Mirrors bench_li_lim's config + tuned SA params so native-vs-WASM is
 * apples-to-apples. Public Li & Lim instances only; no customer data. */
#include "surge.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <instance.txt> [--time-limit SEC] [--seed N] [--iterations N]\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    int time_limit = 5;
    uint64_t seed = 42;
    int iterations = 100000000; /* high so --time-limit is the binding constraint */
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--time-limit") && i + 1 < argc) time_limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--iterations") && i + 1 < argc) iterations = atoi(argv[++i]);
    }

    SGContext *ctx = sg_create();
    if (!ctx) { fprintf(stderr, "ERR create\n"); return 1; }

    SGConfig config;
    sg_config_default(&config);
    config.max_iterations = iterations;
    config.max_time_seconds = time_limit;
    config.seed = seed;
    config.deterministic = 1;
    if (sg_set_config(ctx, &config) != SG_STATUS_OK) { fprintf(stderr, "ERR config\n"); sg_free(ctx); return 1; }

    SGTuneParams tp;
    sg_tune_params_default(&tp);
    tp.sa_accept_pct = 0.074;
    tp.p1_final_temp_ratio = 0.08;
    tp.p2_final_temp_ratio = 0.0001;
    tp.phase15_iters = 2000;
    sg_set_tune_params(ctx, &tp);

    if (sg_load_li_lim_pdptw(ctx, path) != SG_STATUS_OK) { fprintf(stderr, "ERR load %s\n", path); sg_free(ctx); return 1; }
    if (sg_validate_model(ctx) != SG_STATUS_OK) { fprintf(stderr, "ERR validate\n"); sg_free(ctx); return 1; }

    SGStatus st = sg_solve(ctx);
    SGStats s;
    sg_get_stats(ctx, &s);
    printf("seed=%llu status=%d veh=%u dist=%.4f unassigned=%u iters=%lld elapsed=%.3f\n",
           (unsigned long long)seed, (int)st, s.vehicles_used, s.total_distance,
           s.unassigned, (long long)s.iterations, s.elapsed_seconds);
    sg_free(ctx);
    return 0;
}
