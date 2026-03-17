/*
 * FuelWise - Truck Refueling Optimization Library
 * Type Definitions
 *
 * Copyright (c) 2024. All rights reserved.
 */

#ifndef FW_TYPES_H
#define FW_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * API Annotations
 * ============================================================================ */

/*
 * FW_FUTURE_API - Marks functions reserved for future use.
 * These are part of the public API but not yet used by the library,
 * WASM bindings, or API server. They may be used by external code.
 */
#define FW_FUTURE_API  /* Reserved for future use */

/* ============================================================================
 * Shared Library Dependencies
 * ============================================================================ */

#include "sh_geo.h"    /* For SHCoord */
#include "sh_units.h"  /* For unit conversions */

/* ============================================================================
 * Constants (deprecated - use shared library constants)
 * ============================================================================ */

/* Keep FW_DEG_TO_RAD and FW_RAD_TO_DEG for backward compatibility,
 * but prefer SH_DEG_TO_RAD and SH_RAD_TO_DEG from sh_geo.h */
#define FW_DEG_TO_RAD SH_DEG_TO_RAD
#define FW_RAD_TO_DEG SH_RAD_TO_DEG

/* ============================================================================
 * Status Codes
 * ============================================================================ */

typedef enum {
    FW_STATUS_OPTIMAL = 0,      /* Solution found */
    FW_STATUS_INFEASIBLE = 1,   /* No feasible solution exists */
    FW_STATUS_UNBOUNDED = 2,    /* Problem is unbounded */
    FW_STATUS_ERROR = 3,        /* Solver error */
    FW_STATUS_ITERATION_LIMIT = 4,  /* Hit iteration limit */
    FW_STATUS_TIME_LIMIT = 5    /* Hit time limit */
} FWStatus;

/* ============================================================================
 * Deduplication Strategies
 *
 * When a station appears multiple times along a route (e.g., due to loop-backs),
 * these strategies determine which occurrence to keep.
 * ============================================================================ */

typedef enum {
    FW_DEDUP_NONE = 0,      /* No deduplication - keep all occurrences */
    FW_DEDUP_FIRST = 1,     /* Keep first occurrence in a sequence */
    FW_DEDUP_LAST = 2,      /* Keep last occurrence in a sequence */
    FW_DEDUP_CLOSEST = 3    /* Keep the one closest to the polyline */
} FWDedupStrategy;

/* ============================================================================
 * Coordinate Point
 *
 * FWCoord is now an alias to SHCoord from the shared library.
 * Both have the same layout: { double lat; double lon; }
 * ============================================================================ */

typedef SHCoord FWCoord;

/* ============================================================================
 * Internal Unit System
 *
 * FuelWise uses SI units internally for all calculations:
 *   - Distance: meters (m)
 *   - Volume: liters (L)
 *   - Fuel efficiency: L/100km (liters per 100 kilometers)
 *   - Mass: kilograms (kg)
 *   - Price: currency per liter ($/L, €/L, etc.)
 *
 * Unit conversions happen at API boundaries (JSON parsing/serialization).
 * The API accepts SI units by default. Legacy imperial keys (e.g.,
 * "consumption_mpg") are converted to SI at the API boundary via sh_units.h.
 * ============================================================================ */

/* ============================================================================
 * Fuel Station
 *
 * Represents a fuel station with its location and price.
 * ============================================================================ */

typedef struct {
    int id;                     /* Unique station identifier */
    FWCoord location;           /* Geographic location */
    double price;               /* Fuel price per liter ($/L, €/L, etc.) */
    const char *name;           /* Optional station name (can be NULL) */
} FWStation;

/* ============================================================================
 * Polyline (Route Geometry)
 *
 * FWPolyline is now an alias to SHPolyline from the shared library.
 * Note: Field name changed from 'num_points' to 'count' for consistency.
 * ============================================================================ */

#include "sh_polyline.h"

typedef SHPolyline FWPolyline;

/* ============================================================================
 * Snapped Station
 *
 * A station that has been projected onto a route polyline.
 * Contains both the original station info and its position along the route.
 * ============================================================================ */

typedef struct {
    int station_id;                 /* Original station ID */
    double distance_from_start;     /* Meters along route from start */
    double perpendicular_distance;  /* Meters from route (snap distance) */
    FWCoord snap_point;             /* Location where station projects onto route */
    double price;                   /* Fuel price per liter ($/L, €/L, etc.) */
} FWSnappedStation;

/* ============================================================================
 * Route Segment (for Piecewise Consumption)
 *
 * Each segment has a constant weight and consumption rate.
 * Segments are defined by their starting distance; the last segment
 * extends to total_distance.
 * ============================================================================ */

typedef struct {
    double start_distance;      /* Start distance of this segment (meters) */
    double cargo_weight;        /* Cargo weight in this segment (kg) */
    double consumption;         /* Fuel consumption rate (L/100km) */
} FWRouteSegment;

/* ============================================================================
 * Station Filter Configuration
 *
 * Parameters for filtering and snapping stations to a route.
 * All distances in meters internally.
 * ============================================================================ */

typedef struct {
    double max_distance;            /* Max perpendicular distance to include station (meters) */
    double max_dedup_distance;      /* Max distance for deduplication grouping (meters) */
    double min_repeat_distance;     /* Min distance between repeated stations (meters) */
    FWDedupStrategy dedup_strategy; /* How to handle duplicate stations */
} FWFilterConfig;

/* ============================================================================
 * Refueling Problem Definition
 *
 * Complete specification of a refueling optimization problem.
 * All values use SI units internally.
 * ============================================================================ */

typedef struct {
    /* Route */
    double total_distance;          /* Total route distance (meters) */
    int num_segments;               /* Number of route segments (0 = constant rate) */
    FWRouteSegment *segments;       /* Array of segments (NULL if constant rate) */
    double base_consumption;        /* Consumption rate (L/100km) if segments == NULL */

    /* Fuel tank */
    double tank_capacity;           /* Maximum tank capacity (liters) */
    double current_fuel;            /* Current fuel level (liters) */
    double minimum_fuel;            /* Minimum fuel level during route (liters) */
    double minimum_fuel_at_end;     /* Minimum fuel level at destination (liters) */

    /* Stations (snapped to route) */
    int num_stations;               /* Number of fuel stations */
    FWSnappedStation *stations;     /* Array of snapped stations */

    /* Optional constraints */
    double min_purchase;            /* Minimum liters per purchase (0 = no min) */
    double stop_cost;               /* Fixed cost per stop (0 = no cost) */
    double remaining_fuel_value;    /* Credit per liter for remaining fuel (0 = ignore) */
} FWRefuelProblem;

/* ============================================================================
 * MIP Search Telemetry
 *
 * Populated for MILP solves. LP-only solves leave this zeroed.
 * ============================================================================ */

typedef struct {
    int nodes_explored;
    double solve_time_ms;
    double root_lp_time_ms;
    double node_lp_time_ms;
    double strong_branch_time_ms;
    double root_cut_time_ms;
    double non_root_cut_time_ms;

    int lap_nodes_solved;
    int simplex_nodes_solved;
    int node_lp_cold_starts;

    int node_basis_warm_attempts;
    int node_basis_warm_applied;
    int node_basis_warm_rejected;
    int node_basis_staged;
    int warm_reject_invalid_snapshot;
    int warm_reject_restore_failure;
    int warm_reject_stage_failure;
    int warm_reject_stage_solve_rejected;
    int warm_reject_stage_solution_invalid;

    int strong_branch_probes;
    int strong_branch_failures;
    int strong_branch_recoveries;

    int root_cut_rounds;
    int root_cuts_generated;
    int root_cuts_applied;
    int non_root_cut_rounds;
    int non_root_cuts_generated;
    int non_root_cuts_applied;

    int fathom_lp_infeasible;
    int fathom_bound;
    int fathom_integral;
    int fathom_no_branch_var;
} FWMIPTelemetry;

/* ============================================================================
 * Refueling Solution
 *
 * Result of solving a refueling optimization problem.
 * ============================================================================ */

typedef struct {
    FWStatus status;        /* Solution status */
    int num_stops;          /* Number of stops made */
    double total_cost;      /* Total cost (fuel + stop costs - remaining fuel credit) */
    double *purchases;      /* Liters purchased at each station (array of num_stations) */
    int *stop_flags;        /* 1 if stopping at station, 0 otherwise (for MILP) */
    double remaining_fuel;  /* Fuel remaining at destination (liters) */
    double gross_cost;      /* Fuel cost only (before remaining fuel credit) */
    FWMIPTelemetry mip;     /* MILP search telemetry (zero for LP/Benders or on error) */
} FWRefuelSolution;

/* ============================================================================
 * Station Filter Result
 *
 * Result of filtering stations onto a route.
 * ============================================================================ */

typedef struct {
    int count;                  /* Number of stations in result */
    FWSnappedStation *stations; /* Array of snapped stations (sorted by distance) */
} FWFilterResult;

/* ============================================================================
 * Complete Optimization Request
 *
 * All-in-one structure for the high-level fw_optimize_route() API.
 * All values use SI units internally.
 * ============================================================================ */

typedef struct {
    /* Input: Raw stations */
    const FWStation *stations;
    int num_stations;

    /* Input: Route geometry */
    const FWPolyline *route;
    const FWPolyline *overview_route;   /* Optional coarser polyline for speed */

    /* Input: Station filtering */
    FWFilterConfig filter_config;

    /* Input: Vehicle parameters */
    double tank_capacity;       /* liters */
    double current_fuel;        /* liters */
    double consumption;         /* L/100km */
    double minimum_fuel;        /* liters */
    double minimum_fuel_at_end; /* liters */

    /* Input: Optional constraints */
    double min_purchase;        /* liters */
    double stop_cost;           /* currency */
    double remaining_fuel_value;/* currency per liter */

    /* Input: Route segments (optional) */
    int num_segments;
    FWRouteSegment *segments;

    /* Input: Solver options */
    int use_milp;               /* 0 = LP relaxation, 1 = full MILP */
    int verbose;                /* 0 = silent, 1 = print progress */
} FWOptimizeRequest;

/* ============================================================================
 * Complete Optimization Response
 *
 * Result from the high-level fw_optimize_route() API.
 * ============================================================================ */

typedef struct {
    FWStatus status;            /* Overall status */

    /* Filtered stations (intermediate result) */
    int num_filtered_stations;
    FWSnappedStation *filtered_stations;

    /* Solution */
    int num_stops;
    double total_cost;
    double gross_cost;
    double remaining_fuel;

    /* Per-station results (parallel arrays of size num_filtered_stations) */
    double *purchases;          /* Gallons purchased at each filtered station */
    int *stop_flags;            /* Whether we stopped at each station */

    /* Route info */
    double total_distance;
    double total_fuel_consumed;
} FWOptimizeResponse;

#ifdef __cplusplus
}
#endif

#endif /* FW_TYPES_H */
