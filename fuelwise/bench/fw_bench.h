/*
 * FuelWise Validation Benchmark
 *
 * Problem generation, constraint validation, and benchmark driver
 * for testing FuelWise solver correctness.
 *
 * Copyright (c) 2024-2026. All rights reserved.
 */

#ifndef FW_BENCH_H
#define FW_BENCH_H

#include "fw_types.h"
#include "fw_consumption.h"
#include "sh_dist.h"
#include "sh_units.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Solver Type Enumeration
 * ============================================================================ */

typedef enum {
    FW_SOLVER_LP = 0,       /* Pure LP (no stop costs) */
    FW_SOLVER_MILP = 1,     /* Branch and bound MILP */
    FW_SOLVER_BENDERS = 2   /* Benders decomposition */
} FWSolverType;

/* ============================================================================
 * Problem Generation Configuration
 * ============================================================================ */

typedef struct {
    /* Route parameters */
    double route_length_m;          /* Total route length (meters) */

    /* Station distribution (uses sh_rng_gamma from shared/) */
    double mean_station_gap_m;      /* Mean gap between stations */
    double gap_shape;               /* Gamma distribution shape (2.0-3.0 typical) */

    /* Truck parameters */
    double tank_capacity_l;         /* Tank capacity (liters) */
    double tare_weight_kg;          /* Empty truck weight */
    double max_gvw_kg;              /* Maximum gross vehicle weight */

    /* Consumption curve (uses FWConsumptionCurve wrapping SHPiecewiseLinear) */
    FWConsumptionCurve *curve;      /* NULL = use default EU truck */

    /* Weight events (uses SHStepFunc internally) */
    int num_weight_events;          /* 0 = constant weight at tare */
    double cargo_weight_mean_kg;    /* Mean cargo per pickup */
    double cargo_weight_stddev_kg;  /* Cargo weight variance */

    /* Fuel parameters */
    double min_fuel_l;              /* Minimum fuel level to maintain */
    double min_purchase_l;          /* Minimum purchase per stop (0 = LP, >0 = MILP) */
    double stop_cost;               /* Fixed cost per stop (0 = no stop penalty) */
    double start_fuel_fraction;     /* Starting fuel as fraction of tank (0.3-0.8) */

    /* Price distribution (AR(1) spatial correlation model)
     *   price[i] = base_price + correlation * (price[i-1] - base_price) + noise
     *   where noise ~ Normal(0, stddev * sqrt(1 - correlation^2))
     */
    double base_price_per_l;        /* Mean fuel price ($/L) */
    double price_stddev;            /* Price standard deviation */
    double price_correlation;       /* Spatial correlation (0-1) */

    /* RNG (uses SHRng from shared/ - pluggable backend) */
    SHRngType rng_type;             /* Default: SH_RNG_XORSHIFT128 */
    uint64_t seed;                  /* For reproducibility */

    /* Unit system (for display/reporting only - internal always metric) */
    SHUnitSystem units;             /* Default: SH_UNITS_METRIC */

    /* GLPK comparison mode (bench-only) */
    int glpk_compare;               /* 1 to enable GLPK comparison */
} FWBenchConfig;

/* ============================================================================
 * Generated Problem Instance
 * ============================================================================ */

typedef struct {
    FWRefuelProblem problem;
    FWSnappedStation *stations;
    int num_stations;
    FWConsumptionCurve *curve;
    FWWeightProfile *weight_profile;
    double total_fuel_required;     /* For reference */
} FWBenchInstance;

/* ============================================================================
 * Validation Result
 * ============================================================================ */

typedef struct {
    /* Overall result */
    int feasible;                   /* 1 if all constraints satisfied */

    /* Individual constraint status */
    int fuel_balance_ok;            /* All balance equations hold */
    int min_fuel_ok;                /* Never below minimum */
    int tank_capacity_ok;           /* Never exceeded tank */
    int non_negative_purchase_ok;   /* All purchases >= 0 */
    int reaches_destination;        /* Ends with enough fuel */
    int min_purchase_ok;            /* Meets minimum purchase (if set) */
    int stop_flags_ok;              /* Only stopped at allowed stations (MILP) */
    int stop_cost_ok;               /* Stop costs correctly applied (MILP) */
    int cost_ok;                    /* Total cost calculation correct */

    /* Diagnostics */
    double min_fuel_observed;       /* Lowest fuel level seen */
    double max_fuel_observed;       /* Highest fuel level seen */
    int first_violation_station;    /* -1 if none, else station index */
    char error_msg[256];            /* Description of first violation */
} FWValidationResult;

/* ============================================================================
 * Benchmark Results
 * ============================================================================ */

typedef struct {
    /* Counts */
    int num_runs;
    int num_solved;
    int num_feasible;               /* Solutions that passed validation */
    int num_optimal;                /* LP optimal solutions */
    int num_infeasible;             /* Problems solver reported infeasible */
    int num_errors;                 /* Solver errors */

    /* Timing (milliseconds) */
    double solve_time_avg;
    double solve_time_min;
    double solve_time_max;
    double validate_time_avg;

    /* Solution quality */
    double cost_avg;
    double cost_min;
    double cost_max;
    double stops_avg;

    /* GLPK comparison (only populated when glpk_compare enabled) */
    int glpk_enabled;
    int glpk_num_attempted;
    int glpk_num_solved;
    int glpk_num_failed;
    int glpk_num_match;             /* Objectives match within tolerance */
    double glpk_solve_time_avg;
    double glpk_solve_time_min;
    double glpk_solve_time_max;
    double glpk_speedup_avg;        /* GLPK time / Ralph time (> 1 means Ralph faster) */

    /* Objective gap vs GLPK: (ralph_obj - glpk_obj) / glpk_obj * 100 */
    double glpk_gap_min_pct;        /* Best case (smallest gap) */
    double glpk_gap_avg_pct;        /* Average gap */
    double glpk_gap_max_pct;        /* Worst case (largest gap) */
    int glpk_gap_count;             /* Number of instances with valid gap */

    /* MIP telemetry averages (MILP/Benders runs that populate solution.mip) */
    int mip_samples;
    double mip_nodes_avg;
    double mip_root_lp_time_ms_avg;
    double mip_node_lp_time_ms_avg;
    double mip_node_lp_warm_time_ms_avg;
    double mip_node_lp_cold_time_ms_avg;
    double mip_strong_branch_time_ms_avg;
    double mip_strong_branch_probes_avg;
    double mip_cold_starts_avg;
    double mip_probe_child_snapshots_saved_avg;
    double mip_probe_child_warm_applied_avg;
    double mip_relaxation_basis_warm_applied_avg;
    double mip_saved_basis_live_restore_attempted_avg;
    double mip_saved_basis_warm_reopt_succeeded_avg;
    double mip_saved_basis_fallback_no_tableau_avg;
    double mip_saved_basis_fallback_artificial_skip_avg;
    double mip_saved_basis_fallback_size_mismatch_avg;
    double mip_saved_basis_fallback_live_restore_not_attempted_avg;
    double mip_node_basis_staged_avg;
    double mip_warm_rejects_avg;
    double mip_cold_start_no_saved_basis_avg;
    double mip_cold_start_saved_basis_fallback_avg;
    double mip_cold_start_probe_restore_failure_avg;
    double mip_cold_start_live_restore_failure_avg;
    double mip_cold_start_warm_reopt_failure_avg;
    double mip_cold_start_stage_retry_avg;
    double mip_cold_start_branch_recovery_avg;
    double mip_root_cuts_applied_avg;
    double mip_non_root_cuts_generated_avg;
    double mip_non_root_cuts_applied_avg;
    double mip_fathom_lp_infeasible_avg;
    double mip_fathom_bound_avg;
    double mip_fathom_integral_avg;
    double mip_fathom_no_branch_var_avg;
} FWBenchResults;

/* ============================================================================
 * Problem Generation API
 * ============================================================================ */

/*
 * Generate a satisfiable problem instance.
 *
 * The generator ensures:
 * 1. No gap exceeds (tank_capacity - min_fuel) / max_consumption
 * 2. Starting fuel can reach first station
 * 3. Last station can reach destination
 *
 * Parameters:
 *   config - Generation parameters
 *   out    - Output: generated instance
 *
 * Returns:
 *   0 on success, -1 on error
 */
int fw_bench_generate(const FWBenchConfig *config, FWBenchInstance *out);

/*
 * Free instance resources.
 */
void fw_bench_free_instance(FWBenchInstance *instance);

/* ============================================================================
 * Preset Configurations
 * ============================================================================ */

/* Short urban route: 200km, dense stations, light truck */
FWBenchConfig fw_bench_config_short_urban(void);

/* Highway route: 800km, typical truck, 4 weight events */
FWBenchConfig fw_bench_config_highway(void);

/* Long haul: 2000km, sparse rural stations */
FWBenchConfig fw_bench_config_long_haul(void);

/* Tight margins: 500km, challenging constraints */
FWBenchConfig fw_bench_config_tight_margins(void);

/* US Interstate: 2000 miles, Class 8 truck (imperial units) */
FWBenchConfig fw_bench_config_us_interstate(void);

/* Benders benchmarks: fixed station counts for scalability testing */
FWBenchConfig fw_bench_config_benders_30(void);   /* 30 stations */
FWBenchConfig fw_bench_config_benders_50(void);   /* 50 stations */
FWBenchConfig fw_bench_config_benders_100(void);  /* 100 stations */

/* MIP benchmarks: stop_cost + min_purchase, scaling from trivial to stress */
FWBenchConfig fw_bench_config_milp_15(void);      /* ~15 stations, sub-ms sanity */
FWBenchConfig fw_bench_config_milp_30(void);      /* ~30 stations, high stop cost */
FWBenchConfig fw_bench_config_milp_50(void);      /* ~50 stations, high price variance */
FWBenchConfig fw_bench_config_milp_75(void);      /* ~75 stations, tight tank (reach cuts) */
FWBenchConfig fw_bench_config_milp_100(void);     /* ~100 stations, scalability */
FWBenchConfig fw_bench_config_milp_200(void);     /* ~200 stations, stress test */

/* ============================================================================
 * Validation API
 * ============================================================================ */

/*
 * Validate a solution against problem constraints.
 *
 * This is an independent check that does not use solver internals.
 * It simulates the truck driving the route with the given purchases.
 *
 * Parameters:
 *   problem  - The refueling problem
 *   solution - The solution to validate
 *   curve    - Consumption curve (NULL = use base_consumption from problem)
 *   profile  - Weight profile (NULL = constant weight at tare)
 *   result   - Output: detailed validation result
 *
 * Returns:
 *   1 if feasible, 0 if any constraint violated
 */
int fw_validate_solution(
    const FWRefuelProblem *problem,
    const FWRefuelSolution *solution,
    const FWConsumptionCurve *curve,
    const FWWeightProfile *profile,
    FWValidationResult *result
);

/* ============================================================================
 * Benchmark Driver API
 * ============================================================================ */

/*
 * Run benchmark with given configuration.
 *
 * Parameters:
 *   config      - Problem generation config
 *   num_runs    - Number of problems to generate and solve
 *   solver_type - Which solver to use (LP, MILP, or Benders)
 *   results     - Output: benchmark statistics
 *
 * Returns:
 *   0 on success, -1 on fatal error
 */
int fw_bench_run(
    const FWBenchConfig *config,
    int num_runs,
    FWSolverType solver_type,
    FWBenchResults *results
);

/*
 * Print benchmark results to stdout.
 *
 * Parameters:
 *   config     - Configuration used
 *   results    - Results to print
 *   scenario   - Scenario name (for display)
 *   as_json    - 1 for JSON output, 0 for text
 */
void fw_bench_print_results(
    const FWBenchConfig *config,
    const FWBenchResults *results,
    const char *scenario,
    int as_json
);

#ifdef __cplusplus
}
#endif

#endif /* FW_BENCH_H */
