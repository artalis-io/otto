/*
 * ct_collision.c - Label Collision Detection
 *
 * Grid-based collision detection using a bitmap. Each cell in the grid
 * represents a small area of the tile. When a label is placed, all cells
 * it covers are marked as occupied.
 *
 * The bitmap uses 1 bit per cell for memory efficiency:
 * - 256x256 tile with 8px cells = 32x32 grid = 128 bytes
 * - 512x512 tile with 8px cells = 64x64 grid = 512 bytes
 */

#include "ct_collision.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

/*
 * Get bit index in the cells array for a grid cell.
 */
static inline int cell_index(const CTCollisionGrid *grid, int cx, int cy)
{
    return cy * grid->grid_width + cx;
}

/*
 * Check if a single cell is occupied.
 */
static inline int cell_is_occupied(const CTCollisionGrid *grid, int cx, int cy)
{
    if (cx < 0 || cx >= grid->grid_width || cy < 0 || cy >= grid->grid_height) {
        return 0;  /* Out of bounds = not occupied */
    }
    int idx = cell_index(grid, cx, cy);
    return (grid->cells[idx / 8] >> (idx % 8)) & 1;
}

/*
 * Mark a single cell as occupied.
 */
static inline void cell_mark(CTCollisionGrid *grid, int cx, int cy)
{
    if (cx < 0 || cx >= grid->grid_width || cy < 0 || cy >= grid->grid_height) {
        return;  /* Out of bounds = ignore */
    }
    int idx = cell_index(grid, cx, cy);
    grid->cells[idx / 8] |= (1 << (idx % 8));
}

/*
 * Convert pixel coordinates to cell coordinates.
 * Clamps to valid grid range.
 */
static void pixel_to_cell_range(const CTCollisionGrid *grid,
                                int x, int y, int width, int height,
                                int *cx_min, int *cy_min,
                                int *cx_max, int *cy_max)
{
    /* Convert to cell coordinates */
    *cx_min = x / grid->cell_size;
    *cy_min = y / grid->cell_size;
    *cx_max = (x + width - 1) / grid->cell_size;
    *cy_max = (y + height - 1) / grid->cell_size;

    /* Clamp to grid bounds */
    if (*cx_min < 0) *cx_min = 0;
    if (*cy_min < 0) *cy_min = 0;
    if (*cx_max >= grid->grid_width) *cx_max = grid->grid_width - 1;
    if (*cy_max >= grid->grid_height) *cy_max = grid->grid_height - 1;
}

/* ============================================================================
 * Creation / Destruction
 * ============================================================================ */

CTCollisionGrid *ct_collision_create(int tile_width, int tile_height, int cell_size)
{
    if (tile_width <= 0 || tile_height <= 0 || cell_size <= 0) {
        return NULL;
    }

    CTCollisionGrid *grid = calloc(1, sizeof(CTCollisionGrid));
    if (!grid) {
        return NULL;
    }

    grid->tile_width = tile_width;
    grid->tile_height = tile_height;
    grid->cell_size = cell_size;
    grid->grid_width = (tile_width + cell_size - 1) / cell_size;
    grid->grid_height = (tile_height + cell_size - 1) / cell_size;
    grid->num_placements = 0;

    /* Allocate bitmap (1 bit per cell, rounded up to bytes) */
    int total_cells = grid->grid_width * grid->grid_height;
    int num_bytes = (total_cells + 7) / 8;
    grid->cells = calloc(num_bytes, 1);
    if (!grid->cells) {
        free(grid);
        return NULL;
    }

    return grid;
}

void ct_collision_free(CTCollisionGrid *grid)
{
    if (grid) {
        free(grid->cells);
        free(grid);
    }
}

void ct_collision_reset(CTCollisionGrid *grid)
{
    if (!grid || !grid->cells) {
        return;
    }

    int total_cells = grid->grid_width * grid->grid_height;
    int num_bytes = (total_cells + 7) / 8;
    memset(grid->cells, 0, num_bytes);
    grid->num_placements = 0;
}

/* ============================================================================
 * Collision Testing
 * ============================================================================ */

int ct_collision_test(const CTCollisionGrid *grid, int x, int y, int width, int height)
{
    if (!grid || !grid->cells || width <= 0 || height <= 0) {
        return 0;
    }

    /* Rectangle completely outside tile = no collision */
    if (x + width <= 0 || y + height <= 0 ||
        x >= grid->tile_width || y >= grid->tile_height) {
        return 0;
    }

    int cx_min, cy_min, cx_max, cy_max;
    pixel_to_cell_range(grid, x, y, width, height,
                        &cx_min, &cy_min, &cx_max, &cy_max);

    /* Check each cell in the range */
    for (int cy = cy_min; cy <= cy_max; cy++) {
        for (int cx = cx_min; cx <= cx_max; cx++) {
            if (cell_is_occupied(grid, cx, cy)) {
                return 1;  /* Collision found */
            }
        }
    }

    return 0;  /* No collision */
}

void ct_collision_mark(CTCollisionGrid *grid, int x, int y, int width, int height)
{
    if (!grid || !grid->cells || width <= 0 || height <= 0) {
        return;
    }

    /* Rectangle completely outside tile = nothing to mark */
    if (x + width <= 0 || y + height <= 0 ||
        x >= grid->tile_width || y >= grid->tile_height) {
        return;
    }

    int cx_min, cy_min, cx_max, cy_max;
    pixel_to_cell_range(grid, x, y, width, height,
                        &cx_min, &cy_min, &cx_max, &cy_max);

    /* Mark each cell in the range */
    for (int cy = cy_min; cy <= cy_max; cy++) {
        for (int cx = cx_min; cx <= cx_max; cx++) {
            cell_mark(grid, cx, cy);
        }
    }
}

int ct_collision_place(CTCollisionGrid *grid, int x, int y, int width, int height)
{
    if (!grid) {
        return 0;
    }

    /* Test first */
    if (ct_collision_test(grid, x, y, width, height)) {
        return 0;  /* Collision - placement failed */
    }

    /* Mark as occupied */
    ct_collision_mark(grid, x, y, width, height);
    grid->num_placements++;

    return 1;  /* Success */
}

/* ============================================================================
 * Padded Collision
 * ============================================================================ */

int ct_collision_test_padded(const CTCollisionGrid *grid,
                             int x, int y, int width, int height,
                             int padding_x, int padding_y)
{
    return ct_collision_test(grid,
                             x - padding_x,
                             y - padding_y,
                             width + 2 * padding_x,
                             height + 2 * padding_y);
}

void ct_collision_mark_padded(CTCollisionGrid *grid,
                              int x, int y, int width, int height,
                              int padding_x, int padding_y)
{
    ct_collision_mark(grid,
                      x - padding_x,
                      y - padding_y,
                      width + 2 * padding_x,
                      height + 2 * padding_y);
}

int ct_collision_place_padded(CTCollisionGrid *grid,
                              int x, int y, int width, int height,
                              int padding_x, int padding_y)
{
    if (!grid) {
        return 0;
    }

    /* Test with padding */
    if (ct_collision_test_padded(grid, x, y, width, height, padding_x, padding_y)) {
        return 0;  /* Collision - placement failed */
    }

    /* Mark with padding */
    ct_collision_mark_padded(grid, x, y, width, height, padding_x, padding_y);
    grid->num_placements++;

    return 1;  /* Success */
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

int ct_collision_get_count(const CTCollisionGrid *grid)
{
    if (!grid) {
        return 0;
    }
    return grid->num_placements;
}

float ct_collision_get_occupancy(const CTCollisionGrid *grid)
{
    if (!grid || !grid->cells) {
        return 0.0f;
    }

    int total_cells = grid->grid_width * grid->grid_height;
    if (total_cells == 0) {
        return 0.0f;
    }

    /* Count occupied cells */
    int occupied = 0;
    int num_bytes = (total_cells + 7) / 8;

    for (int i = 0; i < num_bytes; i++) {
        uint8_t byte = grid->cells[i];
        /* Count bits in byte (Brian Kernighan's algorithm) */
        while (byte) {
            occupied++;
            byte &= byte - 1;
        }
    }

    /* Handle partial last byte - don't count padding bits */
    int padding_bits = (num_bytes * 8) - total_cells;
    if (padding_bits > 0 && grid->cells[num_bytes - 1]) {
        /* Check if any padding bits were counted */
        uint8_t last_byte = grid->cells[num_bytes - 1];
        uint8_t padding_mask = (uint8_t)(0xFF << (8 - padding_bits));
        uint8_t padding_counted = last_byte & padding_mask;
        while (padding_counted) {
            occupied--;
            padding_counted &= padding_counted - 1;
        }
    }

    return (float)occupied / (float)total_cells;
}
