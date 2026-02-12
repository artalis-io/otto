/*
 * FuelWise - Truck Refueling Optimization Library
 * Refueling Optimization Implementation
 *
 * Copyright (c) 2024. All rights reserved.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "fw_refuel.h"
#include "ralph.h"

/* Maximum stations to prevent integer overflow in allocations */
#define FW_MAX_STATIONS 100000

/* ============================================================================
 * Fuel Consumption Calculation
 *
 * Distance is in meters, consumption is L/100km, fuel is in liters.
 * Formula: fuel (L) = distance (m) / 100000.0 * consumption (L/100km)
 * ============================================================================ */

double fw_calc_fuel_consumed(
    const FWRefuelProblem *problem,
    double from_distance,
    double to_distance)
{
    if (from_distance >= to_distance) return 0.0;

    /* Use constant rate if no segments defined */
    if (problem->num_segments == 0 || problem->segments == NULL) {
        double distance_m = to_distance - from_distance;
        return (distance_m / 100000.0) * problem->base_consumption;
    }

    /* Piecewise linear: sum fuel consumed in each segment */
    double total_fuel = 0.0;
    double current_dist = from_distance;

    for (int s = 0; s < problem->num_segments && current_dist < to_distance; s++) {
        double seg_start = problem->segments[s].start_distance;
        double seg_end;

        /* Determine segment end */
        if (s + 1 < problem->num_segments) {
            seg_end = problem->segments[s + 1].start_distance;
        } else {
            seg_end = problem->total_distance;  /* Last segment extends to end */
        }

        /* Skip segments before our range */
        if (seg_end <= from_distance) continue;

        /* Clip to our range */
        double range_start = (current_dist > seg_start) ? current_dist : seg_start;
        double range_end = (to_distance < seg_end) ? to_distance : seg_end;

        if (range_start < range_end) {
            double segment_distance_m = range_end - range_start;
            double segment_fuel = (segment_distance_m / 100000.0) * problem->segments[s].consumption;
            total_fuel += segment_fuel;
            current_dist = range_end;
        }
    }

    return total_fuel;
}

double fw_calc_total_fuel_consumed(const FWRefuelProblem *problem)
{
    return fw_calc_fuel_consumed(problem, 0, problem->total_distance);
}

/* ============================================================================
 * Refueling Problem Solving - LP
 * ============================================================================ */

int fw_solve_refuel_lp(
    const FWRefuelProblem *problem,
    FWRefuelSolution *solution)
{
    if (!problem || !solution) return -1;

    /* Initialize solution */
    memset(solution, 0, sizeof(FWRefuelSolution));

    int k = problem->num_stations;
    if (k == 0) {
        solution->status = FW_STATUS_OPTIMAL;
        solution->total_cost = 0.0;
        solution->remaining_fuel = problem->current_fuel -
            fw_calc_total_fuel_consumed(problem);
        return 0;
    }

    /* Validate k to prevent integer overflow in allocations */
    if (k < 0 || k > FW_MAX_STATIONS) {
        solution->status = FW_STATUS_ERROR;
        return -1;
    }

    int num_vars = 2 * k;

    RalphModel *model = ralph_create();
    if (!model) {
        solution->status = FW_STATUS_ERROR;
        return -1;
    }

    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    int x_start = 0;
    int y_start = k;

    /* Add x[i] variables - fuel purchased at each station
     *
     * If remaining_fuel_value > 0, we credit remaining fuel.
     * Effective objective coefficient: price[i] - remaining_fuel_value
     */
    double est_price = problem->remaining_fuel_value;
    for (int i = 0; i < k; i++) {
        double obj_coeff = problem->stations[i].price - est_price;
        ralph_add_var(model, 0.0, problem->tank_capacity, obj_coeff, RALPH_CONTINUOUS);
    }

    /* Add y[i] variables - cumulative fuel before arriving */
    double y_upper = problem->current_fuel + k * problem->tank_capacity;
    for (int i = 0; i < k; i++) {
        ralph_add_var(model, 0.0, y_upper, 0.0, RALPH_CONTINUOUS);
    }

    /* Constraint: Fuel balance - y[i] = fuel_current + sum(x[j] for j < i) */
    for (int i = 0; i < k; i++) {
        int nnz = 1 + i;
        int *indices = malloc(nnz * sizeof(int));
        double *values = malloc(nnz * sizeof(double));
        if (!indices || !values) {
            free(indices);
            free(values);
            ralph_free(model);
            solution->status = FW_STATUS_ERROR;
            return -1;
        }

        indices[0] = y_start + i;
        values[0] = 1.0;

        for (int j = 0; j < i; j++) {
            indices[1 + j] = x_start + j;
            values[1 + j] = -1.0;
        }

        ralph_add_constraint(model, nnz, indices, values, RALPH_EQUAL, problem->current_fuel);
        free(indices);
        free(values);
    }

    /* Constraint: Minimum fuel at arrival at each station */
    for (int i = 0; i < k; i++) {
        int indices[1] = {y_start + i};
        double values[1] = {1.0};
        double fuel_consumed = fw_calc_fuel_consumed(problem, 0,
            problem->stations[i].distance_from_start);
        double rhs = problem->minimum_fuel + fuel_consumed;

        ralph_add_constraint(model, 1, indices, values, RALPH_GREATER_EQUAL, rhs);
    }

    /* Constraint: Tank capacity after refueling */
    for (int i = 0; i < k; i++) {
        int indices[2] = {y_start + i, x_start + i};
        double values[2] = {1.0, 1.0};
        double fuel_consumed = fw_calc_fuel_consumed(problem, 0,
            problem->stations[i].distance_from_start);
        double rhs = problem->tank_capacity + fuel_consumed;

        ralph_add_constraint(model, 2, indices, values, RALPH_LESS_EQUAL, rhs);
    }

    /* Constraint: Must reach destination with minimum fuel at end */
    {
        int *indices = malloc(k * sizeof(int));
        double *values = malloc(k * sizeof(double));
        if (!indices || !values) {
            free(indices);
            free(values);
            ralph_free(model);
            solution->status = FW_STATUS_ERROR;
            return -1;
        }

        for (int i = 0; i < k; i++) {
            indices[i] = x_start + i;
            values[i] = 1.0;
        }

        double total_fuel_needed = fw_calc_total_fuel_consumed(problem);
        double min_end = (problem->minimum_fuel_at_end > 0) ?
                         problem->minimum_fuel_at_end : problem->minimum_fuel;
        double rhs = min_end + total_fuel_needed - problem->current_fuel;

        if (rhs > 0) {
            ralph_add_constraint(model, k, indices, values, RALPH_GREATER_EQUAL, rhs);
        }

        free(indices);
        free(values);
    }

    /* Solve */
    ralph_set_int_param(model, "verbose", 0);
    int ret = ralph_optimize(model);
    RalphStatus status = ralph_get_status(model);

    if (ret == 0 && status == RALPH_STATUS_OPTIMAL) {
        double *x = malloc(num_vars * sizeof(double));
        if (!x) {
            ralph_free(model);
            solution->status = FW_STATUS_ERROR;
            return -1;
        }
        ralph_get_solution(model, x);

        /* Allocate solution arrays */
        solution->purchases = malloc(k * sizeof(double));
        solution->stop_flags = calloc(k, sizeof(int));
        if (!solution->purchases || !solution->stop_flags) {
            free(x);
            free(solution->purchases);
            free(solution->stop_flags);
            ralph_free(model);
            solution->status = FW_STATUS_ERROR;
            return -1;
        }

        double gross_cost = 0.0;
        double total_purchased = 0.0;
        int num_stops = 0;

        for (int i = 0; i < k; i++) {
            solution->purchases[i] = x[x_start + i];
            gross_cost += solution->purchases[i] * problem->stations[i].price;
            total_purchased += solution->purchases[i];
            if (solution->purchases[i] > 0.5) {
                solution->stop_flags[i] = 1;
                num_stops++;
            }
        }

        /* Calculate remaining fuel and total cost */
        double total_consumed = fw_calc_total_fuel_consumed(problem);
        solution->remaining_fuel = problem->current_fuel + total_purchased - total_consumed;
        solution->gross_cost = gross_cost;

        if (est_price > 0.0) {
            double empty_capacity = problem->tank_capacity - solution->remaining_fuel;
            solution->total_cost = gross_cost + empty_capacity * est_price;
        } else {
            solution->total_cost = gross_cost;
        }

        solution->num_stops = num_stops;
        solution->status = FW_STATUS_OPTIMAL;

        free(x);
        ralph_free(model);
        return 0;
    }

    /* Handle failure */
    if (status == RALPH_STATUS_INFEASIBLE) {
        solution->status = FW_STATUS_INFEASIBLE;
    } else {
        solution->status = FW_STATUS_ERROR;
    }

    ralph_free(model);
    return -1;
}

/* ============================================================================
 * Domain-Specific MIP Infrastructure
 *
 * Reach cuts: if fuel capacity prevents traversing an interval without
 * refueling, at least one station in that interval must be selected.
 * These strengthen the LP relaxation and prune infeasible branches.
 * ============================================================================ */

/* Hint flags for controlling which MIP enhancements are active.
 * Default (0) enables all hints. Individual flags disable specific hints. */
#define FW_HINT_NO_PRIORITIES     (1 << 0)  /* Disable branching priorities */
#define FW_HINT_NO_DIRECTIONS     (1 << 1)  /* Disable branching directions */
#define FW_HINT_NO_REACH_CUTS     (1 << 2)  /* Disable reach-cut callback */
#define FW_HINT_NO_MANDATORY_FIX  (1 << 3)  /* Disable mandatory station fixing */
#define FW_HINT_NO_DOMINATED_ELIM (1 << 4)  /* Disable dominated station elimination */
#define FW_HINT_NO_SYMMETRY_BREAK (1 << 5)  /* Disable symmetry-breaking constraints */

/* Global hint flags. Default 0 = all enabled.
 * Set before solving, NOT thread-safe if modified concurrently. */
static int fw_mip_hint_flags = 0;
static int fw_presolve = 1;
static unsigned int fw_presolve_mask = 0x110F;  /* Lightweight: fixed+empty+singleton_rows+bound_tight+shift */

void fw_set_mip_hint_flags(int flags) { fw_mip_hint_flags = flags; }
int fw_get_mip_hint_flags(void) { return fw_mip_hint_flags; }
void fw_set_presolve(int enable, unsigned int mask) { fw_presolve = enable; fw_presolve_mask = mask; }

/* Context for reach-cut callback */
typedef struct {
    const FWRefuelProblem *problem;
    int k, z_start;
    int num_intervals;
    int *interval_start;    /* start index of each mandatory-stop interval */
    int *interval_end;      /* end index (exclusive) of each interval */
    int *scratch_indices;   /* pre-allocated for callback */
    double *scratch_coeffs;
} FWReachCutContext;

/*
 * Precompute mandatory-stop intervals based on fuel reach.
 *
 * From origin: walk forward; if current_fuel - consumed < min_fuel before
 * reaching station i, the interval [0, i) must contain a stop.
 *
 * From each station j (assuming full tank): walk forward; if
 * tank_capacity - consumed < min_fuel before reaching station i,
 * the interval (j, i) must contain a stop.
 */
static void fw_compute_reach_intervals(FWReachCutContext *ctx,
                                        const FWRefuelProblem *problem,
                                        int k)
{
    ctx->num_intervals = 0;

    /* Worst-case: k origin intervals + k*k inter-station intervals.
     * In practice much fewer; k*(k+1)/2 is a safe upper bound. */
    int max_intervals = k + k * k;
    if (max_intervals > 100000) max_intervals = 100000;

    ctx->interval_start = (int*)malloc(max_intervals * sizeof(int));
    ctx->interval_end = (int*)malloc(max_intervals * sizeof(int));
    if (!ctx->interval_start || !ctx->interval_end) return;

    /* From origin */
    for (int i = 1; i < k; i++) {
        double consumed = fw_calc_fuel_consumed(problem, 0,
            problem->stations[i].distance_from_start);
        double fuel_at_i = problem->current_fuel - consumed;

        if (fuel_at_i < problem->minimum_fuel) {
            /* Need at least one stop in [0, i) */
            ctx->interval_start[ctx->num_intervals] = 0;
            ctx->interval_end[ctx->num_intervals] = i;
            ctx->num_intervals++;
            if (ctx->num_intervals >= max_intervals) return;
        }
    }

    /* From each station j with full tank */
    for (int j = 0; j < k; j++) {
        for (int i = j + 2; i < k; i++) {
            double consumed = fw_calc_fuel_consumed(problem,
                problem->stations[j].distance_from_start,
                problem->stations[i].distance_from_start);
            double fuel_at_i = problem->tank_capacity - consumed;

            if (fuel_at_i < problem->minimum_fuel) {
                /* Need at least one stop in (j, i) */
                if (i - j > 1) {  /* interval must be non-empty */
                    ctx->interval_start[ctx->num_intervals] = j + 1;
                    ctx->interval_end[ctx->num_intervals] = i;
                    ctx->num_intervals++;
                    if (ctx->num_intervals >= max_intervals) return;
                }
                break;  /* further stations are even farther */
            }
        }
    }

    /* Allocate scratch buffers for callback (max size = k) */
    ctx->scratch_indices = (int*)malloc(k * sizeof(int));
    ctx->scratch_coeffs = (double*)malloc(k * sizeof(double));
}

/*
 * Cut callback matching RalphCutCallback signature.
 * Checks each precomputed interval; if sum(z[j]) < 1 - eps in the LP
 * relaxation, adds cut sum(z[j]) >= 1.
 */
static int fw_reach_cut_generate(
    void *user_data,
    const double *x_relaxation,
    int num_vars,
    RalphCut *cuts,
    int max_cuts)
{
    FWReachCutContext *ctx = (FWReachCutContext*)user_data;
    if (!ctx || !ctx->scratch_indices || !ctx->scratch_coeffs) return 0;

    int num_cuts = 0;
    double eps = 1e-4;

    for (int iv = 0; iv < ctx->num_intervals && num_cuts < max_cuts; iv++) {
        int start = ctx->interval_start[iv];
        int end = ctx->interval_end[iv];

        /* Check if violated: sum z[j] for j in [start, end) < 1 - eps */
        double sum_z = 0.0;
        for (int j = start; j < end; j++) {
            int z_idx = ctx->z_start + j;
            if (z_idx < num_vars) {
                sum_z += x_relaxation[z_idx];
            }
        }

        if (sum_z < 1.0 - eps) {
            /* Build cut using pre-allocated scratch buffers */
            int nnz = 0;
            for (int j = start; j < end; j++) {
                ctx->scratch_indices[nnz] = ctx->z_start + j;
                ctx->scratch_coeffs[nnz] = 1.0;
                nnz++;
            }

            cuts[num_cuts].indices = ctx->scratch_indices;
            cuts[num_cuts].coeffs = ctx->scratch_coeffs;
            cuts[num_cuts].num_vars = nnz;
            cuts[num_cuts].sense = RALPH_GREATER_EQUAL;
            cuts[num_cuts].rhs = 1.0;
            num_cuts++;

            /* Only generate one cut per callback invocation to avoid
             * reusing scratch buffers for multiple cuts simultaneously */
            break;
        }
    }

    return num_cuts;
}

/*
 * Configure MIP hints on a RalphModel: priorities, directions, cut callback,
 * presolve (mandatory station fixing, dominated station elimination),
 * and symmetry-breaking constraints.
 *
 * Priorities: cheaper stations get higher priority (branched first).
 * Directions: cheap stations branch up (try z=1), expensive branch down.
 * Cuts: reach-cut callback for violated intervals.
 * Presolve: fix z[i]=1 for single-station reach intervals,
 *           fix z[i]=0 for dominated expensive stations.
 * Symmetry: z[i] >= z[i+1] for equal-price adjacent pairs.
 */
static void fw_setup_mip_hints(RalphModel *model,
                                const FWRefuelProblem *problem,
                                int k, int z_start,
                                FWReachCutContext *cut_ctx)
{
    int flags = fw_mip_hint_flags;
    int num_vars = ralph_get_num_vars(model);

    /* Compute median price for direction threshold */
    double *prices = (double*)malloc(k * sizeof(double));
    int *priorities = (int*)calloc(num_vars, sizeof(int));
    int *directions = (int*)calloc(num_vars, sizeof(int));

    if (!prices || !priorities || !directions) {
        free(prices);
        free(priorities);
        free(directions);
        return;
    }

    for (int i = 0; i < k; i++) {
        prices[i] = problem->stations[i].price;
    }

    /* Simple median: sort prices and take middle */
    for (int i = 0; i < k - 1; i++) {
        for (int j = i + 1; j < k; j++) {
            if (prices[j] < prices[i]) {
                double tmp = prices[i];
                prices[i] = prices[j];
                prices[j] = tmp;
            }
        }
    }
    double median_price = prices[k / 2];
    free(prices);

    /* Set priorities and directions for z variables */
    if (!(flags & FW_HINT_NO_PRIORITIES) || !(flags & FW_HINT_NO_DIRECTIONS)) {
        for (int i = 0; i < k; i++) {
            double price = problem->stations[i].price;

            /* Higher priority = branched first; cheaper stations get higher priority */
            priorities[z_start + i] = (price > 0.001) ? (int)(1000.0 / price) : 1000;

            /* Cheap stations: try z=1 first; expensive: try z=0 first */
            if (price <= median_price) {
                directions[z_start + i] = RALPH_BRANCH_UP;
            } else {
                directions[z_start + i] = RALPH_BRANCH_DOWN;
            }
        }

        if (!(flags & FW_HINT_NO_PRIORITIES))
            ralph_set_branch_priorities(model, priorities);
        if (!(flags & FW_HINT_NO_DIRECTIONS))
            ralph_set_branch_directions(model, directions);
    }

    free(priorities);
    free(directions);

    /* Set up reach-cut callback */
    memset(cut_ctx, 0, sizeof(FWReachCutContext));
    cut_ctx->problem = problem;
    cut_ctx->k = k;
    cut_ctx->z_start = z_start;

    fw_compute_reach_intervals(cut_ctx, problem, k);

    if (!(flags & FW_HINT_NO_REACH_CUTS) && cut_ctx->num_intervals > 0) {
        RalphCutCallback cb;
        cb.generate_cuts = fw_reach_cut_generate;
        cb.user_data = cut_ctx;
        ralph_set_cut_callback(model, &cb);
    }

    /* ================================================================
     * Presolve: Mandatory Station Fixing & Dominated Elimination
     *
     * 1. Single-station intervals: if a reach interval [s, e) has
     *    exactly one station, that station must be visited → z[s] = 1.
     *
     * 2. Dominated stations: if station i is strictly more expensive
     *    than both neighbors, and neighbors can reach each other
     *    directly (full tank), station i is never optimal → z[i] = 0.
     *    Only applied if removal doesn't leave any interval empty.
     * ================================================================ */

    int *is_mandatory = (int*)calloc(k, sizeof(int));
    int *is_fixed_zero = (int*)calloc(k, sizeof(int));

    if (is_mandatory && is_fixed_zero &&
        cut_ctx->interval_start && cut_ctx->interval_end) {

        /* Pass 1: Fix z[i] = 1 for single-station intervals */
        if (!(flags & FW_HINT_NO_MANDATORY_FIX)) {
            for (int iv = 0; iv < cut_ctx->num_intervals; iv++) {
                int start = cut_ctx->interval_start[iv];
                int end = cut_ctx->interval_end[iv];
                if (end - start == 1) {
                    is_mandatory[start] = 1;
                    ralph_set_var_bounds(model, z_start + start, 1.0, 1.0);
                }
            }
        }

        /* Pass 2: Dominated station elimination
         * Station i is dominated if:
         *   - Not mandatory
         *   - Has neighbors on both sides (0 < i < k-1)
         *   - Strictly more expensive than both neighbors
         *   - Neighbors can reach each other with a full tank
         *   - Removing i doesn't leave any interval with zero eligible stations
         */
        if (!(flags & FW_HINT_NO_DOMINATED_ELIM)) {
            for (int i = 1; i < k - 1; i++) {
                if (is_mandatory[i]) continue;

                double price_i = problem->stations[i].price;
                double price_left = problem->stations[i - 1].price;
                double price_right = problem->stations[i + 1].price;

                /* Must be strictly more expensive than both neighbors */
                if (price_i <= price_left || price_i <= price_right) continue;

                /* Neighbors must be able to reach each other directly */
                double consumed = fw_calc_fuel_consumed(problem,
                    problem->stations[i - 1].distance_from_start,
                    problem->stations[i + 1].distance_from_start);
                if (problem->tank_capacity - consumed < problem->minimum_fuel) continue;

                /* Safety: check that fixing z[i]=0 doesn't empty any interval */
                int safe = 1;
                for (int iv = 0; iv < cut_ctx->num_intervals; iv++) {
                    int s = cut_ctx->interval_start[iv];
                    int e = cut_ctx->interval_end[iv];
                    if (i >= s && i < e) {
                        /* Count other eligible stations in this interval */
                        int others = 0;
                        for (int j = s; j < e; j++) {
                            if (j != i && !is_fixed_zero[j]) others++;
                        }
                        if (others == 0) { safe = 0; break; }
                    }
                }

                if (safe) {
                    is_fixed_zero[i] = 1;
                    ralph_set_var_bounds(model, z_start + i, 0.0, 0.0);
                }
            }
        }
    }

    /* ================================================================
     * Symmetry Breaking: Equal-Price Adjacent Pairs
     *
     * For adjacent stations with identical prices, add z[i] >= z[i+1]
     * to prefer the earlier station. This eliminates symmetric solutions
     * where swapping stop decisions between equal-price neighbors
     * produces the same objective value.
     * ================================================================ */

    if (!(flags & FW_HINT_NO_SYMMETRY_BREAK)) {
        for (int i = 0; i < k - 1; i++) {
            /* Skip pairs where either station is already fixed */
            if (is_mandatory && (is_mandatory[i] || is_mandatory[i + 1])) continue;
            if (is_fixed_zero && (is_fixed_zero[i] || is_fixed_zero[i + 1])) continue;

            double price_diff = problem->stations[i].price - problem->stations[i + 1].price;
            if (price_diff < 0) price_diff = -price_diff;

            if (price_diff < 1e-6) {
                /* z[i] - z[i+1] >= 0: prefer earlier station */
                int indices[2] = {z_start + i, z_start + i + 1};
                double values[2] = {1.0, -1.0};
                ralph_add_constraint(model, 2, indices, values, RALPH_GREATER_EQUAL, 0.0);
            }
        }
    }

    free(is_mandatory);
    free(is_fixed_zero);
}

static void fw_free_cut_context(FWReachCutContext *ctx)
{
    if (!ctx) return;
    free(ctx->interval_start);
    free(ctx->interval_end);
    free(ctx->scratch_indices);
    free(ctx->scratch_coeffs);
    memset(ctx, 0, sizeof(FWReachCutContext));
}

/* ============================================================================
 * Refueling Problem Solving - MILP Model Builder
 *
 * Shared helper that builds the raw MILP model (without domain hints).
 * Used by both fw_solve_refuel_milp() and fw_export_milp_lp().
 *
 * Variable layout: x[0..k-1] purchases, y[k..2k-1] cumulative, z[2k..3k-1] binary
 * Returns RalphModel* on success, NULL on error.
 * ============================================================================ */

static RalphModel *fw_build_milp_model(const FWRefuelProblem *problem)
{
    int k = problem->num_stations;

    RalphModel *model = ralph_create();
    if (!model) return NULL;

    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    int x_start = 0;
    int y_start = k;
    int z_start = 2 * k;

    /* Add x[i] variables - fuel purchased */
    double est_price = problem->remaining_fuel_value;
    for (int i = 0; i < k; i++) {
        double obj_coeff = problem->stations[i].price - est_price;
        ralph_add_var(model, 0.0, problem->tank_capacity, obj_coeff, RALPH_CONTINUOUS);
    }

    /* Add y[i] variables - cumulative fuel before arriving */
    double y_upper = problem->current_fuel + k * problem->tank_capacity;
    for (int i = 0; i < k; i++) {
        ralph_add_var(model, 0.0, y_upper, 0.0, RALPH_CONTINUOUS);
    }

    /* Add z[i] variables - binary stop indicators with stop_cost in objective */
    for (int i = 0; i < k; i++) {
        ralph_add_var(model, 0.0, 1.0, problem->stop_cost, RALPH_BINARY);
    }

    /* Constraint: Fuel balance */
    for (int i = 0; i < k; i++) {
        int nnz = 1 + i;
        int *indices = malloc(nnz * sizeof(int));
        double *values = malloc(nnz * sizeof(double));
        if (!indices || !values) {
            free(indices);
            free(values);
            ralph_free(model);
            return NULL;
        }

        indices[0] = y_start + i;
        values[0] = 1.0;

        for (int j = 0; j < i; j++) {
            indices[1 + j] = x_start + j;
            values[1 + j] = -1.0;
        }

        ralph_add_constraint(model, nnz, indices, values, RALPH_EQUAL, problem->current_fuel);
        free(indices);
        free(values);
    }

    /* Constraint: Minimum fuel at arrival */
    for (int i = 0; i < k; i++) {
        int indices[1] = {y_start + i};
        double values[1] = {1.0};
        double fuel_consumed = fw_calc_fuel_consumed(problem, 0,
            problem->stations[i].distance_from_start);
        double rhs = problem->minimum_fuel + fuel_consumed;

        ralph_add_constraint(model, 1, indices, values, RALPH_GREATER_EQUAL, rhs);
    }

    /* Constraint: Tank capacity after refueling */
    for (int i = 0; i < k; i++) {
        int indices[2] = {y_start + i, x_start + i};
        double values[2] = {1.0, 1.0};
        double fuel_consumed = fw_calc_fuel_consumed(problem, 0,
            problem->stations[i].distance_from_start);
        double rhs = problem->tank_capacity + fuel_consumed;

        ralph_add_constraint(model, 2, indices, values, RALPH_LESS_EQUAL, rhs);
    }

    /* Constraint: Link x[i] to z[i] - x[i] <= max * z[i] */
    for (int i = 0; i < k; i++) {
        int indices[2] = {x_start + i, z_start + i};
        double values[2] = {1.0, -problem->tank_capacity};

        ralph_add_constraint(model, 2, indices, values, RALPH_LESS_EQUAL, 0.0);
    }

    /* Constraint: Minimum purchase if stopping - x[i] >= min_purchase * z[i] */
    if (problem->min_purchase > 0.01) {
        for (int i = 0; i < k; i++) {
            int indices[2] = {x_start + i, z_start + i};
            double values[2] = {1.0, -problem->min_purchase};

            ralph_add_constraint(model, 2, indices, values, RALPH_GREATER_EQUAL, 0.0);
        }
    }

    /* Constraint: Must reach destination */
    {
        int *indices = malloc(k * sizeof(int));
        double *values = malloc(k * sizeof(double));
        if (!indices || !values) {
            free(indices);
            free(values);
            ralph_free(model);
            return NULL;
        }

        for (int i = 0; i < k; i++) {
            indices[i] = x_start + i;
            values[i] = 1.0;
        }

        double total_fuel_needed = fw_calc_total_fuel_consumed(problem);
        double min_end = (problem->minimum_fuel_at_end > 0) ?
                         problem->minimum_fuel_at_end : problem->minimum_fuel;
        double rhs = min_end + total_fuel_needed - problem->current_fuel;

        if (rhs > 0) {
            ralph_add_constraint(model, k, indices, values, RALPH_GREATER_EQUAL, rhs);
        }

        free(indices);
        free(values);
    }

    return model;
}

/* ============================================================================
 * Export MILP as LP file (without domain hints)
 * ============================================================================ */

int fw_export_milp_lp(const FWRefuelProblem *problem, const char *path)
{
    if (!problem || !path) return -1;

    int k = problem->num_stations;
    if (k <= 0 || k > FW_MAX_STATIONS) return -1;

    RalphModel *model = fw_build_milp_model(problem);
    if (!model) return -1;

    int rc = ralph_write_lp(model, path);
    ralph_free(model);
    return rc;
}

/* ============================================================================
 * Refueling Problem Solving - MILP
 * ============================================================================ */

int fw_solve_refuel_milp(
    const FWRefuelProblem *problem,
    FWRefuelSolution *solution)
{
    if (!problem || !solution) return -1;

    /* Initialize solution */
    memset(solution, 0, sizeof(FWRefuelSolution));

    int k = problem->num_stations;
    if (k == 0) {
        solution->status = FW_STATUS_OPTIMAL;
        solution->total_cost = 0.0;
        solution->remaining_fuel = problem->current_fuel -
            fw_calc_total_fuel_consumed(problem);
        return 0;
    }

    /* Validate k to prevent integer overflow in allocations */
    if (k < 0 || k > FW_MAX_STATIONS) {
        solution->status = FW_STATUS_ERROR;
        return -1;
    }

    int num_vars = 3 * k;
    int x_start = 0;
    int z_start = 2 * k;
    double est_price = problem->remaining_fuel_value;

    /* Build the raw MILP model */
    RalphModel *model = fw_build_milp_model(problem);
    if (!model) {
        solution->status = FW_STATUS_ERROR;
        return -1;
    }

    /* Apply domain-specific MIP hints: priorities, directions, reach cuts */
    FWReachCutContext cut_ctx;
    fw_setup_mip_hints(model, problem, k, z_start, &cut_ctx);

    /* Solve */
    int fw_verbose = (getenv("FW_VERBOSE") != NULL);
    ralph_set_int_param(model, "verbose", fw_verbose ? 1 : 0);
    if (fw_presolve) {
        ralph_set_int_param(model, "presolve", 1);
        ralph_set_int_param(model, "presolve_mask", (int)fw_presolve_mask);
    }
    int ret = ralph_optimize(model);
    RalphStatus status = ralph_get_status(model);
    if (fw_verbose) {
        printf("  [fw] k=%d vars=%d nodes=%d status=%d\n",
               k, num_vars, ralph_get_node_count(model), (int)status);
    }

    fw_free_cut_context(&cut_ctx);

    if (ret == 0 && status == RALPH_STATUS_OPTIMAL) {
        double *x = malloc(num_vars * sizeof(double));
        if (!x) {
            ralph_free(model);
            solution->status = FW_STATUS_ERROR;
            return -1;
        }
        ralph_get_solution(model, x);

        /* Allocate solution arrays */
        solution->purchases = malloc(k * sizeof(double));
        solution->stop_flags = malloc(k * sizeof(int));
        if (!solution->purchases || !solution->stop_flags) {
            free(x);
            free(solution->purchases);
            free(solution->stop_flags);
            ralph_free(model);
            solution->status = FW_STATUS_ERROR;
            return -1;
        }

        double gross_cost = 0.0;
        double total_purchased = 0.0;
        double stop_costs = 0.0;
        int num_stops = 0;

        for (int i = 0; i < k; i++) {
            solution->purchases[i] = x[x_start + i];
            solution->stop_flags[i] = (x[z_start + i] > 0.5) ? 1 : 0;

            gross_cost += solution->purchases[i] * problem->stations[i].price;
            total_purchased += solution->purchases[i];
            stop_costs += x[z_start + i] * problem->stop_cost;

            if (solution->stop_flags[i]) {
                num_stops++;
            }
        }

        /* Calculate remaining fuel and total cost */
        double total_consumed = fw_calc_total_fuel_consumed(problem);
        solution->remaining_fuel = problem->current_fuel + total_purchased - total_consumed;
        solution->gross_cost = gross_cost;

        if (est_price > 0.0) {
            double empty_capacity = problem->tank_capacity - solution->remaining_fuel;
            solution->total_cost = gross_cost + stop_costs + empty_capacity * est_price;
        } else {
            solution->total_cost = gross_cost + stop_costs;
        }

        solution->num_stops = num_stops;
        solution->status = FW_STATUS_OPTIMAL;

        free(x);
        ralph_free(model);
        return 0;
    }

    /* Handle failure */
    if (status == RALPH_STATUS_INFEASIBLE) {
        solution->status = FW_STATUS_INFEASIBLE;
    } else {
        solution->status = FW_STATUS_ERROR;
    }

    ralph_free(model);
    return -1;
}

/* ============================================================================
 * Refueling Problem Solving - Benders Decomposition
 *
 * True Benders decomposition using ralph_solve_benders():
 * - Master problem: binary z[i] variables (stop decisions) + θ (recourse cost)
 * - Subproblem: continuous x[i] (purchases), y[i] (cumulative fuel)
 * - Linking constraints: x[i] <= tank_capacity * z[i], x[i] >= min_purchase * z[i]
 *
 * This scales to k > 30 stations where enumeration (2^k) would timeout.
 * ============================================================================ */

/* Threshold for using Benders vs MILP. 0 = always use Benders.
 * Note: global configuration — set once at startup before solving.
 * NOT thread-safe if modified concurrently with fw_solve_refuel_benders(). */
static int fw_benders_threshold = 0;

/*
 * Set the threshold for using Benders decomposition.
 * If num_stations > threshold, use Benders; otherwise use MILP.
 * Set to 0 to always use Benders (default).
 */
void fw_set_benders_threshold(int threshold) {
    fw_benders_threshold = threshold;
}

int fw_get_benders_threshold(void) {
    return fw_benders_threshold;
}

int fw_solve_refuel_benders(
    const FWRefuelProblem *problem,
    FWRefuelSolution *solution)
{
    if (!problem || !solution) return -1;

    memset(solution, 0, sizeof(FWRefuelSolution));

    int k = problem->num_stations;
    if (k == 0) {
        solution->status = FW_STATUS_OPTIMAL;
        solution->total_cost = 0.0;
        solution->remaining_fuel = problem->current_fuel -
            fw_calc_total_fuel_consumed(problem);
        return 0;
    }

    /* Validate k to prevent integer overflow in allocations */
    if (k < 0 || k > FW_MAX_STATIONS) {
        solution->status = FW_STATUS_ERROR;
        return -1;
    }

    /* If no integer constraints needed, use LP directly */
    if (problem->min_purchase < 0.01 && problem->stop_cost < 0.01) {
        return fw_solve_refuel_lp(problem, solution);
    }

    /* Use MILP for small k if threshold is set */
    if (fw_benders_threshold > 0 && k <= fw_benders_threshold) {
        return fw_solve_refuel_milp(problem, solution);
    }

    /*
     * Build full MILP model for Benders decomposition.
     *
     * Variable layout:
     *   x[0..k-1]     - fuel purchased at each station (continuous, subproblem)
     *   y[k..2k-1]    - cumulative fuel before arriving (continuous, subproblem)
     *   z[2k..3k-1]   - stop decisions (binary, master)
     *   θ = 3k        - recourse cost (continuous, master)
     *
     * Constraints:
     *   - Fuel balance: y[i] = current_fuel + sum(x[j] for j < i)
     *   - Min fuel at arrival: y[i] >= min_fuel + consumed_to_i
     *   - Tank capacity: y[i] + x[i] <= tank_capacity + consumed_to_i
     *   - Linking upper: x[i] <= tank_capacity * z[i]
     *   - Linking lower: x[i] >= min_purchase * z[i] (if min_purchase > 0)
     *   - Reach destination: sum(x[i]) >= total_needed
     */

    int x_start = 0;
    int y_start = k;
    int z_start = 2 * k;
    int theta_idx = 3 * k;
    int num_vars = 3 * k + 1;

    RalphModel *model = ralph_create();
    if (!model) {
        solution->status = FW_STATUS_ERROR;
        return -1;
    }

    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    /* Add x[i] variables - fuel purchased (subproblem) */
    double est_price = problem->remaining_fuel_value;
    for (int i = 0; i < k; i++) {
        double obj_coeff = problem->stations[i].price - est_price;
        ralph_add_var(model, 0.0, problem->tank_capacity, obj_coeff, RALPH_CONTINUOUS);
    }

    /* Add y[i] variables - cumulative fuel (subproblem) */
    double y_upper = problem->current_fuel + k * problem->tank_capacity;
    for (int i = 0; i < k; i++) {
        ralph_add_var(model, 0.0, y_upper, 0.0, RALPH_CONTINUOUS);
    }

    /* Add z[i] variables - stop decisions (master) with stop_cost in objective */
    for (int i = 0; i < k; i++) {
        ralph_add_var(model, 0.0, 1.0, problem->stop_cost, RALPH_BINARY);
    }

    /* Add θ variable - recourse cost (master) */
    /* Bounds: θ represents total fuel cost/credit. Use reasonable bounds to avoid
     * numerical issues in the LP solver. The bounds are based on:
     * - max_cost = k * tank_capacity * max_price
     * - max_credit = k * tank_capacity * remaining_fuel_value
     * Using 100x safety margin for robustness. */
    double max_price = 0.0;
    for (int i = 0; i < k; i++) {
        if (problem->stations[i].price > max_price) {
            max_price = problem->stations[i].price;
        }
    }
    double max_fuel_value = k * problem->tank_capacity *
        (max_price > est_price ? max_price : est_price);
    double theta_bound = 100.0 * (max_fuel_value + 1.0);  /* Safety margin */
    double theta_lb = -theta_bound;
    double theta_ub = theta_bound;
    ralph_add_var(model, theta_lb, theta_ub, 1.0, RALPH_CONTINUOUS);

    /* Constraint: Fuel balance - y[i] = current_fuel + sum(x[j] for j < i) */
    for (int i = 0; i < k; i++) {
        int nnz = 1 + i;
        int *indices = malloc(nnz * sizeof(int));
        double *values = malloc(nnz * sizeof(double));
        if (!indices || !values) {
            free(indices);
            free(values);
            ralph_free(model);
            solution->status = FW_STATUS_ERROR;
            return -1;
        }

        indices[0] = y_start + i;
        values[0] = 1.0;

        for (int j = 0; j < i; j++) {
            indices[1 + j] = x_start + j;
            values[1 + j] = -1.0;
        }

        ralph_add_constraint(model, nnz, indices, values, RALPH_EQUAL, problem->current_fuel);
        free(indices);
        free(values);
    }

    /* Constraint: Minimum fuel at arrival */
    for (int i = 0; i < k; i++) {
        int indices[1] = {y_start + i};
        double values[1] = {1.0};
        double fuel_consumed = fw_calc_fuel_consumed(problem, 0,
            problem->stations[i].distance_from_start);
        double rhs = problem->minimum_fuel + fuel_consumed;

        ralph_add_constraint(model, 1, indices, values, RALPH_GREATER_EQUAL, rhs);
    }

    /* Constraint: Tank capacity after refuel */
    for (int i = 0; i < k; i++) {
        int indices[2] = {y_start + i, x_start + i};
        double values[2] = {1.0, 1.0};
        double fuel_consumed = fw_calc_fuel_consumed(problem, 0,
            problem->stations[i].distance_from_start);
        double rhs = problem->tank_capacity + fuel_consumed;

        ralph_add_constraint(model, 2, indices, values, RALPH_LESS_EQUAL, rhs);
    }

    /* Linking constraint: x[i] <= tank_capacity * z[i]
     * Rewritten as: x[i] - tank_capacity * z[i] <= 0 */
    for (int i = 0; i < k; i++) {
        int indices[2] = {x_start + i, z_start + i};
        double values[2] = {1.0, -problem->tank_capacity};

        ralph_add_constraint(model, 2, indices, values, RALPH_LESS_EQUAL, 0.0);
    }

    /* Linking constraint: x[i] >= min_purchase * z[i] (if min_purchase > 0)
     * Rewritten as: x[i] - min_purchase * z[i] >= 0 */
    if (problem->min_purchase > 0.01) {
        for (int i = 0; i < k; i++) {
            int indices[2] = {x_start + i, z_start + i};
            double values[2] = {1.0, -problem->min_purchase};

            ralph_add_constraint(model, 2, indices, values, RALPH_GREATER_EQUAL, 0.0);
        }
    }

    /* Constraint: Reach destination */
    {
        int *indices = malloc(k * sizeof(int));
        double *values = malloc(k * sizeof(double));
        if (!indices || !values) {
            free(indices);
            free(values);
            ralph_free(model);
            solution->status = FW_STATUS_ERROR;
            return -1;
        }

        for (int i = 0; i < k; i++) {
            indices[i] = x_start + i;
            values[i] = 1.0;
        }

        double total_fuel_needed = fw_calc_total_fuel_consumed(problem);
        double min_end = (problem->minimum_fuel_at_end > 0) ?
                         problem->minimum_fuel_at_end : problem->minimum_fuel;
        double rhs = min_end + total_fuel_needed - problem->current_fuel;

        if (rhs > 0) {
            ralph_add_constraint(model, k, indices, values, RALPH_GREATER_EQUAL, rhs);
        }

        free(indices);
        free(values);
    }

    /* Add a trivial master constraint: sum(z[i]) >= 0
     * This is always satisfied (z[i] >= 0 by definition) but needed because
     * Benders requires at least one pure-master constraint to work correctly.
     * Without this, all constraints are linking (involve both z and x).
     */
    {
        int *indices = malloc(k * sizeof(int));
        double *values = malloc(k * sizeof(double));
        if (!indices || !values) {
            free(indices);
            free(values);
            ralph_free(model);
            solution->status = FW_STATUS_ERROR;
            return -1;
        }

        for (int i = 0; i < k; i++) {
            indices[i] = z_start + i;
            values[i] = 1.0;
        }

        ralph_add_constraint(model, k, indices, values, RALPH_GREATER_EQUAL, 0.0);
        free(indices);
        free(values);
    }

    /* Add reach cuts: if we can't reach station i without refueling,
     * at least one station j < i must be visited.
     *
     * For each station i where:
     *   current_fuel - fuel_consumed_to(station_i) < minimum_fuel
     * We add:
     *   sum(z[j] for j < i) >= 1
     *
     * These are domain-specific valid inequalities that strengthen the master
     * problem and often eliminate infeasible solutions early, reducing Benders
     * iterations.
     */
    for (int i = 1; i < k; i++) {
        double fuel_consumed = fw_calc_fuel_consumed(problem, 0,
            problem->stations[i].distance_from_start);
        double fuel_available = problem->current_fuel - fuel_consumed;

        if (fuel_available < problem->minimum_fuel) {
            /* Can't reach station i without refueling - need at least one stop before */
            int *indices = malloc(i * sizeof(int));
            double *values = malloc(i * sizeof(double));
            if (!indices || !values) {
                free(indices);
                free(values);
                ralph_free(model);
                solution->status = FW_STATUS_ERROR;
                return -1;
            }

            for (int j = 0; j < i; j++) {
                indices[j] = z_start + j;
                values[j] = 1.0;
            }

            ralph_add_constraint(model, i, indices, values, RALPH_GREATER_EQUAL, 1.0);
            free(indices);
            free(values);
        }
    }

    /* Inter-station reach cuts: from station j with full tank, if can't reach
     * station i, add sum(z[l] for l in (j, i)) >= 1 as a static constraint.
     * More effective than dynamic cuts for Benders because they constrain
     * the master problem from iteration 1. */
    for (int j = 0; j < k; j++) {
        for (int i = j + 2; i < k; i++) {
            double consumed = fw_calc_fuel_consumed(problem,
                problem->stations[j].distance_from_start,
                problem->stations[i].distance_from_start);
            double fuel_at_i = problem->tank_capacity - consumed;

            if (fuel_at_i < problem->minimum_fuel) {
                /* Need at least one stop in (j, i) */
                int span = i - j - 1;
                if (span > 0) {
                    int *indices = malloc(span * sizeof(int));
                    double *values = malloc(span * sizeof(double));
                    if (!indices || !values) {
                        free(indices);
                        free(values);
                        ralph_free(model);
                        solution->status = FW_STATUS_ERROR;
                        return -1;
                    }

                    for (int l = 0; l < span; l++) {
                        indices[l] = z_start + j + 1 + l;
                        values[l] = 1.0;
                    }

                    ralph_add_constraint(model, span, indices, values,
                                         RALPH_GREATER_EQUAL, 1.0);
                    free(indices);
                    free(values);
                }
                break;  /* further stations are even farther */
            }
        }
    }

    /* Configure Benders decomposition */
    int *master_vars = malloc(k * sizeof(int));
    if (!master_vars) {
        ralph_free(model);
        solution->status = FW_STATUS_ERROR;
        return -1;
    }

    /* Master variables are z[0..k-1] (at indices z_start to z_start+k-1) */
    for (int i = 0; i < k; i++) {
        master_vars[i] = z_start + i;
    }

    RalphBendersConfig config = RALPH_BENDERS_CONFIG_DEFAULT;
    config.master_var_indices = master_vars;
    config.num_master_vars = k;
    config.theta_var = theta_idx;
    config.verbose = 0;

    /* Compute priorities and directions for Benders master (original var space) */
    int *benders_priorities = (int*)calloc(num_vars, sizeof(int));
    int *benders_directions = (int*)calloc(num_vars, sizeof(int));
    if (benders_priorities && benders_directions) {
        /* Compute median price */
        double *prices = (double*)malloc(k * sizeof(double));
        double median_price = 0.0;
        if (prices) {
            for (int i = 0; i < k; i++) prices[i] = problem->stations[i].price;
            for (int i = 0; i < k - 1; i++) {
                for (int j = i + 1; j < k; j++) {
                    if (prices[j] < prices[i]) {
                        double tmp = prices[i]; prices[i] = prices[j]; prices[j] = tmp;
                    }
                }
            }
            median_price = prices[k / 2];
            free(prices);
        }

        for (int i = 0; i < k; i++) {
            double price = problem->stations[i].price;
            benders_priorities[z_start + i] =
                (price > 0.001) ? (int)(1000.0 / price) : 1000;
            benders_directions[z_start + i] =
                (price <= median_price) ? RALPH_BRANCH_UP : RALPH_BRANCH_DOWN;
        }

        config.branch_priorities = benders_priorities;
        config.branch_directions = benders_directions;
    }

    /* Solve with Benders */
    double *x = malloc(num_vars * sizeof(double));
    RalphBendersResult result;

    if (!x) {
        free(benders_priorities);
        free(benders_directions);
        free(master_vars);
        ralph_free(model);
        solution->status = FW_STATUS_ERROR;
        return -1;
    }

    int ret = ralph_solve_benders(model, &config, x, &result);

    free(benders_priorities);
    free(benders_directions);
    free(master_vars);

    if (ret == 0 && result.status == RALPH_STATUS_OPTIMAL) {
        /* Allocate solution arrays */
        solution->purchases = malloc(k * sizeof(double));
        solution->stop_flags = malloc(k * sizeof(int));
        if (!solution->purchases || !solution->stop_flags) {
            free(x);
            free(solution->purchases);
            free(solution->stop_flags);
            ralph_free(model);
            solution->status = FW_STATUS_ERROR;
            return -1;
        }

        double gross_cost = 0.0;
        double total_purchased = 0.0;
        double stop_costs = 0.0;
        int num_stops = 0;

        for (int i = 0; i < k; i++) {
            solution->purchases[i] = x[x_start + i];
            solution->stop_flags[i] = (x[z_start + i] > 0.5) ? 1 : 0;

            gross_cost += solution->purchases[i] * problem->stations[i].price;
            total_purchased += solution->purchases[i];
            stop_costs += solution->stop_flags[i] * problem->stop_cost;

            if (solution->stop_flags[i]) {
                num_stops++;
            }
        }

        double total_consumed = fw_calc_total_fuel_consumed(problem);
        solution->remaining_fuel = problem->current_fuel + total_purchased - total_consumed;
        solution->gross_cost = gross_cost;

        if (est_price > 0.0) {
            double empty_capacity = problem->tank_capacity - solution->remaining_fuel;
            solution->total_cost = gross_cost + stop_costs + empty_capacity * est_price;
        } else {
            solution->total_cost = gross_cost + stop_costs;
        }

        solution->num_stops = num_stops;
        solution->status = FW_STATUS_OPTIMAL;

        free(x);
        ralph_free(model);
        return 0;
    }

    /* Handle failure */
    free(x);
    ralph_free(model);

    if (result.status == RALPH_STATUS_INFEASIBLE) {
        solution->status = FW_STATUS_INFEASIBLE;
    } else {
        solution->status = FW_STATUS_ERROR;
    }

    return -1;
}

/* ============================================================================
 * Solution Management
 * ============================================================================ */

void fw_free_solution(FWRefuelSolution *solution)
{
    if (solution) {
        free(solution->purchases);
        free(solution->stop_flags);
        solution->purchases = NULL;
        solution->stop_flags = NULL;
    }
}

/* ============================================================================
 * Problem Validation
 * ============================================================================ */

int fw_validate_problem(
    const FWRefuelProblem *problem,
    char *error_msg,
    int error_msg_size)
{
    if (!problem) {
        if (error_msg) snprintf(error_msg, error_msg_size, "Problem is NULL");
        return 0;
    }

    /* Check basic parameters */
    if (problem->tank_capacity <= 0) {
        if (error_msg) snprintf(error_msg, error_msg_size, "Invalid tank capacity");
        return 0;
    }

    if (problem->current_fuel < 0 || problem->current_fuel > problem->tank_capacity) {
        if (error_msg) snprintf(error_msg, error_msg_size, "Invalid current fuel level");
        return 0;
    }

    if (problem->minimum_fuel < 0 || problem->minimum_fuel > problem->tank_capacity) {
        if (error_msg) snprintf(error_msg, error_msg_size, "Invalid minimum fuel level");
        return 0;
    }

    if (problem->base_consumption <= 0 && problem->num_segments == 0) {
        if (error_msg) snprintf(error_msg, error_msg_size, "Invalid consumption rate");
        return 0;
    }

    /* Check stations are sorted */
    for (int i = 1; i < problem->num_stations; i++) {
        if (problem->stations[i].distance_from_start <
            problem->stations[i-1].distance_from_start) {
            if (error_msg) snprintf(error_msg, error_msg_size,
                "Stations not sorted by distance");
            return 0;
        }
    }

    /* Check feasibility: can we reach each station? */
    double fuel = problem->current_fuel;
    double last_dist = 0;

    for (int i = 0; i < problem->num_stations; i++) {
        double consumed = fw_calc_fuel_consumed(problem, last_dist,
            problem->stations[i].distance_from_start);

        fuel -= consumed;
        if (fuel < problem->minimum_fuel) {
            /* Could we reach with a full tank? */
            double max_fuel = problem->tank_capacity;
            double max_consumed = fw_calc_fuel_consumed(problem, last_dist,
                problem->stations[i].distance_from_start);
            if (max_fuel - max_consumed < problem->minimum_fuel) {
                if (error_msg) snprintf(error_msg, error_msg_size,
                    "Cannot reach station %d: too far from previous station",
                    problem->stations[i].station_id);
                return 0;
            }
        }

        /* Refuel to max */
        fuel = problem->tank_capacity;
        last_dist = problem->stations[i].distance_from_start;
    }

    /* Check we can reach destination */
    double final_consumed = fw_calc_fuel_consumed(problem, last_dist, problem->total_distance);
    fuel -= final_consumed;
    if (fuel < problem->minimum_fuel_at_end) {
        double max_fuel = problem->tank_capacity;
        if (max_fuel - final_consumed < problem->minimum_fuel_at_end) {
            if (error_msg) snprintf(error_msg, error_msg_size,
                "Cannot reach destination: too far from last station");
            return 0;
        }
    }

    return 1;
}
