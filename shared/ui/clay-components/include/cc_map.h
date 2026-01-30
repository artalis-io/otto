/**
 * Clay Components - Map Widget
 *
 * Immediate mode slippy map component.
 * You own the map state (lat, lon, zoom), the component handles interaction.
 *
 * Usage:
 *   static double lat = 47.4979, lon = 19.0402;
 *   static int zoom = 12;
 *
 *   CcMapResult r = cc_map(CC_ID("map"), &lat, &lon, &zoom, width, height, NULL);
 *   if (r.clicked) {
 *       printf("Clicked at %.4f, %.4f\n", r.click_lat, r.click_lon);
 *   }
 */

#ifndef CC_MAP_H
#define CC_MAP_H

#include "cc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

typedef struct {
    bool panned;        /* Map was panned this frame */
    bool zoomed;        /* Zoom level changed this frame */
    bool clicked;       /* Map area was clicked (not dragged) */
    double click_lat;   /* If clicked, latitude */
    double click_lon;   /* If clicked, longitude */
} CcMapResult;

typedef struct {
    int min_zoom;       /* Minimum zoom level (default: 0) */
    int max_zoom;       /* Maximum zoom level (default: 19) */
    double min_lat;     /* Latitude bounds (default: -85.0) */
    double max_lat;     /* Latitude bounds (default: 85.0) */
} CcMapStyle;

/* Default style */
extern const CcMapStyle CC_MAP_STYLE_DEFAULT;

/* ============================================================================
 * Component
 * ============================================================================ */

/**
 * Map widget - immediate mode
 *
 * The map renders as a Clay custom element. The JavaScript renderer should:
 * 1. Detect custom elements with the map's ID
 * 2. Render tiles based on lat/lon/zoom from WASM exports
 *
 * @param id     Unique identifier (use CC_ID("name"))
 * @param lat    Pointer to latitude (modified on pan)
 * @param lon    Pointer to longitude (modified on pan)
 * @param zoom   Pointer to zoom level (modified on scroll)
 * @param width  Map width in pixels
 * @param height Map height in pixels
 * @param style  Style or NULL for default
 * @return       Result with event flags
 */
CcMapResult cc_map(
    uint32_t id,
    double *lat,
    double *lon,
    int *zoom,
    float width,
    float height,
    const CcMapStyle *style
);

/* ============================================================================
 * Projection Utilities (useful for renderers)
 * ============================================================================ */

/* Convert longitude to tile X coordinate */
double cc_map_lon_to_tile_x(double lon, int zoom);

/* Convert latitude to tile Y coordinate */
double cc_map_lat_to_tile_y(double lat, int zoom);

/* Convert tile X to longitude */
double cc_map_tile_x_to_lon(double x, int zoom);

/* Convert tile Y to latitude */
double cc_map_tile_y_to_lat(double y, int zoom);

/* Convert screen offset to lat/lon delta */
void cc_map_screen_to_geo_delta(
    double lat, int zoom,
    float dx, float dy,
    double *dlat, double *dlon
);

/* ============================================================================
 * Drag/Scroll Handling (call from platform layer)
 * ============================================================================ */

/* Start drag at given screen position */
void cc_map_pointer_down(uint32_t id, double lat, double lon, float x, float y);

/* Update drag position - returns true if dragging, updates out_lat/out_lon */
bool cc_map_pointer_move(uint32_t id, int zoom, float x, float y, double *out_lat, double *out_lon);

/* End drag - returns true if was dragging */
bool cc_map_pointer_up(uint32_t id);

/* Check if map is currently being dragged */
bool cc_map_is_dragging(uint32_t id);

/* Handle scroll/zoom - returns clamped new zoom level */
int cc_map_scroll(int current_zoom, int delta, int min_zoom, int max_zoom);

#ifdef __cplusplus
}
#endif

#endif /* CC_MAP_H */
