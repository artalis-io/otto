/*
 * Piecewise-Linear Functions - Implementation
 *
 * Copyright (c) 2024-2026. All rights reserved.
 */

#include "sh_piecewise.h"
#include <stdlib.h>
#include <math.h>

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

SHPiecewiseLinear *sh_pwl_create(int initial_capacity)
{
    if (initial_capacity < 2) {
        initial_capacity = 2;
    }

    SHPiecewiseLinear *pwl = malloc(sizeof(SHPiecewiseLinear));
    if (!pwl) {
        return NULL;
    }

    pwl->x = malloc(initial_capacity * sizeof(double));
    pwl->y = malloc(initial_capacity * sizeof(double));
    if (!pwl->x || !pwl->y) {
        free(pwl->x);
        free(pwl->y);
        free(pwl);
        return NULL;
    }

    pwl->num_points = 0;
    pwl->capacity = initial_capacity;
    return pwl;
}

void sh_pwl_free(SHPiecewiseLinear *pwl)
{
    if (pwl) {
        free(pwl->x);
        free(pwl->y);
        free(pwl);
    }
}

/* ============================================================================
 * Building
 * ============================================================================ */

static int sh_pwl_grow(SHPiecewiseLinear *pwl)
{
    int new_capacity = pwl->capacity * 2;
    double *new_x = realloc(pwl->x, new_capacity * sizeof(double));
    double *new_y = realloc(pwl->y, new_capacity * sizeof(double));

    if (!new_x || !new_y) {
        /* Realloc failed, but original pointers still valid */
        if (new_x && new_x != pwl->x) free(new_x);
        if (new_y && new_y != pwl->y) free(new_y);
        return -1;
    }

    pwl->x = new_x;
    pwl->y = new_y;
    pwl->capacity = new_capacity;
    return 0;
}

int sh_pwl_add_point(SHPiecewiseLinear *pwl, double x, double y)
{
    if (!pwl) {
        return -1;
    }

    /* Check ordering */
    if (pwl->num_points > 0 && x <= pwl->x[pwl->num_points - 1]) {
        return -1;  /* X must be strictly increasing */
    }

    /* Grow if needed */
    if (pwl->num_points >= pwl->capacity) {
        if (sh_pwl_grow(pwl) != 0) {
            return -1;
        }
    }

    pwl->x[pwl->num_points] = x;
    pwl->y[pwl->num_points] = y;
    pwl->num_points++;
    return 0;
}

int sh_pwl_add_points(SHPiecewiseLinear *pwl, const double *x, const double *y, int n)
{
    if (!pwl || !x || !y || n <= 0) {
        return -1;
    }

    for (int i = 0; i < n; i++) {
        if (sh_pwl_add_point(pwl, x[i], y[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

/* ============================================================================
 * Evaluation
 * ============================================================================ */

/*
 * Binary search to find the segment containing x.
 * Returns index i such that x[i] <= x < x[i+1], or:
 *   -1 if x < x[0]
 *   num_points-1 if x >= x[num_points-1]
 */
static int sh_pwl_find_segment(const SHPiecewiseLinear *pwl, double x)
{
    if (x < pwl->x[0]) {
        return -1;
    }
    if (x >= pwl->x[pwl->num_points - 1]) {
        return pwl->num_points - 1;
    }

    /* Binary search */
    int lo = 0, hi = pwl->num_points - 1;
    while (lo < hi - 1) {
        int mid = (lo + hi) / 2;
        if (x < pwl->x[mid]) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    return lo;
}

double sh_pwl_eval(const SHPiecewiseLinear *pwl, double x)
{
    return sh_pwl_eval_ex(pwl, x, SH_PWL_CLAMP);
}

double sh_pwl_eval_ex(const SHPiecewiseLinear *pwl, double x, SHPwlExtrapolation mode)
{
    if (!pwl || pwl->num_points < 2) {
        return NAN;
    }

    int seg = sh_pwl_find_segment(pwl, x);

    /* Handle left extrapolation */
    if (seg < 0) {
        switch (mode) {
            case SH_PWL_CLAMP:
                return pwl->y[0];
            case SH_PWL_EXTRAPOLATE: {
                double slope = (pwl->y[1] - pwl->y[0]) / (pwl->x[1] - pwl->x[0]);
                return pwl->y[0] + slope * (x - pwl->x[0]);
            }
            case SH_PWL_NAN:
                return NAN;
        }
    }

    /* Handle right extrapolation */
    if (seg >= pwl->num_points - 1) {
        switch (mode) {
            case SH_PWL_CLAMP:
                return pwl->y[pwl->num_points - 1];
            case SH_PWL_EXTRAPOLATE: {
                int n = pwl->num_points;
                double slope = (pwl->y[n-1] - pwl->y[n-2]) / (pwl->x[n-1] - pwl->x[n-2]);
                return pwl->y[n-1] + slope * (x - pwl->x[n-1]);
            }
            case SH_PWL_NAN:
                return NAN;
        }
    }

    /* Linear interpolation within segment */
    double x0 = pwl->x[seg];
    double x1 = pwl->x[seg + 1];
    double y0 = pwl->y[seg];
    double y1 = pwl->y[seg + 1];
    double t = (x - x0) / (x1 - x0);
    return y0 + t * (y1 - y0);
}

/* ============================================================================
 * Integration
 * ============================================================================ */

/*
 * Integrate a single linear segment from a to b.
 * y(x) = y0 + (y1-y0)/(x1-x0) * (x - x0)
 * ∫y(x)dx = y0*x + (y1-y0)/(x1-x0) * (x^2/2 - x0*x)
 */
static double integrate_segment(double x0, double y0, double x1, double y1, double a, double b)
{
    if (a >= b) {
        return 0.0;
    }

    /* For constant segment (x0 == x1), just return y0 * width */
    if (x1 == x0) {
        return y0 * (b - a);
    }

    double slope = (y1 - y0) / (x1 - x0);

    /* Integral of y0 + slope*(x - x0) from a to b */
    /* = y0*(b-a) + slope*((b^2-a^2)/2 - x0*(b-a)) */
    /* = y0*(b-a) + slope*(b-a)*((b+a)/2 - x0) */
    double width = b - a;
    double mid = (a + b) / 2.0;
    return width * (y0 + slope * (mid - x0));
}

double sh_pwl_integrate(const SHPiecewiseLinear *pwl, double a, double b)
{
    if (!pwl || pwl->num_points < 2) {
        return NAN;
    }

    /* Handle reversed bounds */
    if (a > b) {
        return -sh_pwl_integrate(pwl, b, a);
    }
    if (a == b) {
        return 0.0;
    }

    double result = 0.0;
    double x_min = pwl->x[0];
    double x_max = pwl->x[pwl->num_points - 1];

    /* Handle left clamped region */
    if (a < x_min) {
        double end = (b < x_min) ? b : x_min;
        result += pwl->y[0] * (end - a);
        a = end;
    }

    /* Handle right clamped region */
    if (b > x_max) {
        double start = (a > x_max) ? a : x_max;
        result += pwl->y[pwl->num_points - 1] * (b - start);
        b = start;
    }

    /* Integrate over segments */
    if (a < b) {
        int seg_start = sh_pwl_find_segment(pwl, a);
        if (seg_start < 0) seg_start = 0;

        for (int i = seg_start; i < pwl->num_points - 1 && pwl->x[i] < b; i++) {
            double seg_a = (a > pwl->x[i]) ? a : pwl->x[i];
            double seg_b = (b < pwl->x[i + 1]) ? b : pwl->x[i + 1];

            if (seg_a < seg_b) {
                result += integrate_segment(
                    pwl->x[i], pwl->y[i],
                    pwl->x[i + 1], pwl->y[i + 1],
                    seg_a, seg_b
                );
            }
        }
    }

    return result;
}

/* ============================================================================
 * Utilities
 * ============================================================================ */

double sh_pwl_min_x(const SHPiecewiseLinear *pwl)
{
    if (!pwl || pwl->num_points == 0) {
        return NAN;
    }
    return pwl->x[0];
}

double sh_pwl_max_x(const SHPiecewiseLinear *pwl)
{
    if (!pwl || pwl->num_points == 0) {
        return NAN;
    }
    return pwl->x[pwl->num_points - 1];
}

double sh_pwl_min_y(const SHPiecewiseLinear *pwl)
{
    if (!pwl || pwl->num_points == 0) {
        return NAN;
    }
    double min = pwl->y[0];
    for (int i = 1; i < pwl->num_points; i++) {
        if (pwl->y[i] < min) {
            min = pwl->y[i];
        }
    }
    return min;
}

double sh_pwl_max_y(const SHPiecewiseLinear *pwl)
{
    if (!pwl || pwl->num_points == 0) {
        return NAN;
    }
    double max = pwl->y[0];
    for (int i = 1; i < pwl->num_points; i++) {
        if (pwl->y[i] > max) {
            max = pwl->y[i];
        }
    }
    return max;
}

int sh_pwl_is_valid(const SHPiecewiseLinear *pwl)
{
    return pwl && pwl->num_points >= 2;
}
