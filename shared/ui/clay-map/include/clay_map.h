/**
 * Clay Map Component
 *
 * A Leaflet-like map viewer component built with Clay UI.
 * Handles map state, tile coordinates, and UI overlays.
 */

#ifndef CLAY_MAP_H
#define CLAY_MAP_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

typedef struct {
    double lat;
    double lon;
    int zoom;
    double view_lat;
    double view_lon;
    float view_zoom;
    int width;
    int height;
    bool dragging;
    float drag_start_x;
    float drag_start_y;
    double drag_start_lat;
    double drag_start_lon;
} ClayMapState;

typedef struct {
    bool show_controls;
    bool show_tile_info;
    int layer_type;
    char status_text[128];
} ClayMapUIState;

/* ============================================================================
 * API Functions
 * ============================================================================ */

/* Initialize map state */
void clay_map_init(ClayMapState *map, int width, int height);

/* Set map center position */
void clay_map_set_center(ClayMapState *map, double lat, double lon);

/* Set zoom level (clamped to 0-19) */
void clay_map_set_zoom(ClayMapState *map, int zoom);

/* Handle resize */
void clay_map_resize(ClayMapState *map, int width, int height);

/* Handle pointer input */
void clay_map_pointer_move(ClayMapState *map, float x, float y);
void clay_map_pointer_down(ClayMapState *map, float x, float y);
void clay_map_pointer_up(ClayMapState *map, float x, float y);

/* Handle scroll/zoom */
void clay_map_scroll(ClayMapState *map, float delta, float x, float y);

/* ============================================================================
 * Coordinate Conversion
 * ============================================================================ */

/* Convert longitude to tile X coordinate */
double clay_map_lon_to_tile_x(double lon, int zoom);

/* Convert latitude to tile Y coordinate */
double clay_map_lat_to_tile_y(double lat, int zoom);

/* Convert tile X to longitude */
double clay_map_tile_x_to_lon(double x, int zoom);

/* Convert tile Y to latitude */
double clay_map_tile_y_to_lat(double y, int zoom);

#ifdef __cplusplus
}
#endif

#endif /* CLAY_MAP_H */
