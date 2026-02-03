/**
 * Clay Components - Map Widget Internal Header
 *
 * Shared types and constants for map implementation files.
 * Not part of public API.
 */

#ifndef CS_MAP_INTERNAL_H
#define CS_MAP_INTERNAL_H

#include "cs_map.h"
#include <stdbool.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define CS_PI 3.14159265358979323846
#define DEG_TO_RAD (CS_PI / 180.0)
#define RAD_TO_DEG (180.0 / CS_PI)

/* Web Mercator latitude limits - beyond this, projection math breaks down */
#define MAX_LATITUDE 85.051129

/* Click detection threshold in pixels */
#define CLICK_THRESHOLD 5.0f

/* Hit test tolerance in pixels (added to object bounds) */
#define HIT_TEST_TOLERANCE 4.0f

/* Zoom animation easing factor (higher = faster) */
#define ZOOM_EASE_FACTOR 10.0f

/* Maximum stack depth for iterative Douglas-Peucker */
#define DP_STACK_SIZE 64

/* ============================================================================
 * Internal Types
 * ============================================================================ */

/**
 * Overlay storage structure.
 */
typedef struct {
    int type;                       /* CS_OVERLAY_POLYLINE or CS_OVERLAY_MARKER */
    uint32_t id;
    union {
        struct {
            int point_start;        /* Index into polyline_points */
            int point_count;
            CsPolylineStyle style;
        } polyline;
        struct {
            double lat, lon;
            CsMarkerStyle style;
        } marker;
    };
} CsOverlay;

/**
 * Per-map state structure.
 */
typedef struct {
    uint32_t map_id;  /* 0 = empty slot */

    /* Drag state */
    bool dragging_map;
    bool dragging_marker;
    uint32_t dragging_overlay_id;
    float drag_start_x, drag_start_y;
    double drag_start_lat, drag_start_lon;
    /* For marker drag: original marker position */
    double marker_drag_start_lat, marker_drag_start_lon;

    /* Click detection */
    bool pending_click;
    float click_x, click_y;

    /* State tracking for panned/zoomed detection */
    double prev_lat, prev_lon;
    int prev_zoom;
    bool state_initialized;

    /* Smooth zoom animation */
    double visual_zoom;
    bool visual_zoom_initialized;

    /* Per-map overlays */
    CsOverlay overlays[CS_MAP_MAX_OVERLAYS];
    int overlay_count;

    /* Dynamic polyline point buffer */
    CsGeoPoint *polyline_points;
    int polyline_point_count;
    int polyline_point_capacity;

    /* Current zoom for this frame (used for zoom-aware simplification) */
    double current_zoom;

    /* Interaction state */
    uint32_t hovered_overlay_id;
    uint32_t clicked_overlay_id;

    /* Drag ended this frame (for result) */
    bool drag_ended_this_frame;
    double drag_end_lat, drag_end_lon;
} CsMapState;

/* ============================================================================
 * Internal Functions (shared between map implementation files)
 * ============================================================================ */

/* Clamp latitude to valid Web Mercator range */
double cs_map_clamp_latitude(double lat);

/* Clamp zoom to valid range */
int cs_map_clamp_zoom(int zoom);

/* Get or create state for a map ID */
CsMapState* cs_map_get_state(uint32_t id);

/* Get active map in begin/end context */
CsMapState* cs_map_get_active(void);

/* Set active map */
void cs_map_set_active(CsMapState *ms);

/* Ensure polyline buffer has capacity */
bool cs_map_ensure_polyline_capacity(CsMapState *ms, int needed);

/* Douglas-Peucker simplification */
int cs_map_simplify_polyline(
    const CsGeoPoint *points,
    int count,
    double epsilon,
    CsGeoPoint *out,
    int out_capacity
);

/* Geo to screen coordinate conversion */
void cs_map_geo_to_screen(
    double lat, double lon,
    double center_lat, double center_lon,
    double zoom,
    float map_width, float map_height,
    float *out_x, float *out_y
);

#endif /* CS_MAP_INTERNAL_H */
