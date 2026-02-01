/**
 * Clay Components - Map Widget
 *
 * Immediate mode slippy map component with overlay support.
 * You own the map state (lat, lon, zoom), the component handles interaction.
 *
 * Basic usage:
 *   static double lat = 47.4979, lon = 19.0402;
 *   static int zoom = 12;
 *   CsMapResult r = cs_map(CS_ID("map"), &lat, &lon, &zoom, width, height, NULL);
 *
 * With overlays (begin/end pattern):
 *   CsMapResult r = cs_map_begin(CS_ID("map"), &lat, &lon, &zoom, w, h, NULL);
 *   cs_polyline(CS_ID("route"), points, count, &polyline_style);
 *   cs_marker(CS_ID("start"), 47.5, 19.0, &marker_style);
 *   cs_map_end();
 *
 * LIMITATION: Currently supports only a single map instance per application.
 */

#ifndef CS_MAP_H
#define CS_MAP_H

#include "cs_common.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Overlay Capacity (compile-time configurable)
 * ============================================================================ */

#ifndef CS_MAP_MAX_OVERLAYS
#define CS_MAP_MAX_OVERLAYS 64
#endif

#ifndef CS_MAP_MAX_POLYLINE_POINTS
#define CS_MAP_MAX_POLYLINE_POINTS 1024
#endif

/* ============================================================================
 * Geographic Types
 * ============================================================================ */

typedef struct {
    double lat;
    double lon;
} CsGeoPoint;

/* ============================================================================
 * Overlay Styles
 * ============================================================================ */

typedef struct {
    float r, g, b, a;
} CsColor;

typedef struct {
    CsColor color;          /* Line color (default: blue) */
    float width;            /* Line width in pixels (default: 3.0) */
    bool dashed;            /* Dashed line (default: false) */
    float dash_length;      /* Dash length in pixels (default: 10.0) */
    float gap_length;       /* Gap length in pixels (default: 5.0) */
} CsPolylineStyle;

typedef struct {
    CsColor color;          /* Fill color (default: red) */
    float radius;           /* Marker radius in pixels (default: 8.0) */
    CsColor border_color;   /* Border color (default: white) */
    float border_width;     /* Border width in pixels (default: 2.0) */
} CsMarkerStyle;

/* Default styles */
extern const CsPolylineStyle CS_POLYLINE_STYLE_DEFAULT;
extern const CsMarkerStyle CS_MARKER_STYLE_DEFAULT;

/* ============================================================================
 * Map Result
 * ============================================================================ */

typedef struct {
    bool panned;            /* Map was panned this frame */
    bool zoomed;            /* Zoom level changed this frame */
    bool clicked;           /* Map area was clicked (not on overlay) */
    double click_lat;       /* If clicked, latitude */
    double click_lon;       /* If clicked, longitude */
    uint32_t hovered_id;    /* ID of hovered overlay (0 if none) */
    uint32_t clicked_id;    /* ID of clicked overlay (0 if none) */
} CsMapResult;

typedef struct {
    int min_zoom;           /* Minimum zoom level (default: 0) */
    int max_zoom;           /* Maximum zoom level (default: 19) */
    double min_lat;         /* Latitude bounds (default: -85.0) */
    double max_lat;         /* Latitude bounds (default: 85.0) */
} CsMapStyle;

/* Default style */
extern const CsMapStyle CS_MAP_STYLE_DEFAULT;

/* ============================================================================
 * Simple Map API (no overlays)
 * ============================================================================ */

/**
 * Map widget - immediate mode, simple version without overlays
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
 * Map with Overlays API (begin/end pattern)
 * ============================================================================ */

/**
 * Begin map context - call before adding overlays
 *
 * Usage:
 *   CsMapResult r = cs_map_begin(CS_ID("map"), &lat, &lon, &zoom, w, h, NULL);
 *   cs_polyline(CS_ID("route"), points, count, &style);
 *   cs_marker(CS_ID("start"), 47.5, 19.0, &style);
 *   cs_map_end();
 *
 * @return Result with event flags (hovered_id/clicked_id populated after cs_map_end)
 */
CsMapResult cs_map_begin(
    uint32_t id,
    double *lat,
    double *lon,
    int *zoom,
    float width,
    float height,
    const CsMapStyle *style
);

/**
 * End map context - finalizes overlay processing
 */
void cs_map_end(void);

/**
 * Add a polyline overlay (only valid between cs_map_begin/cs_map_end)
 *
 * @param id     Unique identifier for interaction tracking
 * @param points Array of geographic points
 * @param count  Number of points
 * @param style  Style or NULL for default
 */
void cs_polyline(
    uint32_t id,
    const CsGeoPoint *points,
    int count,
    const CsPolylineStyle *style
);

/**
 * Add a marker overlay (only valid between cs_map_begin/cs_map_end)
 *
 * @param id    Unique identifier for interaction tracking
 * @param lat   Marker latitude
 * @param lon   Marker longitude
 * @param style Style or NULL for default
 */
void cs_marker(
    uint32_t id,
    double lat,
    double lon,
    const CsMarkerStyle *style
);

/* ============================================================================
 * Overlay Buffer Access (for renderers)
 * ============================================================================ */

/* Overlay types */
#define CS_OVERLAY_NONE     0
#define CS_OVERLAY_POLYLINE 1
#define CS_OVERLAY_MARKER   2

/* Get number of overlays in current frame */
int cs_map_overlay_count(void);

/* Get overlay type by index */
int cs_map_overlay_type(int index);

/* Get overlay ID by index */
uint32_t cs_map_overlay_id(int index);

/* Polyline accessors */
int cs_map_overlay_polyline_count(int index);
double cs_map_overlay_polyline_lat(int index, int point_index);
double cs_map_overlay_polyline_lon(int index, int point_index);
float cs_map_overlay_polyline_color_r(int index);
float cs_map_overlay_polyline_color_g(int index);
float cs_map_overlay_polyline_color_b(int index);
float cs_map_overlay_polyline_color_a(int index);
float cs_map_overlay_polyline_width(int index);

/* Marker accessors */
double cs_map_overlay_marker_lat(int index);
double cs_map_overlay_marker_lon(int index);
float cs_map_overlay_marker_radius(int index);
float cs_map_overlay_marker_color_r(int index);
float cs_map_overlay_marker_color_g(int index);
float cs_map_overlay_marker_color_b(int index);
float cs_map_overlay_marker_color_a(int index);
float cs_map_overlay_marker_border_r(int index);
float cs_map_overlay_marker_border_g(int index);
float cs_map_overlay_marker_border_b(int index);
float cs_map_overlay_marker_border_a(int index);
float cs_map_overlay_marker_border_width(int index);

/* Set hovered overlay (called from JS after hit testing) */
void cs_map_set_hovered_overlay(uint32_t id);

/* Set clicked overlay (called from JS after hit testing) */
void cs_map_set_clicked_overlay(uint32_t id);

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
