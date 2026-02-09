/*
 * Step Functions
 *
 * A function that holds constant values between discrete breakpoints.
 * Used for weight profiles, zone-based rates, discrete state changes.
 *
 * Copyright (c) 2024-2026. All rights reserved.
 */

#ifndef SH_STEPFUNC_H
#define SH_STEPFUNC_H

#include "sh_piecewise.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

/*
 * Step function: f(x) = y_i for x in [x_i, x_{i+1})
 * Value is constant between breakpoints.
 *
 * For x < x[0], returns initial_value.
 * For x >= x[n-1], returns y[n-1].
 */
typedef struct {
    int num_steps;          /* Number of step changes */
    int capacity;           /* Allocated capacity */
    double *x;              /* Step boundaries (sorted ascending) */
    double *y;              /* Value after each step boundary */
    double initial_value;   /* Value for x < x[0] */
} SHStepFunc;

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/*
 * Create an empty step function.
 *
 * Parameters:
 *   initial_value    - Value for x before first step
 *   initial_capacity - Initial capacity for steps (grows as needed)
 *
 * Returns:
 *   Allocated function, or NULL on allocation failure.
 *   Caller must call sh_step_free().
 */
SHStepFunc *sh_step_create(double initial_value, int initial_capacity);

/*
 * Free a step function.
 *
 * Parameters:
 *   sf - Function to free (can be NULL)
 */
void sh_step_free(SHStepFunc *sf);

/* ============================================================================
 * Building
 * ============================================================================ */

/*
 * Add a step that changes value by delta at position x.
 * Steps must be added in ascending X order.
 *
 * New value = previous value + delta
 *
 * Parameters:
 *   sf    - Function to modify
 *   x     - Position of step (must be > last added X)
 *   delta - Change in value at this step
 *
 * Returns:
 *   0 on success, -1 on error (out of order, allocation failure)
 */
int sh_step_add(SHStepFunc *sf, double x, double delta);

/*
 * Set absolute value starting at position x.
 * Steps must be added in ascending X order.
 *
 * Parameters:
 *   sf    - Function to modify
 *   x     - Position of step (must be > last added X)
 *   value - New absolute value at this step
 *
 * Returns:
 *   0 on success, -1 on error
 */
int sh_step_set(SHStepFunc *sf, double x, double value);

/* ============================================================================
 * Evaluation
 * ============================================================================ */

/*
 * Evaluate the function at a point.
 *
 * Parameters:
 *   sf - Function to evaluate
 *   x  - Point to evaluate at
 *
 * Returns:
 *   f(x), or NaN if sf is NULL
 */
double sh_step_eval(const SHStepFunc *sf, double x);

/* ============================================================================
 * Integration
 * ============================================================================ */

/*
 * Compute the definite integral ∫f(x)dx from a to b.
 *
 * Parameters:
 *   sf - Function to integrate
 *   a  - Lower bound
 *   b  - Upper bound
 *
 * Returns:
 *   ∫[a,b] f(x) dx, or NaN if sf is invalid
 */
double sh_step_integrate(const SHStepFunc *sf, double a, double b);

/*
 * Compute the integral of a composed function: ∫g(f(x))dx from a to b
 *
 * Where f is this step function and g is a piecewise-linear function.
 * This computes the composition g∘f, NOT the product g*f.
 *
 * Example: f(x) = weight at distance x
 *          g(w) = fuel consumption at weight w
 *          Result = total fuel consumed over [a,b]
 *
 * Algorithm:
 *   1. Find all step boundaries in [a, b]
 *   2. For each constant-value sub-segment [s, t]:
 *      a. value = sh_step_eval(sf, s)
 *      b. g_value = sh_pwl_eval(pwl, value)
 *      c. result += g_value * (t - s)
 *   3. Return result
 *
 * Parameters:
 *   sf  - Step function (e.g., weight profile)
 *   pwl - Piecewise-linear function (e.g., consumption curve)
 *   a   - Lower bound
 *   b   - Upper bound
 *
 * Returns:
 *   ∫[a,b] g(f(x)) dx, or NaN if inputs are invalid
 */
double sh_step_pwl_integrate(
    const SHStepFunc *sf,
    const SHPiecewiseLinear *pwl,
    double a,
    double b
);

/* ============================================================================
 * Utilities
 * ============================================================================ */

/*
 * Get the minimum value over all intervals.
 */
double sh_step_min(const SHStepFunc *sf);

/*
 * Get the maximum value over all intervals.
 */
double sh_step_max(const SHStepFunc *sf);

/*
 * Count the number of step changes in range [a, b].
 */
int sh_step_count_changes(const SHStepFunc *sf, double a, double b);

/*
 * Get the value just before a step boundary.
 * Returns initial_value if x <= first step.
 */
double sh_step_value_before(const SHStepFunc *sf, double x);

#ifdef __cplusplus
}
#endif

#endif /* SH_STEPFUNC_H */
