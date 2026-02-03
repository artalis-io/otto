/**
 * Clay Components - Map Projection Utilities
 *
 * Web Mercator projection and coordinate conversion functions.
 */

#include "cs_map_internal.h"
#include <math.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

/* ============================================================================
 * Clamping Utilities
 * ============================================================================ */

double cs_map_clamp_latitude(double lat) {
    if (lat > MAX_LATITUDE) return MAX_LATITUDE;
    if (lat < -MAX_LATITUDE) return -MAX_LATITUDE;
    return lat;
}

int cs_map_clamp_zoom(int zoom) {
    if (zoom < 0) return 0;
    if (zoom > 30) return 30;
    return zoom;
}

/* ============================================================================
 * Tile Coordinate Conversion
 * ============================================================================ */

EXPORT double cs_map_lon_to_tile_x(double lon, int zoom) {
    zoom = cs_map_clamp_zoom(zoom);
    return (lon + 180.0) / 360.0 * (double)(1 << zoom);
}

EXPORT double cs_map_lat_to_tile_y(double lat, int zoom) {
    lat = cs_map_clamp_latitude(lat);
    zoom = cs_map_clamp_zoom(zoom);
    double lat_rad = lat * DEG_TO_RAD;
    return (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / PI) / 2.0 * (double)(1 << zoom);
}

EXPORT double cs_map_tile_x_to_lon(double x, int zoom) {
    zoom = cs_map_clamp_zoom(zoom);
    return x / (double)(1 << zoom) * 360.0 - 180.0;
}

EXPORT double cs_map_tile_y_to_lat(double y, int zoom) {
    zoom = cs_map_clamp_zoom(zoom);
    double n = PI - 2.0 * PI * y / (double)(1 << zoom);
    return RAD_TO_DEG * atan(0.5 * (exp(n) - exp(-n)));
}

/* ============================================================================
 * Screen/Geo Conversion
 * ============================================================================ */

EXPORT void cs_map_screen_to_geo_delta(
    double lat, double zoom,
    float dx, float dy,
    double *dlat, double *dlon
) {
    lat = cs_map_clamp_latitude(lat);
    if (zoom < 0) zoom = 0;
    if (zoom > 22) zoom = 22;

    double scale = 256.0 * pow(2.0, zoom);

    if (dlon) {
        *dlon = (double)dx * 360.0 / scale;
    }

    if (dlat) {
        double lat_rad = lat * DEG_TO_RAD;
        double center_y = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / PI) / 2.0 * scale;
        double new_y = center_y - (double)dy;
        double n = PI - 2.0 * PI * new_y / scale;
        double new_lat = RAD_TO_DEG * atan(0.5 * (exp(n) - exp(-n)));
        *dlat = new_lat - lat;
    }
}

void cs_map_geo_to_screen(
    double lat, double lon,
    double center_lat, double center_lon,
    double zoom,
    float map_width, float map_height,
    float *out_x, float *out_y
) {
    double scale = 256.0 * pow(2.0, zoom);

    /* Center in world coordinates */
    double center_x = (center_lon + 180.0) / 360.0 * scale;
    double center_lat_rad = center_lat * DEG_TO_RAD;
    double center_y = (1.0 - log(tan(center_lat_rad) + 1.0 / cos(center_lat_rad)) / PI) / 2.0 * scale;

    /* Point in world coordinates */
    double px = (lon + 180.0) / 360.0 * scale;
    double lat_rad = lat * DEG_TO_RAD;
    double py = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / PI) / 2.0 * scale;

    /* Convert to screen coordinates */
    *out_x = (float)(map_width / 2.0 + (px - center_x));
    *out_y = (float)(map_height / 2.0 + (py - center_y));
}
