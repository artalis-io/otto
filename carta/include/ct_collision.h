/*
 * ct_collision.h - Label Collision Detection
 *
 * Grid-based collision detection for map label placement.
 * Used to prevent overlapping labels on tile rendering.
 *
 * Usage:
 *   CTCollisionGrid *grid = ct_collision_create(256, 256, 8);
 *   if (!ct_collision_test(grid, x, y, w, h)) {
 *       ct_collision_mark(grid, x, y, w, h);
 *       // Place label here
 *   }
 *   ct_collision_free(grid);
 */

#ifndef CT_COLLISION_H
#define CT_COLLISION_H

#include "ct_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

/*
 * Grid-based collision index for fast overlap detection.
 * Divides tile into cells; each cell tracks if it's occupied.
 */
typedef struct {
    uint8_t *cells;          /* Occupied cells bitmap (1 bit per cell) */
    int grid_width;          /* Number of cells horizontally */
    int grid_height;         /* Number of cells vertically */
    int cell_size;           /* Pixels per cell */
    int tile_width;          /* Tile width in pixels */
    int tile_height;         /* Tile height in pixels */
    int num_placements;      /* Number of rectangles placed */
} CTCollisionGrid;

/* ============================================================================
 * Creation / Destruction
 * ============================================================================ */

/*
 * Create a collision grid for a tile.
 *
 * @param tile_width   Tile width in pixels (e.g., 256, 512)
 * @param tile_height  Tile height in pixels
 * @param cell_size    Pixels per grid cell (e.g., 8). Smaller = more precise but more memory.
 * @return             New collision grid, or NULL on error
 */
CTCollisionGrid *ct_collision_create(int tile_width, int tile_height, int cell_size);

/*
 * Free a collision grid.
 */
void ct_collision_free(CTCollisionGrid *grid);

/*
 * Reset a collision grid for reuse (clears all occupied cells).
 */
void ct_collision_reset(CTCollisionGrid *grid);

/* ============================================================================
 * Collision Testing
 * ============================================================================ */

/*
 * Test if a rectangle would collide with any occupied cells.
 *
 * @param grid   Collision grid
 * @param x      Rectangle left edge (pixels, can be negative)
 * @param y      Rectangle top edge (pixels, can be negative)
 * @param width  Rectangle width (pixels)
 * @param height Rectangle height (pixels)
 * @return       1 if collision detected, 0 if area is free
 */
int ct_collision_test(const CTCollisionGrid *grid, int x, int y, int width, int height);

/*
 * Mark a rectangle as occupied.
 *
 * @param grid   Collision grid
 * @param x      Rectangle left edge (pixels)
 * @param y      Rectangle top edge (pixels)
 * @param width  Rectangle width (pixels)
 * @param height Rectangle height (pixels)
 */
void ct_collision_mark(CTCollisionGrid *grid, int x, int y, int width, int height);

/*
 * Test and mark in one atomic operation.
 * If the area is free, marks it as occupied and returns success.
 *
 * @param grid   Collision grid
 * @param x      Rectangle left edge (pixels)
 * @param y      Rectangle top edge (pixels)
 * @param width  Rectangle width (pixels)
 * @param height Rectangle height (pixels)
 * @return       1 if placement succeeded (area was free), 0 if collision
 */
int ct_collision_place(CTCollisionGrid *grid, int x, int y, int width, int height);

/* ============================================================================
 * Padded Collision (convenience functions)
 * ============================================================================ */

/*
 * Test with padding around the rectangle.
 * Useful to ensure labels don't touch each other.
 *
 * @param grid      Collision grid
 * @param x         Rectangle left edge (pixels)
 * @param y         Rectangle top edge (pixels)
 * @param width     Rectangle width (pixels)
 * @param height    Rectangle height (pixels)
 * @param padding_x Horizontal padding (pixels, applied to both sides)
 * @param padding_y Vertical padding (pixels, applied to both sides)
 * @return          1 if collision detected, 0 if area is free
 */
int ct_collision_test_padded(const CTCollisionGrid *grid,
                             int x, int y, int width, int height,
                             int padding_x, int padding_y);

/*
 * Mark with padding around the rectangle.
 */
void ct_collision_mark_padded(CTCollisionGrid *grid,
                              int x, int y, int width, int height,
                              int padding_x, int padding_y);

/*
 * Test and mark with padding in one operation.
 */
int ct_collision_place_padded(CTCollisionGrid *grid,
                              int x, int y, int width, int height,
                              int padding_x, int padding_y);

/* ============================================================================
 * Statistics
 * ============================================================================ */

/*
 * Get the number of successful placements.
 */
int ct_collision_get_count(const CTCollisionGrid *grid);

/*
 * Get the percentage of grid cells that are occupied.
 * @return Occupancy ratio [0.0, 1.0]
 */
float ct_collision_get_occupancy(const CTCollisionGrid *grid);

#ifdef __cplusplus
}
#endif

#endif /* CT_COLLISION_H */
