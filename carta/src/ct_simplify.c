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
 * Recursive Douglas-Peucker simplification.
 *
 * Marks points to keep in the 'keep' array.
 */
static void dp_recursive(const CTTilePoint *points, int start, int end,
                         float tolerance, int *keep)
{
    if (end <= start + 1) return;

    /* Find point with maximum distance from line segment */
    float max_dist = 0;
    int max_idx = start;

    for (int i = start + 1; i < end; i++) {
        float dist = perpendicular_distance(
            points[i].x, points[i].y,
            points[start].x, points[start].y,
            points[end].x, points[end].y
        );
        if (dist > max_dist) {
            max_dist = dist;
            max_idx = i;
        }
    }

    /* If max distance exceeds tolerance, keep the point and recurse */
    if (max_dist > tolerance) {
        keep[max_idx] = 1;
        dp_recursive(points, start, max_idx, tolerance, keep);
        dp_recursive(points, max_idx, end, tolerance, keep);
    }
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

    /* Run Douglas-Peucker */
    dp_recursive(points, 0, n - 1, tolerance, keep);

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
        dp_recursive(points, anchor, anchor2, tolerance, keep);
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
                dp_recursive(temp, 0, temp_n - 1, tolerance, temp_keep);

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
        dp_recursive(points, anchor2, anchor, tolerance, keep);
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
                dp_recursive(temp, 0, temp_n - 1, tolerance, temp_keep);

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

static void dp_recursive_geo(const CTCoord *coords, int start, int end,
                             double tolerance, int *keep)
{
    if (end <= start + 1) return;

    double max_dist = 0;
    int max_idx = start;

    for (int i = start + 1; i < end; i++) {
        double dist = perpendicular_distance_geo(
            coords[i].lon, coords[i].lat,
            coords[start].lon, coords[start].lat,
            coords[end].lon, coords[end].lat
        );
        if (dist > max_dist) {
            max_dist = dist;
            max_idx = i;
        }
    }

    if (max_dist > tolerance) {
        keep[max_idx] = 1;
        dp_recursive_geo(coords, start, max_idx, tolerance, keep);
        dp_recursive_geo(coords, max_idx, end, tolerance, keep);
    }
}

void ct_simplify_coords_inplace(CTCoord *coords, int *num_coords, double tolerance)
{
    int n = *num_coords;
    if (n <= 2) return;

    int *keep = calloc(n, sizeof(int));
    if (!keep) return;

    keep[0] = 1;
    keep[n - 1] = 1;

    dp_recursive_geo(coords, 0, n - 1, tolerance, keep);

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
