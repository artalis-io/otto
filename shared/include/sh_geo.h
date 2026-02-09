/*
 * sh_geo.h - Geographic utilities
 *
 * Coordinate types, haversine distance, and bounding box operations.
 * Used by velo (routing) and carta (tiles).
 */

#ifndef SH_GEO_H
#define SH_GEO_H

#include <stdint.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

/* Earth's mean radius in meters (WGS-84) */
#define SH_EARTH_RADIUS_M 6371000.0

/* Conversion factors */
#define SH_DEG_TO_RAD (3.14159265358979323846 / 180.0)
#define SH_RAD_TO_DEG (180.0 / 3.14159265358979323846)

/* ============================================================================
 * Coordinate Types
 * ============================================================================ */

/* Floating-point coordinate (latitude/longitude in degrees) */
typedef struct {
    double lat;  /* Latitude: -90 to +90 */
    double lon;  /* Longitude: -180 to +180 */
} SHCoord;

/*
 * Fixed-point coordinate using OSM standard: 100 nanodegrees (1e7 scale)
 * This matches OSM PBF format and fits in int32_t for all valid coordinates.
 * Range: -180 to +180 degrees = -1.8e9 to +1.8e9 (fits in int32_t)
 */
typedef struct {
    int32_t lat;  /* Latitude * 1e7 */
    int32_t lon;  /* Longitude * 1e7 */
} SHCoordFixed;

/* Scale factor for fixed-point coordinates */
#define SH_COORD_SCALE 1e7

/* Bounding box (in degrees) */
typedef struct {
    double min_lat;
    double min_lon;
    double max_lat;
    double max_lon;
} SHBBox;

/* ============================================================================
 * Coordinate Conversion
 * ============================================================================ */

/* Convert fixed-point to floating-point coordinates */
#define SH_FIXED_TO_COORD(fixed) ((SHCoord){ \
    .lat = (fixed).lat / SH_COORD_SCALE, \
    .lon = (fixed).lon / SH_COORD_SCALE  \
})

/* Convert floating-point to fixed-point coordinates */
#define SH_COORD_TO_FIXED(coord) ((SHCoordFixed){ \
    .lat = (int32_t)((coord).lat * SH_COORD_SCALE), \
    .lon = (int32_t)((coord).lon * SH_COORD_SCALE)  \
})

/* ============================================================================
 * Distance Calculations
 * ============================================================================ */

/*
 * Calculate great-circle distance using haversine formula.
 * Returns distance in meters.
 */
double sh_haversine(SHCoord a, SHCoord b);

/*
 * Fast equirectangular distance approximation.
 * Accurate within ~0.5% for distances < 500km.
 * Returns distance in meters.
 */
double sh_distance_fast(SHCoord a, SHCoord b);

/*
 * Haversine distance for fixed-point coordinates.
 */
double sh_haversine_fixed(SHCoordFixed a, SHCoordFixed b);

/*
 * Fast distance for fixed-point coordinates.
 */
double sh_distance_fast_fixed(SHCoordFixed a, SHCoordFixed b);

/* ============================================================================
 * Coordinate Utilities
 * ============================================================================ */

/*
 * Check if a coordinate is valid (within valid lat/lon ranges).
 */
int sh_coord_valid(SHCoord c);

/*
 * Check if a coordinate is within a bounding box.
 */
int sh_coord_in_bbox(SHCoord c, SHBBox bbox);

/*
 * Calculate the midpoint between two coordinates.
 */
SHCoord sh_coord_midpoint(SHCoord a, SHCoord b);

/*
 * Calculate initial bearing from a to b (in degrees, 0-360).
 */
double sh_bearing(SHCoord a, SHCoord b);

/*
 * Calculate destination point given start, bearing, and distance.
 */
SHCoord sh_destination(SHCoord start, double bearing_deg, double distance_m);

/* ============================================================================
 * Bounding Box Operations
 * ============================================================================ */

/*
 * Initialize a bounding box to empty state.
 */
void sh_bbox_init(SHBBox *bbox);

/*
 * Check if a bounding box is valid (not empty).
 */
int sh_bbox_valid(SHBBox bbox);

/*
 * Expand a bounding box to include a coordinate.
 */
void sh_bbox_expand(SHBBox *bbox, SHCoord c);

/*
 * Check if two bounding boxes intersect.
 */
int sh_bbox_intersects(SHBBox a, SHBBox b);

/*
 * Compute the union of two bounding boxes.
 */
SHBBox sh_bbox_union(SHBBox a, SHBBox b);

/* ============================================================================
 * Web Mercator Projection (EPSG:3857)
 * ============================================================================ */

/*
 * Convert lat/lon to Web Mercator coordinates.
 * Output x, y are in meters from origin.
 */
void sh_latlon_to_mercator(double lat, double lon, double *x, double *y);

/*
 * Convert Web Mercator coordinates to lat/lon.
 */
void sh_mercator_to_latlon(double x, double y, double *lat, double *lon);

/*
 * Convert lat/lon to tile coordinates at given zoom level.
 */
void sh_latlon_to_tile(double lat, double lon, int zoom, int *tile_x, int *tile_y);

/*
 * Get bounding box for a tile at given zoom level.
 */
SHBBox sh_tile_bounds(int zoom, int tile_x, int tile_y);

/* ============================================================================
 * Local Cartesian Projection
 *
 * Convert between lat/lon and local Cartesian coordinates (meters from a
 * reference point). Uses equirectangular approximation, suitable for small
 * areas (< 500km from reference).
 * ============================================================================ */

/*
 * Convert lat/lon to local Cartesian coordinates.
 *
 * @param coord Geographic coordinate to convert
 * @param ref   Reference point (origin of local system)
 * @param x     Output: local X coordinate in meters (east-positive)
 * @param y     Output: local Y coordinate in meters (north-positive)
 */
void sh_latlon_to_local(SHCoord coord, SHCoord ref, double *x, double *y);

/*
 * Convert local Cartesian coordinates back to lat/lon.
 *
 * @param x     Local X coordinate in meters
 * @param y     Local Y coordinate in meters
 * @param ref   Reference point (origin of local system)
 * @param coord Output: geographic coordinate
 */
void sh_local_to_latlon(double x, double y, SHCoord ref, SHCoord *coord);

#endif /* SH_GEO_H */
