/*
 * lc_spatial.h - Spatial Index for Reverse Geocoding
 *
 * Implements a grid-based spatial index for fast nearest-neighbor queries.
 * Used for reverse geocoding (coordinates → entity).
 */

#ifndef LC_SPATIAL_H
#define LC_SPATIAL_H

#include <stdint.h>
#include <stddef.h>
#include "sh_geo.h"
#include "lc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Grid Configuration
 * ============================================================================ */

/* Default cell size in degrees (roughly 1km at equator) */
#define LC_GRID_DEFAULT_CELL_SIZE 0.01

/* ============================================================================
 * Grid Structure
 * ============================================================================ */

typedef struct {
    double cell_size_lat;       /* Cell size in latitude degrees */
    double cell_size_lon;       /* Cell size in longitude degrees */
    int grid_width;             /* Number of cells in longitude direction */
    int grid_height;            /* Number of cells in latitude direction */
    SHBBox bounds;              /* Geographic bounds of the grid */

    uint32_t **cells;           /* 2D array of entity ID lists */
    uint32_t *cell_counts;      /* Number of entities per cell */
    uint32_t *cell_capacities;  /* Capacity of each cell */

    uint32_t total_entries;     /* Total entity insertions */
    size_t memory_used;
} LCSpatialGrid;

/* ============================================================================
 * Nearest Result
 * ============================================================================ */

typedef struct {
    uint32_t entity_id;
    double distance_m;          /* Distance in meters */
} LCNearestResult;

/* ============================================================================
 * Grid API
 * ============================================================================ */

/*
 * Create a spatial grid covering the given bounding box.
 * cell_size: cell dimension in degrees (use LC_GRID_DEFAULT_CELL_SIZE or 0 for default)
 */
LCSpatialGrid *lc_grid_create(SHBBox bounds, double cell_size);

/*
 * Free spatial grid.
 */
void lc_grid_free(LCSpatialGrid *grid);

/*
 * Insert an entity at a coordinate.
 */
LCStatus lc_grid_insert(LCSpatialGrid *grid, SHCoord coord, uint32_t entity_id);

/*
 * Query for entities in the same cell as a point.
 * Returns number of results.
 */
size_t lc_grid_query_point(const LCSpatialGrid *grid, SHCoord coord,
                           size_t max_results, uint32_t *results);

/*
 * Query for entities within a radius (meters) of a point.
 * Returns number of results.
 */
size_t lc_grid_query_radius(const LCSpatialGrid *grid, SHCoord coord,
                            double radius_m, size_t max_results, uint32_t *results);

/*
 * Find nearest entities to a point (requires entity store for coordinates).
 * Returns number of results, sorted by distance ascending.
 */
size_t lc_grid_find_nearest(const LCSpatialGrid *grid, const LCEntityStore *store,
                            SHCoord coord, size_t max_results, LCNearestResult *results);

/*
 * Get cell index for a coordinate.
 * Returns -1 if out of bounds.
 */
int lc_grid_cell_index(const LCSpatialGrid *grid, SHCoord coord);

/*
 * Get grid statistics.
 */
uint32_t lc_grid_entry_count(const LCSpatialGrid *grid);
size_t lc_grid_memory_usage(const LCSpatialGrid *grid);
int lc_grid_cell_count(const LCSpatialGrid *grid);

/* ============================================================================
 * Point-to-Line Distance Functions
 * ============================================================================ */

/*
 * Calculate minimum distance (in meters) from a point to a line segment.
 * Uses haversine-based calculation for geographic accuracy.
 *
 * @param point   The query point
 * @param seg_a   First endpoint of the segment
 * @param seg_b   Second endpoint of the segment
 * @return        Distance in meters
 */
double lc_point_to_segment_distance(SHCoord point, SHCoord seg_a, SHCoord seg_b);

/*
 * Calculate minimum distance (in meters) from a point to a linestring.
 * Returns the minimum distance to any segment in the linestring.
 *
 * @param point   The query point
 * @param line    The linestring (array of points)
 * @return        Distance in meters, or -1 if line is NULL or empty
 */
double lc_point_to_linestring_distance(SHCoord point, const LCLineString *line);

/*
 * Calculate distance from point to an entity, using geometry if available.
 * For streets with geometry, uses point-to-line distance.
 * For other entities, uses haversine to centroid.
 *
 * @param point   The query point
 * @param entity  The entity
 * @return        Distance in meters
 */
double lc_point_to_entity_distance(SHCoord point, const LCEntity *entity);

#ifdef __cplusplus
}
#endif

#endif /* LC_SPATIAL_H */
