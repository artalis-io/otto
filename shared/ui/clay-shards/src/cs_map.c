/**
 * Clay Components - Map Widget Implementation
 */

#include "cs_map.h"
#include "cs_internal.h"
#include "clay.h"
#include <math.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define PI 3.14159265358979323846
#define DEG_TO_RAD (PI / 180.0)
#define RAD_TO_DEG (180.0 / PI)

/* Web Mercator latitude limits - beyond this, projection math breaks down */
#define MAX_LATITUDE 85.051129

/* Meters per pixel at equator for zoom 0 */
#define METERS_PER_PIXEL_Z0 156543.03392

/* Clamp latitude to valid Web Mercator range */
static double clamp_latitude(double lat) {
    if (lat > MAX_LATITUDE) return MAX_LATITUDE;
    if (lat < -MAX_LATITUDE) return -MAX_LATITUDE;
    return lat;
}

/* Clamp zoom to valid range (0-30 to avoid overflow in 1 << zoom) */
static int clamp_zoom(int zoom) {
    if (zoom < 0) return 0;
    if (zoom > 30) return 30;
    return zoom;
}

/* ============================================================================
 * Default Style
 * ============================================================================ */

const CsMapStyle CC_MAP_STYLE_DEFAULT = {
    .min_zoom = 0,
    .max_zoom = 19,
    .min_lat = -85.0,
    .max_lat = 85.0,
};

/* ============================================================================
 * Map Drag State
 * ============================================================================ */

/* Click detection threshold in pixels */
#define CLICK_THRESHOLD 5.0f

/* Zoom animation easing factor (higher = faster) */
#define ZOOM_EASE_FACTOR 10.0f

/* Per-map drag state (keyed by ID) */
typedef struct {
    uint32_t id;
    bool dragging;
    float drag_start_x;
    float drag_start_y;
    double drag_start_lat;
    double drag_start_lon;
    /* Click detection - set by pointer_up, consumed by cs_map */
    bool pending_click;
    float click_x;
    float click_y;
    /* Change tracking for panned/zoomed result fields */
    double prev_lat;
    double prev_lon;
    int prev_zoom;
    bool state_initialized;
    /* Smooth zoom animation */
    double visual_zoom;
    bool visual_zoom_initialized;
} CcMapDragState;

/* Global drag state - LIMITATION: only one map supported at a time.
 * The ID check in cs_map_pointer_move() ensures correct behavior if
 * cs_map() is called with different IDs, but drag state is shared.
 * Future: use a hash table for multiple map support if needed. */
static CcMapDragState g_map_drag = {0};

/* ============================================================================
 * Projection Utilities
 * ============================================================================ */

double cs_map_lon_to_tile_x(double lon, int zoom) {
    zoom = clamp_zoom(zoom);
    return (lon + 180.0) / 360.0 * (double)(1 << zoom);
}

double cs_map_lat_to_tile_y(double lat, int zoom) {
    lat = clamp_latitude(lat);
    zoom = clamp_zoom(zoom);
    double lat_rad = lat * DEG_TO_RAD;
    return (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / PI) / 2.0 * (double)(1 << zoom);
}

double cs_map_tile_x_to_lon(double x, int zoom) {
    zoom = clamp_zoom(zoom);
    return x / (double)(1 << zoom) * 360.0 - 180.0;
}

double cs_map_tile_y_to_lat(double y, int zoom) {
    zoom = clamp_zoom(zoom);
    double n = PI - 2.0 * PI * y / (double)(1 << zoom);
    return RAD_TO_DEG * atan(0.5 * (exp(n) - exp(-n)));
}

void cs_map_screen_to_geo_delta(
    double lat, int zoom,
    float dx, float dy,
    double *dlat, double *dlon
) {
    lat = clamp_latitude(lat);
    zoom = clamp_zoom(zoom);
    /* Meters per pixel at current latitude and zoom */
    double meters_per_pixel = METERS_PER_PIXEL_Z0 * cos(lat * DEG_TO_RAD) / (double)(1 << zoom);
    
    /* Convert to degrees (approximate) */
    /* 111320 meters per degree longitude at equator */
    /* 110540 meters per degree latitude (roughly constant) */
    if (dlon) *dlon = (double)dx * meters_per_pixel / 111320.0;
    if (dlat) *dlat = (double)dy * meters_per_pixel / 110540.0;
}

/* ============================================================================
 * Component
 * ============================================================================ */

CsMapResult cs_map(
    uint32_t id,
    double *lat,
    double *lon,
    int *zoom,
    float width,
    float height,
    const CsMapStyle *style
) {
    CsMapResult result = {0};
    CsState *g = cs_get_state();
    
    if (!style) style = &CC_MAP_STYLE_DEFAULT;
    
    /* Build Clay element - uses custom render type for tile layer */
    Clay_ElementId clay_id = (Clay_ElementId){.id = id, .stringId = {0}};
    
    CLAY(clay_id, {
        .layout = {
            .sizing = {
                .width = CLAY_SIZING_FIXED(width),
                .height = CLAY_SIZING_FIXED(height)
            }
        },
        /* Transparent - tiles are rendered separately by the platform layer */
        .backgroundColor = (Clay_Color){0, 0, 0, 0}
    }) {
        /* Empty - tiles rendered by platform (JS/native) based on lat/lon/zoom */
    }
    
    /* Get element bounds for hit testing */
    Clay_BoundingBox box = Clay_GetElementData(clay_id).boundingBox;
    
    /* Check if pointer is over the map */
    bool is_hovered = Clay_PointerOver(clay_id);
    
    /* Initialize drag state for this map if needed */
    if (g_map_drag.id != id) {
        g_map_drag.id = id;
        g_map_drag.dragging = false;
    }
    
    /* Handle drag start - actual drag tracking done via cs_map_pointer_down() */
    if (is_hovered && g->pending_click && !g_map_drag.dragging) {
        g->clicked_id = id;
    }
    
    /* Check for pending click from pointer_up */
    if (g_map_drag.id == id && g_map_drag.pending_click) {
        g_map_drag.pending_click = false;
        result.clicked = true;

        /* Convert click screen position to lat/lon */
        float click_offset_x = g_map_drag.click_x - (box.x + width / 2.0f);
        float click_offset_y = g_map_drag.click_y - (box.y + height / 2.0f);

        double dlat, dlon;
        cs_map_screen_to_geo_delta(*lat, *zoom, click_offset_x, -click_offset_y, &dlat, &dlon);

        result.click_lat = *lat + dlat;
        result.click_lon = *lon + dlon;
    }

    /* Detect panned/zoomed by comparing to previous state */
    if (g_map_drag.id == id && g_map_drag.state_initialized) {
        if (*lat != g_map_drag.prev_lat || *lon != g_map_drag.prev_lon) {
            result.panned = true;
        }
        if (*zoom != g_map_drag.prev_zoom) {
            result.zoomed = true;
        }
    }

    /* Clamp values */
    if (*lat > style->max_lat) *lat = style->max_lat;
    if (*lat < style->min_lat) *lat = style->min_lat;
    /* Use fmod for O(1) longitude wrapping instead of while loop */
    *lon = fmod(*lon + 180.0, 360.0);
    if (*lon < 0) *lon += 360.0;
    *lon -= 180.0;
    if (*zoom < style->min_zoom) *zoom = style->min_zoom;
    if (*zoom > style->max_zoom) *zoom = style->max_zoom;

    /* Update previous state for next frame's panned/zoomed detection */
    g_map_drag.prev_lat = *lat;
    g_map_drag.prev_lon = *lon;
    g_map_drag.prev_zoom = *zoom;
    g_map_drag.state_initialized = true;

    return result;
}

/* ============================================================================
 * Drag Handling (called from platform layer)
 * ============================================================================ */

void cs_map_pointer_down(uint32_t id, double lat, double lon, float x, float y) {
    g_map_drag.id = id;
    g_map_drag.dragging = true;
    g_map_drag.drag_start_lat = lat;
    g_map_drag.drag_start_lon = lon;
    g_map_drag.drag_start_x = x;
    g_map_drag.drag_start_y = y;
    g_map_drag.pending_click = false;
}

bool cs_map_pointer_move(uint32_t id, int zoom, float x, float y, double *out_lat, double *out_lon) {
    if (g_map_drag.id != id || !g_map_drag.dragging) {
        return false;
    }
    
    /* Calculate delta from drag start */
    float dx = g_map_drag.drag_start_x - x;
    float dy = y - g_map_drag.drag_start_y;  /* Y is inverted in screen coords */
    
    double dlat, dlon;
    cs_map_screen_to_geo_delta(g_map_drag.drag_start_lat, zoom, dx, dy, &dlat, &dlon);
    
    *out_lat = g_map_drag.drag_start_lat + dlat;
    *out_lon = g_map_drag.drag_start_lon + dlon;
    
    return true;
}

bool cs_map_pointer_up(uint32_t id, float x, float y) {
    if (g_map_drag.id != id) return false;

    bool was_dragging = g_map_drag.dragging;
    g_map_drag.dragging = false;

    /* Detect click vs drag based on movement distance */
    if (was_dragging) {
        float dx = x - g_map_drag.drag_start_x;
        float dy = y - g_map_drag.drag_start_y;
        float dist = sqrtf(dx * dx + dy * dy);

        if (dist < CLICK_THRESHOLD) {
            /* This was a click, not a drag */
            g_map_drag.pending_click = true;
            g_map_drag.click_x = x;
            g_map_drag.click_y = y;
        }
    }

    return was_dragging;
}

bool cs_map_is_dragging(uint32_t id) {
    return g_map_drag.id == id && g_map_drag.dragging;
}

int cs_map_scroll(int current_zoom, int delta, int min_zoom, int max_zoom) {
    int new_zoom = current_zoom + delta;
    if (new_zoom < min_zoom) new_zoom = min_zoom;
    if (new_zoom > max_zoom) new_zoom = max_zoom;
    return new_zoom;
}

/* ============================================================================
 * Smooth Zoom Animation
 * ============================================================================ */

void cs_map_update_zoom_animation(uint32_t id, int target_zoom, float dt) {
    if (g_map_drag.id != id) return;

    /* Initialize visual zoom if needed */
    if (!g_map_drag.visual_zoom_initialized) {
        g_map_drag.visual_zoom = (double)target_zoom;
        g_map_drag.visual_zoom_initialized = true;
        return;
    }

    /* Exponential easing toward target */
    double target = (double)target_zoom;
    double diff = target - g_map_drag.visual_zoom;

    /* Snap if very close */
    if (fabs(diff) < 0.001) {
        g_map_drag.visual_zoom = target;
    } else {
        /* Ease toward target: visual += diff * factor * dt */
        g_map_drag.visual_zoom += diff * ZOOM_EASE_FACTOR * (double)dt;

        /* Prevent overshooting */
        if ((diff > 0 && g_map_drag.visual_zoom > target) ||
            (diff < 0 && g_map_drag.visual_zoom < target)) {
            g_map_drag.visual_zoom = target;
        }
    }
}

double cs_map_get_visual_zoom(uint32_t id) {
    if (g_map_drag.id != id || !g_map_drag.visual_zoom_initialized) {
        return 0.0;
    }
    return g_map_drag.visual_zoom;
}

bool cs_map_is_zoom_animating(uint32_t id) {
    if (g_map_drag.id != id || !g_map_drag.visual_zoom_initialized) {
        return false;
    }
    double diff = (double)g_map_drag.prev_zoom - g_map_drag.visual_zoom;
    return fabs(diff) > 0.001;
}
