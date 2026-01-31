/**
 * Clay Components - Map Widget
 *
 * Immediate mode slippy map component.
 * You own the map state (lat, lon, zoom), the component handles interaction.
 *
 * LIMITATION: Currently supports only a single map instance per application.
 * The drag state is stored globally, so multiple cs_map() calls will share
 * drag state. A future version may support multiple maps via a hash table
 * or user-provided state pointer.
 *
 * Usage:
 *   static double lat = 47.4979, lon = 19.0402;
 *   static int zoom = 12;
 *
 *   CsMapResult r = cs_map(CS_ID("map"), &lat, &lon, &zoom, width, height, NULL);
 *   if (r.clicked) {
 *       printf("Clicked at %.4f, %.4f\n", r.click_lat, r.click_lon);
 *   }
 */

#ifndef CS_MAP_H
#define CS_MAP_H

#include "cs_common.h"

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
} CsMapResult;

typedef struct {
    int min_zoom;       /* Minimum zoom level (default: 0) */
    int max_zoom;       /* Maximum zoom level (default: 19) */
    double min_lat;     /* Latitude bounds (default: -85.0) */
    double max_lat;     /* Latitude bounds (default: 85.0) */
} CsMapStyle;

/* Default style */
extern const CsMapStyle CC_MAP_STYLE_DEFAULT;

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
 * @param id     Unique identifier (use CS_ID("name"))
 * @param lat    Pointer to latitude (modified on pan)
 * @param lon    Pointer to longitude (modified on pan)
 * @param zoom   Pointer to zoom level (modified on scroll)
 * @param width  Map width in pixels
 * @param height Map height in pixels
 * @param style  Style or NULL for default
 * @return       Result with event flags
 */
CsMapResult cs_map(
    uint32_t id,
    double *lat,
    double *lon,
    int *zoom,
    float width,
    float height,
    const CsMapStyle *style
);

/* ============================================================================
 * Projection Utilities (useful for renderers)
 * ============================================================================ */

/* Convert longitude to tile X coordinate */
double cs_map_lon_to_tile_x(double lon, int zoom);

/* Convert latitude to tile Y coordinate */
double cs_map_lat_to_tile_y(double lat, int zoom);

/* Convert tile X to longitude */
double cs_map_tile_x_to_lon(double x, int zoom);

/* Convert tile Y to latitude */
double cs_map_tile_y_to_lat(double y, int zoom);

/* Convert screen offset to lat/lon delta */
void cs_map_screen_to_geo_delta(
    double lat, int zoom,
    float dx, float dy,
    double *dlat, double *dlon
);

/* ============================================================================
 * Drag/Scroll Handling (call from platform layer)
 * ============================================================================ */

/* Start drag at given screen position */
void cs_map_pointer_down(uint32_t id, double lat, double lon, float x, float y);

/* Update drag position - returns true if dragging, updates out_lat/out_lon */
bool cs_map_pointer_move(uint32_t id, int zoom, float x, float y, double *out_lat, double *out_lon);

/* End drag - returns true if was dragging. If movement < threshold, registers as click. */
bool cs_map_pointer_up(uint32_t id, float x, float y);

/* Check if map is currently being dragged */
bool cs_map_is_dragging(uint32_t id);

/* Handle scroll/zoom - returns clamped new zoom level */
int cs_map_scroll(int current_zoom, int delta, int min_zoom, int max_zoom);

/* ============================================================================
 * Smooth Zoom Animation
 * ============================================================================ */

/* Update zoom animation (call each frame with delta time) */
void cs_map_update_zoom_animation(uint32_t id, int target_zoom, float dt);

/* Get current visual zoom (float for smooth animation) */
double cs_map_get_visual_zoom(uint32_t id);

/* Check if zoom animation is in progress */
bool cs_map_is_zoom_animating(uint32_t id);

#ifdef __cplusplus
}
#endif

#endif /* CS_MAP_H */
