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
    .neighbor_k            = 30 \
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
    .neighbor_k            = 30 \
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
        {  500,    1, RT_TUNE },   /* SMALL */
        {  500,    2, RT_TUNE },   /* MEDIUM */
        {  500,    5, RT_TUNE },   /* LARGE */
        {  250,   10, RT_TUNE },   /* XLARGE */
        {  250,   15, RT_TUNE },   /* MASSIVE */
    },
    /* FAST */
    {
        { 2500,    5, BASE_TUNE }, /* SMALL */
        { 2500,   10, BASE_TUNE }, /* MEDIUM */
        { 2500,   30, BASE_TUNE }, /* LARGE */
        { 1500,   60, BASE_TUNE }, /* XLARGE */
        { 1000,   90, BASE_TUNE }, /* MASSIVE */
    },
    /* NEAR_OPTIMAL */
    {
        { 10000,  15, BASE_TUNE }, /* SMALL */
        { 10000,  45, BASE_TUNE }, /* MEDIUM */
        {  5000, 120, BASE_TUNE }, /* LARGE */
        {  3000, 300, BASE_TUNE }, /* XLARGE */
        {  2000, 600, BASE_TUNE }, /* MASSIVE */
    },
    /* BEST */
    {
        { 50000,   60, BASE_TUNE }, /* SMALL */
        { 25000,  180, BASE_TUNE }, /* MEDIUM */
        { 10000,  600, BASE_TUNE }, /* LARGE */
        {  5000, 1200, BASE_TUNE }, /* XLARGE */
        {  3000, 1800, BASE_TUNE }, /* MASSIVE */
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
