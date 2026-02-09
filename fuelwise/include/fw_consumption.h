/*
 * FuelWise - Weight-Dependent Consumption Model
 *
 * This module provides realistic fuel consumption modeling based on truck weight.
 * It wraps the shared library's piecewise-linear and step function primitives
 * with domain-specific validation for trucking.
 *
 * Key concepts:
 *   - Consumption Curve: Maps weight (kg) → consumption rate (L/100km)
 *   - Weight Profile: Models weight changes along route (pickups/deliveries)
 *   - Fuel Calculation: Integrates consumption(weight(distance)) over route
 *
 * Copyright (c) 2024-2026. All rights reserved.
 */

#ifndef FW_CONSUMPTION_H
#define FW_CONSUMPTION_H

#include "sh_piecewise.h"
#include "sh_stepfunc.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Consumption Curve
 *
 * Maps truck weight → fuel consumption rate.
 * Wraps SHPiecewiseLinear with trucking domain validation.
 * ============================================================================ */

typedef struct {
    SHPiecewiseLinear *pwl;     /* Underlying piecewise-linear function */
    double min_weight_kg;       /* Valid range minimum (tare weight) */
    double max_weight_kg;       /* Valid range maximum (max GVW) */
} FWConsumptionCurve;

/*
 * Create an empty consumption curve.
 *
 * Parameters:
 *   initial_capacity - Initial number of points to allocate
 *
 * Returns:
 *   Allocated curve, or NULL on failure.
 *   Caller must call fw_consumption_curve_free().
 */
FWConsumptionCurve *fw_consumption_curve_create(int initial_capacity);

/*
 * Free a consumption curve.
 *
 * Parameters:
 *   curve - Curve to free (can be NULL)
 */
void fw_consumption_curve_free(FWConsumptionCurve *curve);

/*
 * Add a point to the consumption curve.
 * Points must be added in ascending weight order.
 *
 * Parameters:
 *   curve           - Curve to modify
 *   weight_kg       - Weight in kilograms
 *   consumption_l100km - Fuel consumption in L/100km at this weight
 *
 * Returns:
 *   0 on success, -1 on error
 */
int fw_consumption_curve_add_point(
    FWConsumptionCurve *curve,
    double weight_kg,
    double consumption_l100km
);

/*
 * Get consumption rate at a given weight.
 *
 * Parameters:
 *   curve     - Consumption curve
 *   weight_kg - Weight in kilograms
 *
 * Returns:
 *   Consumption rate in L/100km, clamped to curve bounds
 */
double fw_consumption_at_weight(const FWConsumptionCurve *curve, double weight_kg);

/*
 * Get maximum consumption rate from the curve.
 * Used for conservative satisfiability checks.
 */
double fw_consumption_max(const FWConsumptionCurve *curve);

/*
 * Check if a curve is valid (has at least 2 points).
 */
int fw_consumption_curve_is_valid(const FWConsumptionCurve *curve);

/* ============================================================================
 * Built-in Curves
 *
 * Pre-configured curves for common truck types.
 * Caller must free with fw_consumption_curve_free().
 * ============================================================================ */

/*
 * European standard truck (40t GVW)
 * Based on Volvo FH, Mercedes Actros data.
 */
FWConsumptionCurve *fw_curve_eu_standard(void);

/*
 * US Class 8 truck (36t GVW)
 * Based on Freightliner Cascadia, Kenworth T680 data.
 */
FWConsumptionCurve *fw_curve_us_class8(void);

/*
 * Light truck (12t GVW)
 */
FWConsumptionCurve *fw_curve_light_truck(void);

/* ============================================================================
 * Weight Profile
 *
 * Models truck weight changes along a route (pickups/deliveries).
 * Wraps SHStepFunc with trucking domain validation.
 * ============================================================================ */

typedef struct {
    SHStepFunc *step_func;      /* Underlying step function (weight vs distance) */
    double tare_weight_kg;      /* Empty truck weight (initial value) */
    double max_gvw_kg;          /* Maximum gross vehicle weight */
} FWWeightProfile;

/*
 * Create a weight profile.
 *
 * Parameters:
 *   tare_weight_kg - Empty truck weight (kg)
 *   max_gvw_kg     - Maximum gross vehicle weight (kg)
 *
 * Returns:
 *   Allocated profile, or NULL on failure.
 *   Caller must call fw_weight_profile_free().
 */
FWWeightProfile *fw_weight_profile_create(double tare_weight_kg, double max_gvw_kg);

/*
 * Free a weight profile.
 *
 * Parameters:
 *   profile - Profile to free (can be NULL)
 */
void fw_weight_profile_free(FWWeightProfile *profile);

/*
 * Add a weight event (pickup or delivery).
 *
 * Parameters:
 *   profile         - Profile to modify
 *   distance_m      - Distance from start (meters), must be increasing
 *   weight_delta_kg - Change in weight (positive = pickup, negative = delivery)
 *
 * Returns:
 *   0 on success, -1 on error (out of order, would exceed limits)
 */
int fw_weight_profile_add_event(
    FWWeightProfile *profile,
    double distance_m,
    double weight_delta_kg
);

/*
 * Validate a weight profile.
 *
 * Checks:
 *   - Weight never goes below tare weight
 *   - Weight never exceeds max GVW
 *
 * Parameters:
 *   profile   - Profile to validate
 *   error_msg - Buffer for error message (can be NULL)
 *   msg_size  - Size of error_msg buffer
 *
 * Returns:
 *   1 if valid, 0 if invalid
 */
int fw_weight_profile_validate(
    const FWWeightProfile *profile,
    char *error_msg,
    size_t msg_size
);

/*
 * Get weight at a specific distance along the route.
 *
 * Parameters:
 *   profile    - Weight profile
 *   distance_m - Distance from start (meters)
 *
 * Returns:
 *   Weight in kg at that distance
 */
double fw_weight_at_distance(const FWWeightProfile *profile, double distance_m);

/*
 * Get maximum weight reached during the route.
 */
double fw_weight_max(const FWWeightProfile *profile);

/*
 * Get minimum weight during the route (usually tare weight).
 */
double fw_weight_min(const FWWeightProfile *profile);

/* ============================================================================
 * Fuel Calculation
 *
 * Combines consumption curve and weight profile to calculate fuel used.
 * ============================================================================ */

/*
 * Calculate fuel consumed between two distances along the route.
 *
 * This integrates consumption(weight(distance)) over [from_m, to_m].
 * Uses the composition integral from the shared library.
 *
 * Parameters:
 *   curve    - Consumption curve (weight → L/100km)
 *   profile  - Weight profile (distance → weight)
 *   from_m   - Start distance (meters)
 *   to_m     - End distance (meters)
 *
 * Returns:
 *   Fuel consumed in liters, or NaN on error
 */
double fw_calc_fuel_for_segment(
    const FWConsumptionCurve *curve,
    const FWWeightProfile *profile,
    double from_m,
    double to_m
);

/*
 * Calculate total fuel for a route with constant weight.
 *
 * Convenience function when weight doesn't change.
 *
 * Parameters:
 *   curve     - Consumption curve
 *   weight_kg - Constant weight for entire route
 *   from_m    - Start distance (meters)
 *   to_m      - End distance (meters)
 *
 * Returns:
 *   Fuel consumed in liters, or NaN on error
 */
double fw_calc_fuel_constant_weight(
    const FWConsumptionCurve *curve,
    double weight_kg,
    double from_m,
    double to_m
);

/* ============================================================================
 * Memory Ownership Notes
 *
 * - All *_create() functions return owned pointers; caller must call *_free()
 * - The underlying SHPiecewiseLinear and SHStepFunc are owned by the wrappers
 *   and freed automatically by *_free()
 * ============================================================================ */

#ifdef __cplusplus
}
#endif

#endif /* FW_CONSUMPTION_H */
