/*
 * lc_spatial.c - Spatial Index for Reverse Geocoding
 *
 * Grid-based spatial index for fast nearest-neighbor queries.
 */

#include "lc_spatial.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ============================================================================
 * Grid Management
 * ============================================================================ */

LCSpatialGrid *lc_grid_create(SHBBox bounds, double cell_size)
{
    if (cell_size <= 0) cell_size = LC_GRID_DEFAULT_CELL_SIZE;

    /* Validate bounds */
    if (bounds.max_lat <= bounds.min_lat || bounds.max_lon <= bounds.min_lon) {
        return NULL;
    }

    LCSpatialGrid *grid = calloc(1, sizeof(LCSpatialGrid));
    if (!grid) return NULL;

    grid->bounds = bounds;
    grid->cell_size_lat = cell_size;
    grid->cell_size_lon = cell_size;

    /* Calculate grid dimensions */
    double lat_range = bounds.max_lat - bounds.min_lat;
    double lon_range = bounds.max_lon - bounds.min_lon;

    grid->grid_height = (int)ceil(lat_range / cell_size);
    grid->grid_width = (int)ceil(lon_range / cell_size);

    if (grid->grid_height <= 0) grid->grid_height = 1;
    if (grid->grid_width <= 0) grid->grid_width = 1;

    int total_cells = grid->grid_width * grid->grid_height;

    /* Allocate cell arrays */
    grid->cells = calloc(total_cells, sizeof(uint32_t *));
    grid->cell_counts = calloc(total_cells, sizeof(uint32_t));
    grid->cell_capacities = calloc(total_cells, sizeof(uint32_t));

    if (!grid->cells || !grid->cell_counts || !grid->cell_capacities) {
        free(grid->cells);
        free(grid->cell_counts);
        free(grid->cell_capacities);
        free(grid);
        return NULL;
    }

    grid->memory_used = sizeof(LCSpatialGrid) +
                        total_cells * (sizeof(uint32_t *) + 2 * sizeof(uint32_t));

    return grid;
}

void lc_grid_free(LCSpatialGrid *grid)
{
    if (!grid) return;

    int total_cells = grid->grid_width * grid->grid_height;
    for (int i = 0; i < total_cells; i++) {
        free(grid->cells[i]);
    }
    free(grid->cells);
    free(grid->cell_counts);
    free(grid->cell_capacities);
    free(grid);
}

int lc_grid_cell_index(const LCSpatialGrid *grid, SHCoord coord)
{
    if (!grid) return -1;

    /* Check bounds */
    if (coord.lat < grid->bounds.min_lat || coord.lat > grid->bounds.max_lat ||
        coord.lon < grid->bounds.min_lon || coord.lon > grid->bounds.max_lon) {
        return -1;
    }

    int row = (int)((coord.lat - grid->bounds.min_lat) / grid->cell_size_lat);
    int col = (int)((coord.lon - grid->bounds.min_lon) / grid->cell_size_lon);

    /* Clamp to valid range */
    if (row >= grid->grid_height) row = grid->grid_height - 1;
    if (col >= grid->grid_width) col = grid->grid_width - 1;
    if (row < 0) row = 0;
    if (col < 0) col = 0;

    return row * grid->grid_width + col;
}

LCStatus lc_grid_insert(LCSpatialGrid *grid, SHCoord coord, uint32_t entity_id)
{
    if (!grid) return LC_ERROR_INVALID_PARAM;

    int cell_idx = lc_grid_cell_index(grid, coord);
    if (cell_idx < 0) {
        /* Out of bounds - expand bounds? For now, skip */
        return LC_OK;
    }

    /* Check if already present */
    for (uint32_t i = 0; i < grid->cell_counts[cell_idx]; i++) {
        if (grid->cells[cell_idx][i] == entity_id) {
            return LC_OK;
        }
    }

    /* Grow cell array if needed */
    if (grid->cell_counts[cell_idx] >= grid->cell_capacities[cell_idx]) {
        uint32_t new_cap = grid->cell_capacities[cell_idx] == 0 ? 4 :
                           grid->cell_capacities[cell_idx] * 2;
        uint32_t *new_ids = realloc(grid->cells[cell_idx], new_cap * sizeof(uint32_t));
        if (!new_ids) return LC_ERROR_OUT_OF_MEMORY;
        grid->cells[cell_idx] = new_ids;
        grid->memory_used += (new_cap - grid->cell_capacities[cell_idx]) * sizeof(uint32_t);
        grid->cell_capacities[cell_idx] = new_cap;
    }

    grid->cells[cell_idx][grid->cell_counts[cell_idx]++] = entity_id;
    grid->total_entries++;

    return LC_OK;
}

size_t lc_grid_query_point(const LCSpatialGrid *grid, SHCoord coord,
                           size_t max_results, uint32_t *results)
{
    if (!grid || !results || max_results == 0) return 0;

    int cell_idx = lc_grid_cell_index(grid, coord);
    if (cell_idx < 0) return 0;

    size_t count = 0;
    for (uint32_t i = 0; i < grid->cell_counts[cell_idx] && count < max_results; i++) {
        results[count++] = grid->cells[cell_idx][i];
    }

    return count;
}

/*
 * Smallest cosine of latitude the longitude span is computed with. At the
 * poles the true value is zero and the span is unbounded; 1e-4 corresponds
 * to about 0.006 degrees of latitude away from the pole, which is closer
 * than any grid cell, so it changes nothing for coordinates that are not
 * essentially polar.
 */
#define LC_MIN_COS_LAT 1e-4

/*
 * Narrow a span of fractional cell positions to the cells that exist.
 *
 * Returns 0 and leaves *lo and *hi untouched when the span misses the grid
 * entirely; otherwise clips it to [0, limit) and returns 1.
 *
 * The clipping is done in double and only then converted, because the
 * positions can legitimately be far outside int -- a query at a pole, or one
 * well outside the indexed area -- and converting an out-of-range double to
 * int is undefined rather than merely inaccurate. NaN fails every comparison
 * here and so reports no intersection, which is both a defined answer and the
 * right one.
 */
static int lc_clip_cell_span(double lo_pos, double hi_pos, int limit,
                             int *lo, int *hi)
{
    if (limit <= 0) return 0;
    if (!(lo_pos <= hi_pos)) return 0;                  /* empty, or NaN */
    if (!(hi_pos >= 0.0) || !(lo_pos < (double)limit)) return 0;

    *lo = (lo_pos > 0.0) ? (int)lo_pos : 0;
    *hi = (hi_pos < (double)(limit - 1)) ? (int)hi_pos : limit - 1;
    return 1;
}

size_t lc_grid_query_radius(const LCSpatialGrid *grid, SHCoord coord,
                            double radius_m, size_t max_results, uint32_t *results)
{
    if (!grid || !results || max_results == 0) return 0;

    /* Convert radius to degrees (rough approximation) */
    double radius_lat = radius_m / 111000.0;  /* ~111km per degree latitude */

    /*
     * cos() reaches zero at the poles, so a degree of longitude shrinks to
     * nothing and radius_lon explodes: at lat = -90 it came out as 1.5e14
     * degrees for a 100m radius, and the cell quotients below then fell
     * outside int, which is undefined. -90 is a latitude the API accepts, so
     * an ordinary /api/v1/reverse?lat=-90&lon=180 reached it.
     *
     * Clamping the cosine bounds the span. At the poles every longitude is
     * effectively the same place, so widening to the whole row is also the
     * honest answer rather than a fudge.
     */
    double cos_lat = cos(coord.lat * M_PI / 180.0);
    if (cos_lat < LC_MIN_COS_LAT) cos_lat = LC_MIN_COS_LAT;
    double radius_lon = radius_m / (111000.0 * cos_lat);

    /*
     * Clipped as doubles and then converted, not converted and then clipped.
     * The old order cast first, so the value had already left the range of
     * int before anything looked at it.
     *
     * A span that misses the grid returns nothing, which is what the old
     * clamping arrived at too -- there, by the accident of min_* only being
     * clamped upwards and max_* only downwards, so the two crossed and the
     * loop below ran zero times. Saying it directly keeps that outcome
     * without depending on the accident.
     */
    int min_row, max_row, min_col, max_col;

    if (!lc_clip_cell_span((coord.lat - radius_lat - grid->bounds.min_lat)
                               / grid->cell_size_lat,
                           (coord.lat + radius_lat - grid->bounds.min_lat)
                               / grid->cell_size_lat,
                           grid->grid_height, &min_row, &max_row))
        return 0;

    if (!lc_clip_cell_span((coord.lon - radius_lon - grid->bounds.min_lon)
                               / grid->cell_size_lon,
                           (coord.lon + radius_lon - grid->bounds.min_lon)
                               / grid->cell_size_lon,
                           grid->grid_width, &min_col, &max_col))
        return 0;

    size_t count = 0;
    for (int row = min_row; row <= max_row && count < max_results; row++) {
        for (int col = min_col; col <= max_col && count < max_results; col++) {
            int cell_idx = row * grid->grid_width + col;
            for (uint32_t i = 0; i < grid->cell_counts[cell_idx] && count < max_results; i++) {
                /* Check for duplicates */
                int found = 0;
                for (size_t j = 0; j < count; j++) {
                    if (results[j] == grid->cells[cell_idx][i]) {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    results[count++] = grid->cells[cell_idx][i];
                }
            }
        }
    }

    return count;
}

/* Comparator for sorting by distance */
static int nearest_compare(const void *a, const void *b)
{
    const LCNearestResult *ra = (const LCNearestResult *)a;
    const LCNearestResult *rb = (const LCNearestResult *)b;
    if (ra->distance_m < rb->distance_m) return -1;
    if (ra->distance_m > rb->distance_m) return 1;
    return 0;
}

size_t lc_grid_find_nearest(const LCSpatialGrid *grid, const LCEntityStore *store,
                            SHCoord coord, size_t max_results, LCNearestResult *results)
{
    if (!grid || !store || !results || max_results == 0) return 0;

    /* Start with a small radius and expand if needed.
     * We use a larger buffer (1024) to ensure we get enough candidates
     * from nearby cells before they get displaced by entities from farther cells. */
    double search_radius = 200.0;  /* Start with 200m */
    uint32_t candidates[1024];
    size_t num_candidates = 0;

    /* Expand radius until we have at least max_results candidates */
    while (num_candidates < max_results && search_radius < 100000.0) {
        num_candidates = lc_grid_query_radius(grid, coord, search_radius, 1024, candidates);
        search_radius *= 2;
    }

    if (num_candidates == 0) return 0;

    /* Calculate distances and build result array */
    LCNearestResult *temp = malloc(num_candidates * sizeof(LCNearestResult));
    if (!temp) return 0;

    size_t valid_count = 0;
    for (size_t i = 0; i < num_candidates; i++) {
        if (candidates[i] >= store->count) continue;

        const LCEntity *entity = &store->entities[candidates[i]];

        /* Use geometry-aware distance calculation */
        double dist = lc_point_to_entity_distance(coord, entity);

        temp[valid_count].entity_id = candidates[i];
        temp[valid_count].distance_m = dist;
        valid_count++;
    }

    /* Sort by distance */
    if (valid_count > 1) {
        qsort(temp, valid_count, sizeof(LCNearestResult), nearest_compare);
    }

    /* Copy top results */
    size_t result_count = valid_count < max_results ? valid_count : max_results;
    memcpy(results, temp, result_count * sizeof(LCNearestResult));

    free(temp);
    return result_count;
}

uint32_t lc_grid_entry_count(const LCSpatialGrid *grid)
{
    return grid ? grid->total_entries : 0;
}

size_t lc_grid_memory_usage(const LCSpatialGrid *grid)
{
    return grid ? grid->memory_used : 0;
}

int lc_grid_cell_count(const LCSpatialGrid *grid)
{
    return grid ? grid->grid_width * grid->grid_height : 0;
}

/* ============================================================================
 * Point-to-Line Distance Functions
 * ============================================================================ */

/*
 * Project a point onto a line segment and compute distance.
 *
 * The algorithm:
 * 1. Compute the projection parameter t = dot(P-A, B-A) / |B-A|^2
 * 2. Clamp t to [0, 1] to stay on the segment
 * 3. The closest point on segment is A + t*(B-A)
 * 4. Return haversine distance from P to that closest point
 *
 * We work in degrees since the segments are typically short (< 1km),
 * and use haversine for the final distance calculation.
 */
double lc_point_to_segment_distance(SHCoord point, SHCoord seg_a, SHCoord seg_b)
{
    /* Handle degenerate segment (points are the same) */
    double dx = seg_b.lon - seg_a.lon;
    double dy = seg_b.lat - seg_a.lat;
    double seg_len_sq = dx * dx + dy * dy;

    if (seg_len_sq < 1e-14) {
        /* Segment is a point, just return distance to that point */
        return sh_haversine(point, seg_a);
    }

    /* Compute projection parameter t */
    double px = point.lon - seg_a.lon;
    double py = point.lat - seg_a.lat;
    double t = (px * dx + py * dy) / seg_len_sq;

    /* Clamp t to [0, 1] */
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    /* Find closest point on segment */
    SHCoord closest;
    closest.lon = seg_a.lon + t * dx;
    closest.lat = seg_a.lat + t * dy;

    /* Return haversine distance to closest point */
    return sh_haversine(point, closest);
}

double lc_point_to_linestring_distance(SHCoord point, const LCLineString *line)
{
    if (!line || !line->points || line->count < 1) {
        return -1.0;
    }

    if (line->count == 1) {
        /* Single point, just return distance to it */
        return sh_haversine(point, line->points[0]);
    }

    /* Find minimum distance to any segment */
    double min_dist = lc_point_to_segment_distance(point,
                                                    line->points[0],
                                                    line->points[1]);

    for (uint32_t i = 1; i < line->count - 1; i++) {
        double dist = lc_point_to_segment_distance(point,
                                                    line->points[i],
                                                    line->points[i + 1]);
        if (dist < min_dist) {
            min_dist = dist;
        }
    }

    return min_dist;
}

double lc_point_to_entity_distance(SHCoord point, const LCEntity *entity)
{
    if (!entity) return -1.0;

    /* For streets with geometry, use line distance */
    if (entity->fclass == LC_CLASS_STREET && entity->geometry) {
        double line_dist = lc_point_to_linestring_distance(point, entity->geometry);
        if (line_dist >= 0) {
            return line_dist;
        }
    }

    /* Fall back to centroid distance */
    return sh_haversine(point, entity->centroid);
}
