/**
 * Clay Components - Polyline Simplification
 *
 * Douglas-Peucker algorithm for reducing polyline point count.
 */

#include "cs_map_internal.h"
#include "cs_internal.h"  /* For cs_record_error, cs_alloc, cs_free */
#include <math.h>
#include <string.h>  /* For memset */

/* ============================================================================
 * Perpendicular Distance Calculation
 * ============================================================================ */

/**
 * Calculate perpendicular distance from point to line segment.
 * Uses geographic coordinates directly (works for small areas).
 */
static double perpendicular_distance(
    double px, double py,
    double x1, double y1,
    double x2, double y2
) {
    double dx = x2 - x1;
    double dy = y2 - y1;
    double len_sq = dx * dx + dy * dy;

    if (len_sq < 1e-12) {
        /* Line is a point */
        dx = px - x1;
        dy = py - y1;
        return sqrt(dx * dx + dy * dy);
    }

    /* Project point onto line */
    double t = ((px - x1) * dx + (py - y1) * dy) / len_sq;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    double proj_x = x1 + t * dx;
    double proj_y = y1 + t * dy;

    dx = px - proj_x;
    dy = py - proj_y;
    return sqrt(dx * dx + dy * dy);
}

/* ============================================================================
 * Douglas-Peucker Algorithm (Iterative)
 * ============================================================================ */

/**
 * Douglas-Peucker iterative implementation.
 * Marks points to keep in the 'keep' array.
 * Uses explicit stack to avoid call stack overflow on large polylines.
 */
static void douglas_peucker_iterative(
    const CsGeoPoint *points,
    int start, int end,
    double epsilon,
    bool *keep
) {
    /* Explicit stack of (start, end) pairs */
    struct { int start; int end; } stack[DP_STACK_SIZE];
    int stack_top = 0;

    /* Push initial range */
    stack[stack_top].start = start;
    stack[stack_top].end = end;
    stack_top++;

    while (stack_top > 0) {
        /* Pop */
        stack_top--;
        int s = stack[stack_top].start;
        int e = stack[stack_top].end;

        if (e <= s + 1) {
            continue;
        }

        /* Find point with maximum distance from line */
        double max_dist = 0.0;
        int max_idx = s;

        double x1 = points[s].lon;
        double y1 = points[s].lat;
        double x2 = points[e].lon;
        double y2 = points[e].lat;

        for (int i = s + 1; i < e; i++) {
            double dist = perpendicular_distance(
                points[i].lon, points[i].lat,
                x1, y1, x2, y2
            );
            if (dist > max_dist) {
                max_dist = dist;
                max_idx = i;
            }
        }

        /* If max distance exceeds epsilon, split and push both halves */
        if (max_dist > epsilon) {
            keep[max_idx] = true;

            /* Push both sub-ranges if stack has space */
            if (stack_top < DP_STACK_SIZE - 1) {
                stack[stack_top].start = s;
                stack[stack_top].end = max_idx;
                stack_top++;
                stack[stack_top].start = max_idx;
                stack[stack_top].end = e;
                stack_top++;
            }
            /* If stack is full, we lose some precision but don't crash */
        }
    }
}

/**
 * Count how many points Douglas-Peucker would keep with given epsilon.
 */
static int count_kept_points(const CsGeoPoint *points, int count, double epsilon) {
    if (count <= 2) return count;

    bool *keep = (bool *)cs_alloc((size_t)count * sizeof(bool));
    if (!keep) {
        /* cs_alloc already records the error */
        return 2;
    }
    memset(keep, 0, (size_t)count * sizeof(bool));

    keep[0] = true;
    keep[count - 1] = true;
    douglas_peucker_iterative(points, 0, count - 1, epsilon, keep);

    int kept = 0;
    for (int i = 0; i < count; i++) {
        if (keep[i]) kept++;
    }
    cs_free(keep);
    return kept;
}

/* ============================================================================
 * Public Simplification API
 * ============================================================================ */

/**
 * Simplify a polyline using Douglas-Peucker algorithm with adaptive epsilon.
 * Automatically increases epsilon until output fits within capacity.
 * Returns the number of points in the simplified output.
 *
 * @param points      Input points
 * @param count       Number of input points
 * @param epsilon     Initial tolerance in degrees
 * @param out         Output buffer (can be same as input for in-place)
 * @param out_capacity Maximum output points
 */
int cs_map_simplify_polyline(
    const CsGeoPoint *points,
    int count,
    double epsilon,
    CsGeoPoint *out,
    int out_capacity
) {
    if (count <= 2) {
        /* Nothing to simplify */
        int n = (count < out_capacity) ? count : out_capacity;
        if (out != points) {
            for (int i = 0; i < n; i++) {
                out[i] = points[i];
            }
        }
        return n;
    }

    if (out_capacity < 2) {
        out[0] = points[0];
        return 1;
    }

    /* Adaptive epsilon: increase until we fit within capacity */
    double current_epsilon = epsilon;
    int kept = count_kept_points(points, count, current_epsilon);

    /* Double epsilon until we fit (max 20 iterations to prevent infinite loop) */
    for (int iter = 0; iter < 20 && kept > out_capacity; iter++) {
        current_epsilon *= 2.0;
        kept = count_kept_points(points, count, current_epsilon);
    }

    /* If still too many, use uniform sampling as last resort */
    if (kept > out_capacity) {
        /* Sample evenly, always including first and last */
        out[0] = points[0];
        out[out_capacity - 1] = points[count - 1];

        if (out_capacity > 2) {
            double step = (double)(count - 1) / (double)(out_capacity - 1);
            for (int i = 1; i < out_capacity - 1; i++) {
                int idx = (int)(i * step);
                if (idx >= count) idx = count - 1;
                out[i] = points[idx];
            }
        }
        return out_capacity;
    }

    /* Allocate keep flags for final pass using custom allocator */
    bool *keep = (bool *)cs_alloc((size_t)count * sizeof(bool));
    if (!keep) {
        /* Fallback: just copy first/last (error already recorded) */
        out[0] = points[0];
        out[1] = points[count - 1];
        return 2;
    }
    memset(keep, 0, (size_t)count * sizeof(bool));

    /* Always keep first and last */
    keep[0] = true;
    keep[count - 1] = true;

    /* Run Douglas-Peucker with adaptive epsilon */
    douglas_peucker_iterative(points, 0, count - 1, current_epsilon, keep);

    /* Copy kept points to output */
    int out_count = 0;
    for (int i = 0; i < count && out_count < out_capacity; i++) {
        if (keep[i]) {
            out[out_count++] = points[i];
        }
    }

    cs_free(keep);
    return out_count;
}
