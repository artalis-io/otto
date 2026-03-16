#include "sg_internal.h"

/* Shared tune params for all cells (pre-tuning baseline from 100-request tuning).
   sa_accept_pct=0.074, p1_final=0.08, p2_final=0.0001, neighbor_k=30.
   All other fields use sentinel to fall through to hardcoded defaults. */
#define BASE_TUNE { \
    .phase1_fraction       = 0.60, \
    .phase15_iters         = 2000, \
    .sa_accept_pct         = 0.074, \
    .p1_final_temp_ratio   = 0.08, \
    .p2_final_temp_ratio   = 0.0001, \
    .pen_target_start      = SG_TUNE_SENTINEL_D, \
    .pen_target_end        = SG_TUNE_SENTINEL_D, \
    .pen_tolerance         = SG_TUNE_SENTINEL_D, \
    .pen_increase          = SG_TUNE_SENTINEL_D, \
    .pen_decrease          = SG_TUNE_SENTINEL_D, \
    .pen_p15_target        = SG_TUNE_SENTINEL_D, \
    .pen_p15_tolerance     = SG_TUNE_SENTINEL_D, \
    .pen_p15_increase      = SG_TUNE_SENTINEL_D, \
    .pen_p15_decrease      = SG_TUNE_SENTINEL_D, \
    .reaction_factor       = SG_TUNE_SENTINEL_D, \
    .reward_best           = SG_TUNE_SENTINEL_D, \
    .reward_better         = SG_TUNE_SENTINEL_D, \
    .reward_accepted       = SG_TUNE_SENTINEL_D, \
    .segment_size          = SG_TUNE_SENTINEL_I, \
    .worst_randomness      = SG_TUNE_SENTINEL_D, \
    .shaw_randomness       = SG_TUNE_SENTINEL_D, \
    .route_cluster_randomness = SG_TUNE_SENTINEL_D, \
    .time_cluster_randomness  = SG_TUNE_SENTINEL_D, \
    .pd_shaw_randomness    = SG_TUNE_SENTINEL_D, \
    .route_shaw_randomness = SG_TUNE_SENTINEL_D, \
    .string_l_max          = SG_TUNE_SENTINEL_I, \
    .neighbor_k            = 30, \
    .gen_reheat_ratio      = SG_TUNE_SENTINEL_D, \
    .gen_cooling_stretch   = SG_TUNE_SENTINEL_D \
}

/* Tuned for GH-200 scale (101-200 requests) via bench_tune all-tiers (S22).
   Key changes from BASE_TUNE: SA much warmer (0.200 vs 0.074) but cools
   much deeper (p1_final 1.3% vs 8%), more time on distance polish (40% vs 60% P1),
   gentle penalty decrease (0.95 vs sentinel), slow ALNS reaction (0.14), strong
   "best" reward (50.0), route shaw more random (10.0 vs sentinel). */
#define MEDIUM_TUNE { \
    .phase1_fraction       = 0.40, \
    .phase15_iters         = 2000, \
    .sa_accept_pct         = 0.200, \
    .p1_final_temp_ratio   = 0.0126, \
    .p2_final_temp_ratio   = 0.0008, \
    .pen_target_start      = 0.50, \
    .pen_target_end        = 0.30, \
    .pen_tolerance         = 0.15, \
    .pen_increase          = 2.00, \
    .pen_decrease          = 0.95, \
    .pen_p15_target        = SG_TUNE_SENTINEL_D, \
    .pen_p15_tolerance     = SG_TUNE_SENTINEL_D, \
    .pen_p15_increase      = SG_TUNE_SENTINEL_D, \
    .pen_p15_decrease      = SG_TUNE_SENTINEL_D, \
    .reaction_factor       = 0.14, \
    .reward_best           = 50.00, \
    .reward_better         = 1.00, \
    .reward_accepted       = 0.50, \
    .segment_size          = 50, \
    .worst_randomness      = 10.00, \
    .shaw_randomness       = 4.64, \
    .route_cluster_randomness = 3.16, \
    .time_cluster_randomness  = 10.00, \
    .pd_shaw_randomness    = 3.16, \
    .route_shaw_randomness = 10.00, \
    .string_l_max          = 8, \
    .neighbor_k            = 20, \
    .gen_reheat_ratio      = SG_TUNE_SENTINEL_D, \
    .gen_cooling_stretch   = SG_TUNE_SENTINEL_D \
}

/* MEDIUM_TUNE with reduced phase1.5 for realtime profile */
#define RT_MEDIUM_TUNE { \
    .phase1_fraction       = 0.40, \
    .phase15_iters         = 200, \
    .sa_accept_pct         = 0.200, \
    .p1_final_temp_ratio   = 0.0126, \
    .p2_final_temp_ratio   = 0.0008, \
    .pen_target_start      = 0.50, \
    .pen_target_end        = 0.30, \
    .pen_tolerance         = 0.15, \
    .pen_increase          = 2.00, \
    .pen_decrease          = 0.95, \
    .pen_p15_target        = SG_TUNE_SENTINEL_D, \
    .pen_p15_tolerance     = SG_TUNE_SENTINEL_D, \
    .pen_p15_increase      = SG_TUNE_SENTINEL_D, \
    .pen_p15_decrease      = SG_TUNE_SENTINEL_D, \
    .reaction_factor       = 0.14, \
    .reward_best           = 50.00, \
    .reward_better         = 1.00, \
    .reward_accepted       = 0.50, \
    .segment_size          = 50, \
    .worst_randomness      = 10.00, \
    .shaw_randomness       = 4.64, \
    .route_cluster_randomness = 3.16, \
    .time_cluster_randomness  = 10.00, \
    .pd_shaw_randomness    = 3.16, \
    .route_shaw_randomness = 10.00, \
    .string_l_max          = 8, \
    .neighbor_k            = 20, \
    .gen_reheat_ratio      = SG_TUNE_SENTINEL_D, \
    .gen_cooling_stretch   = SG_TUNE_SENTINEL_D \
}

/* Tuned for GH-400 scale (201-400 requests) via bench_tune all-tiers (S22).
   Key changes from BASE_TUNE: SA much cooler (0.010 vs 0.074), Phase 2 cools
   slower (0.0063 vs 0.0001), more time on distance polish (40% vs 60% P1),
   aggressive penalty adaptation, faster ALNS learning, smaller neighborhood.
   S25: gen_reheat_ratio=2.0 — R1 grid search showed -8.5pp avg distance gap
   (r1_4_1 +15.4→+13.2%, r1_4_10 +52.1→+32.4%, r1_4_7 +29.5→+26.3%). */
#define LARGE_TUNE { \
    .phase1_fraction       = 0.40, \
    .phase15_iters         = 1000, \
    .sa_accept_pct         = 0.010, \
    .p1_final_temp_ratio   = 0.0795, \
    .p2_final_temp_ratio   = 0.0063, \
    .pen_target_start      = 0.50, \
    .pen_target_end        = 0.30, \
    .pen_tolerance         = 0.15, \
    .pen_increase          = 2.00, \
    .pen_decrease          = 0.50, \
    .pen_p15_target        = SG_TUNE_SENTINEL_D, \
    .pen_p15_tolerance     = SG_TUNE_SENTINEL_D, \
    .pen_p15_increase      = SG_TUNE_SENTINEL_D, \
    .pen_p15_decrease      = SG_TUNE_SENTINEL_D, \
    .reaction_factor       = 0.50, \
    .reward_best           = 7.66, \
    .reward_better         = 20.00, \
    .reward_accepted       = 0.50, \
    .segment_size          = 50, \
    .worst_randomness      = 10.00, \
    .shaw_randomness       = 4.64, \
    .route_cluster_randomness = 3.16, \
    .time_cluster_randomness  = 10.00, \
    .pd_shaw_randomness    = 3.16, \
    .route_shaw_randomness = 3.16, \
    .string_l_max          = 8, \
    .neighbor_k            = 20, \
    .gen_reheat_ratio      = 2.00, \
    .gen_cooling_stretch   = SG_TUNE_SENTINEL_D \
}

/* S27e: Tuned for GH-800 scale (401-800 requests).  Key differences from
   LARGE_TUNE: less time on Phase 1 (0.35 vs 0.40) because vehicle count
   settles faster at large scale, colder SA start (0.005 vs 0.010) for more
   exploitation in the huge space, higher gen_reheat_ratio (2.5 vs 2.0) so
   warm-started generations escape inherited structure, broader neighborhood
   (k=25 vs 20) for longer routes. */
#define XLARGE_TUNE { \
    .phase1_fraction       = 0.35, \
    .phase15_iters         = 1000, \
    .sa_accept_pct         = 0.005, \
    .p1_final_temp_ratio   = 0.0795, \
    .p2_final_temp_ratio   = 0.0063, \
    .pen_target_start      = 0.50, \
    .pen_target_end        = 0.30, \
    .pen_tolerance         = 0.15, \
    .pen_increase          = 2.00, \
    .pen_decrease          = 0.50, \
    .pen_p15_target        = SG_TUNE_SENTINEL_D, \
    .pen_p15_tolerance     = SG_TUNE_SENTINEL_D, \
    .pen_p15_increase      = SG_TUNE_SENTINEL_D, \
    .pen_p15_decrease      = SG_TUNE_SENTINEL_D, \
    .reaction_factor       = 0.50, \
    .reward_best           = 7.66, \
    .reward_better         = 20.00, \
    .reward_accepted       = 0.50, \
    .segment_size          = 50, \
    .worst_randomness      = 10.00, \
    .shaw_randomness       = 4.64, \
    .route_cluster_randomness = 3.16, \
    .time_cluster_randomness  = 10.00, \
    .pd_shaw_randomness    = 3.16, \
    .route_shaw_randomness = 3.16, \
    .string_l_max          = 8, \
    .neighbor_k            = 25, \
    .gen_reheat_ratio      = 2.50, \
    .gen_cooling_stretch   = SG_TUNE_SENTINEL_D \
}

/* RT variant of XLARGE_TUNE */
#define RT_XLARGE_TUNE { \
    .phase1_fraction       = 0.35, \
    .phase15_iters         = 200, \
    .sa_accept_pct         = 0.005, \
    .p1_final_temp_ratio   = 0.0795, \
    .p2_final_temp_ratio   = 0.0063, \
    .pen_target_start      = 0.50, \
    .pen_target_end        = 0.30, \
    .pen_tolerance         = 0.15, \
    .pen_increase          = 2.00, \
    .pen_decrease          = 0.50, \
    .pen_p15_target        = SG_TUNE_SENTINEL_D, \
    .pen_p15_tolerance     = SG_TUNE_SENTINEL_D, \
    .pen_p15_increase      = SG_TUNE_SENTINEL_D, \
    .pen_p15_decrease      = SG_TUNE_SENTINEL_D, \
    .reaction_factor       = 0.50, \
    .reward_best           = 7.66, \
    .reward_better         = 20.00, \
    .reward_accepted       = 0.50, \
    .segment_size          = 50, \
    .worst_randomness      = 10.00, \
    .shaw_randomness       = 4.64, \
    .route_cluster_randomness = 3.16, \
    .time_cluster_randomness  = 10.00, \
    .pd_shaw_randomness    = 3.16, \
    .route_shaw_randomness = 3.16, \
    .string_l_max          = 8, \
    .neighbor_k            = 25, \
    .gen_reheat_ratio      = 2.50, \
    .gen_cooling_stretch   = SG_TUNE_SENTINEL_D \
}

/* Realtime profile: phase1.5 only 200 iters */
#define RT_TUNE { \
    .phase1_fraction       = 0.60, \
    .phase15_iters         = 200, \
    .sa_accept_pct         = 0.074, \
    .p1_final_temp_ratio   = 0.08, \
    .p2_final_temp_ratio   = 0.0001, \
    .pen_target_start      = SG_TUNE_SENTINEL_D, \
    .pen_target_end        = SG_TUNE_SENTINEL_D, \
    .pen_tolerance         = SG_TUNE_SENTINEL_D, \
    .pen_increase          = SG_TUNE_SENTINEL_D, \
    .pen_decrease          = SG_TUNE_SENTINEL_D, \
    .pen_p15_target        = SG_TUNE_SENTINEL_D, \
    .pen_p15_tolerance     = SG_TUNE_SENTINEL_D, \
    .pen_p15_increase      = SG_TUNE_SENTINEL_D, \
    .pen_p15_decrease      = SG_TUNE_SENTINEL_D, \
    .reaction_factor       = SG_TUNE_SENTINEL_D, \
    .reward_best           = SG_TUNE_SENTINEL_D, \
    .reward_better         = SG_TUNE_SENTINEL_D, \
    .reward_accepted       = SG_TUNE_SENTINEL_D, \
    .segment_size          = SG_TUNE_SENTINEL_I, \
    .worst_randomness      = SG_TUNE_SENTINEL_D, \
    .shaw_randomness       = SG_TUNE_SENTINEL_D, \
    .route_cluster_randomness = SG_TUNE_SENTINEL_D, \
    .time_cluster_randomness  = SG_TUNE_SENTINEL_D, \
    .pd_shaw_randomness    = SG_TUNE_SENTINEL_D, \
    .route_shaw_randomness = SG_TUNE_SENTINEL_D, \
    .string_l_max          = SG_TUNE_SENTINEL_I, \
    .neighbor_k            = 30, \
    .gen_reheat_ratio      = SG_TUNE_SENTINEL_D, \
    .gen_cooling_stretch   = SG_TUNE_SENTINEL_D \
}

/* LARGE_TUNE with reduced phase1.5 for realtime profile */
#define RT_LARGE_TUNE { \
    .phase1_fraction       = 0.40, \
    .phase15_iters         = 200, \
    .sa_accept_pct         = 0.010, \
    .p1_final_temp_ratio   = 0.0795, \
    .p2_final_temp_ratio   = 0.0063, \
    .pen_target_start      = 0.50, \
    .pen_target_end        = 0.30, \
    .pen_tolerance         = 0.15, \
    .pen_increase          = 2.00, \
    .pen_decrease          = 0.50, \
    .pen_p15_target        = SG_TUNE_SENTINEL_D, \
    .pen_p15_tolerance     = SG_TUNE_SENTINEL_D, \
    .pen_p15_increase      = SG_TUNE_SENTINEL_D, \
    .pen_p15_decrease      = SG_TUNE_SENTINEL_D, \
    .reaction_factor       = 0.50, \
    .reward_best           = 7.66, \
    .reward_better         = 20.00, \
    .reward_accepted       = 0.50, \
    .segment_size          = 50, \
    .worst_randomness      = 10.00, \
    .shaw_randomness       = 4.64, \
    .route_cluster_randomness = 3.16, \
    .time_cluster_randomness  = 10.00, \
    .pd_shaw_randomness    = 3.16, \
    .route_shaw_randomness = 3.16, \
    .string_l_max          = 8, \
    .neighbor_k            = 20, \
    .gen_reheat_ratio      = 2.00, \
    .gen_cooling_stretch   = SG_TUNE_SENTINEL_D \
}

/*
 * Profile × Scale matrix.
 *
 * Rows: REALTIME, FAST, NEAR_OPTIMAL, BEST
 * Cols: SMALL(1-100), MEDIUM(101-200), LARGE(201-400), XLARGE(401-800), MASSIVE(801+)
 *
 * Initial iteration/time budgets are scaled from the 100-request baseline.
 * Tune params start as copies of the 100-request tuned values and get
 * refined per-cell by the tuner later.
 */
const SGProfileCell k_profile_matrix[SG_PROFILE_COUNT][SG_SCALE_COUNT] = {
    /* REALTIME */
    {
        {  500,    1, RT_TUNE },          /* SMALL */
        {  500,    2, RT_MEDIUM_TUNE },   /* MEDIUM (S22 tuned) */
        {  500,    5, RT_LARGE_TUNE },    /* LARGE (S22 tuned) */
        {  250,   10, RT_XLARGE_TUNE },   /* XLARGE (S27e) */
        {  250,   15, RT_TUNE },          /* MASSIVE */
    },
    /* FAST */
    {
        { 2500,    5, BASE_TUNE },     /* SMALL */
        { 2500,   10, MEDIUM_TUNE },   /* MEDIUM (S22 tuned) */
        { 2500,   30, LARGE_TUNE },    /* LARGE (S22 tuned) */
        { 1500,   60, XLARGE_TUNE },   /* XLARGE (S27e) */
        { 1000,   90, BASE_TUNE },     /* MASSIVE */
    },
    /* NEAR_OPTIMAL */
    {
        { 10000,  15, BASE_TUNE },     /* SMALL */
        { 10000,  45, MEDIUM_TUNE },   /* MEDIUM (S22 tuned) */
        { 30000, 120, LARGE_TUNE },    /* LARGE — raised from 5K */
        { 20000, 300, XLARGE_TUNE },   /* XLARGE (S27e: 15K→20K, XLARGE_TUNE) */
        {  8000, 600, BASE_TUNE },     /* MASSIVE */
    },
    /* BEST */
    {
        { 50000,   60, BASE_TUNE },    /* SMALL */
        { 25000,  180, MEDIUM_TUNE },  /* MEDIUM (S22 tuned) */
        { 50000,  600, LARGE_TUNE },   /* LARGE — raised from 10K */
        { 50000, 1200, XLARGE_TUNE },  /* XLARGE (S27e: 30K→50K, XLARGE_TUNE) */
        { 15000, 1800, BASE_TUNE },    /* MASSIVE */
    },
};

SGScale sg_scale_from_count(uint32_t num_requests) {
    if (num_requests <= 100) return SG_SCALE_SMALL;
    if (num_requests <= 200) return SG_SCALE_MEDIUM;
    if (num_requests <= 400) return SG_SCALE_LARGE;
    if (num_requests <= 800) return SG_SCALE_XLARGE;
    return SG_SCALE_MASSIVE;
}

SGStatus sg_profile_matrix_apply(SGContext *ctx, SGProfile profile, SGScale scale) {
    const SGProfileCell *cell;

    if (!ctx) return SG_STATUS_INVALID_ARG;
    if ((int)profile < 0 || (int)profile >= SG_PROFILE_COUNT) return SG_STATUS_INVALID_ARG;
    if ((int)scale < 0 || (int)scale >= SG_SCALE_COUNT) return SG_STATUS_INVALID_ARG;

    cell = &k_profile_matrix[profile][scale];

    ctx->config.max_iterations = cell->max_iterations;
    ctx->config.max_time_seconds = cell->max_time_seconds;

    return sg_set_tune_params(ctx, &cell->tune);
}
