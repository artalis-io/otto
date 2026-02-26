#include "sg_internal.h"

/* Adaptive penalty strategy state (HGS-style per-constraint self-adjustment) */
typedef struct {
    double target_feasible;
    double tolerance;
    double increase_factor;
    double decrease_factor;
    double penalty_min;
    double penalty_max;
    double cost_scale;     /* reference cost for proportional bounds */
    /* Per-constraint feasibility tracking */
    uint32_t feasible_count[SG_PENALTY_COUNT];
    uint32_t total_count;  /* shared: total solutions evaluated */
} SGAdaptivePenaltyState;

/* --- Adaptive strategy callbacks --- */

static void sg_adaptive_update(SGPenaltyManager *mgr) {
    SGAdaptivePenaltyState *st = (SGAdaptivePenaltyState *)mgr->state;
    int k;

    if (st->total_count == 0) return;

    for (k = 0; k < SG_PENALTY_COUNT; k++) {
        double frac = (double)st->feasible_count[k] / (double)st->total_count;
        if (frac < st->target_feasible - st->tolerance) {
            /* Too many infeasible for this constraint — increase penalty */
            mgr->weight[k] *= st->increase_factor;
            if (mgr->weight[k] > st->penalty_max)
                mgr->weight[k] = st->penalty_max;
        } else if (frac > st->target_feasible + st->tolerance) {
            /* Too few infeasible — decrease penalty to allow more exploration */
            mgr->weight[k] *= st->decrease_factor;
            if (mgr->weight[k] < st->penalty_min)
                mgr->weight[k] = st->penalty_min;
        }
    }

    /* Reset counters for next segment */
    memset(st->feasible_count, 0, sizeof(st->feasible_count));
    st->total_count = 0;
}

static void sg_adaptive_record(SGPenaltyManager *mgr,
                               const double violations[SG_PENALTY_COUNT]) {
    SGAdaptivePenaltyState *st = (SGAdaptivePenaltyState *)mgr->state;
    int k;

    st->total_count++;
    for (k = 0; k < SG_PENALTY_COUNT; k++) {
        if (violations[k] < 1e-9) {
            st->feasible_count[k]++;
        }
    }
}

static void sg_adaptive_reset(SGPenaltyManager *mgr) {
    SGAdaptivePenaltyState *st = (SGAdaptivePenaltyState *)mgr->state;
    double w0;
    int k;

    memset(st->feasible_count, 0, sizeof(st->feasible_count));
    st->total_count = 0;
    w0 = st->cost_scale > 1.0 ? st->cost_scale / 100.0 : 1.0;
    for (k = 0; k < SG_PENALTY_COUNT; k++) {
        mgr->weight[k] = w0;
    }
}

/* --- Public API --- */

void sg_penalty_init_adaptive(SGPenaltyManager *mgr, double target_feasible,
                              double tolerance, double increase_factor,
                              double decrease_factor, double cost_scale) {
    SGAdaptivePenaltyState *st;
    double w0;
    int k;

    if (!mgr) return;

    /* Free any previous state */
    sg_penalty_free(mgr);

    st = (SGAdaptivePenaltyState *)calloc(1, sizeof(*st));
    if (!st) return;

    /* Guard: clamp cost_scale to safe numeric range.
     * cost_scale approximates the typical per-route objective magnitude.
     * Penalty bounds are derived proportionally:
     *   penalty_min  = cost_scale * 1e-4  (below this, penalty has no steering effect)
     *   penalty_max  = cost_scale * 100   (above this, search freezes numerically)
     *   initial_w    = cost_scale / 100    (starts small, adapts up/down)
     * These ratios assume objective magnitude is comparable to cost_scale. */
    if (cost_scale != cost_scale) cost_scale = 1.0;       /* NaN guard */
    if (cost_scale < 1.0) cost_scale = 1.0;               /* floor */
    if (cost_scale > 1e12) cost_scale = 1e12;              /* ceiling */

    st->target_feasible = target_feasible;
    st->tolerance = tolerance;
    st->increase_factor = increase_factor;
    st->decrease_factor = decrease_factor;
    st->cost_scale = cost_scale;
    st->penalty_min = cost_scale * 1e-4;
    st->penalty_max = cost_scale * 100.0;
    st->total_count = 0;
    memset(st->feasible_count, 0, sizeof(st->feasible_count));

    w0 = cost_scale / 100.0;
    for (k = 0; k < SG_PENALTY_COUNT; k++) {
        mgr->weight[k] = w0;
    }
    mgr->enabled = 1;
    mgr->update = sg_adaptive_update;
    mgr->record = sg_adaptive_record;
    mgr->reset = sg_adaptive_reset;
    mgr->state = st;
}

/* --- Progressive penalty: lerp target_feasible from start to end over segments --- */

typedef struct {
    SGAdaptivePenaltyState base;
    double target_start;
    double target_end;
    uint32_t total_segments;
    uint32_t segments_elapsed;
} SGProgressivePenaltyState;

static void sg_progressive_update(SGPenaltyManager *mgr) {
    SGProgressivePenaltyState *ps = (SGProgressivePenaltyState *)mgr->state;
    double frac;

    /* Lerp target_feasible based on elapsed segments */
    ps->segments_elapsed++;
    frac = (ps->total_segments > 1)
           ? (double)ps->segments_elapsed / (double)ps->total_segments
           : 1.0;
    if (frac > 1.0) frac = 1.0;
    ps->base.target_feasible = ps->target_start + frac * (ps->target_end - ps->target_start);

    /* Delegate to standard adaptive update logic */
    sg_adaptive_update(mgr);
}

static void sg_progressive_record(SGPenaltyManager *mgr,
                                   const double violations[SG_PENALTY_COUNT]) {
    sg_adaptive_record(mgr, violations);
}

static void sg_progressive_reset(SGPenaltyManager *mgr) {
    SGProgressivePenaltyState *ps = (SGProgressivePenaltyState *)mgr->state;
    ps->segments_elapsed = 0;
    ps->base.target_feasible = ps->target_start;
    sg_adaptive_reset(mgr);
}

void sg_penalty_init_progressive(SGPenaltyManager *mgr, double target_start,
                                 double target_end, double tolerance,
                                 double increase_factor, double decrease_factor,
                                 double cost_scale, uint32_t total_segments) {
    SGProgressivePenaltyState *ps;

    if (!mgr) return;

    /* Bootstrap with standard adaptive init (sets weights, bounds, callbacks) */
    sg_penalty_init_adaptive(mgr, target_start, tolerance, increase_factor,
                             decrease_factor, cost_scale);
    if (!mgr->state) return;

    /* Upgrade state to progressive (base is prefix-compatible) */
    ps = (SGProgressivePenaltyState *)realloc(mgr->state, sizeof(*ps));
    if (!ps) return;  /* keep adaptive state as fallback */

    ps->target_start = target_start;
    ps->target_end = target_end;
    ps->total_segments = total_segments > 0 ? total_segments : 1;
    ps->segments_elapsed = 0;

    mgr->state = ps;
    mgr->update = sg_progressive_update;
    mgr->record = sg_progressive_record;
    mgr->reset = sg_progressive_reset;
}

void sg_penalty_free(SGPenaltyManager *mgr) {
    if (!mgr) return;
    free(mgr->state);
    mgr->state = NULL;
    mgr->update = NULL;
    mgr->record = NULL;
    mgr->reset = NULL;
    mgr->enabled = 0;
}

int sg_solution_is_feasible(const SGRouteSolution *sol) {
    int k;
    if (!sol) return 0;
    for (k = 0; k < SG_PENALTY_COUNT; k++) {
        if (sol->violations[k] > 1e-9) return 0;
    }
    return 1;
}

double sg_solution_total_violation(const SGRouteSolution *sol) {
    double total = 0.0;
    int k;
    if (!sol) return 0.0;
    for (k = 0; k < SG_PENALTY_COUNT; k++) {
        total += sol->violations[k];
    }
    return total;
}
