/*
 * Step Functions - Implementation
 *
 * Copyright (c) 2024-2026. All rights reserved.
 */

#include "sh_stepfunc.h"
#include <stdlib.h>
#include <math.h>

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

SHStepFunc *sh_step_create(double initial_value, int initial_capacity)
{
    if (initial_capacity < 4) {
        initial_capacity = 4;
    }

    SHStepFunc *sf = malloc(sizeof(SHStepFunc));
    if (!sf) {
        return NULL;
    }

    sf->x = malloc(initial_capacity * sizeof(double));
    sf->y = malloc(initial_capacity * sizeof(double));
    if (!sf->x || !sf->y) {
        free(sf->x);
        free(sf->y);
        free(sf);
        return NULL;
    }

    sf->num_steps = 0;
    sf->capacity = initial_capacity;
    sf->initial_value = initial_value;
    return sf;
}

void sh_step_free(SHStepFunc *sf)
{
    if (sf) {
        free(sf->x);
        free(sf->y);
        free(sf);
    }
}

/* ============================================================================
 * Building
 * ============================================================================ */

static int sh_step_grow(SHStepFunc *sf)
{
    int new_capacity = sf->capacity * 2;
    double *new_x = realloc(sf->x, new_capacity * sizeof(double));
    double *new_y = realloc(sf->y, new_capacity * sizeof(double));

    if (!new_x || !new_y) {
        if (new_x && new_x != sf->x) free(new_x);
        if (new_y && new_y != sf->y) free(new_y);
        return -1;
    }

    sf->x = new_x;
    sf->y = new_y;
    sf->capacity = new_capacity;
    return 0;
}

int sh_step_add(SHStepFunc *sf, double x, double delta)
{
    if (!sf) {
        return -1;
    }

    /* Check ordering */
    if (sf->num_steps > 0 && x <= sf->x[sf->num_steps - 1]) {
        return -1;
    }

    /* Grow if needed */
    if (sf->num_steps >= sf->capacity) {
        if (sh_step_grow(sf) != 0) {
            return -1;
        }
    }

    /* Compute new absolute value */
    double prev_value = (sf->num_steps == 0) ? sf->initial_value : sf->y[sf->num_steps - 1];

    sf->x[sf->num_steps] = x;
    sf->y[sf->num_steps] = prev_value + delta;
    sf->num_steps++;
    return 0;
}

int sh_step_set(SHStepFunc *sf, double x, double value)
{
    if (!sf) {
        return -1;
    }

    /* Check ordering */
    if (sf->num_steps > 0 && x <= sf->x[sf->num_steps - 1]) {
        return -1;
    }

    /* Grow if needed */
    if (sf->num_steps >= sf->capacity) {
        if (sh_step_grow(sf) != 0) {
            return -1;
        }
    }

    sf->x[sf->num_steps] = x;
    sf->y[sf->num_steps] = value;
    sf->num_steps++;
    return 0;
}

/* ============================================================================
 * Evaluation
 * ============================================================================ */

/*
 * Binary search to find the step index for x.
 * Returns index i such that x[i] <= x < x[i+1], or:
 *   -1 if x < x[0] (use initial_value)
 *   num_steps-1 if x >= x[num_steps-1]
 */
static int sh_step_find_index(const SHStepFunc *sf, double x)
{
    if (sf->num_steps == 0 || x < sf->x[0]) {
        return -1;
    }

    /* Binary search for rightmost step <= x */
    int lo = 0, hi = sf->num_steps - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (sf->x[mid] <= x) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

double sh_step_eval(const SHStepFunc *sf, double x)
{
    if (!sf) {
        return NAN;
    }

    int idx = sh_step_find_index(sf, x);
    if (idx < 0) {
        return sf->initial_value;
    }
    return sf->y[idx];
}

/* ============================================================================
 * Integration
 * ============================================================================ */

double sh_step_integrate(const SHStepFunc *sf, double a, double b)
{
    if (!sf) {
        return NAN;
    }

    /* Handle reversed bounds */
    if (a > b) {
        return -sh_step_integrate(sf, b, a);
    }
    if (a == b) {
        return 0.0;
    }

    double result = 0.0;
    double pos = a;

    /* Find starting step */
    int idx = sh_step_find_index(sf, a);
    double current_value = (idx < 0) ? sf->initial_value : sf->y[idx];

    /* Integrate through each step */
    for (int i = (idx < 0 ? 0 : idx + 1); i < sf->num_steps && pos < b; i++) {
        double next_pos = sf->x[i];
        if (next_pos > b) {
            next_pos = b;
        }
        if (next_pos > pos) {
            result += current_value * (next_pos - pos);
            pos = next_pos;
        }
        if (i < sf->num_steps) {
            current_value = sf->y[i];
        }
    }

    /* Handle remaining region after last step */
    if (pos < b) {
        result += current_value * (b - pos);
    }

    return result;
}

double sh_step_pwl_integrate(
    const SHStepFunc *sf,
    const SHPiecewiseLinear *pwl,
    double a,
    double b
)
{
    if (!sf || !pwl || !sh_pwl_is_valid(pwl)) {
        return NAN;
    }

    /* Handle reversed bounds */
    if (a > b) {
        return -sh_step_pwl_integrate(sf, pwl, b, a);
    }
    if (a == b) {
        return 0.0;
    }

    double result = 0.0;
    double pos = a;

    /* Find starting step */
    int idx = sh_step_find_index(sf, a);
    double current_step_value = (idx < 0) ? sf->initial_value : sf->y[idx];

    /* Integrate through each step */
    for (int i = (idx < 0 ? 0 : idx + 1); i <= sf->num_steps && pos < b; i++) {
        /* Determine the end of this constant-value region */
        double next_pos;
        if (i < sf->num_steps) {
            next_pos = sf->x[i];
            if (next_pos > b) {
                next_pos = b;
            }
        } else {
            next_pos = b;
        }

        if (next_pos > pos) {
            /* Evaluate the piecewise-linear function at the current step value */
            double pwl_value = sh_pwl_eval(pwl, current_step_value);
            result += pwl_value * (next_pos - pos);
            pos = next_pos;
        }

        /* Update to next step's value */
        if (i < sf->num_steps) {
            current_step_value = sf->y[i];
        }
    }

    return result;
}

/* ============================================================================
 * Utilities
 * ============================================================================ */

double sh_step_min(const SHStepFunc *sf)
{
    if (!sf) {
        return NAN;
    }

    double min = sf->initial_value;
    for (int i = 0; i < sf->num_steps; i++) {
        if (sf->y[i] < min) {
            min = sf->y[i];
        }
    }
    return min;
}

double sh_step_max(const SHStepFunc *sf)
{
    if (!sf) {
        return NAN;
    }

    double max = sf->initial_value;
    for (int i = 0; i < sf->num_steps; i++) {
        if (sf->y[i] > max) {
            max = sf->y[i];
        }
    }
    return max;
}

int sh_step_count_changes(const SHStepFunc *sf, double a, double b)
{
    if (!sf || a > b) {
        return 0;
    }

    int count = 0;
    for (int i = 0; i < sf->num_steps; i++) {
        if (sf->x[i] >= a && sf->x[i] <= b) {
            count++;
        }
    }
    return count;
}

double sh_step_value_before(const SHStepFunc *sf, double x)
{
    if (!sf) {
        return NAN;
    }

    /* Find the step just before x */
    int idx = -1;
    for (int i = 0; i < sf->num_steps; i++) {
        if (sf->x[i] < x) {
            idx = i;
        } else {
            break;
        }
    }

    if (idx < 0) {
        return sf->initial_value;
    }
    return sf->y[idx];
}
