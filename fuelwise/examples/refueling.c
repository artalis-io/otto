/*
 * Truck Refueling Optimization Example
 *
 * This example solves the 1D truck refueling problem using Mixed Integer Linear Programming.
 * Given a route with fuel stations at known distances and prices, find the minimum cost
 * refueling strategy.
 *
 * Solves the optimal refueling strategy for a truck traveling along a 1D route.
 *
 * MILP Model:
 * -----------
 * Constants:
 *   d_max   - Total distance of the route (miles)
 *   c_avg   - Fuel consumption (miles per gallon)
 *   x_max   - Maximum fuel tank capacity (gallons)
 *   x_min   - Minimum fuel level to maintain (gallons)
 *   x_mfa   - Minimum fuel amount per purchase (gallons)
 *   x_0     - Initial fuel level (gallons)
 *   k       - Number of fuel stations
 *   d[i]    - Distance of station i from start (miles)
 *   p[i]    - Price per gallon at station i ($/gallon)
 *
 * Decision Variables:
 *   x[i]    - Fuel to purchase at station i (gallons), semi-continuous
 *   y[i]    - Cumulative fuel before arriving at station i (gallons)
 *   z[i]    - Binary indicator: 1 if we stop at station i
 *
 * Objective:
 *   minimize sum(x[i] * p[i])
 *
 * Constraints:
 *   y[i] = x_0 + sum(x[j] for j < i)           (fuel balance)
 *   y[i] - d[i]/c_avg >= x_min                 (minimum fuel at arrival)
 *   y[i] <= x_max + d[i-1]/c_avg               (tank capacity after refuel)
 *   x[i] <= x_max * z[i]                       (link x to z)
 *   x[i] >= x_mfa * z[i]                       (minimum purchase if stopping)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ralph_lp.h"
#include "ralph_mip.h"

/* ============================================================================
 * GEOSPATIAL UTILITIES
 *
 * Functions for filtering and snapping fuel stations along a polyline route.
 * ============================================================================ */

#define EARTH_RADIUS_MILES 3958.8
#define DEG_TO_RAD (M_PI / 180.0)
#define RAD_TO_DEG (180.0 / M_PI)

/* Fuel station with geographic coordinates */
typedef struct {
    int id;
    double lat;
    double lon;
    double price_per_gallon;
    char *name;  /* Optional */
} FuelStation;

/* Point on a polyline */
typedef struct {
    double lat;
    double lon;
} PolylinePoint;

/* Filtered station result - snapped to polyline */
typedef struct {
    int id;
    double distance_from_start;  /* Miles along polyline */
    double price_per_gallon;
    double perpendicular_distance;  /* Distance from original point to polyline */
    double snap_lat;  /* Snapped location */
    double snap_lon;
} FilteredStation;

/* Result of filtering operation */
typedef struct {
    int count;
    FilteredStation *stations;
} FilterResult;

/*
 * Calculate the Haversine distance between two lat/lon points in miles
 */
static double haversine_distance(double lat1, double lon1, double lat2, double lon2)
{
    double dlat = (lat2 - lat1) * DEG_TO_RAD;
    double dlon = (lon2 - lon1) * DEG_TO_RAD;

    double a = sin(dlat / 2) * sin(dlat / 2) +
               cos(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) *
               sin(dlon / 2) * sin(dlon / 2);

    double c = 2 * atan2(sqrt(a), sqrt(1 - a));

    return EARTH_RADIUS_MILES * c;
}

/*
 * Convert lat/lon to local Cartesian coordinates (miles from reference point)
 * Uses equirectangular approximation which is accurate for small distances
 */
static void latlon_to_local(double lat, double lon, double ref_lat, double ref_lon,
                            double *x, double *y)
{
    /* Scale longitude by cosine of latitude */
    double cos_lat = cos(ref_lat * DEG_TO_RAD);

    *x = (lon - ref_lon) * DEG_TO_RAD * EARTH_RADIUS_MILES * cos_lat;
    *y = (lat - ref_lat) * DEG_TO_RAD * EARTH_RADIUS_MILES;
}

/*
 * Convert local Cartesian coordinates back to lat/lon
 */
static void local_to_latlon(double x, double y, double ref_lat, double ref_lon,
                            double *lat, double *lon)
{
    double cos_lat = cos(ref_lat * DEG_TO_RAD);

    *lon = ref_lon + (x / (EARTH_RADIUS_MILES * cos_lat)) * RAD_TO_DEG;
    *lat = ref_lat + (y / EARTH_RADIUS_MILES) * RAD_TO_DEG;
}

/*
 * Project a point onto a line segment in local coordinates
 *
 * Returns the parameter t where:
 *   t <= 0: closest point is segment start
 *   t >= 1: closest point is segment end
 *   0 < t < 1: closest point is on the segment
 *
 * Also returns the projected point coordinates
 */
static double project_point_on_segment(
    double px, double py,           /* Point to project */
    double ax, double ay,           /* Segment start */
    double bx, double by,           /* Segment end */
    double *proj_x, double *proj_y) /* Output: projected point */
{
    double dx = bx - ax;
    double dy = by - ay;
    double len_sq = dx * dx + dy * dy;

    if (len_sq < 1e-12) {
        /* Degenerate segment (start == end) */
        *proj_x = ax;
        *proj_y = ay;
        return 0.0;
    }

    /* Calculate projection parameter t */
    double t = ((px - ax) * dx + (py - ay) * dy) / len_sq;

    /* Clamp t to [0, 1] */
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    /* Calculate projected point */
    *proj_x = ax + t * dx;
    *proj_y = ay + t * dy;

    return t;
}

/*
 * Calculate perpendicular distance from a point to a line segment
 * Returns both the distance and the closest point on the segment
 */
static double point_to_segment_distance(
    double lat_p, double lon_p,     /* Point */
    double lat_a, double lon_a,     /* Segment start */
    double lat_b, double lon_b,     /* Segment end */
    double *closest_lat, double *closest_lon)  /* Output: closest point */
{
    /* Use segment midpoint as reference for local coordinates */
    double ref_lat = (lat_a + lat_b) / 2.0;
    double ref_lon = (lon_a + lon_b) / 2.0;

    /* Convert to local coordinates */
    double px, py, ax, ay, bx, by;
    latlon_to_local(lat_p, lon_p, ref_lat, ref_lon, &px, &py);
    latlon_to_local(lat_a, lon_a, ref_lat, ref_lon, &ax, &ay);
    latlon_to_local(lat_b, lon_b, ref_lat, ref_lon, &bx, &by);

    /* Project point onto segment */
    double proj_x, proj_y;
    project_point_on_segment(px, py, ax, ay, bx, by, &proj_x, &proj_y);

    /* Convert projected point back to lat/lon */
    local_to_latlon(proj_x, proj_y, ref_lat, ref_lon, closest_lat, closest_lon);

    /* Calculate distance from point to projected point */
    double dx = px - proj_x;
    double dy = py - proj_y;
    return sqrt(dx * dx + dy * dy);
}

/*
 * Find the closest point on a polyline to a given point
 *
 * Returns:
 *   - perpendicular_distance: distance from point to closest point on polyline
 *   - segment_index: which segment contains the closest point
 *   - t: parameter along that segment (0=start, 1=end)
 *   - closest_lat, closest_lon: the snapped point coordinates
 */
static double find_closest_point_on_polyline(
    double lat_p, double lon_p,
    PolylinePoint *polyline, int num_points,
    int *segment_index, double *t,
    double *closest_lat, double *closest_lon)
{
    if (num_points < 2) {
        /* Degenerate polyline */
        if (num_points == 1) {
            *closest_lat = polyline[0].lat;
            *closest_lon = polyline[0].lon;
            *segment_index = 0;
            *t = 0.0;
            return haversine_distance(lat_p, lon_p, polyline[0].lat, polyline[0].lon);
        }
        return -1.0;  /* Error */
    }

    double min_distance = 1e18;
    int best_segment = 0;
    double best_t = 0.0;
    double best_lat = polyline[0].lat;
    double best_lon = polyline[0].lon;

    /* Check each segment */
    for (int i = 0; i < num_points - 1; i++) {
        double snap_lat, snap_lon;
        double dist = point_to_segment_distance(
            lat_p, lon_p,
            polyline[i].lat, polyline[i].lon,
            polyline[i + 1].lat, polyline[i + 1].lon,
            &snap_lat, &snap_lon);

        if (dist < min_distance) {
            min_distance = dist;
            best_segment = i;
            best_lat = snap_lat;
            best_lon = snap_lon;

            /* Calculate t for this segment */
            double seg_len = haversine_distance(
                polyline[i].lat, polyline[i].lon,
                polyline[i + 1].lat, polyline[i + 1].lon);
            double dist_to_snap = haversine_distance(
                polyline[i].lat, polyline[i].lon,
                snap_lat, snap_lon);
            best_t = (seg_len > 1e-9) ? dist_to_snap / seg_len : 0.0;
        }
    }

    *segment_index = best_segment;
    *t = best_t;
    *closest_lat = best_lat;
    *closest_lon = best_lon;

    return min_distance;
}

/*
 * Calculate the cumulative distance along a polyline up to a specific point
 *
 * segment_index: which segment the point is on
 * t: parameter along that segment (0 to 1)
 */
static double distance_along_polyline(
    PolylinePoint *polyline, int num_points,
    int segment_index, double t)
{
    double total_distance = 0.0;

    /* Sum distances of complete segments before segment_index */
    for (int i = 0; i < segment_index && i < num_points - 1; i++) {
        total_distance += haversine_distance(
            polyline[i].lat, polyline[i].lon,
            polyline[i + 1].lat, polyline[i + 1].lon);
    }

    /* Add partial distance within the segment */
    if (segment_index < num_points - 1) {
        double seg_len = haversine_distance(
            polyline[segment_index].lat, polyline[segment_index].lon,
            polyline[segment_index + 1].lat, polyline[segment_index + 1].lon);
        total_distance += t * seg_len;
    }

    return total_distance;
}

/*
 * Calculate total length of a polyline
 */
static double polyline_total_length(PolylinePoint *polyline, int num_points)
{
    double total = 0.0;
    for (int i = 0; i < num_points - 1; i++) {
        total += haversine_distance(
            polyline[i].lat, polyline[i].lon,
            polyline[i + 1].lat, polyline[i + 1].lon);
    }
    return total;
}

/*
 * Comparison function for sorting FilteredStation by distance_from_start
 */
static int compare_filtered_stations(const void *a, const void *b)
{
    const FilteredStation *sa = (const FilteredStation *)a;
    const FilteredStation *sb = (const FilteredStation *)b;

    if (sa->distance_from_start < sb->distance_from_start) return -1;
    if (sa->distance_from_start > sb->distance_from_start) return 1;
    return 0;
}

/*
 * Filter and snap fuel stations along a polyline
 *
 * This function:
 *   1. Takes a list of fuel stations with geographic coordinates
 *   2. Takes a polyline defining a route
 *   3. Filters to keep only stations within max_radius_miles of the polyline
 *   4. Snaps each station to its closest point on the polyline
 *   5. Orders stations by their distance from the start of the polyline
 *
 * Parameters:
 *   stations: array of FuelStation structs
 *   num_stations: number of stations in the array
 *   polyline: array of PolylinePoint structs defining the route
 *   num_polyline_points: number of points in the polyline
 *   max_radius_miles: maximum distance from polyline to include a station
 *
 * Returns:
 *   FilterResult with filtered and sorted stations
 *   Caller is responsible for freeing result.stations
 */
FilterResult filter_and_snap_stations_on_polyline(
    FuelStation *stations, int num_stations,
    PolylinePoint *polyline, int num_polyline_points,
    double max_radius_miles)
{
    FilterResult result = {0, NULL};

    if (num_stations == 0 || num_polyline_points < 2) {
        return result;
    }

    /* Allocate space for all stations (we'll trim later) */
    FilteredStation *filtered = malloc(num_stations * sizeof(FilteredStation));
    int count = 0;

    /* Process each station */
    for (int i = 0; i < num_stations; i++) {
        int segment_index;
        double t;
        double snap_lat, snap_lon;

        /* Find closest point on polyline */
        double perp_distance = find_closest_point_on_polyline(
            stations[i].lat, stations[i].lon,
            polyline, num_polyline_points,
            &segment_index, &t,
            &snap_lat, &snap_lon);

        /* Check if within radius */
        if (perp_distance <= max_radius_miles) {
            /* Calculate distance along polyline */
            double dist_from_start = distance_along_polyline(
                polyline, num_polyline_points,
                segment_index, t);

            /* Add to filtered list */
            filtered[count].id = stations[i].id;
            filtered[count].distance_from_start = dist_from_start;
            filtered[count].price_per_gallon = stations[i].price_per_gallon;
            filtered[count].perpendicular_distance = perp_distance;
            filtered[count].snap_lat = snap_lat;
            filtered[count].snap_lon = snap_lon;
            count++;
        }
    }

    /* Sort by distance from start */
    if (count > 0) {
        qsort(filtered, count, sizeof(FilteredStation), compare_filtered_stations);
    }

    /* Shrink allocation to actual size */
    if (count > 0) {
        FilteredStation *shrunk = realloc(filtered, count * sizeof(FilteredStation));
        result.stations = shrunk ? shrunk : filtered;
    } else {
        free(filtered);
        result.stations = NULL;
    }

    result.count = count;
    return result;
}

/*
 * Free FilterResult memory
 */
void free_filter_result(FilterResult *result)
{
    if (result && result->stations) {
        free(result->stations);
        result->stations = NULL;
        result->count = 0;
    }
}

/*
 * Print filter result for debugging
 */
void print_filter_result(FilterResult *result, FuelStation *original_stations, int num_original)
{
    (void)original_stations;  /* Available for extended output if needed */
    (void)num_original;

    printf("\nFiltered Stations Along Route:\n");
    printf("  %-4s  %-10s  %-12s  %-10s  %-10s\n",
           "ID", "Dist (mi)", "Price", "Off-route", "Snap Location");
    printf("  %-4s  %-10s  %-12s  %-10s  %-10s\n",
           "----", "----------", "------------", "----------", "--------------------");

    for (int i = 0; i < result->count; i++) {
        FilteredStation *fs = &result->stations[i];

        printf("  %-4d  %7.1f mi  $%.2f/gal  %7.2f mi  (%.4f, %.4f)\n",
               fs->id, fs->distance_from_start, fs->price_per_gallon,
               fs->perpendicular_distance, fs->snap_lat, fs->snap_lon);
    }
    printf("\n");
}

/* ============================================================================
 * END GEOSPATIAL UTILITIES
 * ============================================================================ */

/* Problem parameters */
typedef struct {
    double total_distance;      /* d_max: total route distance (miles) */
    double fuel_consumption;    /* c_avg: miles per gallon */
    double tank_capacity;       /* x_max: maximum fuel (gallons) */
    double min_fuel_level;      /* x_min: minimum fuel to maintain (gallons) */
    double min_purchase;        /* x_mfa: minimum purchase amount (gallons) */
    double initial_fuel;        /* x_0: starting fuel level (gallons) */

    int num_stations;           /* k: number of fuel stations */
    double *station_distances;  /* d[i]: distance from start (miles) */
    double *station_prices;     /* p[i]: price per gallon ($/gallon) */
    char **station_names;       /* Optional station names for display */
} RefuelingProblem;

/* Solution structure */
typedef struct {
    int status;
    double total_cost;
    int num_stops;
    int *stop_indices;          /* Which stations to stop at */
    double *purchase_amounts;   /* How much to buy at each stop */
    double *fuel_levels;        /* Fuel level arriving at each station */
} RefuelingSolution;

/*
 * Create a sample refueling problem
 */
RefuelingProblem* create_sample_problem(void)
{
    RefuelingProblem *prob = malloc(sizeof(RefuelingProblem));

    /* Truck parameters */
    prob->total_distance = 500.0;       /* 500 mile route */
    prob->fuel_consumption = 6.5;       /* 6.5 mpg (typical semi truck) */
    prob->tank_capacity = 150.0;        /* 150 gallon tank */
    prob->min_fuel_level = 20.0;        /* Keep at least 20 gallons reserve */
    prob->min_purchase = 10.0;          /* Minimum 10 gallon purchase */
    prob->initial_fuel = 80.0;          /* Start with 80 gallons */

    /* Fuel stations along the route */
    prob->num_stations = 8;
    prob->station_distances = malloc(prob->num_stations * sizeof(double));
    prob->station_prices = malloc(prob->num_stations * sizeof(double));
    prob->station_names = malloc(prob->num_stations * sizeof(char*));

    /* Station data: distance (miles), price ($/gal), name */
    double distances[] = {45.0, 95.0, 140.0, 210.0, 275.0, 330.0, 400.0, 460.0};
    double prices[] = {3.89, 4.15, 3.65, 3.95, 3.45, 4.25, 3.75, 4.05};
    const char *names[] = {
        "Flying J #1", "Pilot #23", "Love's #45", "TA #12",
        "Petro #8", "Pilot #67", "Love's #89", "Flying J #2"
    };

    for (int i = 0; i < prob->num_stations; i++) {
        prob->station_distances[i] = distances[i];
        prob->station_prices[i] = prices[i];
        prob->station_names[i] = strdup(names[i]);
    }

    return prob;
}

/*
 * Free problem memory
 */
void free_problem(RefuelingProblem *prob)
{
    if (prob) {
        free(prob->station_distances);
        free(prob->station_prices);
        for (int i = 0; i < prob->num_stations; i++) {
            free(prob->station_names[i]);
        }
        free(prob->station_names);
        free(prob);
    }
}

/*
 * Free solution memory
 */
void free_solution(RefuelingSolution *sol)
{
    if (sol) {
        free(sol->stop_indices);
        free(sol->purchase_amounts);
        free(sol->fuel_levels);
        free(sol);
    }
}

/*
 * Solve the refueling problem using LP (simplified, no minimum purchase constraint)
 *
 * Variable layout:
 *   x[0..k-1]   - fuel purchased at each station (continuous)
 *   y[0..k-1]   - cumulative fuel before arriving at station i (continuous)
 *
 * Total variables: 2k
 */
RefuelingSolution* solve_refueling_lp(RefuelingProblem *prob)
{
    int k = prob->num_stations;
    int num_vars = 2 * k;  /* x[k], y[k] */

    /* Create Ralph model */
    RalphLPModel *model = ralph_lp_create();
    if (!model) {
        fprintf(stderr, "Failed to create Ralph model\n");
        return NULL;
    }

    ralph_lp_set_obj_sense(model, RALPH_LP_OBJ_MINIMIZE);

    /* Variable indices */
    int x_start = 0;        /* x[i] at index i */
    int y_start = k;        /* y[i] at index k + i */

    /* Add variables */

    /* x[i]: fuel purchased at station i (continuous, 0 to tank_capacity) */
    for (int i = 0; i < k; i++) {
        ralph_lp_add_var(model, 0.0, prob->tank_capacity, prob->station_prices[i], RALPH_LP_VAR_CONTINUOUS);
    }

    /* y[i]: cumulative fuel before arriving at station i */
    double y_upper = prob->initial_fuel + k * prob->tank_capacity;
    for (int i = 0; i < k; i++) {
        ralph_lp_add_var(model, 0.0, y_upper, 0.0, RALPH_LP_VAR_CONTINUOUS);
    }

    /* Constraints */

    /* Constraint 1: Fuel balance */
    for (int i = 0; i < k; i++) {
        int nnz = 1 + i;
        int *indices = malloc(nnz * sizeof(int));
        double *values = malloc(nnz * sizeof(double));

        indices[0] = y_start + i;
        values[0] = 1.0;

        for (int j = 0; j < i; j++) {
            indices[1 + j] = x_start + j;
            values[1 + j] = -1.0;
        }

        ralph_lp_add_constraint(model, nnz, indices, values, RALPH_LP_SENSE_EQUAL, prob->initial_fuel);

        free(indices);
        free(values);
    }

    /* Constraint 2: Minimum fuel at arrival */
    for (int i = 0; i < k; i++) {
        int indices[1] = {y_start + i};
        double values[1] = {1.0};
        double rhs = prob->min_fuel_level + prob->station_distances[i] / prob->fuel_consumption;

        ralph_lp_add_constraint(model, 1, indices, values, RALPH_LP_SENSE_GREATER_EQUAL, rhs);
    }

    /* Constraint 3: Tank capacity after refueling */
    for (int i = 0; i < k; i++) {
        int indices[2] = {y_start + i, x_start + i};
        double values[2] = {1.0, 1.0};
        double rhs = prob->tank_capacity + prob->station_distances[i] / prob->fuel_consumption;

        ralph_lp_add_constraint(model, 2, indices, values, RALPH_LP_SENSE_LESS_EQUAL, rhs);
    }

    /* Constraint 4: Reach destination */
    {
        int *indices = malloc(k * sizeof(int));
        double *values = malloc(k * sizeof(double));

        for (int i = 0; i < k; i++) {
            indices[i] = x_start + i;
            values[i] = 1.0;
        }

        double rhs = prob->min_fuel_level +
                     prob->total_distance / prob->fuel_consumption -
                     prob->initial_fuel;

        if (rhs > 0) {
            ralph_lp_add_constraint(model, k, indices, values, RALPH_LP_SENSE_GREATER_EQUAL, rhs);
        }

        free(indices);
        free(values);
    }

    /* Solve */
    printf("Solving LP relaxation...\n");
    printf("Variables: %d, Constraints: %d\n", num_vars, ralph_lp_get_num_cons(model));
    ralph_lp_set_int_param(model, "verbose", 1);

    int ret = ralph_lp_optimize(model);
    RalphLPStatus status = ralph_lp_get_status(model);
    printf("LP status: %s\n", ralph_lp_status_string(status));

    /* Extract solution */
    RefuelingSolution *sol = malloc(sizeof(RefuelingSolution));
    sol->status = status;
    sol->stop_indices = malloc(k * sizeof(int));
    sol->purchase_amounts = malloc(k * sizeof(double));
    sol->fuel_levels = malloc(k * sizeof(double));
    sol->num_stops = 0;
    sol->total_cost = 0.0;

    if (ret == 0 && status == RALPH_LP_STATUS_OPTIMAL) {
        double *x = malloc(num_vars * sizeof(double));
        ralph_lp_get_solution(model, x);
        sol->total_cost = ralph_lp_get_objval(model);

        for (int i = 0; i < k; i++) {
            sol->purchase_amounts[i] = x[x_start + i];
            sol->fuel_levels[i] = x[y_start + i];

            if (x[x_start + i] > 0.01) {
                sol->stop_indices[sol->num_stops++] = i;
            }
        }

        free(x);
    }

    ralph_lp_free(model);
    return sol;
}

/*
 * Solve the refueling problem using MILP
 *
 * Variable layout:
 *   x[0..k-1]   - fuel purchased at each station (continuous)
 *   y[1..k]     - cumulative fuel before arriving at station i (continuous)
 *   z[0..k-1]   - binary indicator for stopping at station i
 *
 * Total variables: 3k
 */
RefuelingSolution* solve_refueling_milp(RefuelingProblem *prob)
{
    int k = prob->num_stations;
    int num_vars = 3 * k;  /* x[k], y[k], z[k] */

    /* Create Ralph model */
    RalphMIPModel *model = ralph_mip_create();
    if (!model) {
        fprintf(stderr, "Failed to create Ralph model\n");
        return NULL;
    }

    ralph_lp_set_obj_sense(model, RALPH_LP_OBJ_MINIMIZE);

    /* Variable indices */
    int x_start = 0;        /* x[i] at index i */
    int y_start = k;        /* y[i] at index k + i */
    int z_start = 2 * k;    /* z[i] at index 2k + i */

    /*
     * Add variables
     */

    /* x[i]: fuel purchased at station i (continuous, 0 to tank_capacity) */
    /* Objective coefficient is price[i] */
    for (int i = 0; i < k; i++) {
        ralph_lp_add_var(model, 0.0, prob->tank_capacity, prob->station_prices[i], RALPH_LP_VAR_CONTINUOUS);
    }

    /* y[i]: cumulative fuel before arriving at station i (continuous) */
    /* These are auxiliary variables with 0 objective coefficient */
    /* y[i] = x_0 + sum(x[j] for j < i) can grow large as we buy fuel at multiple stations */
    /* Upper bound: we could theoretically buy at every station, so max is roughly */
    /* initial + k * tank_capacity, but tank capacity constraints limit the actual values */
    /* Use a generous upper bound to avoid infeasibility */
    double y_upper = prob->initial_fuel + k * prob->tank_capacity;
    for (int i = 0; i < k; i++) {
        ralph_lp_add_var(model, 0.0, y_upper, 0.0, RALPH_LP_VAR_CONTINUOUS);
    }

    /* z[i]: binary indicator for stopping at station i */
    for (int i = 0; i < k; i++) {
        ralph_lp_add_var(model, 0.0, 1.0, 0.0, RALPH_LP_VAR_BINARY);
    }

    /*
     * Add constraints
     */

    /* Constraint 1: Fuel balance
     * y[i] = x_0 + sum(x[j] for j < i)
     *
     * For i = 0: y[0] = x_0  (no purchases yet)
     * For i > 0: y[i] = x_0 + x[0] + x[1] + ... + x[i-1]
     *
     * Rewritten as: y[i] - sum(x[j] for j < i) = x_0
     */
    for (int i = 0; i < k; i++) {
        int nnz = 1 + i;  /* y[i] plus x[0..i-1] */
        int *indices = malloc(nnz * sizeof(int));
        double *values = malloc(nnz * sizeof(double));

        /* y[i] coefficient = 1 */
        indices[0] = y_start + i;
        values[0] = 1.0;

        /* x[j] coefficients = -1 for j < i */
        for (int j = 0; j < i; j++) {
            indices[1 + j] = x_start + j;
            values[1 + j] = -1.0;
        }

        ralph_lp_add_constraint(model, nnz, indices, values, RALPH_LP_SENSE_EQUAL, prob->initial_fuel);

        free(indices);
        free(values);
    }

    /* Constraint 2: Minimum fuel at arrival
     * y[i] - d[i]/c_avg >= x_min
     * Rewritten: y[i] >= x_min + d[i]/c_avg
     */
    for (int i = 0; i < k; i++) {
        int indices[1] = {y_start + i};
        double values[1] = {1.0};
        double rhs = prob->min_fuel_level + prob->station_distances[i] / prob->fuel_consumption;

        ralph_lp_add_constraint(model, 1, indices, values, RALPH_LP_SENSE_GREATER_EQUAL, rhs);
    }

    /* Constraint 3: Tank capacity after refueling
     * y[i] + x[i] - d[i]/c_avg <= x_max
     *
     * This ensures that after buying x[i] gallons and before consuming
     * fuel to the next station, we don't exceed tank capacity.
     *
     * Rewritten: y[i] + x[i] <= x_max + d[i]/c_avg
     */
    for (int i = 0; i < k; i++) {
        int indices[2] = {y_start + i, x_start + i};
        double values[2] = {1.0, 1.0};
        double rhs = prob->tank_capacity + prob->station_distances[i] / prob->fuel_consumption;

        ralph_lp_add_constraint(model, 2, indices, values, RALPH_LP_SENSE_LESS_EQUAL, rhs);
    }

    /* Constraint 4: Link x[i] to z[i] (upper bound)
     * x[i] <= x_max * z[i]
     * If z[i] = 0, then x[i] = 0
     * If z[i] = 1, then x[i] <= x_max
     *
     * Rewritten: x[i] - x_max * z[i] <= 0
     */
    for (int i = 0; i < k; i++) {
        int indices[2] = {x_start + i, z_start + i};
        double values[2] = {1.0, -prob->tank_capacity};

        ralph_lp_add_constraint(model, 2, indices, values, RALPH_LP_SENSE_LESS_EQUAL, 0.0);
    }

    /* Constraint 5: Minimum purchase if stopping
     * x[i] >= x_mfa * z[i]
     * If z[i] = 1, then x[i] >= x_mfa
     * If z[i] = 0, then x[i] >= 0 (already satisfied)
     *
     * Rewritten: x[i] - x_mfa * z[i] >= 0
     *
     * NOTE: Only add this for problems where min_purchase > 0
     */
    if (prob->min_purchase > 0.01) {
        for (int i = 0; i < k; i++) {
            int indices[2] = {x_start + i, z_start + i};
            double values[2] = {1.0, -prob->min_purchase};

            ralph_lp_add_constraint(model, 2, indices, values, RALPH_LP_SENSE_GREATER_EQUAL, 0.0);
        }
    }

    /* Constraint 6: Must have enough fuel to reach destination
     * Final fuel level >= x_min
     * y[k-1] + x[k-1] - (d_max - d[k-1])/c_avg >= x_min  (at last station)
     *
     * But we need to track fuel at destination, so add a constraint:
     * x_0 + sum(x[i]) - d_max/c_avg >= x_min
     * Rewritten: sum(x[i]) >= x_min + d_max/c_avg - x_0
     */
    {
        int *indices = malloc(k * sizeof(int));
        double *values = malloc(k * sizeof(double));

        for (int i = 0; i < k; i++) {
            indices[i] = x_start + i;
            values[i] = 1.0;
        }

        double rhs = prob->min_fuel_level +
                     prob->total_distance / prob->fuel_consumption -
                     prob->initial_fuel;

        /* Only add if we actually need to buy fuel */
        if (rhs > 0) {
            ralph_lp_add_constraint(model, k, indices, values, RALPH_LP_SENSE_GREATER_EQUAL, rhs);
        }

        free(indices);
        free(values);
    }

    /* Solve the model */
    printf("Solving refueling optimization...\n");
    printf("Variables: %d (purchases: %d, fuel levels: %d, stop indicators: %d)\n",
           num_vars, k, k, k);
    printf("Constraints: %d\n", ralph_lp_get_num_cons(model));

    /* Set solver parameters */
    ralph_mip_set_int_param(model, "verbose", 1);

    int ret = ralph_mip_optimize(model);
    if (ret != 0) {
        fprintf(stderr, "Optimization failed\n");
        ralph_mip_free(model);
        return NULL;
    }

    RalphLPStatus status = ralph_mip_get_status(model);
    printf("Optimization status: %s\n", ralph_lp_status_string(status));

    /* Extract solution */
    RefuelingSolution *sol = malloc(sizeof(RefuelingSolution));
    sol->status = status;
    sol->stop_indices = malloc(k * sizeof(int));
    sol->purchase_amounts = malloc(k * sizeof(double));
    sol->fuel_levels = malloc(k * sizeof(double));
    sol->num_stops = 0;
    sol->total_cost = 0.0;

    if (status == RALPH_LP_STATUS_OPTIMAL) {
        double *x = malloc(num_vars * sizeof(double));
        ralph_mip_get_solution(model, x);
        sol->total_cost = ralph_mip_get_objval(model);

        /* Extract purchases and fuel levels */
        for (int i = 0; i < k; i++) {
            sol->purchase_amounts[i] = x[x_start + i];
            sol->fuel_levels[i] = x[y_start + i];

            if (x[x_start + i] > 0.01) {  /* Threshold for "stopping" */
                sol->stop_indices[sol->num_stops++] = i;
            }
        }

        free(x);
    }

    ralph_mip_free(model);
    return sol;
}

/*
 * Print the problem details
 */
void print_problem(RefuelingProblem *prob)
{
    printf("\n");
    printf("=================================================================\n");
    printf("                  TRUCK REFUELING OPTIMIZATION                   \n");
    printf("=================================================================\n");
    printf("\n");
    printf("Route Parameters:\n");
    printf("  Total distance:      %.1f miles\n", prob->total_distance);
    printf("  Fuel consumption:    %.1f mpg\n", prob->fuel_consumption);
    printf("  Fuel needed:         %.1f gallons\n",
           prob->total_distance / prob->fuel_consumption);
    printf("\n");
    printf("Truck Parameters:\n");
    printf("  Tank capacity:       %.1f gallons\n", prob->tank_capacity);
    printf("  Minimum fuel level:  %.1f gallons\n", prob->min_fuel_level);
    printf("  Minimum purchase:    %.1f gallons\n", prob->min_purchase);
    printf("  Initial fuel:        %.1f gallons\n", prob->initial_fuel);
    printf("  Max range:           %.1f miles\n",
           (prob->initial_fuel - prob->min_fuel_level) * prob->fuel_consumption);
    printf("\n");
    printf("Fuel Stations:\n");
    printf("  %-4s  %-15s  %10s  %10s\n", "ID", "Name", "Distance", "Price");
    printf("  %-4s  %-15s  %10s  %10s\n", "----", "---------------", "----------", "----------");
    for (int i = 0; i < prob->num_stations; i++) {
        printf("  %-4d  %-15s  %7.1f mi  $%.2f/gal\n",
               i, prob->station_names[i],
               prob->station_distances[i], prob->station_prices[i]);
    }
    printf("\n");
}

/*
 * Print the solution details
 */
void print_solution(RefuelingProblem *prob, RefuelingSolution *sol)
{
    printf("=================================================================\n");
    printf("                         SOLUTION                               \n");
    printf("=================================================================\n");
    printf("\n");

    if (sol->status != RALPH_LP_STATUS_OPTIMAL) {
        printf("No optimal solution found. Status: %d\n", sol->status);
        return;
    }

    printf("Optimal Refueling Plan:\n");
    printf("  Number of stops:     %d\n", sol->num_stops);
    printf("  Total fuel cost:     $%.2f\n", sol->total_cost);
    printf("\n");

    /* Calculate total fuel purchased */
    double total_purchased = 0.0;
    for (int i = 0; i < prob->num_stations; i++) {
        total_purchased += sol->purchase_amounts[i];
    }
    printf("  Total fuel bought:   %.1f gallons\n", total_purchased);
    printf("  Average price:       $%.3f/gal\n",
           total_purchased > 0 ? sol->total_cost / total_purchased : 0);
    printf("\n");

    printf("Refueling Schedule:\n");
    printf("  %-4s  %-15s  %10s  %10s  %12s  %12s\n",
           "Stop", "Station", "Distance", "Price", "Buy (gal)", "Cost");
    printf("  %-4s  %-15s  %10s  %10s  %12s  %12s\n",
           "----", "---------------", "----------", "----------", "------------", "------------");

    int stop_num = 1;
    for (int i = 0; i < prob->num_stations; i++) {
        if (sol->purchase_amounts[i] > 0.01) {
            double cost = sol->purchase_amounts[i] * prob->station_prices[i];
            printf("  %-4d  %-15s  %7.1f mi  $%.2f/gal  %9.1f gal  $%9.2f\n",
                   stop_num++, prob->station_names[i],
                   prob->station_distances[i], prob->station_prices[i],
                   sol->purchase_amounts[i], cost);
        }
    }
    printf("\n");

    /* Print detailed fuel level trace */
    printf("Fuel Level Trace:\n");
    printf("  %-20s  %10s  %12s  %12s\n",
           "Location", "Distance", "Fuel Before", "Fuel After");
    printf("  %-20s  %10s  %12s  %12s\n",
           "--------------------", "----------", "------------", "------------");

    double current_fuel = prob->initial_fuel;
    printf("  %-20s  %7.1f mi  %9.1f gal  %9.1f gal\n",
           "Start", 0.0, current_fuel, current_fuel);

    double last_distance = 0.0;
    for (int i = 0; i < prob->num_stations; i++) {
        /* Fuel consumed to reach this station */
        double consumed = (prob->station_distances[i] - last_distance) / prob->fuel_consumption;
        double fuel_before = current_fuel - consumed;
        double fuel_after = fuel_before + sol->purchase_amounts[i];

        if (sol->purchase_amounts[i] > 0.01) {
            printf("  %-20s  %7.1f mi  %9.1f gal  %9.1f gal  (+%.1f)\n",
                   prob->station_names[i], prob->station_distances[i],
                   fuel_before, fuel_after, sol->purchase_amounts[i]);
        }

        current_fuel = fuel_after;
        last_distance = prob->station_distances[i];
    }

    /* Final fuel at destination */
    double consumed = (prob->total_distance - last_distance) / prob->fuel_consumption;
    double final_fuel = current_fuel - consumed;
    printf("  %-20s  %7.1f mi  %9.1f gal\n",
           "Destination", prob->total_distance, final_fuel);
    printf("\n");
}

/*
 * Create a more challenging problem with more stations
 */
RefuelingProblem* create_long_haul_problem(void)
{
    RefuelingProblem *prob = malloc(sizeof(RefuelingProblem));

    /* Long haul truck parameters */
    prob->total_distance = 1200.0;      /* 1200 mile route (e.g., LA to Dallas) */
    prob->fuel_consumption = 6.0;       /* 6.0 mpg */
    prob->tank_capacity = 200.0;        /* 200 gallon tank (dual tanks) */
    prob->min_fuel_level = 30.0;        /* Keep 30 gallons reserve */
    prob->min_purchase = 0.0;           /* No minimum purchase constraint for LP model */
    prob->initial_fuel = 100.0;         /* Start half full */

    /* More fuel stations along the route */
    prob->num_stations = 15;
    prob->station_distances = malloc(prob->num_stations * sizeof(double));
    prob->station_prices = malloc(prob->num_stations * sizeof(double));
    prob->station_names = malloc(prob->num_stations * sizeof(char*));

    double distances[] = {
        75.0, 150.0, 220.0, 310.0, 380.0,
        450.0, 530.0, 620.0, 700.0, 780.0,
        860.0, 950.0, 1020.0, 1100.0, 1150.0
    };

    /* Prices vary - some expensive (highway), some cheap (off-route deals) */
    double prices[] = {
        4.25, 3.89, 4.45, 3.55, 4.15,   /* Varies */
        3.45, 4.35, 3.75, 4.05, 3.35,   /* Station at 780 is cheapest */
        3.95, 4.25, 3.65, 4.15, 3.85
    };

    const char *names[] = {
        "Barstow TA", "Needles Pilot", "Kingman Love's", "Flagstaff FJ",
        "Holbrook Petro", "Gallup TA", "Grants Pilot", "Albuquerque Love's",
        "Santa Rosa FJ", "Tucumcari Petro", "Amarillo TA", "Shamrock Pilot",
        "Elk City Love's", "OKC FJ", "Dallas Pilot"
    };

    for (int i = 0; i < prob->num_stations; i++) {
        prob->station_distances[i] = distances[i];
        prob->station_prices[i] = prices[i];
        prob->station_names[i] = strdup(names[i]);
    }

    return prob;
}

/*
 * Demonstrate the polyline filtering and snapping functionality
 */
void demo_polyline_filtering(void)
{
    printf("\n>>> EXAMPLE 3: Polyline Filtering and Snapping <<<\n\n");
    printf("=================================================================\n");
    printf("              POLYLINE STATION FILTERING DEMO                    \n");
    printf("=================================================================\n\n");

    /*
     * Define a route from Los Angeles to Phoenix via I-10
     * Simplified polyline with key waypoints
     */
    PolylinePoint route[] = {
        {34.0522, -118.2437},  /* Los Angeles */
        {33.9425, -117.9294},  /* Riverside */
        {33.7701, -116.9715},  /* Palm Springs area */
        {33.6846, -115.5041},  /* Blythe */
        {33.4484, -112.0740},  /* Phoenix */
    };
    int num_route_points = 5;

    /*
     * Fuel stations in the region (mix of on-route and off-route)
     * Some are close to I-10, others are too far
     */
    FuelStation stations[] = {
        /* Close to route (should be included) */
        {1, 33.9500, -117.9000, 4.29, "Riverside TA"},
        {2, 33.7800, -116.4500, 4.15, "Desert Center Pilot"},
        {3, 33.6100, -114.5900, 4.05, "Quartzsite Love's"},
        {4, 33.4300, -111.9400, 3.89, "Mesa Flying J"},

        /* On route in LA */
        {5, 34.0400, -118.1500, 4.45, "LA Downtown Shell"},

        /* Slightly off route but within radius */
        {6, 33.8500, -117.2000, 4.19, "Beaumont Chevron"},
        {7, 33.5500, -112.1000, 3.95, "Buckeye TA"},

        /* Too far from route (should be excluded) */
        {8, 34.5000, -117.5000, 3.99, "Victorville (too far N)"},
        {9, 32.7157, -117.1611, 4.35, "San Diego (too far S)"},
        {10, 35.1983, -111.6513, 4.09, "Flagstaff (too far N)"},

        /* Edge case - exactly on a route point */
        {11, 33.6846, -115.5041, 3.85, "Blythe (on route)"},
    };
    int num_stations = 11;

    /* Calculate and display route info */
    double route_length = polyline_total_length(route, num_route_points);
    printf("Route: Los Angeles to Phoenix via I-10\n");
    printf("Route length: %.1f miles\n", route_length);
    printf("Filter radius: 15 miles from route\n\n");

    printf("All Fuel Stations in Database:\n");
    printf("  %-4s  %-25s  %-20s  %-10s\n", "ID", "Name", "Location", "Price");
    printf("  %-4s  %-25s  %-20s  %-10s\n", "----", "-------------------------",
           "--------------------", "----------");
    for (int i = 0; i < num_stations; i++) {
        printf("  %-4d  %-25s  (%.4f, %.4f)  $%.2f/gal\n",
               stations[i].id, stations[i].name,
               stations[i].lat, stations[i].lon, stations[i].price_per_gallon);
    }
    printf("\n");

    /* Filter stations within 15 miles of the route */
    double max_radius = 15.0;  /* miles */
    FilterResult result = filter_and_snap_stations_on_polyline(
        stations, num_stations,
        route, num_route_points,
        max_radius);

    printf("Filtered Results (within %.0f miles of route):\n", max_radius);
    printf("Found %d stations along route (excluded %d)\n\n",
           result.count, num_stations - result.count);

    print_filter_result(&result, stations, num_stations);

    /* Show how to use filtered results for optimization */
    printf("Ready for Optimization:\n");
    printf("These stations can now be used with the refueling optimizer.\n");
    printf("Station data format: (id, distance_from_start, price_per_gallon)\n\n");

    printf("  ");
    for (int i = 0; i < result.count; i++) {
        printf("(%d, %.1f, $%.2f)",
               result.stations[i].id,
               result.stations[i].distance_from_start,
               result.stations[i].price_per_gallon);
        if (i < result.count - 1) printf(", ");
    }
    printf("\n\n");

    /* Create a RefuelingProblem from the filtered stations */
    if (result.count > 0) {
        printf("Creating optimization problem from filtered stations...\n\n");

        RefuelingProblem prob;
        prob.total_distance = route_length;
        prob.fuel_consumption = 6.5;
        prob.tank_capacity = 150.0;
        prob.min_fuel_level = 20.0;
        prob.min_purchase = 0.0;
        prob.initial_fuel = 60.0;

        prob.num_stations = result.count;
        prob.station_distances = malloc(result.count * sizeof(double));
        prob.station_prices = malloc(result.count * sizeof(double));
        prob.station_names = malloc(result.count * sizeof(char*));

        for (int i = 0; i < result.count; i++) {
            prob.station_distances[i] = result.stations[i].distance_from_start;
            prob.station_prices[i] = result.stations[i].price_per_gallon;

            /* Find original name */
            prob.station_names[i] = strdup("Station");
            for (int j = 0; j < num_stations; j++) {
                if (stations[j].id == result.stations[i].id && stations[j].name) {
                    free(prob.station_names[i]);
                    prob.station_names[i] = strdup(stations[j].name);
                    break;
                }
            }
        }

        print_problem(&prob);

        RefuelingSolution *sol = solve_refueling_lp(&prob);
        print_solution(&prob, sol);

        free_solution(sol);
        for (int i = 0; i < prob.num_stations; i++) {
            free(prob.station_names[i]);
        }
        free(prob.station_distances);
        free(prob.station_prices);
        free(prob.station_names);
    }

    free_filter_result(&result);
}

int main(void)
{
    printf("\n");
    printf("*****************************************************************\n");
    printf("*          Ralph LP/MIP Solver - Refueling Example              *\n");
    printf("*****************************************************************\n");

    /* Example 1: Short haul */
    printf("\n>>> EXAMPLE 1: Short Haul Route (500 miles) <<<\n");

    RefuelingProblem *prob1 = create_sample_problem();
    print_problem(prob1);

    RefuelingSolution *sol1 = solve_refueling_milp(prob1);
    print_solution(prob1, sol1);

    free_solution(sol1);
    free_problem(prob1);

    /* Example 2: Long haul - use LP solver (more robust for this problem size) */
    printf("\n>>> EXAMPLE 2: Long Haul Route (1200 miles) <<<\n");

    RefuelingProblem *prob2 = create_long_haul_problem();
    print_problem(prob2);

    RefuelingSolution *sol2 = solve_refueling_lp(prob2);
    print_solution(prob2, sol2);

    free_solution(sol2);
    free_problem(prob2);

    /* Example 3: Polyline filtering and optimization */
    demo_polyline_filtering();

    printf("=================================================================\n");
    printf("                    Examples completed.                          \n");
    printf("=================================================================\n\n");

    return 0;
}
