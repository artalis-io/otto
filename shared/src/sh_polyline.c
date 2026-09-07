/*
 * sh_polyline.c - Polyline utilities implementation
 *
 * Consolidates polyline code from:
 * - velo/src/polyline.c (Google Polyline encoding/decoding)
 * - fuelwise/src/fw_geo.c (polyline geometry functions)
 * - clayshards/clay-shards/src/cs_map_simplify.c (Douglas-Peucker)
 */

#include "sh_polyline.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ============================================================================
 * Google Polyline Encoding/Decoding
 * ============================================================================ */

/*
 * Encode a single value using the Google Polyline algorithm.
 * Returns number of characters written.
 */
static size_t encode_value(int value, char *output, size_t capacity)
{
    size_t written = 0;

    /* Left-shift and invert if negative.
     *
     * Done in unsigned. Left-shifting a negative int is undefined behaviour
     * (C11 6.5.7p4), and `value << 1` can also overflow a signed int for a
     * large coordinate delta, which is equally undefined. The bit pattern is
     * identical, and `>>= 5` becomes a logical shift -- correct here, because
     * the value is always non-negative after the inversion. */
    uint32_t uv = (uint32_t)value;
    uint32_t encoded = uv << 1;
    if (value < 0) encoded = ~encoded;

    /* Break into 5-bit chunks */
    while (encoded >= 0x20) {
        if (written >= capacity) return 0;
        output[written++] = (char)((encoded & 0x1F) | 0x20) + 63;
        encoded >>= 5;
    }

    if (written >= capacity) return 0;
    output[written++] = (char)(encoded + 63);

    return written;
}

size_t sh_polyline_encode(const double *coords, size_t count, int precision,
                          char *output, size_t capacity)
{
    if (!coords || !output || count == 0 || capacity == 0) return 0;

    double factor = pow(10, precision);
    int prev_lat = 0;
    int prev_lon = 0;
    size_t written = 0;

    for (size_t i = 0; i < count; i++) {
        double lat = coords[i * 2];
        double lon = coords[i * 2 + 1];

        /* Round to precision and convert to integer */
        int curr_lat = (int)round(lat * factor);
        int curr_lon = (int)round(lon * factor);

        /* Calculate deltas */
        int dlat = curr_lat - prev_lat;
        int dlon = curr_lon - prev_lon;

        /* Encode latitude delta */
        size_t n = encode_value(dlat, output + written, capacity - written);
        if (n == 0 && dlat != 0) return 0;
        written += n;

        /* Encode longitude delta */
        n = encode_value(dlon, output + written, capacity - written);
        if (n == 0 && dlon != 0) return 0;
        written += n;

        prev_lat = curr_lat;
        prev_lon = curr_lon;
    }

    /* Null terminate */
    if (written >= capacity) return 0;
    output[written] = '\0';

    return written;
}

size_t sh_polyline_decode(const char *encoded, int precision,
                          double *coords, size_t capacity)
{
    if (!encoded || !coords || capacity == 0) return 0;

    double factor = pow(10, precision);
    size_t len = strlen(encoded);
    size_t pos = 0;
    size_t count = 0;
    int lat = 0;
    int lon = 0;

    while (pos < len) {
        /* Decode latitude */
        int result = 0;
        int shift = 0;
        int b;
        do {
            if (pos >= len) return 0;
            if (shift > 30) return 0;  /* Prevent overflow from malformed input */
            b = encoded[pos++] - 63;
            result |= (b & 0x1F) << shift;
            shift += 5;
        } while (b >= 0x20);
        int dlat = (result & 1) ? ~(result >> 1) : (result >> 1);
        lat += dlat;

        /* Decode longitude */
        result = 0;
        shift = 0;
        do {
            if (pos >= len) return 0;
            if (shift > 30) return 0;  /* Prevent overflow from malformed input */
            b = encoded[pos++] - 63;
            result |= (b & 0x1F) << shift;
            shift += 5;
        } while (b >= 0x20);
        int dlon = (result & 1) ? ~(result >> 1) : (result >> 1);
        lon += dlon;

        /* Store coordinate */
        if (count >= capacity) break;
        coords[count * 2] = lat / factor;
        coords[count * 2 + 1] = lon / factor;
        count++;
    }

    return count;
}

size_t sh_polyline_max_encoded_size(size_t count)
{
    /*
     * Each coordinate pair (lat, lon) can have deltas.
     * Worst case: each value needs up to 6 chunks of 5 bits = 6 chars.
     * Two values per point = 12 chars max per point.
     * Add 1 for null terminator.
     */
    if (count > (SIZE_MAX - 1) / 12) {
        return SIZE_MAX;  /* Overflow protection - caller should check */
    }
    return count * 12 + 1;
}

/* ============================================================================
 * Polyline Geometry - Internal Helpers
 * ============================================================================ */

/*
 * Convert lat/lon to local Cartesian coordinates (meters from reference point).
 * Uses equirectangular approximation, suitable for small areas.
 */
static void latlon_to_local(SHCoord coord, SHCoord ref, double *x, double *y)
{
    double cos_lat = cos(ref.lat * SH_DEG_TO_RAD);
    *x = (coord.lon - ref.lon) * SH_DEG_TO_RAD * SH_EARTH_RADIUS_M * cos_lat;
    *y = (coord.lat - ref.lat) * SH_DEG_TO_RAD * SH_EARTH_RADIUS_M;
}

/*
 * Convert local Cartesian coordinates back to lat/lon.
 */
static void local_to_latlon(double x, double y, SHCoord ref, SHCoord *coord)
{
    double cos_lat = cos(ref.lat * SH_DEG_TO_RAD);

    /* Guard against division by near-zero at poles (|lat| > 89.9 degrees). */
    if (cos_lat < 1e-6) {
        cos_lat = 1e-6;
    }

    coord->lon = ref.lon + (x / (SH_EARTH_RADIUS_M * cos_lat)) * SH_RAD_TO_DEG;
    coord->lat = ref.lat + (y / SH_EARTH_RADIUS_M) * SH_RAD_TO_DEG;
}

/*
 * Project a point onto a line segment in local Cartesian coordinates.
 * Returns the parameter t along the segment [0,1].
 */
static double project_point_on_segment(
    double px, double py,
    double ax, double ay,
    double bx, double by,
    double *proj_x, double *proj_y)
{
    double dx = bx - ax;
    double dy = by - ay;
    double len_sq = dx * dx + dy * dy;

    if (len_sq < 1e-12) {
        /* Degenerate segment */
        *proj_x = ax;
        *proj_y = ay;
        return 0.0;
    }

    double t = ((px - ax) * dx + (py - ay) * dy) / len_sq;

    /* Clamp t to [0, 1] */
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    *proj_x = ax + t * dx;
    *proj_y = ay + t * dy;
    return t;
}

/* ============================================================================
 * Polyline Geometry - Public API
 * ============================================================================ */

double sh_polyline_length(const SHPolyline *polyline)
{
    if (polyline == NULL || polyline->count < 2) {
        return 0.0;
    }

    double total = 0.0;
    for (int i = 0; i < polyline->count - 1; i++) {
        total += sh_haversine(polyline->points[i], polyline->points[i + 1]);
    }
    return total;
}

double sh_point_to_segment_distance(
    SHCoord point,
    SHCoord seg_start,
    SHCoord seg_end,
    SHCoord *closest)
{
    /* Use midpoint as reference for local coordinate system */
    SHCoord ref = {
        .lat = (seg_start.lat + seg_end.lat) / 2.0,
        .lon = (seg_start.lon + seg_end.lon) / 2.0
    };

    /* Convert all points to local Cartesian */
    double px, py, ax, ay, bx, by;
    latlon_to_local(point, ref, &px, &py);
    latlon_to_local(seg_start, ref, &ax, &ay);
    latlon_to_local(seg_end, ref, &bx, &by);

    /* Project point onto segment */
    double proj_x, proj_y;
    project_point_on_segment(px, py, ax, ay, bx, by, &proj_x, &proj_y);

    /* Convert projection back to lat/lon if requested */
    if (closest != NULL) {
        local_to_latlon(proj_x, proj_y, ref, closest);
    }

    /* Calculate distance in local coordinates */
    double dx = px - proj_x;
    double dy = py - proj_y;
    return sqrt(dx * dx + dy * dy);
}

double sh_find_closest_on_polyline(
    SHCoord point,
    const SHPolyline *polyline,
    int *segment_index,
    double *t,
    SHCoord *closest)
{
    if (polyline == NULL || polyline->count < 1) {
        return -1.0;
    }

    if (polyline->count == 1) {
        if (segment_index) *segment_index = 0;
        if (t) *t = 0.0;
        if (closest) *closest = polyline->points[0];
        return sh_haversine(point, polyline->points[0]);
    }

    double min_distance = 1e18;
    int best_segment = 0;
    double best_t = 0.0;
    SHCoord best_closest = polyline->points[0];

    for (int i = 0; i < polyline->count - 1; i++) {
        SHCoord snap;
        double dist = sh_point_to_segment_distance(
            point,
            polyline->points[i],
            polyline->points[i + 1],
            &snap
        );

        if (dist < min_distance) {
            min_distance = dist;
            best_segment = i;
            best_closest = snap;

            /* Calculate t parameter */
            double seg_len = sh_haversine(
                polyline->points[i],
                polyline->points[i + 1]
            );
            double dist_to_snap = sh_haversine(
                polyline->points[i],
                snap
            );
            best_t = (seg_len > 1e-9) ? dist_to_snap / seg_len : 0.0;
        }
    }

    if (segment_index) *segment_index = best_segment;
    if (t) *t = best_t;
    if (closest) *closest = best_closest;
    return min_distance;
}

double sh_distance_along_polyline(
    const SHPolyline *polyline,
    int segment_index,
    double t)
{
    if (polyline == NULL || polyline->count < 2) {
        return 0.0;
    }

    /* Bounds check on segment_index */
    if (segment_index < 0 || segment_index >= polyline->count) {
        return 0.0;
    }

    double total_distance = 0.0;

    /* Sum up complete segments before the target segment */
    for (int i = 0; i < segment_index && i < polyline->count - 1; i++) {
        total_distance += sh_haversine(
            polyline->points[i],
            polyline->points[i + 1]
        );
    }

    /* Add partial distance within the target segment */
    if (segment_index < polyline->count - 1) {
        double seg_len = sh_haversine(
            polyline->points[segment_index],
            polyline->points[segment_index + 1]
        );
        total_distance += t * seg_len;
    }

    return total_distance;
}

int sh_subsample_polyline(
    const SHPolyline *polyline,
    int target_points,
    SHPolyline *result)
{
    if (polyline == NULL || result == NULL || target_points < 2) {
        return -1;
    }

    if (polyline->count <= target_points) {
        /* No subsampling needed, copy the polyline */
        result->count = polyline->count;
        /* Check for overflow before allocation */
        if (result->count < 0 ||
            (size_t)result->count > SIZE_MAX / sizeof(SHCoord)) {
            return -1;
        }
        result->points = malloc((size_t)result->count * sizeof(SHCoord));
        if (result->points == NULL) {
            return -1;
        }
        for (int i = 0; i < result->count; i++) {
            result->points[i] = polyline->points[i];
        }
        return 0;
    }

    /* Calculate step size */
    int step = (polyline->count - 1) / (target_points - 1);
    if (step < 1) step = 1;

    /* Count actual points we'll include */
    int count = 0;
    for (int i = 0; i < polyline->count; i += step) {
        count++;
    }
    /* Always include last point */
    if ((polyline->count - 1) % step != 0) {
        count++;
    }

    /* Allocate result - check for overflow */
    if (count < 0 || (size_t)count > SIZE_MAX / sizeof(SHCoord)) {
        return -1;
    }
    result->points = malloc((size_t)count * sizeof(SHCoord));
    if (result->points == NULL) {
        return -1;
    }
    result->count = count;

    /* Copy points */
    int idx = 0;
    for (int i = 0; i < polyline->count && idx < count - 1; i += step) {
        result->points[idx++] = polyline->points[i];
    }
    /* Always include last point */
    result->points[idx] = polyline->points[polyline->count - 1];

    return 0;
}

/* ============================================================================
 * Douglas-Peucker Simplification
 * ============================================================================ */

/*
 * Calculate perpendicular distance from point to line segment.
 * Uses geographic coordinates directly (works for small areas).
 */
static double perpendicular_distance_deg(
    double px, double py,
    double x1, double y1,
    double x2, double y2)
{
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

/*
 * Douglas-Peucker iterative implementation.
 * Marks points to keep in the 'keep' array.
 * Uses explicit stack to avoid call stack overflow on large polylines.
 */
static void douglas_peucker_iterative(
    const SHCoord *points,
    int start, int end,
    double epsilon,
    bool *keep)
{
    /* Explicit stack of (start, end) pairs */
    struct { int start; int end; } stack[SH_DP_STACK_SIZE];
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
            double dist = perpendicular_distance_deg(
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
            if (stack_top < SH_DP_STACK_SIZE - 1) {
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

/*
 * Count how many points Douglas-Peucker would keep with given epsilon.
 */
static int count_kept_points(const SHCoord *points, int count, double epsilon)
{
    if (count <= 2) return count;

    /* Check for integer overflow before allocation */
    if (count < 0 || (size_t)count > SIZE_MAX / sizeof(bool)) {
        return 2;
    }

    bool *keep = (bool *)malloc((size_t)count * sizeof(bool));
    if (!keep) {
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
    free(keep);
    return kept;
}

int sh_polyline_simplify(
    const SHPolyline *input,
    double epsilon,
    SHPolyline *output,
    int output_capacity)
{
    if (!input || !output || output_capacity < 1) {
        return -1;
    }

    /* Handle single point case */
    if (input->count == 1) {
        output->points[0] = input->points[0];
        output->count = 1;
        return 1;
    }

    /* Need at least capacity 2 for normal simplification */
    if (output_capacity < 2) {
        output->points[0] = input->points[0];
        output->count = 1;
        return 1;
    }

    if (input->count <= 2) {
        /* Nothing to simplify */
        int n = (input->count < output_capacity) ? input->count : output_capacity;
        for (int i = 0; i < n; i++) {
            output->points[i] = input->points[i];
        }
        output->count = n;
        return n;
    }

    /* Allocate keep flags */
    if (input->count < 0 || (size_t)input->count > SIZE_MAX / sizeof(bool)) {
        return -1;
    }

    bool *keep = (bool *)malloc((size_t)input->count * sizeof(bool));
    if (!keep) {
        /* Fallback: just copy first/last */
        output->points[0] = input->points[0];
        output->points[1] = input->points[input->count - 1];
        output->count = 2;
        return 2;
    }
    memset(keep, 0, (size_t)input->count * sizeof(bool));

    /* Always keep first and last */
    keep[0] = true;
    keep[input->count - 1] = true;

    /* Run Douglas-Peucker */
    douglas_peucker_iterative(input->points, 0, input->count - 1, epsilon, keep);

    /* Copy kept points to output */
    int out_count = 0;
    for (int i = 0; i < input->count && out_count < output_capacity; i++) {
        if (keep[i]) {
            output->points[out_count++] = input->points[i];
        }
    }

    free(keep);
    output->count = out_count;
    return out_count;
}

int sh_polyline_simplify_adaptive(
    const SHPolyline *input,
    double initial_epsilon,
    SHPolyline *output,
    int output_capacity)
{
    if (!input || !output || output_capacity < 2) {
        return -1;
    }

    if (input->count <= 2) {
        int n = (input->count < output_capacity) ? input->count : output_capacity;
        for (int i = 0; i < n; i++) {
            output->points[i] = input->points[i];
        }
        output->count = n;
        return n;
    }

    /* Adaptive epsilon: increase until we fit within capacity */
    double current_epsilon = initial_epsilon;
    int kept = count_kept_points(input->points, input->count, current_epsilon);

    /* Double epsilon until we fit (max 20 iterations to prevent infinite loop) */
    for (int iter = 0; iter < 20 && kept > output_capacity; iter++) {
        current_epsilon *= 2.0;
        kept = count_kept_points(input->points, input->count, current_epsilon);
    }

    /* If still too many, use uniform sampling as last resort */
    if (kept > output_capacity) {
        /* Sample evenly, always including first and last */
        output->points[0] = input->points[0];
        output->points[output_capacity - 1] = input->points[input->count - 1];

        if (output_capacity > 2) {
            double step = (double)(input->count - 1) / (double)(output_capacity - 1);
            for (int i = 1; i < output_capacity - 1; i++) {
                int idx = (int)(i * step);
                if (idx >= input->count) idx = input->count - 1;
                output->points[i] = input->points[idx];
            }
        }
        output->count = output_capacity;
        return output_capacity;
    }

    /* Run final simplification with adaptive epsilon */
    return sh_polyline_simplify(input, current_epsilon, output, output_capacity);
}
