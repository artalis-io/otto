/*
 * sh_spatial_grid.h - Uniform spatial grid for geographic points
 *
 * Provides efficient spatial indexing for:
 * - Point-in-cell queries
 * - Radius queries
 * - Nearest neighbor search
 *
 * Supports two modes:
 * - Dynamic: Points can be added incrementally (locus-style)
 * - CSR: Compressed sparse row format for read-only grids (velo-style)
 */

#ifndef SH_SPATIAL_GRID_H
#define SH_SPATIAL_GRID_H

#include "sh_geo.h"
#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * Status Codes
 * ============================================================================ */

typedef enum {
    SH_GRID_OK = 0,
    SH_GRID_ERROR_NULL_PARAM,
    SH_GRID_ERROR_OUT_OF_MEMORY,
    SH_GRID_ERROR_OUT_OF_BOUNDS,
    SH_GRID_ERROR_INVALID_CONFIG
} SHGridStatus;

/* ============================================================================
 * Grid Configuration
 * ============================================================================ */

typedef struct {
    SHBBox bounds;           /* Geographic bounds */
    double cell_size_lat;    /* Cell height in degrees */
    double cell_size_lon;    /* Cell width in degrees (at equator) */
    int grid_width;          /* Number of columns */
    int grid_height;         /* Number of rows */
} SHGridConfig;

/* ============================================================================
 * Dynamic Spatial Grid
 *
 * Points can be added incrementally. Each cell has a dynamic array.
 * Best for: Building indices, geocoding (sparse data).
 * ============================================================================ */

typedef struct {
    SHGridConfig config;
    uint32_t **cells;         /* 2D array of entity ID lists */
    uint32_t *cell_counts;    /* Number of entities per cell */
    uint32_t *cell_capacities;/* Capacity of each cell */
    uint32_t total_entries;   /* Total entities inserted */
    size_t memory_used;       /* Approximate memory usage */
} SHDynamicGrid;

/*
 * Create a dynamic grid with given bounds and cell size.
 * cell_size is in degrees (0.01 = ~1km at equator).
 */
SHDynamicGrid *sh_dynamic_grid_create(SHBBox bounds, double cell_size);

/*
 * Free a dynamic grid and all its memory.
 */
void sh_dynamic_grid_free(SHDynamicGrid *grid);

/*
 * Insert a point with an entity ID.
 * Coordinates outside bounds are silently ignored.
 */
SHGridStatus sh_dynamic_grid_insert(SHDynamicGrid *grid, SHCoord coord, uint32_t entity_id);

/*
 * Query all entities in the cell containing the given point.
 * Returns number of results written to results array.
 */
size_t sh_dynamic_grid_query_point(const SHDynamicGrid *grid, SHCoord coord,
                                   size_t max_results, uint32_t *results);

/*
 * Query all entities within radius_m meters of the given point.
 * Returns number of results written to results array.
 * Note: Results may have duplicates if entity spans multiple cells.
 */
size_t sh_dynamic_grid_query_radius(const SHDynamicGrid *grid, SHCoord coord,
                                    double radius_m, size_t max_results, uint32_t *results);

/*
 * Get the number of cells in the grid.
 */
size_t sh_dynamic_grid_num_cells(const SHDynamicGrid *grid);

/*
 * Get the total number of entries.
 */
size_t sh_dynamic_grid_num_entries(const SHDynamicGrid *grid);

/* ============================================================================
 * CSR Spatial Grid (Compressed Sparse Row)
 *
 * Read-only grid with all data in two contiguous arrays.
 * Best for: Routing graphs (dense data, parallel construction).
 * ============================================================================ */

typedef struct {
    SHGridConfig config;
    uint32_t *cell_offsets;   /* Start offset for each cell (size: num_cells + 1) */
    uint32_t *cell_nodes;     /* All node indices, grouped by cell */
    size_t num_nodes;         /* Total nodes in the grid */
} SHCSRGrid;

/*
 * Create a CSR grid from coordinate and node index arrays.
 * coords and node_indices must have num_nodes elements.
 * Coordinates outside bounds are silently ignored.
 */
SHCSRGrid *sh_csr_grid_create(SHBBox bounds, double cell_size,
                               const SHCoordFixed *coords, size_t num_nodes);

/*
 * Alternative: Create from SHCoord array (floating point).
 */
SHCSRGrid *sh_csr_grid_create_f(SHBBox bounds, double cell_size,
                                 const SHCoord *coords, size_t num_nodes);

/*
 * Free a CSR grid.
 */
void sh_csr_grid_free(SHCSRGrid *grid);

/*
 * Query all nodes in the cell containing the given point.
 * Returns pointers to start and end of the node range.
 * Returns 0 on success, non-zero if out of bounds.
 */
int sh_csr_grid_query_cell(const SHCSRGrid *grid, SHCoord coord,
                           const uint32_t **start, const uint32_t **end);

/*
 * Get the cell index for a coordinate.
 * Returns -1 if coordinate is outside bounds.
 */
int sh_csr_grid_cell_index(const SHCSRGrid *grid, SHCoord coord);

/*
 * Get the node range for a specific cell.
 */
void sh_csr_grid_cell_range(const SHCSRGrid *grid, int cell_idx,
                            const uint32_t **start, const uint32_t **end);

/* ============================================================================
 * Nearest Neighbor Result
 * ============================================================================ */

typedef struct {
    uint32_t node_id;
    double distance_m;
} SHNearestResult;

/*
 * Callback for computing distance from a query point to a node.
 * Used by nearest neighbor search to support custom distance metrics.
 */
typedef double (*SHDistanceCallback)(SHCoord query, uint32_t node_id, void *user_data);

/*
 * Find nearest node to a query point using expanding ring search.
 * Uses the provided distance callback to compute actual distances.
 * Returns the nearest node index, or UINT32_MAX if grid is empty.
 */
uint32_t sh_csr_grid_find_nearest(const SHCSRGrid *grid, SHCoord coord,
                                   SHDistanceCallback dist_fn, void *user_data,
                                   double *out_distance);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/*
 * Create grid configuration from bounds and cell size.
 */
SHGridConfig sh_grid_config_create(SHBBox bounds, double cell_size);

/*
 * Get cell index for a coordinate within a grid config.
 * Returns -1 if out of bounds.
 */
int sh_grid_cell_index(const SHGridConfig *config, SHCoord coord);

/*
 * Get cell row and column for a coordinate.
 * Returns 0 on success, -1 if out of bounds.
 */
int sh_grid_cell_rowcol(const SHGridConfig *config, SHCoord coord, int *row, int *col);

#endif /* SH_SPATIAL_GRID_H */
