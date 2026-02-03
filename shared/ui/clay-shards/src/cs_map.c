/**
 * Clay Components - Map Widget Implementation
 *
 * Multi-instance architecture: Each map has its own state stored in a
 * hash table keyed by map_id. Supports up to CS_MAP_STATE_CAPACITY maps.
 *
 * This file contains:
 * - State management and hash table
 * - Buffer management
 * - Hit testing
 * - Core component and interaction
 * - Overlay management
 * - Accessors for JS renderer
 *
 * See also:
 * - cs_map_projection.c - Web Mercator projection utilities
 * - cs_map_simplify.c - Douglas-Peucker polyline simplification
 */

#include "cs_map_internal.h"
#include "cs_internal.h"
#include "clay.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>  /* For SIZE_MAX */

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

/* ============================================================================
 * Default Styles
 * ============================================================================ */

const CsMapStyle CS_MAP_STYLE_DEFAULT = {
    .min_zoom = 0,
    .max_zoom = 19,
    .min_lat = -85.0,
    .max_lat = 85.0,
};

const CsPolylineStyle CS_POLYLINE_STYLE_DEFAULT = {
    .color = {0.2f, 0.5f, 1.0f, 1.0f},  /* Blue */
    .width = 3.0f,
    .dashed = false,
    .dash_length = 10.0f,
    .gap_length = 5.0f,
};

const CsMarkerStyle CS_MARKER_STYLE_DEFAULT = {
    .color = {1.0f, 0.3f, 0.3f, 1.0f},  /* Red */
    .radius = 8.0f,
    .border_color = {1.0f, 1.0f, 1.0f, 1.0f},  /* White */
    .border_width = 2.0f,
    .draggable = false,
};

/* ============================================================================
 * State Management (Thread-Local)
 * ============================================================================ */

/* Hash table for per-map state - each thread has its own maps */
static CS_THREAD_LOCAL CsMapState g_map_states[CS_MAP_STATE_CAPACITY];

/* Current active map in begin/end context */
static CS_THREAD_LOCAL CsMapState *g_active_map = NULL;

CsMapState* cs_map_get_state(uint32_t id) {
    if (id == 0) return NULL;

    uint32_t slot = id % CS_MAP_STATE_CAPACITY;
    for (int i = 0; i < CS_MAP_STATE_CAPACITY; i++) {
        uint32_t idx = (slot + i) % CS_MAP_STATE_CAPACITY;
        if (g_map_states[idx].map_id == id) {
            return &g_map_states[idx];
        }
        if (g_map_states[idx].map_id == 0) {
            /* Empty slot - initialize it */
            memset(&g_map_states[idx], 0, sizeof(CsMapState));
            g_map_states[idx].map_id = id;
            return &g_map_states[idx];
        }
    }
    cs_record_error(CS_ERR_CAPACITY_EXCEEDED);
    return NULL;  /* Table full */
}

CsMapState* cs_map_get_active(void) {
    return g_active_map;
}

void cs_map_set_active(CsMapState *ms) {
    g_active_map = ms;
}

/* ============================================================================
 * Dynamic Polyline Buffer Management
 * ============================================================================ */

bool cs_map_ensure_polyline_capacity(CsMapState *ms, int needed) {
    if (ms->polyline_point_capacity >= needed) {
        return true;
    }

    /* Calculate new capacity (double until we reach needed or max) */
    int new_capacity = ms->polyline_point_capacity;
    if (new_capacity == 0) {
        new_capacity = CS_MAP_POLYLINE_INITIAL_CAPACITY;
    }
    while (new_capacity < needed && new_capacity < CS_MAP_POLYLINE_MAX_CAPACITY) {
        new_capacity *= 2;
    }

    /* Cap at max */
    if (new_capacity > CS_MAP_POLYLINE_MAX_CAPACITY) {
        new_capacity = CS_MAP_POLYLINE_MAX_CAPACITY;
    }

    /* If still not enough after hitting max, return false */
    if (new_capacity < needed) {
        cs_record_error(CS_ERR_CAPACITY_EXCEEDED);
        return false;
    }

    /* Check for integer overflow before allocation (defensive - capacity is bounded) */
    if ((size_t)new_capacity > SIZE_MAX / sizeof(CsGeoPoint)) {
        cs_record_error(CS_ERR_INVALID_ARGUMENT);
        return false;
    }

    /* Reallocate using custom allocator */
    CsGeoPoint *new_buffer = (CsGeoPoint *)cs_realloc(
        ms->polyline_points,
        (size_t)new_capacity * sizeof(CsGeoPoint)
    );
    if (!new_buffer) {
        /* cs_realloc already records the error */
        return false;
    }

    ms->polyline_points = new_buffer;
    ms->polyline_point_capacity = new_capacity;
    return true;
}

/* ============================================================================
 * Hit Testing
 * ============================================================================ */

/* Point to line segment distance squared */
static float point_to_segment_dist_sq(
    float px, float py,
    float x1, float y1,
    float x2, float y2
) {
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len_sq = dx * dx + dy * dy;

    if (len_sq < 0.0001f) {
        /* Segment is a point */
        float ddx = px - x1;
        float ddy = py - y1;
        return ddx * ddx + ddy * ddy;
    }

    /* Project point onto line, clamped to segment */
    float t = ((px - x1) * dx + (py - y1) * dy) / len_sq;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    float proj_x = x1 + t * dx;
    float proj_y = y1 + t * dy;

    float ddx = px - proj_x;
    float ddy = py - proj_y;
    return ddx * ddx + ddy * ddy;
}

EXPORT uint32_t cs_map_hit_test(
    uint32_t map_id,
    float px, float py,
    double center_lat, double center_lon,
    double zoom,
    float map_width, float map_height
) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms) return 0;

    /* Check overlays in reverse order (topmost first) */
    for (int i = ms->overlay_count - 1; i >= 0; i--) {
        CsOverlay *o = &ms->overlays[i];

        if (o->type == CS_OVERLAY_MARKER) {
            float mx, my;
            cs_map_geo_to_screen(o->marker.lat, o->marker.lon,
                         center_lat, center_lon, zoom,
                         map_width, map_height, &mx, &my);

            float dx = px - mx;
            float dy = py - my;
            float dist = sqrtf(dx * dx + dy * dy);
            float hit_radius = o->marker.style.radius + HIT_TEST_TOLERANCE;

            if (dist <= hit_radius) {
                return o->id;
            }
        } else if (o->type == CS_OVERLAY_POLYLINE) {
            /* Check each segment of the polyline */
            int start = o->polyline.point_start;
            int count = o->polyline.point_count;
            float half_width = o->polyline.style.width / 2.0f + HIT_TEST_TOLERANCE;
            float threshold_sq = half_width * half_width;

            for (int j = 0; j < count - 1; j++) {
                float x1, y1, x2, y2;
                cs_map_geo_to_screen(
                    ms->polyline_points[start + j].lat,
                    ms->polyline_points[start + j].lon,
                    center_lat, center_lon, zoom,
                    map_width, map_height, &x1, &y1
                );
                cs_map_geo_to_screen(
                    ms->polyline_points[start + j + 1].lat,
                    ms->polyline_points[start + j + 1].lon,
                    center_lat, center_lon, zoom,
                    map_width, map_height, &x2, &y2
                );

                float dist_sq = point_to_segment_dist_sq(px, py, x1, y1, x2, y2);
                if (dist_sq <= threshold_sq) {
                    return o->id;
                }
            }
        }
    }

    return 0;
}

/* ============================================================================
 * Core Component
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
    CsMapState *ms = cs_map_get_state(id);

    if (!ms) return result;
    if (!style) style = &CS_MAP_STYLE_DEFAULT;

    /* Build Clay element - uses custom render type for tile layer */
    Clay_ElementId clay_id = (Clay_ElementId){.id = id, .stringId = {0}};

    CLAY(clay_id, {
        .layout = {
            .sizing = {
                .width = CLAY_SIZING_FIXED(width),
                .height = CLAY_SIZING_FIXED(height)
            }
        },
        .backgroundColor = (Clay_Color){0, 0, 0, 0}
    }) {
    }

    /* Get element bounds for hit testing */
    Clay_BoundingBox box = Clay_GetElementData(clay_id).boundingBox;

    /* Check if pointer is over the map */
    bool is_hovered = Clay_PointerOver(clay_id);

    /* Handle drag start */
    if (is_hovered && g->pending_click && !ms->dragging_map && !ms->dragging_marker) {
        g->clicked_id = id;
    }

    /* Check for pending click from pointer_up */
    if (ms->pending_click) {
        ms->pending_click = false;
        result.clicked = true;

        float click_offset_x = ms->click_x - (box.x + width / 2.0f);
        float click_offset_y = ms->click_y - (box.y + height / 2.0f);

        double dlat, dlon;
        cs_map_screen_to_geo_delta(*lat, *zoom, click_offset_x, -click_offset_y, &dlat, &dlon);

        result.click_lat = *lat + dlat;
        result.click_lon = *lon + dlon;
    }

    /* Populate result with interaction state */
    result.hovered_id = ms->hovered_overlay_id;
    result.clicked_id = ms->clicked_overlay_id;

    /* Marker drag result */
    if (ms->dragging_marker) {
        result.dragged_marker_id = ms->dragging_overlay_id;
        result.dragged_marker_lat = ms->drag_end_lat;
        result.dragged_marker_lon = ms->drag_end_lon;
    }
    if (ms->drag_ended_this_frame) {
        result.drag_ended = true;
        result.dragged_marker_id = ms->dragging_overlay_id;
        result.dragged_marker_lat = ms->drag_end_lat;
        result.dragged_marker_lon = ms->drag_end_lon;
        ms->drag_ended_this_frame = false;
    }

    /* Clear clicked overlay after reading */
    ms->clicked_overlay_id = 0;

    /* Detect panned/zoomed by comparing to previous state */
    if (ms->state_initialized) {
        if (*lat != ms->prev_lat || *lon != ms->prev_lon) {
            result.panned = true;
        }
        if (*zoom != ms->prev_zoom) {
            result.zoomed = true;
        }
    }

    /* Clamp values */
    if (*lat > style->max_lat) *lat = style->max_lat;
    if (*lat < style->min_lat) *lat = style->min_lat;
    *lon = fmod(*lon + 180.0, 360.0);
    if (*lon < 0) *lon += 360.0;
    *lon -= 180.0;
    if (*zoom < style->min_zoom) *zoom = style->min_zoom;
    if (*zoom > style->max_zoom) *zoom = style->max_zoom;

    /* Update previous state for next frame */
    ms->prev_lat = *lat;
    ms->prev_lon = *lon;
    ms->prev_zoom = *zoom;
    ms->state_initialized = true;

    return result;
}

/* ============================================================================
 * Drag Handling
 * ============================================================================ */

void cs_map_pointer_down(uint32_t id, double lat, double lon, float x, float y,
                         float map_width, float map_height, double zoom) {
    CsMapState *ms = cs_map_get_state(id);
    if (!ms) return;

    /* Check if clicking on a draggable marker */
    uint32_t hit = cs_map_hit_test(id, x, y, lat, lon, zoom, map_width, map_height);
    if (hit != 0) {
        for (int i = 0; i < ms->overlay_count; i++) {
            if (ms->overlays[i].id == hit &&
                ms->overlays[i].type == CS_OVERLAY_MARKER &&
                ms->overlays[i].marker.style.draggable) {
                /* Start marker drag */
                ms->dragging_marker = true;
                ms->dragging_map = false;
                ms->dragging_overlay_id = hit;
                ms->drag_start_x = x;
                ms->drag_start_y = y;
                ms->marker_drag_start_lat = ms->overlays[i].marker.lat;
                ms->marker_drag_start_lon = ms->overlays[i].marker.lon;
                ms->drag_end_lat = ms->overlays[i].marker.lat;
                ms->drag_end_lon = ms->overlays[i].marker.lon;
                ms->pending_click = false;
                return;
            }
        }
    }

    /* Start map pan */
    ms->dragging_map = true;
    ms->dragging_marker = false;
    ms->dragging_overlay_id = 0;
    ms->drag_start_lat = lat;
    ms->drag_start_lon = lon;
    ms->drag_start_x = x;
    ms->drag_start_y = y;
    ms->pending_click = false;
}

bool cs_map_pointer_move(uint32_t id, double zoom, float x, float y,
                         float map_width, float map_height,
                         double *out_lat, double *out_lon) {
    (void)map_width; (void)map_height;
    CsMapState *ms = cs_map_get_state(id);
    if (!ms) return false;

    if (ms->dragging_marker) {
        /* Update marker position based on drag */
        float dx = x - ms->drag_start_x;
        float dy = ms->drag_start_y - y;  /* Y inverted */

        double dlat, dlon;
        cs_map_screen_to_geo_delta(ms->marker_drag_start_lat, zoom, dx, dy, &dlat, &dlon);

        ms->drag_end_lat = ms->marker_drag_start_lat + dlat;
        ms->drag_end_lon = ms->marker_drag_start_lon + dlon;

        /* Update the marker in the overlay array for rendering */
        for (int i = 0; i < ms->overlay_count; i++) {
            if (ms->overlays[i].id == ms->dragging_overlay_id) {
                ms->overlays[i].marker.lat = ms->drag_end_lat;
                ms->overlays[i].marker.lon = ms->drag_end_lon;
                break;
            }
        }

        /* Return original map position (not changed) */
        *out_lat = ms->drag_start_lat;
        *out_lon = ms->drag_start_lon;
        return true;
    }

    if (ms->dragging_map) {
        float dx = ms->drag_start_x - x;
        float dy = y - ms->drag_start_y;

        double dlat, dlon;
        cs_map_screen_to_geo_delta(ms->drag_start_lat, zoom, dx, dy, &dlat, &dlon);

        *out_lat = ms->drag_start_lat + dlat;
        *out_lon = ms->drag_start_lon + dlon;

        return true;
    }

    return false;
}

bool cs_map_pointer_up(uint32_t id, float x, float y) {
    CsMapState *ms = cs_map_get_state(id);
    if (!ms) return false;

    bool was_dragging_map = ms->dragging_map;
    bool was_dragging_marker = ms->dragging_marker;

    if (was_dragging_marker) {
        /* Marker drag ended */
        ms->drag_ended_this_frame = true;
        ms->dragging_marker = false;
        /* dragging_overlay_id preserved for result */
        return true;
    }

    ms->dragging_map = false;
    ms->dragging_marker = false;

    if (was_dragging_map) {
        float dx = x - ms->drag_start_x;
        float dy = y - ms->drag_start_y;
        float dist = sqrtf(dx * dx + dy * dy);

        if (dist < CLICK_THRESHOLD) {
            ms->pending_click = true;
            ms->click_x = x;
            ms->click_y = y;
        }
    }

    return was_dragging_map;
}

bool cs_map_is_dragging(uint32_t id) {
    CsMapState *ms = cs_map_get_state(id);
    if (!ms) return false;
    return ms->dragging_map;
}

bool cs_map_is_dragging_marker(uint32_t id) {
    CsMapState *ms = cs_map_get_state(id);
    if (!ms) return false;
    return ms->dragging_marker;
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
    CsMapState *ms = cs_map_get_state(id);
    if (!ms) return;

    if (!ms->visual_zoom_initialized) {
        ms->visual_zoom = (double)target_zoom;
        ms->visual_zoom_initialized = true;
        return;
    }

    double target = (double)target_zoom;
    double diff = target - ms->visual_zoom;

    if (fabs(diff) < 0.001) {
        ms->visual_zoom = target;
    } else {
        ms->visual_zoom += diff * ZOOM_EASE_FACTOR * (double)dt;

        if ((diff > 0 && ms->visual_zoom > target) ||
            (diff < 0 && ms->visual_zoom < target)) {
            ms->visual_zoom = target;
        }
    }
}

double cs_map_get_visual_zoom(uint32_t id) {
    CsMapState *ms = cs_map_get_state(id);
    if (!ms || !ms->visual_zoom_initialized) return 0.0;
    return ms->visual_zoom;
}

bool cs_map_is_zoom_animating(uint32_t id) {
    CsMapState *ms = cs_map_get_state(id);
    if (!ms || !ms->visual_zoom_initialized) return false;
    double diff = (double)ms->prev_zoom - ms->visual_zoom;
    return fabs(diff) > 0.001;
}

/* ============================================================================
 * Map with Overlays (begin/end pattern)
 * ============================================================================ */

CsMapResult cs_map_begin(
    uint32_t id,
    double *lat,
    double *lon,
    int *zoom,
    float width,
    float height,
    const CsMapStyle *style
) {
    CsMapState *ms = cs_map_get_state(id);
    if (ms) {
        /* Reset overlay state for new frame */
        ms->overlay_count = 0;
        ms->polyline_point_count = 0;

        /* Store current zoom for zoom-aware simplification */
        ms->current_zoom = ms->visual_zoom_initialized ? ms->visual_zoom : (double)*zoom;

        /* Set active map for overlay functions */
        g_active_map = ms;
    }

    /* Call base cs_map for layout and interaction */
    return cs_map(id, lat, lon, zoom, width, height, style);
}

void cs_map_end(void) {
    g_active_map = NULL;
}

/**
 * Calculate zoom-dependent epsilon for Douglas-Peucker simplification.
 * At high zoom (18+): very small epsilon = full detail
 * At low zoom (0): larger epsilon = aggressive simplification
 */
static double zoom_to_epsilon(double zoom) {
    const double BASE_EPSILON = 0.00001;  /* ~1m at zoom 18 */
    const double MAX_ZOOM = 18.0;

    if (zoom >= MAX_ZOOM) return BASE_EPSILON;
    if (zoom < 0) zoom = 0;

    /* Epsilon doubles for each zoom level below 18 */
    return BASE_EPSILON * pow(2.0, MAX_ZOOM - zoom);
}

void cs_polyline(
    uint32_t id,
    const CsGeoPoint *points,
    int count,
    const CsPolylineStyle *style
) {
    if (!g_active_map) return;
    if (g_active_map->overlay_count >= CS_MAP_MAX_OVERLAYS) return;
    if (count <= 0) return;

    if (!style) style = &CS_POLYLINE_STYLE_DEFAULT;

    /* Calculate zoom-dependent epsilon */
    double epsilon = zoom_to_epsilon(g_active_map->current_zoom);

    /* Simplify based on zoom level */
    int max_points = CS_MAP_POLYLINE_MAX_CAPACITY - g_active_map->polyline_point_count;
    if (max_points <= 2) return;

    /* Ensure we have buffer space */
    if (!cs_map_ensure_polyline_capacity(g_active_map, g_active_map->polyline_point_count + max_points)) {
        max_points = g_active_map->polyline_point_capacity - g_active_map->polyline_point_count;
        if (max_points <= 2) return;
    }

    int point_start = g_active_map->polyline_point_count;
    int simplified_count = cs_map_simplify_polyline(
        points, count,
        epsilon,
        &g_active_map->polyline_points[point_start],
        max_points
    );

    g_active_map->polyline_point_count += simplified_count;

    CsOverlay *overlay = &g_active_map->overlays[g_active_map->overlay_count++];
    overlay->type = CS_OVERLAY_POLYLINE;
    overlay->id = id;
    overlay->polyline.point_start = point_start;
    overlay->polyline.point_count = simplified_count;
    overlay->polyline.style = *style;
}

void cs_marker(
    uint32_t id,
    double lat,
    double lon,
    const CsMarkerStyle *style
) {
    if (!g_active_map) return;
    if (g_active_map->overlay_count >= CS_MAP_MAX_OVERLAYS) return;

    if (!style) style = &CS_MARKER_STYLE_DEFAULT;

    CsOverlay *overlay = &g_active_map->overlays[g_active_map->overlay_count++];
    overlay->type = CS_OVERLAY_MARKER;
    overlay->id = id;
    overlay->marker.lat = lat;
    overlay->marker.lon = lon;
    overlay->marker.style = *style;
}

/* ============================================================================
 * Overlay Accessors (for JS renderer)
 * ============================================================================ */

EXPORT int cs_map_overlay_count(uint32_t map_id) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms) return 0;
    return ms->overlay_count;
}

EXPORT int cs_map_overlay_type(uint32_t map_id, int index) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || index < 0 || index >= ms->overlay_count) return CS_OVERLAY_NONE;
    return ms->overlays[index].type;
}

EXPORT uint32_t cs_map_overlay_id(uint32_t map_id, int index) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || index < 0 || index >= ms->overlay_count) return 0;
    return ms->overlays[index].id;
}

/* Polyline accessors */
EXPORT int cs_map_overlay_polyline_count(uint32_t map_id, int index) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || index < 0 || index >= ms->overlay_count) return 0;
    if (ms->overlays[index].type != CS_OVERLAY_POLYLINE) return 0;
    return ms->overlays[index].polyline.point_count;
}

EXPORT double cs_map_overlay_polyline_lat(uint32_t map_id, int index, int point_index) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || index < 0 || index >= ms->overlay_count) return 0;
    if (ms->overlays[index].type != CS_OVERLAY_POLYLINE) return 0;
    int start = ms->overlays[index].polyline.point_start;
    int count = ms->overlays[index].polyline.point_count;
    if (point_index < 0 || point_index >= count) return 0;
    return ms->polyline_points[start + point_index].lat;
}

EXPORT double cs_map_overlay_polyline_lon(uint32_t map_id, int index, int point_index) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || index < 0 || index >= ms->overlay_count) return 0;
    if (ms->overlays[index].type != CS_OVERLAY_POLYLINE) return 0;
    int start = ms->overlays[index].polyline.point_start;
    int count = ms->overlays[index].polyline.point_count;
    if (point_index < 0 || point_index >= count) return 0;
    return ms->polyline_points[start + point_index].lon;
}

EXPORT float cs_map_overlay_polyline_color_r(uint32_t map_id, int index) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || index < 0 || index >= ms->overlay_count) return 0;
    if (ms->overlays[index].type != CS_OVERLAY_POLYLINE) return 0;
    return ms->overlays[index].polyline.style.color.r;
}

EXPORT float cs_map_overlay_polyline_color_g(uint32_t map_id, int index) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || index < 0 || index >= ms->overlay_count) return 0;
    if (ms->overlays[index].type != CS_OVERLAY_POLYLINE) return 0;
    return ms->overlays[index].polyline.style.color.g;
}

EXPORT float cs_map_overlay_polyline_color_b(uint32_t map_id, int index) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || index < 0 || index >= ms->overlay_count) return 0;
    if (ms->overlays[index].type != CS_OVERLAY_POLYLINE) return 0;
    return ms->overlays[index].polyline.style.color.b;
}

EXPORT float cs_map_overlay_polyline_color_a(uint32_t map_id, int index) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || index < 0 || index >= ms->overlay_count) return 0;
    if (ms->overlays[index].type != CS_OVERLAY_POLYLINE) return 0;
    return ms->overlays[index].polyline.style.color.a;
}

EXPORT float cs_map_overlay_polyline_width(uint32_t map_id, int index) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || index < 0 || index >= ms->overlay_count) return 0;
    if (ms->overlays[index].type != CS_OVERLAY_POLYLINE) return 0;
    return ms->overlays[index].polyline.style.width;
}

/* Marker accessors */
EXPORT double cs_map_overlay_marker_lat(uint32_t map_id, int index) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || index < 0 || index >= ms->overlay_count) return 0;
    if (ms->overlays[index].type != CS_OVERLAY_MARKER) return 0;
    return ms->overlays[index].marker.lat;
}

EXPORT double cs_map_overlay_marker_lon(uint32_t map_id, int index) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || index < 0 || index >= ms->overlay_count) return 0;
    if (ms->overlays[index].type != CS_OVERLAY_MARKER) return 0;
    return ms->overlays[index].marker.lon;
}

EXPORT float cs_map_overlay_marker_radius(uint32_t map_id, int index) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || index < 0 || index >= ms->overlay_count) return 0;
    if (ms->overlays[index].type != CS_OVERLAY_MARKER) return 0;
    return ms->overlays[index].marker.style.radius;
}

EXPORT float cs_map_overlay_marker_color_r(uint32_t map_id, int index) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || index < 0 || index >= ms->overlay_count) return 0;
    if (ms->overlays[index].type != CS_OVERLAY_MARKER) return 0;
    return ms->overlays[index].marker.style.color.r;
}

EXPORT float cs_map_overlay_marker_color_g(uint32_t map_id, int index) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || index < 0 || index >= ms->overlay_count) return 0;
    if (ms->overlays[index].type != CS_OVERLAY_MARKER) return 0;
    return ms->overlays[index].marker.style.color.g;
}

EXPORT float cs_map_overlay_marker_color_b(uint32_t map_id, int index) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || index < 0 || index >= ms->overlay_count) return 0;
    if (ms->overlays[index].type != CS_OVERLAY_MARKER) return 0;
    return ms->overlays[index].marker.style.color.b;
}

EXPORT float cs_map_overlay_marker_color_a(uint32_t map_id, int index) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || index < 0 || index >= ms->overlay_count) return 0;
    if (ms->overlays[index].type != CS_OVERLAY_MARKER) return 0;
    return ms->overlays[index].marker.style.color.a;
}

EXPORT float cs_map_overlay_marker_border_r(uint32_t map_id, int index) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || index < 0 || index >= ms->overlay_count) return 0;
    if (ms->overlays[index].type != CS_OVERLAY_MARKER) return 0;
    return ms->overlays[index].marker.style.border_color.r;
}

EXPORT float cs_map_overlay_marker_border_g(uint32_t map_id, int index) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || index < 0 || index >= ms->overlay_count) return 0;
    if (ms->overlays[index].type != CS_OVERLAY_MARKER) return 0;
    return ms->overlays[index].marker.style.border_color.g;
}

EXPORT float cs_map_overlay_marker_border_b(uint32_t map_id, int index) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || index < 0 || index >= ms->overlay_count) return 0;
    if (ms->overlays[index].type != CS_OVERLAY_MARKER) return 0;
    return ms->overlays[index].marker.style.border_color.b;
}

EXPORT float cs_map_overlay_marker_border_a(uint32_t map_id, int index) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || index < 0 || index >= ms->overlay_count) return 0;
    if (ms->overlays[index].type != CS_OVERLAY_MARKER) return 0;
    return ms->overlays[index].marker.style.border_color.a;
}

EXPORT float cs_map_overlay_marker_border_width(uint32_t map_id, int index) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || index < 0 || index >= ms->overlay_count) return 0;
    if (ms->overlays[index].type != CS_OVERLAY_MARKER) return 0;
    return ms->overlays[index].marker.style.border_width;
}

EXPORT bool cs_map_overlay_marker_draggable(uint32_t map_id, int index) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || index < 0 || index >= ms->overlay_count) return false;
    if (ms->overlays[index].type != CS_OVERLAY_MARKER) return false;
    return ms->overlays[index].marker.style.draggable;
}

/* Interaction state accessors/setters */
EXPORT void cs_map_set_hovered_overlay(uint32_t map_id, uint32_t overlay_id) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (ms) ms->hovered_overlay_id = overlay_id;
}

EXPORT void cs_map_set_clicked_overlay(uint32_t map_id, uint32_t overlay_id) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (ms) ms->clicked_overlay_id = overlay_id;
}

EXPORT uint32_t cs_map_get_hovered_overlay(uint32_t map_id) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms) return 0;
    return ms->hovered_overlay_id;
}

EXPORT uint32_t cs_map_get_dragging_overlay(uint32_t map_id) {
    CsMapState *ms = cs_map_get_state(map_id);
    if (!ms || !ms->dragging_marker) return 0;
    return ms->dragging_overlay_id;
}

/* ============================================================================
 * Cleanup
 * ============================================================================ */

void cs_map_destroy(uint32_t id) {
    if (id == 0) return;

    uint32_t slot = id % CS_MAP_STATE_CAPACITY;
    for (int i = 0; i < CS_MAP_STATE_CAPACITY; i++) {
        uint32_t idx = (slot + i) % CS_MAP_STATE_CAPACITY;
        if (g_map_states[idx].map_id == id) {
            /* Free polyline buffer using custom allocator */
            if (g_map_states[idx].polyline_points) {
                cs_free(g_map_states[idx].polyline_points);
            }
            /* Clear the slot */
            memset(&g_map_states[idx], 0, sizeof(CsMapState));
            return;
        }
        if (g_map_states[idx].map_id == 0) {
            return;  /* Not found */
        }
    }
}

void cs_map_cleanup(void) {
    for (int i = 0; i < CS_MAP_STATE_CAPACITY; i++) {
        if (g_map_states[i].map_id != 0) {
            /* Free polyline buffer using custom allocator */
            if (g_map_states[i].polyline_points) {
                cs_free(g_map_states[i].polyline_points);
            }
            memset(&g_map_states[i], 0, sizeof(CsMapState));
        }
    }
    g_active_map = NULL;
}
