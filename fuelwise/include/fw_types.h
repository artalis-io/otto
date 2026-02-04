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
 * Constants
 * ============================================================================ */

#define FW_EARTH_RADIUS_MILES 3958.8
#define FW_DEG_TO_RAD (3.14159265358979323846 / 180.0)
#define FW_RAD_TO_DEG (180.0 / 3.14159265358979323846)

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
 * ============================================================================ */

typedef struct {
    double lat;     /* Latitude in degrees */
    double lon;     /* Longitude in degrees */
} FWCoord;

/* ============================================================================
 * Fuel Station
 *
 * Represents a fuel station with its location and price.
 * ============================================================================ */

typedef struct {
    int id;                     /* Unique station identifier */
    FWCoord location;           /* Geographic location */
    double price_per_gallon;    /* Fuel price in $/gallon */
    const char *name;           /* Optional station name (can be NULL) */
} FWStation;

/* ============================================================================
 * Polyline (Route Geometry)
 *
 * A sequence of coordinates representing a route or path.
 * ============================================================================ */

typedef struct {
    FWCoord *points;    /* Array of coordinate points */
    int num_points;     /* Number of points in the array */
} FWPolyline;

/* ============================================================================
 * Snapped Station
 *
 * A station that has been projected onto a route polyline.
 * Contains both the original station info and its position along the route.
 * ============================================================================ */

typedef struct {
    int station_id;                 /* Original station ID */
    double distance_from_start;     /* Miles along route from start */
    double perpendicular_distance;  /* Miles from route (snap distance) */
    FWCoord snap_point;             /* Location where station projects onto route */
    double price_per_gallon;        /* Fuel price */
} FWSnappedStation;

/* ============================================================================
 * Route Segment (for Piecewise Consumption)
 *
 * Each segment has a constant weight and consumption rate.
 * Segments are defined by their starting distance; the last segment
 * extends to total_distance.
 * ============================================================================ */

typedef struct {
    double start_distance;      /* Start distance of this segment (miles) */
    double cargo_weight_lbs;    /* Cargo weight in this segment (lbs) */
    double consumption_mpg;     /* Fuel consumption rate (miles per gallon) */
} FWRouteSegment;

/* ============================================================================
 * Station Filter Configuration
 *
 * Parameters for filtering and snapping stations to a route.
 * ============================================================================ */

typedef struct {
    double max_distance_miles;          /* Max perpendicular distance to include station */
    double max_dedup_distance_miles;    /* Max distance for deduplication grouping */
    double min_repeat_distance_miles;   /* Min distance between repeated stations */
    FWDedupStrategy dedup_strategy;     /* How to handle duplicate stations */
} FWFilterConfig;

/* ============================================================================
 * Refueling Problem Definition
 *
 * Complete specification of a refueling optimization problem.
 * ============================================================================ */

typedef struct {
    /* Route */
    double total_distance;          /* Total route distance in miles */
    int num_segments;               /* Number of route segments (0 = constant rate) */
    FWRouteSegment *segments;       /* Array of segments (NULL if constant rate) */
    double base_consumption_mpg;    /* Used if segments == NULL */

    /* Fuel tank */
    double tank_capacity;           /* Maximum tank capacity in gallons */
    double current_fuel;            /* Current fuel level in gallons */
    double minimum_fuel;            /* Minimum fuel level during route */
    double minimum_fuel_at_end;     /* Minimum fuel level at destination */

    /* Stations (snapped to route) */
    int num_stations;               /* Number of fuel stations */
    FWSnappedStation *stations;     /* Array of snapped stations */

    /* Optional constraints */
    double min_purchase;            /* Minimum gallons per purchase (0 = no min) */
    double stop_cost;               /* Fixed cost per stop in $ (0 = no cost) */
    double remaining_fuel_value;    /* $/gallon credit for remaining fuel (0 = ignore) */
} FWRefuelProblem;

/* ============================================================================
 * Refueling Solution
 *
 * Result of solving a refueling optimization problem.
 * ============================================================================ */

typedef struct {
    FWStatus status;        /* Solution status */
    int num_stops;          /* Number of stops made */
    double total_cost;      /* Total cost (fuel + stop costs - remaining fuel credit) */
    double *purchases;      /* Gallons purchased at each station (array of num_stations) */
    int *stop_flags;        /* 1 if stopping at station, 0 otherwise (for MILP) */
    double remaining_fuel;  /* Fuel remaining at destination */
    double gross_cost;      /* Fuel cost only (before remaining fuel credit) */
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
    double tank_capacity;
    double current_fuel;
    double consumption_mpg;
    double minimum_fuel;
    double minimum_fuel_at_end;

    /* Input: Optional constraints */
    double min_purchase;
    double stop_cost;
    double remaining_fuel_value;

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
