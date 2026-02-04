/*
 * ct_simplify.c - Geometry simplification using Douglas-Peucker algorithm
 */

#include "ct_simplify.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>

/* ============================================================================
 * Douglas-Peucker Implementation
 * ============================================================================ */

/*
 * Calculate perpendicular distance from point to line segment.
 */
static float perpendicular_distance(int px, int py,
                                    int x1, int y1, int x2, int y2)
{
    float dx = (float)(x2 - x1);
    float dy = (float)(y2 - y1);
    float len_sq = dx * dx + dy * dy;

    if (len_sq < 1e-10f) {
        /* Line segment is a point */
        dx = (float)(px - x1);
        dy = (float)(py - y1);
        return sqrtf(dx * dx + dy * dy);
    }

    /* Project point onto line and find distance */
    float t = ((px - x1) * dx + (py - y1) * dy) / len_sq;
    t = fmaxf(0.0f, fminf(1.0f, t));

    float proj_x = x1 + t * dx;
    float proj_y = y1 + t * dy;

    dx = px - proj_x;
    dy = py - proj_y;
    return sqrtf(dx * dx + dy * dy);
}

/*
 * Stack frame for iterative Douglas-Peucker.
 */
typedef struct {
    int start;
    int end;
} DPFrame;

/*
 * Iterative Douglas-Peucker simplification.
 *
 * Uses explicit stack to avoid stack overflow on long linestrings.
 * Marks points to keep in the 'keep' array.
 */
static void dp_iterative(const CTTilePoint *points, int start, int end,
                         float tolerance, int *keep, int n)
{
    if (end <= start + 1) return;

    /* Allocate stack - worst case is n/2 frames */
    int stack_cap = (n / 2) + 2;
    DPFrame *stack = malloc(stack_cap * sizeof(DPFrame));
    if (!stack) return;  /* Fail gracefully - no simplification */

    int stack_size = 0;

    /* Push initial frame */
    stack[stack_size].start = start;
    stack[stack_size].end = end;
    stack_size++;

    while (stack_size > 0) {
        /* Pop frame */
        stack_size--;
        int s = stack[stack_size].start;
        int e = stack[stack_size].end;

        if (e <= s + 1) continue;

        /* Find point with maximum distance from line segment */
        float max_dist = 0;
        int max_idx = s;

        for (int i = s + 1; i < e; i++) {
            float dist = perpendicular_distance(
                points[i].x, points[i].y,
                points[s].x, points[s].y,
                points[e].x, points[e].y
            );
            if (dist > max_dist) {
                max_dist = dist;
                max_idx = i;
            }
        }

        /* If max distance exceeds tolerance, keep the point and process subsegments */
        if (max_dist > tolerance) {
            keep[max_idx] = 1;

            /* Push both subsegments (order doesn't matter for correctness) */
            if (stack_size + 2 <= stack_cap) {
                stack[stack_size].start = s;
                stack[stack_size].end = max_idx;
                stack_size++;

                stack[stack_size].start = max_idx;
                stack[stack_size].end = e;
                stack_size++;
            }
        }
    }

    free(stack);
}

void ct_simplify_line_inplace(CTTilePoint *points, int *num_points, float tolerance)
{
    int n = *num_points;
    if (n <= 2) return;

    /* Allocate keep array */
    int *keep = calloc(n, sizeof(int));
    if (!keep) return;

    /* Always keep first and last */
    keep[0] = 1;
    keep[n - 1] = 1;

    /* Run Douglas-Peucker (iterative to avoid stack overflow) */
    dp_iterative(points, 0, n - 1, tolerance, keep, n);

    /* Compact the array */
    int write_idx = 0;
    for (int i = 0; i < n; i++) {
        if (keep[i]) {
            points[write_idx++] = points[i];
        }
    }

    *num_points = write_idx;
    free(keep);
}

void ct_simplify_poly_inplace(CTTilePoint *points, int *num_points, float tolerance)
{
    int n = *num_points;
    if (n <= 3) return;

    /* For polygons, we need to handle the wrap-around.
     * Strategy: find the point furthest from its neighbors as anchor,
     * then simplify the ring in two halves. */

    /* Allocate keep array */
    int *keep = calloc(n, sizeof(int));
    if (!keep) return;

    /* Find point with maximum curvature as anchor */
    float max_angle_change = 0;
    int anchor = 0;

    for (int i = 0; i < n; i++) {
        int prev = (i + n - 1) % n;
        int next = (i + 1) % n;

        float dx1 = (float)(points[i].x - points[prev].x);
        float dy1 = (float)(points[i].y - points[prev].y);
        float dx2 = (float)(points[next].x - points[i].x);
        float dy2 = (float)(points[next].y - points[i].y);

        /* Cross product indicates turn angle */
        float cross = dx1 * dy2 - dy1 * dx2;
        float angle_change = fabsf(cross);

        if (angle_change > max_angle_change) {
            max_angle_change = angle_change;
            anchor = i;
        }
    }

    /* Find second anchor (opposite side of polygon) */
    int anchor2 = (anchor + n / 2) % n;

    /* Keep both anchors */
    keep[anchor] = 1;
    keep[anchor2] = 1;

    /* Simplify first half: anchor to anchor2 */
    if (anchor2 > anchor) {
        dp_iterative(points, anchor, anchor2, tolerance, keep, n);
    } else {
        /* Wrap-around case: need to handle separately */
        CTTilePoint *temp = malloc((n - anchor + anchor2 + 1) * sizeof(CTTilePoint));
        if (temp) {
            int temp_n = 0;
            for (int i = anchor; i < n; i++) temp[temp_n++] = points[i];
            for (int i = 0; i <= anchor2; i++) temp[temp_n++] = points[i];

            int *temp_keep = calloc(temp_n, sizeof(int));
            if (temp_keep) {
                temp_keep[0] = 1;
                temp_keep[temp_n - 1] = 1;
                dp_iterative(temp, 0, temp_n - 1, tolerance, temp_keep, temp_n);

                /* Map back to original indices */
                int idx = anchor;
                for (int i = 0; i < temp_n; i++) {
                    if (temp_keep[i]) keep[idx] = 1;
                    idx = (idx + 1) % n;
                }
                free(temp_keep);
            }
            free(temp);
        }
    }

    /* Simplify second half: anchor2 to anchor */
    if (anchor > anchor2) {
        dp_iterative(points, anchor2, anchor, tolerance, keep, n);
    } else {
        /* Wrap-around case */
        CTTilePoint *temp = malloc((n - anchor2 + anchor + 1) * sizeof(CTTilePoint));
        if (temp) {
            int temp_n = 0;
            for (int i = anchor2; i < n; i++) temp[temp_n++] = points[i];
            for (int i = 0; i <= anchor; i++) temp[temp_n++] = points[i];

            int *temp_keep = calloc(temp_n, sizeof(int));
            if (temp_keep) {
                temp_keep[0] = 1;
                temp_keep[temp_n - 1] = 1;
                dp_iterative(temp, 0, temp_n - 1, tolerance, temp_keep, temp_n);

                int idx = anchor2;
                for (int i = 0; i < temp_n; i++) {
                    if (temp_keep[i]) keep[idx] = 1;
                    idx = (idx + 1) % n;
                }
                free(temp_keep);
            }
            free(temp);
        }
    }

    /* Compact the array */
    int write_idx = 0;
    for (int i = 0; i < n; i++) {
        if (keep[i]) {
            points[write_idx++] = points[i];
        }
    }

    /* Ensure at least 3 points for valid polygon */
    if (write_idx < 3) write_idx = (n < 3) ? n : 3;

    *num_points = write_idx;
    free(keep);
}

/* ============================================================================
 * Geographic Coordinate Simplification
 * ============================================================================ */

static double perpendicular_distance_geo(double px, double py,
                                         double x1, double y1,
                                         double x2, double y2)
{
    double dx = x2 - x1;
    double dy = y2 - y1;
    double len_sq = dx * dx + dy * dy;

    if (len_sq < 1e-20) {
        dx = px - x1;
        dy = py - y1;
        return sqrt(dx * dx + dy * dy);
    }

    double t = ((px - x1) * dx + (py - y1) * dy) / len_sq;
    t = fmax(0.0, fmin(1.0, t));

    double proj_x = x1 + t * dx;
    double proj_y = y1 + t * dy;

    dx = px - proj_x;
    dy = py - proj_y;
    return sqrt(dx * dx + dy * dy);
}

/*
 * Iterative Douglas-Peucker for geographic coordinates.
 */
static void dp_iterative_geo(const CTCoord *coords, int start, int end,
                             double tolerance, int *keep, int n)
{
    if (end <= start + 1) return;

    /* Allocate stack - worst case is n/2 frames */
    int stack_cap = (n / 2) + 2;
    DPFrame *stack = malloc(stack_cap * sizeof(DPFrame));
    if (!stack) return;

    int stack_size = 0;

    /* Push initial frame */
    stack[stack_size].start = start;
    stack[stack_size].end = end;
    stack_size++;

    while (stack_size > 0) {
        stack_size--;
        int s = stack[stack_size].start;
        int e = stack[stack_size].end;

        if (e <= s + 1) continue;

        double max_dist = 0;
        int max_idx = s;

        for (int i = s + 1; i < e; i++) {
            double dist = perpendicular_distance_geo(
                coords[i].lon, coords[i].lat,
                coords[s].lon, coords[s].lat,
                coords[e].lon, coords[e].lat
            );
            if (dist > max_dist) {
                max_dist = dist;
                max_idx = i;
            }
        }

        if (max_dist > tolerance) {
            keep[max_idx] = 1;

            if (stack_size + 2 <= stack_cap) {
                stack[stack_size].start = s;
                stack[stack_size].end = max_idx;
                stack_size++;

                stack[stack_size].start = max_idx;
                stack[stack_size].end = e;
                stack_size++;
            }
        }
    }

    free(stack);
}

void ct_simplify_coords_inplace(CTCoord *coords, int *num_coords, double tolerance)
{
    int n = *num_coords;
    if (n <= 2) return;

    int *keep = calloc(n, sizeof(int));
    if (!keep) return;

    keep[0] = 1;
    keep[n - 1] = 1;

    dp_iterative_geo(coords, 0, n - 1, tolerance, keep, n);

    int write_idx = 0;
    for (int i = 0; i < n; i++) {
        if (keep[i]) {
            coords[write_idx++] = coords[i];
        }
    }

    *num_coords = write_idx;
    free(keep);
}

/* ============================================================================
 * Tolerance Calculation
 * ============================================================================ */

float ct_simplify_tolerance(int zoom)
{
    /*
     * At zoom 14 (reference), tolerance = 1 unit (sub-pixel)
     * At lower zooms, tolerance increases (more simplification)
     * At higher zooms, tolerance decreases (less simplification)
     *
     * Tolerance doubles for each zoom level decrease.
     * MVT extent is 4096, so 1 pixel at z14 = 4096/256 = 16 units
     */
    int ref_zoom = 14;
    float ref_tolerance = 4.0f;  /* ~0.25 pixels at z14 */

    if (zoom >= ref_zoom) {
        /* Less simplification at higher zooms */
        return ref_tolerance / (float)(1 << (zoom - ref_zoom));
    } else {
        /* More simplification at lower zooms */
        return ref_tolerance * (float)(1 << (ref_zoom - zoom));
    }
}

double ct_simplify_tolerance_degrees(int zoom)
{
    /*
     * At equator: 360 degrees = 40,075 km = 40,075,000 m
     * 1 degree = ~111 km
     * At zoom 14: 1 tile = ~10 km, so ~0.09 degrees
     * 1 pixel at z14 = 10km/256 = ~39m = ~0.00035 degrees
     */
    double ref_tolerance = 0.0001;  /* ~11 meters at equator */
    int ref_zoom = 14;

    if (zoom >= ref_zoom) {
        return ref_tolerance / (double)(1 << (zoom - ref_zoom));
    } else {
        return ref_tolerance * (double)(1 << (ref_zoom - zoom));
    }
}
