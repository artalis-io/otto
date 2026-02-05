/*
 * sh_spatial_grid.c - Uniform spatial grid for geographic points
 *
 * Implements both dynamic and CSR (compressed sparse row) spatial grids.
 */

#include "sh_spatial_grid.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

/* Default cell size: ~1km at equator */
#define SH_GRID_DEFAULT_CELL_SIZE 0.01

/* Minimum grid dimensions */
#define SH_GRID_MIN_SIZE 1

/* Maximum grid dimensions (prevent memory explosion) */
#define SH_GRID_MAX_SIZE 10000

/* Initial capacity for dynamic cell arrays */
#define SH_GRID_INITIAL_CELL_CAPACITY 4

/* Meters per degree latitude (approximate) */
#define SH_METERS_PER_DEG_LAT 111000.0

/* Very large distance value */
#define SH_GRID_INF 1e308

/* ============================================================================
 * Grid Configuration
 * ============================================================================ */

SHGridConfig sh_grid_config_create(SHBBox bounds, double cell_size)
{
    SHGridConfig config;

    if (cell_size <= 0) cell_size = SH_GRID_DEFAULT_CELL_SIZE;

    config.bounds = bounds;
    config.cell_size_lat = cell_size;
    config.cell_size_lon = cell_size;  /* Could adjust for latitude */

    double lat_span = bounds.max_lat - bounds.min_lat;
    double lon_span = bounds.max_lon - bounds.min_lon;

    config.grid_height = (int)ceil(lat_span / cell_size);
    config.grid_width = (int)ceil(lon_span / cell_size);

    /* Clamp to valid range */
    if (config.grid_height < SH_GRID_MIN_SIZE) config.grid_height = SH_GRID_MIN_SIZE;
    if (config.grid_width < SH_GRID_MIN_SIZE) config.grid_width = SH_GRID_MIN_SIZE;
    if (config.grid_height > SH_GRID_MAX_SIZE) config.grid_height = SH_GRID_MAX_SIZE;
    if (config.grid_width > SH_GRID_MAX_SIZE) config.grid_width = SH_GRID_MAX_SIZE;

    return config;
}

int sh_grid_cell_index(const SHGridConfig *config, SHCoord coord)
{
    if (!config) return -1;

    int row = (int)((coord.lat - config->bounds.min_lat) / config->cell_size_lat);
    int col = (int)((coord.lon - config->bounds.min_lon) / config->cell_size_lon);

    if (row < 0 || row >= config->grid_height) return -1;
    if (col < 0 || col >= config->grid_width) return -1;

    return row * config->grid_width + col;
}

int sh_grid_cell_rowcol(const SHGridConfig *config, SHCoord coord, int *row, int *col)
{
    if (!config || !row || !col) return -1;

    *row = (int)((coord.lat - config->bounds.min_lat) / config->cell_size_lat);
    *col = (int)((coord.lon - config->bounds.min_lon) / config->cell_size_lon);

    if (*row < 0 || *row >= config->grid_height) return -1;
    if (*col < 0 || *col >= config->grid_width) return -1;

    return 0;
}

/* ============================================================================
 * Dynamic Grid Implementation
 * ============================================================================ */

SHDynamicGrid *sh_dynamic_grid_create(SHBBox bounds, double cell_size)
{
    SHDynamicGrid *grid = calloc(1, sizeof(SHDynamicGrid));
    if (!grid) return NULL;

    grid->config = sh_grid_config_create(bounds, cell_size);

    size_t num_cells = (size_t)grid->config.grid_height * (size_t)grid->config.grid_width;

    grid->cells = calloc(num_cells, sizeof(uint32_t *));
    grid->cell_counts = calloc(num_cells, sizeof(uint32_t));
    grid->cell_capacities = calloc(num_cells, sizeof(uint32_t));

    if (!grid->cells || !grid->cell_counts || !grid->cell_capacities) {
        free(grid->cells);
        free(grid->cell_counts);
        free(grid->cell_capacities);
        free(grid);
        return NULL;
    }

    grid->memory_used = sizeof(SHDynamicGrid) +
                        num_cells * sizeof(uint32_t *) +
                        num_cells * sizeof(uint32_t) * 2;

    return grid;
}

void sh_dynamic_grid_free(SHDynamicGrid *grid)
{
    if (!grid) return;

    size_t num_cells = (size_t)grid->config.grid_height * (size_t)grid->config.grid_width;

    for (size_t i = 0; i < num_cells; i++) {
        free(grid->cells[i]);
    }

    free(grid->cells);
    free(grid->cell_counts);
    free(grid->cell_capacities);
    free(grid);
}

SHGridStatus sh_dynamic_grid_insert(SHDynamicGrid *grid, SHCoord coord, uint32_t entity_id)
{
    if (!grid) return SH_GRID_ERROR_NULL_PARAM;

    int cell_idx = sh_grid_cell_index(&grid->config, coord);
    if (cell_idx < 0) {
        /* Out of bounds - silently ignore */
        return SH_GRID_OK;
    }

    /* Check for duplicate */
    for (uint32_t i = 0; i < grid->cell_counts[cell_idx]; i++) {
        if (grid->cells[cell_idx][i] == entity_id) {
            return SH_GRID_OK;
        }
    }

    /* Grow cell array if needed */
    if (grid->cell_counts[cell_idx] >= grid->cell_capacities[cell_idx]) {
        uint32_t new_cap = grid->cell_capacities[cell_idx] == 0
                          ? SH_GRID_INITIAL_CELL_CAPACITY
                          : grid->cell_capacities[cell_idx] * 2;

        uint32_t *new_ids = realloc(grid->cells[cell_idx], new_cap * sizeof(uint32_t));
        if (!new_ids) return SH_GRID_ERROR_OUT_OF_MEMORY;

        grid->cells[cell_idx] = new_ids;
        grid->memory_used += (new_cap - grid->cell_capacities[cell_idx]) * sizeof(uint32_t);
        grid->cell_capacities[cell_idx] = new_cap;
    }

    grid->cells[cell_idx][grid->cell_counts[cell_idx]++] = entity_id;
    grid->total_entries++;

    return SH_GRID_OK;
}

size_t sh_dynamic_grid_query_point(const SHDynamicGrid *grid, SHCoord coord,
                                   size_t max_results, uint32_t *results)
{
    if (!grid || !results || max_results == 0) return 0;

    int cell_idx = sh_grid_cell_index(&grid->config, coord);
    if (cell_idx < 0) return 0;

    size_t count = 0;
    for (uint32_t i = 0; i < grid->cell_counts[cell_idx] && count < max_results; i++) {
        results[count++] = grid->cells[cell_idx][i];
    }

    return count;
}

size_t sh_dynamic_grid_query_radius(const SHDynamicGrid *grid, SHCoord coord,
                                    double radius_m, size_t max_results, uint32_t *results)
{
    if (!grid || !results || max_results == 0) return 0;

    /* Convert radius to degrees */
    double radius_lat = radius_m / SH_METERS_PER_DEG_LAT;
    double cos_lat = cos(coord.lat * SH_DEG_TO_RAD);
    double radius_lon = radius_lat / (cos_lat > 0.01 ? cos_lat : 0.01);

    /* Determine search rectangle */
    int min_row = (int)((coord.lat - radius_lat - grid->config.bounds.min_lat)
                        / grid->config.cell_size_lat);
    int max_row = (int)((coord.lat + radius_lat - grid->config.bounds.min_lat)
                        / grid->config.cell_size_lat);
    int min_col = (int)((coord.lon - radius_lon - grid->config.bounds.min_lon)
                        / grid->config.cell_size_lon);
    int max_col = (int)((coord.lon + radius_lon - grid->config.bounds.min_lon)
                        / grid->config.cell_size_lon);

    /* Clamp to grid bounds */
    if (min_row < 0) min_row = 0;
    if (max_row >= grid->config.grid_height) max_row = grid->config.grid_height - 1;
    if (min_col < 0) min_col = 0;
    if (max_col >= grid->config.grid_width) max_col = grid->config.grid_width - 1;

    size_t count = 0;
    for (int row = min_row; row <= max_row && count < max_results; row++) {
        for (int col = min_col; col <= max_col && count < max_results; col++) {
            int cell_idx = row * grid->config.grid_width + col;
            for (uint32_t i = 0; i < grid->cell_counts[cell_idx] && count < max_results; i++) {
                uint32_t eid = grid->cells[cell_idx][i];

                /* Check for duplicates in results */
                int found = 0;
                for (size_t j = 0; j < count; j++) {
                    if (results[j] == eid) {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    results[count++] = eid;
                }
            }
        }
    }

    return count;
}

size_t sh_dynamic_grid_num_cells(const SHDynamicGrid *grid)
{
    if (!grid) return 0;
    return (size_t)grid->config.grid_height * (size_t)grid->config.grid_width;
}

size_t sh_dynamic_grid_num_entries(const SHDynamicGrid *grid)
{
    return grid ? grid->total_entries : 0;
}

/* ============================================================================
 * CSR Grid Implementation
 * ============================================================================ */

SHCSRGrid *sh_csr_grid_create(SHBBox bounds, double cell_size,
                               const SHCoordFixed *coords, size_t num_nodes)
{
    if (!coords && num_nodes > 0) return NULL;

    SHCSRGrid *grid = calloc(1, sizeof(SHCSRGrid));
    if (!grid) return NULL;

    grid->config = sh_grid_config_create(bounds, cell_size);
    grid->num_nodes = num_nodes;

    size_t num_cells = (size_t)grid->config.grid_height * (size_t)grid->config.grid_width;

    /* Phase 1: Count nodes per cell */
    uint32_t *cell_counts = calloc(num_cells, sizeof(uint32_t));
    if (!cell_counts) {
        free(grid);
        return NULL;
    }

    for (size_t i = 0; i < num_nodes; i++) {
        SHCoord c = SH_FIXED_TO_COORD(coords[i]);
        int cell_idx = sh_grid_cell_index(&grid->config, c);
        if (cell_idx >= 0) {
            cell_counts[cell_idx]++;
        }
    }

    /* Phase 2: Compute prefix sums for offsets */
    grid->cell_offsets = malloc((num_cells + 1) * sizeof(uint32_t));
    if (!grid->cell_offsets) {
        free(cell_counts);
        free(grid);
        return NULL;
    }

    grid->cell_offsets[0] = 0;
    for (size_t i = 0; i < num_cells; i++) {
        grid->cell_offsets[i + 1] = grid->cell_offsets[i] + cell_counts[i];
    }

    /* Phase 3: Fill cell_nodes array */
    grid->cell_nodes = malloc(num_nodes * sizeof(uint32_t));
    if (!grid->cell_nodes) {
        free(grid->cell_offsets);
        free(cell_counts);
        free(grid);
        return NULL;
    }

    /* Reset counts to use as fill indices */
    memset(cell_counts, 0, num_cells * sizeof(uint32_t));

    for (size_t i = 0; i < num_nodes; i++) {
        SHCoord c = SH_FIXED_TO_COORD(coords[i]);
        int cell_idx = sh_grid_cell_index(&grid->config, c);
        if (cell_idx >= 0) {
            size_t pos = grid->cell_offsets[cell_idx] + cell_counts[cell_idx];
            grid->cell_nodes[pos] = (uint32_t)i;
            cell_counts[cell_idx]++;
        }
    }

    free(cell_counts);
    return grid;
}

SHCSRGrid *sh_csr_grid_create_f(SHBBox bounds, double cell_size,
                                 const SHCoord *coords, size_t num_nodes)
{
    if (!coords && num_nodes > 0) return NULL;

    SHCSRGrid *grid = calloc(1, sizeof(SHCSRGrid));
    if (!grid) return NULL;

    grid->config = sh_grid_config_create(bounds, cell_size);
    grid->num_nodes = num_nodes;

    size_t num_cells = (size_t)grid->config.grid_height * (size_t)grid->config.grid_width;

    /* Phase 1: Count nodes per cell */
    uint32_t *cell_counts = calloc(num_cells, sizeof(uint32_t));
    if (!cell_counts) {
        free(grid);
        return NULL;
    }

    for (size_t i = 0; i < num_nodes; i++) {
        int cell_idx = sh_grid_cell_index(&grid->config, coords[i]);
        if (cell_idx >= 0) {
            cell_counts[cell_idx]++;
        }
    }

    /* Phase 2: Compute prefix sums for offsets */
    grid->cell_offsets = malloc((num_cells + 1) * sizeof(uint32_t));
    if (!grid->cell_offsets) {
        free(cell_counts);
        free(grid);
        return NULL;
    }

    grid->cell_offsets[0] = 0;
    for (size_t i = 0; i < num_cells; i++) {
        grid->cell_offsets[i + 1] = grid->cell_offsets[i] + cell_counts[i];
    }

    /* Phase 3: Fill cell_nodes array */
    grid->cell_nodes = malloc(num_nodes * sizeof(uint32_t));
    if (!grid->cell_nodes) {
        free(grid->cell_offsets);
        free(cell_counts);
        free(grid);
        return NULL;
    }

    memset(cell_counts, 0, num_cells * sizeof(uint32_t));

    for (size_t i = 0; i < num_nodes; i++) {
        int cell_idx = sh_grid_cell_index(&grid->config, coords[i]);
        if (cell_idx >= 0) {
            size_t pos = grid->cell_offsets[cell_idx] + cell_counts[cell_idx];
            grid->cell_nodes[pos] = (uint32_t)i;
            cell_counts[cell_idx]++;
        }
    }

    free(cell_counts);
    return grid;
}

void sh_csr_grid_free(SHCSRGrid *grid)
{
    if (!grid) return;
    free(grid->cell_offsets);
    free(grid->cell_nodes);
    free(grid);
}

int sh_csr_grid_query_cell(const SHCSRGrid *grid, SHCoord coord,
                           const uint32_t **start, const uint32_t **end)
{
    if (!grid || !start || !end) return -1;

    int cell_idx = sh_grid_cell_index(&grid->config, coord);
    if (cell_idx < 0) {
        *start = NULL;
        *end = NULL;
        return -1;
    }

    *start = &grid->cell_nodes[grid->cell_offsets[cell_idx]];
    *end = &grid->cell_nodes[grid->cell_offsets[cell_idx + 1]];
    return 0;
}

int sh_csr_grid_cell_index(const SHCSRGrid *grid, SHCoord coord)
{
    if (!grid) return -1;
    return sh_grid_cell_index(&grid->config, coord);
}

void sh_csr_grid_cell_range(const SHCSRGrid *grid, int cell_idx,
                            const uint32_t **start, const uint32_t **end)
{
    if (!grid || !start || !end) return;

    size_t num_cells = (size_t)grid->config.grid_height * (size_t)grid->config.grid_width;
    if (cell_idx < 0 || (size_t)cell_idx >= num_cells) {
        *start = NULL;
        *end = NULL;
        return;
    }

    *start = &grid->cell_nodes[grid->cell_offsets[cell_idx]];
    *end = &grid->cell_nodes[grid->cell_offsets[cell_idx + 1]];
}

uint32_t sh_csr_grid_find_nearest(const SHCSRGrid *grid, SHCoord coord,
                                   SHDistanceCallback dist_fn, void *user_data,
                                   double *out_distance)
{
    if (!grid || !dist_fn) return UINT32_MAX;

    int center_row, center_col;
    if (sh_grid_cell_rowcol(&grid->config, coord, &center_row, &center_col) != 0) {
        /* Coord outside grid - clamp to nearest edge */
        center_row = (int)((coord.lat - grid->config.bounds.min_lat)
                          / grid->config.cell_size_lat);
        center_col = (int)((coord.lon - grid->config.bounds.min_lon)
                          / grid->config.cell_size_lon);
        if (center_row < 0) center_row = 0;
        if (center_row >= grid->config.grid_height) center_row = grid->config.grid_height - 1;
        if (center_col < 0) center_col = 0;
        if (center_col >= grid->config.grid_width) center_col = grid->config.grid_width - 1;
    }

    uint32_t best_idx = UINT32_MAX;
    double best_dist = SH_GRID_INF;

    int max_radius = grid->config.grid_height > grid->config.grid_width
                    ? grid->config.grid_height : grid->config.grid_width;

    /* Expanding ring search */
    for (int radius = 0; radius <= max_radius; radius++) {
        int found_in_ring = 0;

        for (int dr = -radius; dr <= radius; dr++) {
            for (int dc = -radius; dc <= radius; dc++) {
                /* Only check ring perimeter (not interior) */
                if (radius > 0 && abs(dr) != radius && abs(dc) != radius) continue;

                int row = center_row + dr;
                int col = center_col + dc;
                if (row < 0 || row >= grid->config.grid_height) continue;
                if (col < 0 || col >= grid->config.grid_width) continue;

                int cell_idx = row * grid->config.grid_width + col;
                uint32_t start = grid->cell_offsets[cell_idx];
                uint32_t end = grid->cell_offsets[cell_idx + 1];

                for (uint32_t i = start; i < end; i++) {
                    uint32_t node_id = grid->cell_nodes[i];
                    double dist = dist_fn(coord, node_id, user_data);
                    if (dist < best_dist) {
                        best_dist = dist;
                        best_idx = node_id;
                        found_in_ring = 1;
                    }
                }
            }
        }

        /* If we found something, verify with one more ring to ensure we didn't miss closer */
        if (found_in_ring && radius > 0) {
            /* Cell size in meters (approximate) */
            double cell_size_m = grid->config.cell_size_lat * SH_METERS_PER_DEG_LAT;

            /* If best distance is less than one cell diagonal, we're done */
            if (best_dist < cell_size_m * 1.42) {
                break;
            }

            /* Otherwise, search one more ring to be sure */
            int extra_rings = (int)(best_dist / cell_size_m) + 1;
            if (radius + extra_rings > max_radius) extra_rings = max_radius - radius;

            for (int r2 = radius + 1; r2 <= radius + extra_rings; r2++) {
                for (int dr = -r2; dr <= r2; dr++) {
                    for (int dc = -r2; dc <= r2; dc++) {
                        if (abs(dr) != r2 && abs(dc) != r2) continue;

                        int row = center_row + dr;
                        int col = center_col + dc;
                        if (row < 0 || row >= grid->config.grid_height) continue;
                        if (col < 0 || col >= grid->config.grid_width) continue;

                        int cell_idx = row * grid->config.grid_width + col;
                        uint32_t start_idx = grid->cell_offsets[cell_idx];
                        uint32_t end_idx = grid->cell_offsets[cell_idx + 1];

                        for (uint32_t i = start_idx; i < end_idx; i++) {
                            uint32_t node_id = grid->cell_nodes[i];
                            double dist = dist_fn(coord, node_id, user_data);
                            if (dist < best_dist) {
                                best_dist = dist;
                                best_idx = node_id;
                            }
                        }
                    }
                }
            }
            break;
        }
    }

    if (out_distance) *out_distance = best_dist;
    return best_idx;
}
