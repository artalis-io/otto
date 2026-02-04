/*
 * ct_tile.h - Tile coordinate math and Web Mercator projection
 *
 * Functions for converting between geographic coordinates (lat/lon)
 * and tile coordinates (z/x/y) using Web Mercator projection.
 */

#ifndef CT_TILE_H
#define CT_TILE_H

#include "ct_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Tile Coordinate Functions
 * ============================================================================ */

/*
 * Convert lat/lon to tile coordinates at a given zoom level.
 * Uses Web Mercator projection (EPSG:3857).
 *
 * @param lat    Latitude in degrees (-85.051 to 85.051)
 * @param lon    Longitude in degrees (-180 to 180)
 * @param zoom   Zoom level (0 to CT_MAX_ZOOM)
 * @param tile_x Output tile X coordinate
 * @param tile_y Output tile Y coordinate
 */
void ct_latlon_to_tile(double lat, double lon, int zoom,
                       int *tile_x, int *tile_y);

/*
 * Convert lat/lon to pixel position within a tile.
 *
 * @param lat    Latitude in degrees
 * @param lon    Longitude in degrees
 * @param tile   Tile coordinates
 * @param extent Pixel extent (e.g., 4096 for MVT, 256 for raster)
 * @param px     Output pixel X (0 to extent-1)
 * @param py     Output pixel Y (0 to extent-1)
 */
void ct_latlon_to_tile_pixel(double lat, double lon, CTTileCoord tile,
                             int extent, int *px, int *py);

/*
 * Fast batch conversion of fixed-point lat/lon to tile pixels.
 * Converts points in-place from nanodegrees to tile pixel coordinates.
 * Much faster than calling ct_latlon_to_tile_pixel() in a loop.
 *
 * @param tile       Tile coordinates
 * @param extent     Pixel extent
 * @param points     Array of points (x=lon*1e7, y=lat*1e7 on input, pixels on output)
 * @param num_points Number of points to transform
 */
void ct_batch_transform_points(CTTileCoord tile, int extent,
                               CTTilePoint *points, int num_points);

/*
 * Get geographic bounds of a tile.
 *
 * @param tile Tile coordinates
 * @return Bounding box in lat/lon
 */
CTBBox ct_tile_bounds(CTTileCoord tile);

/*
 * Convert tile coordinates to the center lat/lon.
 *
 * @param tile Tile coordinates
 * @param lat  Output latitude
 * @param lon  Output longitude
 */
void ct_tile_to_latlon(CTTileCoord tile, double *lat, double *lon);

/* ============================================================================
 * Tile Enumeration
 * ============================================================================ */

/*
 * Get all tiles that intersect a bounding box at a zoom level.
 *
 * OWNERSHIP: Caller must free() the returned *tiles array.
 *
 * @param bbox   Geographic bounds to cover
 * @param zoom   Zoom level
 * @param tiles  Output array of tile coordinates (caller owns, must free)
 * @return Number of tiles, or 0 on error
 */
int ct_tiles_for_bbox(CTBBox bbox, int zoom, CTTileCoord **tiles);

/*
 * Get the parent tile (one zoom level up).
 */
CTTileCoord ct_tile_parent(CTTileCoord tile);

/*
 * Get child tiles (one zoom level down).
 * Returns 4 tiles in children array.
 */
void ct_tile_children(CTTileCoord tile, CTTileCoord children[4]);

/*
 * Check if a tile coordinate is valid.
 */
int ct_tile_is_valid(CTTileCoord tile);

/* ============================================================================
 * Web Mercator Projection
 * ============================================================================ */

/*
 * Project lat/lon to Web Mercator coordinates (meters).
 * Origin is at (0, 0), X increases east, Y increases north.
 *
 * @param lat Latitude in degrees
 * @param lon Longitude in degrees
 * @param x   Output X in meters
 * @param y   Output Y in meters
 */
void ct_latlon_to_mercator(double lat, double lon, double *x, double *y);

/*
 * Unproject Web Mercator to lat/lon.
 *
 * @param x   X in meters
 * @param y   Y in meters
 * @param lat Output latitude
 * @param lon Output longitude
 */
void ct_mercator_to_latlon(double x, double y, double *lat, double *lon);

/* ============================================================================
 * Tile Management
 * ============================================================================ */

/*
 * Initialize a tile structure.
 * Must call ct_tile_free() when done to release memory.
 */
void ct_tile_init(CTTile *tile, CTTileCoord coord);

/*
 * Add a feature to a tile.
 *
 * OWNERSHIP: The tile takes ownership of the following pointers via shallow copy:
 *   - feature->points      (CTTilePoint array)
 *   - feature->ring_ends   (int array, for polygons with holes)
 *   - feature->prop_keys   (char* array)
 *   - feature->prop_values (char* array)
 *
 * After this call:
 *   - The caller must NOT free these pointers
 *   - The caller must NOT use the feature struct (it was copied)
 *   - ct_tile_free() will free all owned memory
 *
 * @param tile    Target tile (must be initialized)
 * @param feature Feature to add (shallow copied, ownership transferred)
 * @return CT_OK on success, CT_ERROR_OUT_OF_MEMORY if allocation fails
 */
CTStatus ct_tile_add_feature(CTTile *tile, const CTFeature *feature);

/*
 * Clear all features from a tile.
 * Frees all feature memory (points, ring_ends, properties) but keeps
 * the tile's feature array allocated for reuse.
 */
void ct_tile_clear(CTTile *tile);

/*
 * Free all memory associated with a tile.
 * After this call, the tile struct is zeroed and safe to reinitialize.
 */
void ct_tile_free(CTTile *tile);

/* ============================================================================
 * Geometry Utilities
 * ============================================================================ */

/*
 * Clip a linestring to tile bounds.
 * Returns clipped segments (may be multiple if line exits and re-enters).
 *
 * OWNERSHIP: Caller must free() both *out and *segments arrays.
 *
 * @param points     Input points (in tile coordinates, borrowed)
 * @param num_points Number of input points
 * @param extent     Tile extent (points outside 0..extent are clipped)
 * @param buffer     Buffer around tile (allow some overshoot)
 * @param out        Output clipped points (caller owns, must free)
 * @param out_count  Output point count
 * @param segments   Output segment end indices (caller owns, must free)
 * @param seg_count  Output segment count
 */
void ct_clip_linestring(const CTTilePoint *points, int num_points,
                        int extent, int buffer,
                        CTTilePoint **out, int *out_count,
                        int **segments, int *seg_count);

/*
 * Clip a polygon to tile bounds using Sutherland-Hodgman algorithm.
 *
 * OWNERSHIP: Caller must free() the *out array.
 *
 * @param points     Input points (in tile coordinates, borrowed)
 * @param num_points Number of input points
 * @param extent     Tile extent
 * @param buffer     Buffer around tile
 * @param out        Output clipped points (caller owns, must free)
 * @param out_count  Output point count
 */
void ct_clip_polygon(const CTTilePoint *points, int num_points,
                     int extent, int buffer,
                     CTTilePoint **out, int *out_count);

/*
 * Simplify a linestring using Douglas-Peucker algorithm.
 *
 * OWNERSHIP: Caller must free() the *out array.
 *
 * @param points     Input points (borrowed)
 * @param num_points Number of input points
 * @param tolerance  Simplification tolerance (in tile units)
 * @param out        Output simplified points (caller owns, must free)
 * @param out_count  Output point count
 */
void ct_simplify_linestring(const CTTilePoint *points, int num_points,
                            double tolerance,
                            CTTilePoint **out, int *out_count);

#ifdef __cplusplus
}
#endif

#endif /* CT_TILE_H */
