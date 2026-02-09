/*
 * FuelWise - Weight-Dependent Consumption Model
 * Implementation
 *
 * Copyright (c) 2024-2026. All rights reserved.
 */

#include "fw_consumption.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

/* ============================================================================
 * Consumption Curve
 * ============================================================================ */

FWConsumptionCurve *fw_consumption_curve_create(int initial_capacity)
{
    FWConsumptionCurve *curve = malloc(sizeof(FWConsumptionCurve));
    if (!curve) {
        return NULL;
    }

    curve->pwl = sh_pwl_create(initial_capacity);
    if (!curve->pwl) {
        free(curve);
        return NULL;
    }

    curve->min_weight_kg = 0.0;
    curve->max_weight_kg = 0.0;
    return curve;
}

void fw_consumption_curve_free(FWConsumptionCurve *curve)
{
    if (curve) {
        sh_pwl_free(curve->pwl);
        free(curve);
    }
}

int fw_consumption_curve_add_point(
    FWConsumptionCurve *curve,
    double weight_kg,
    double consumption_l100km
)
{
    if (!curve || weight_kg < 0 || consumption_l100km < 0) {
        return -1;
    }

    int result = sh_pwl_add_point(curve->pwl, weight_kg, consumption_l100km);
    if (result != 0) {
        return result;
    }

    /* Update weight range */
    if (curve->pwl->num_points == 1) {
        curve->min_weight_kg = weight_kg;
        curve->max_weight_kg = weight_kg;
    } else {
        if (weight_kg < curve->min_weight_kg) {
            curve->min_weight_kg = weight_kg;
        }
        if (weight_kg > curve->max_weight_kg) {
            curve->max_weight_kg = weight_kg;
        }
    }

    return 0;
}

double fw_consumption_at_weight(const FWConsumptionCurve *curve, double weight_kg)
{
    if (!curve || !curve->pwl) {
        return NAN;
    }
    return sh_pwl_eval(curve->pwl, weight_kg);
}

double fw_consumption_max(const FWConsumptionCurve *curve)
{
    if (!curve || !curve->pwl) {
        return NAN;
    }
    return sh_pwl_max_y(curve->pwl);
}

int fw_consumption_curve_is_valid(const FWConsumptionCurve *curve)
{
    return curve && curve->pwl && sh_pwl_is_valid(curve->pwl);
}

/* ============================================================================
 * Built-in Curves
 * ============================================================================ */

FWConsumptionCurve *fw_curve_eu_standard(void)
{
    FWConsumptionCurve *curve = fw_consumption_curve_create(6);
    if (!curve) {
        return NULL;
    }

    /* European truck (40t GVW) - based on Volvo FH, Mercedes Actros */
    fw_consumption_curve_add_point(curve, 15000.0, 24.0);   /* Empty (tare) */
    fw_consumption_curve_add_point(curve, 20000.0, 26.5);   /* Light load */
    fw_consumption_curve_add_point(curve, 25000.0, 28.5);   /* Half load */
    fw_consumption_curve_add_point(curve, 30000.0, 31.0);   /* Heavy load */
    fw_consumption_curve_add_point(curve, 35000.0, 33.5);   /* Near max */
    fw_consumption_curve_add_point(curve, 40000.0, 36.5);   /* Maximum GVW */

    return curve;
}

FWConsumptionCurve *fw_curve_us_class8(void)
{
    FWConsumptionCurve *curve = fw_consumption_curve_create(6);
    if (!curve) {
        return NULL;
    }

    /* US Class 8 truck (36t GVW) - based on Freightliner Cascadia, Kenworth T680 */
    /* Higher consumption due to larger engines, higher speeds */
    fw_consumption_curve_add_point(curve, 13600.0, 28.0);   /* Empty (tare) */
    fw_consumption_curve_add_point(curve, 18000.0, 30.5);   /* Light load */
    fw_consumption_curve_add_point(curve, 23000.0, 33.0);   /* Half load */
    fw_consumption_curve_add_point(curve, 28000.0, 36.0);   /* Heavy load */
    fw_consumption_curve_add_point(curve, 33000.0, 39.0);   /* Near max */
    fw_consumption_curve_add_point(curve, 36000.0, 42.0);   /* Maximum GVW */

    return curve;
}

FWConsumptionCurve *fw_curve_light_truck(void)
{
    FWConsumptionCurve *curve = fw_consumption_curve_create(4);
    if (!curve) {
        return NULL;
    }

    /* Light truck (12t GVW) */
    fw_consumption_curve_add_point(curve, 5000.0, 15.0);    /* Empty */
    fw_consumption_curve_add_point(curve, 7500.0, 18.0);    /* Light load */
    fw_consumption_curve_add_point(curve, 10000.0, 21.0);   /* Half load */
    fw_consumption_curve_add_point(curve, 12000.0, 24.0);   /* Maximum GVW */

    return curve;
}

/* ============================================================================
 * Weight Profile
 * ============================================================================ */

FWWeightProfile *fw_weight_profile_create(double tare_weight_kg, double max_gvw_kg)
{
    if (tare_weight_kg < 0 || max_gvw_kg < tare_weight_kg) {
        return NULL;
    }

    FWWeightProfile *profile = malloc(sizeof(FWWeightProfile));
    if (!profile) {
        return NULL;
    }

    profile->step_func = sh_step_create(tare_weight_kg, 8);
    if (!profile->step_func) {
        free(profile);
        return NULL;
    }

    profile->tare_weight_kg = tare_weight_kg;
    profile->max_gvw_kg = max_gvw_kg;
    return profile;
}

void fw_weight_profile_free(FWWeightProfile *profile)
{
    if (profile) {
        sh_step_free(profile->step_func);
        free(profile);
    }
}

int fw_weight_profile_add_event(
    FWWeightProfile *profile,
    double distance_m,
    double weight_delta_kg
)
{
    if (!profile || !profile->step_func) {
        return -1;
    }

    /* Get current weight at end */
    double current_weight;
    if (profile->step_func->num_steps == 0) {
        current_weight = profile->tare_weight_kg;
    } else {
        current_weight = profile->step_func->y[profile->step_func->num_steps - 1];
    }

    double new_weight = current_weight + weight_delta_kg;

    /* Validate weight limits */
    if (new_weight < profile->tare_weight_kg) {
        return -1;  /* Would go below tare weight (negative cargo) */
    }
    if (new_weight > profile->max_gvw_kg) {
        return -1;  /* Would exceed max GVW */
    }

    return sh_step_add(profile->step_func, distance_m, weight_delta_kg);
}

int fw_weight_profile_validate(
    const FWWeightProfile *profile,
    char *error_msg,
    size_t msg_size
)
{
    if (!profile || !profile->step_func) {
        if (error_msg && msg_size > 0) {
            snprintf(error_msg, msg_size, "Profile is NULL");
        }
        return 0;
    }

    /* Check initial value */
    if (profile->step_func->initial_value < profile->tare_weight_kg) {
        if (error_msg && msg_size > 0) {
            snprintf(error_msg, msg_size, "Initial weight %.1f < tare %.1f",
                     profile->step_func->initial_value, profile->tare_weight_kg);
        }
        return 0;
    }

    /* Check each step */
    for (int i = 0; i < profile->step_func->num_steps; i++) {
        double weight = profile->step_func->y[i];
        if (weight < profile->tare_weight_kg) {
            if (error_msg && msg_size > 0) {
                snprintf(error_msg, msg_size,
                         "Weight %.1f at step %d < tare %.1f",
                         weight, i, profile->tare_weight_kg);
            }
            return 0;
        }
        if (weight > profile->max_gvw_kg) {
            if (error_msg && msg_size > 0) {
                snprintf(error_msg, msg_size,
                         "Weight %.1f at step %d > max GVW %.1f",
                         weight, i, profile->max_gvw_kg);
            }
            return 0;
        }
    }

    return 1;
}

double fw_weight_at_distance(const FWWeightProfile *profile, double distance_m)
{
    if (!profile || !profile->step_func) {
        return NAN;
    }
    return sh_step_eval(profile->step_func, distance_m);
}

double fw_weight_max(const FWWeightProfile *profile)
{
    if (!profile || !profile->step_func) {
        return NAN;
    }
    return sh_step_max(profile->step_func);
}

double fw_weight_min(const FWWeightProfile *profile)
{
    if (!profile || !profile->step_func) {
        return NAN;
    }
    return sh_step_min(profile->step_func);
}

/* ============================================================================
 * Fuel Calculation
 * ============================================================================ */

double fw_calc_fuel_for_segment(
    const FWConsumptionCurve *curve,
    const FWWeightProfile *profile,
    double from_m,
    double to_m
)
{
    if (!curve || !profile) {
        return NAN;
    }
    if (!fw_consumption_curve_is_valid(curve)) {
        return NAN;
    }
    if (!profile->step_func) {
        return NAN;
    }

    /* sh_step_pwl_integrate computes: ∫ pwl(step(x)) dx
     * where step = weight profile, pwl = consumption curve
     * Result is in (L/100km * m), divide by 100000 to get liters */
    double integral = sh_step_pwl_integrate(
        profile->step_func,
        curve->pwl,
        from_m,
        to_m
    );

    return integral / 100000.0;
}

double fw_calc_fuel_constant_weight(
    const FWConsumptionCurve *curve,
    double weight_kg,
    double from_m,
    double to_m
)
{
    if (!curve || !fw_consumption_curve_is_valid(curve)) {
        return NAN;
    }

    double consumption = fw_consumption_at_weight(curve, weight_kg);
    double distance_m = to_m - from_m;

    /* consumption is in L/100km, distance in meters */
    /* fuel = consumption * (distance_m / 100000) */
    return consumption * distance_m / 100000.0;
}
