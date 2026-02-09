/*
 * Piecewise-Linear Functions
 *
 * A function defined by linear interpolation between (x, y) breakpoints.
 * Used for consumption curves, speed profiles, pricing tiers, etc.
 *
 * Copyright (c) 2024-2026. All rights reserved.
 */

#ifndef SH_PIECEWISE_H
#define SH_PIECEWISE_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

/*
 * Piecewise-linear function: f(x) defined by (x, y) breakpoints.
 * Interpolates linearly between points.
 */
typedef struct {
    int num_points;         /* Number of breakpoints (≥2 for valid function) */
    int capacity;           /* Allocated capacity */
    double *x;              /* X values (sorted ascending) */
    double *y;              /* Y values at each X */
} SHPiecewiseLinear;

/*
 * Extrapolation behavior for evaluation outside the defined range.
 */
typedef enum {
    SH_PWL_CLAMP,           /* Clamp to first/last Y value */
    SH_PWL_EXTRAPOLATE,     /* Extend first/last segment slope */
    SH_PWL_NAN              /* Return NaN outside range */
} SHPwlExtrapolation;

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/*
 * Create an empty piecewise-linear function.
 *
 * Parameters:
 *   initial_capacity - Initial capacity for points (grows as needed)
 *
 * Returns:
 *   Allocated function, or NULL on allocation failure.
 *   Caller must call sh_pwl_free().
 */
SHPiecewiseLinear *sh_pwl_create(int initial_capacity);

/*
 * Free a piecewise-linear function.
 *
 * Parameters:
 *   pwl - Function to free (can be NULL)
 */
void sh_pwl_free(SHPiecewiseLinear *pwl);

/* ============================================================================
 * Building
 * ============================================================================ */

/*
 * Add a point to the function.
 * Points must be added in ascending X order.
 *
 * Parameters:
 *   pwl - Function to modify
 *   x   - X coordinate (must be > last added X)
 *   y   - Y coordinate
 *
 * Returns:
 *   0 on success, -1 on error (out of order, allocation failure)
 */
int sh_pwl_add_point(SHPiecewiseLinear *pwl, double x, double y);

/*
 * Add multiple points to the function.
 * Points must be in ascending X order.
 *
 * Parameters:
 *   pwl - Function to modify
 *   x   - Array of X coordinates
 *   y   - Array of Y coordinates
 *   n   - Number of points
 *
 * Returns:
 *   0 on success, -1 on error
 */
int sh_pwl_add_points(SHPiecewiseLinear *pwl, const double *x, const double *y, int n);

/* ============================================================================
 * Evaluation
 * ============================================================================ */

/*
 * Evaluate the function at a point.
 * Uses SH_PWL_CLAMP for values outside the defined range.
 *
 * Parameters:
 *   pwl - Function to evaluate
 *   x   - Point to evaluate at
 *
 * Returns:
 *   f(x), or NaN if pwl is NULL or has < 2 points
 */
double sh_pwl_eval(const SHPiecewiseLinear *pwl, double x);

/*
 * Evaluate the function with explicit extrapolation mode.
 *
 * Parameters:
 *   pwl  - Function to evaluate
 *   x    - Point to evaluate at
 *   mode - Extrapolation behavior
 *
 * Returns:
 *   f(x), or NaN if pwl is invalid or mode is SH_PWL_NAN and x is outside range
 */
double sh_pwl_eval_ex(const SHPiecewiseLinear *pwl, double x, SHPwlExtrapolation mode);

/* ============================================================================
 * Integration
 * ============================================================================ */

/*
 * Compute the definite integral ∫f(x)dx from a to b.
 *
 * Uses clamping for regions outside the defined range.
 *
 * Parameters:
 *   pwl - Function to integrate
 *   a   - Lower bound
 *   b   - Upper bound
 *
 * Returns:
 *   ∫[a,b] f(x) dx, or NaN if pwl is invalid
 */
double sh_pwl_integrate(const SHPiecewiseLinear *pwl, double a, double b);

/* ============================================================================
 * Utilities
 * ============================================================================ */

/*
 * Get the minimum X value (left boundary).
 */
double sh_pwl_min_x(const SHPiecewiseLinear *pwl);

/*
 * Get the maximum X value (right boundary).
 */
double sh_pwl_max_x(const SHPiecewiseLinear *pwl);

/*
 * Get the minimum Y value across all points.
 */
double sh_pwl_min_y(const SHPiecewiseLinear *pwl);

/*
 * Get the maximum Y value across all points.
 */
double sh_pwl_max_y(const SHPiecewiseLinear *pwl);

/*
 * Check if the function is valid (has at least 2 points).
 */
int sh_pwl_is_valid(const SHPiecewiseLinear *pwl);

#ifdef __cplusplus
}
#endif

#endif /* SH_PIECEWISE_H */
