/**
 * ClayShards Demo - Map Viewer Application
 *
 * Demonstrates ClayShards immediate-mode UI components:
 *   - cs_map: Pan/zoom map interaction
 *   - cs_input: Text input with cursor/selection
 *   - cs_button: Clickable buttons with variants
 *
 * This file contains domain-specific code:
 *   - Application state (map position, UI settings)
 *   - UI layout (panels, buttons, inputs)
 *   - Domain-specific exports (map getters, pointer handling)
 *
 * Generic Clay integration is in cs_clay.c (clay-shards).
 * Generic text cursor rendering is in text-cursor.js (clay-shards-webgl).
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

#define CLAY_IMPLEMENTATION
#include "clay.h"
#include "cs_immediate.h"
#include "cs_map_provider.h"

/* ============================================================================
 * Application State
 *
 * Domain-specific state only. Generic Clay state is in cs_clay.c.
 * ============================================================================ */

typedef struct {
    double lat;
    double lon;
    int zoom;
    int width;
    int height;
    uint32_t component_id;
} MapState;

typedef struct {
    bool show_tile_info;
    int layer_type;
} UIPanels;

/* Route state for click-to-route */
typedef struct {
    bool has_start;
    bool has_end;
    CsGeoPoint start;
    CsGeoPoint end;
    /* Route geometry from provider */
    CsGeoPoint points[CS_PROVIDER_MAX_ROUTE_POINTS];
    int point_count;
    double distance_m;
    double duration_s;
    bool loading;
    bool error;
} RouteState;

typedef struct {
    char search[256];
    int search_len;
} UIText;

typedef struct {
    char prev_query[256];       /* Track changes */
    int prev_len;
    bool show_results;          /* Show dropdown */
} SearchState;

/* Scratch buffers for formatted strings */
typedef struct {
    char coord[64];
    char zoom[32];
    char tile[48];
    char route_info[64];
} Scratch;

typedef struct {
    MapState map;
    UIPanels panels;
    UIText text;
    Scratch scratch;
    RouteState route;
    SearchState search;
} AppState;

static AppState g_app = {
    .map = { .lat = 47.4979, .lon = 19.0402, .zoom = 12, .width = 800, .height = 600 },
    .panels = { .show_tile_info = true, .layer_type = 0 },
    .text = { .search = "", .search_len = 0 },
    .search = { .prev_query = "", .prev_len = 0, .show_results = false },
};

/* ============================================================================
 * Theme
 * ============================================================================ */

static const struct {
    Clay_Color bg_panel;
    Clay_Color bg_overlay;
    Clay_Color text;
    Clay_Color text_muted;
    Clay_Color text_dark;
    Clay_Color border;
} THEME = {
    .bg_panel   = {40, 40, 40, 230},
    .bg_overlay = {0, 0, 0, 180},
    .text       = {255, 255, 255, 255},
    .text_muted = {180, 180, 180, 255},
    .text_dark  = {40, 40, 40, 255},
    .border     = {100, 100, 100, 255},
};

/* ============================================================================
 * UI Layout - Domain Specific
 * ============================================================================ */

static void render_zoom_controls(void) {
    /* Square buttons for zoom +/-
     * Each glyph needs different offset due to different vertical bounds:
     * "+" spans -0.078 to 0.703, "-" spans 0.14 to 0.39 */
    const CsButtonStyle zoom_plus = {
        .variant = CS_BTN_DEFAULT,
        .font_size = 18,
        .corner_radius = 4,
        .width = 36,
        .height = 36,
        .text_offset_y = -2
    };
    const CsButtonStyle zoom_minus = {
        .variant = CS_BTN_DEFAULT,
        .font_size = 18,
        .corner_radius = 4,
        .width = 36,
        .height = 36,
        .text_offset_y = -5
    };

    CLAY(CLAY_ID("ZoomControls"), {
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_RIGHT_TOP, .parent = CLAY_ATTACH_POINT_RIGHT_TOP },
            .offset = {-16, 160}
        },
        .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 2 }
    }) {
        if (cs_button(CS_ID("zoom_in"), "+", &zoom_plus).clicked) {
            g_app.map.zoom = cs_map_scroll(g_app.map.zoom, 1, 0, 19);
        }
        if (cs_button(CS_ID("zoom_out"), "-", &zoom_minus).clicked) {
            g_app.map.zoom = cs_map_scroll(g_app.map.zoom, -1, 0, 19);
        }
    }
}

static void render_layer_panel(void) {
    CLAY(CLAY_ID("LayerPanel"), {
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_RIGHT_TOP, .parent = CLAY_ATTACH_POINT_RIGHT_TOP },
            .offset = {-16, 16}
        },
        .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = CLAY_PADDING_ALL(8), .childGap = 4 },
        .backgroundColor = THEME.bg_panel,
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .border = { .width = {1, 1, 1, 1}, .color = THEME.border }
    }) {
        CLAY_TEXT(CLAY_STRING("Layers"), CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text_muted }));

        /* Fixed width buttons for consistent alignment */
        const CsButtonStyle sel = {CS_BTN_PRIMARY, 14, 8, 8, 4, 70};
        const CsButtonStyle def = {CS_BTN_DEFAULT, 14, 8, 8, 4, 70};

        if (cs_button(CS_ID("layer_carta"), "Carta", g_app.panels.layer_type == 0 ? &sel : &def).clicked)
            g_app.panels.layer_type = 0;
        if (cs_button(CS_ID("layer_osm"), "OSM", g_app.panels.layer_type == 1 ? &sel : &def).clicked)
            g_app.panels.layer_type = 1;
    }
}

static void render_info_panel(void) {
    snprintf(g_app.scratch.coord, sizeof(g_app.scratch.coord), "%.4f, %.4f", g_app.map.lat, g_app.map.lon);
    snprintf(g_app.scratch.zoom, sizeof(g_app.scratch.zoom), "Zoom: %d", g_app.map.zoom);

    CLAY(CLAY_ID("InfoPanel"), {
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_TOP },
            .offset = {16, 16}
        },
        .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = CLAY_PADDING_ALL(12), .childGap = 8 },
        .backgroundColor = THEME.bg_panel,
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .border = { .width = {1, 1, 1, 1}, .color = THEME.border }
    }) {
        CLAY_TEXT(CLAY_STRING("ClayShards Map"), CLAY_TEXT_CONFIG({ .fontSize = 16, .textColor = THEME.text }));

        CLAY(CLAY_ID("CoordRow"), { .layout = { .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = 8 } }) {
            CLAY_TEXT(CLAY_STRING("Center:"), CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text_muted }));
            CLAY_TEXT(((Clay_String){ .chars = g_app.scratch.coord, .length = (int)strlen(g_app.scratch.coord) }),
                      CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text }));
        }

        CLAY_TEXT(((Clay_String){ .chars = g_app.scratch.zoom, .length = (int)strlen(g_app.scratch.zoom) }),
                  CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text }));

        const CsInputStyle input_style = { .width = 200, .height = 28, .font_size = 12, .padding = 8, .corner_radius = 4 };
        CsInputResult search_result = cs_input(CS_ID("search"), g_app.text.search, &g_app.text.search_len,
                 sizeof(g_app.text.search), "Search location...", &input_style);

        /* Detect search text changes - trigger new search */
        bool query_changed = (g_app.text.search_len != g_app.search.prev_len ||
                              strcmp(g_app.text.search, g_app.search.prev_query) != 0);
        if (query_changed && g_app.text.search_len >= 2) {
            /* Search with bias toward current map center */
            cs_provider_search_near(g_app.text.search, g_app.map.lat, g_app.map.lon);
            strncpy(g_app.search.prev_query, g_app.text.search, sizeof(g_app.search.prev_query) - 1);
            g_app.search.prev_len = g_app.text.search_len;
            g_app.search.show_results = true;
        }

        /* Show search results dropdown */
        int result_count = cs_provider_search_count();
        if (g_app.search.show_results && result_count > 0 && g_app.text.search_len >= 2) {
            CLAY(CLAY_ID("SearchResults"), {
                .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 2 },
            }) {
                for (int i = 0; i < result_count && i < 5; i++) {
                    const CsSearchResult *r = cs_provider_search_result(i);
                    if (!r) continue;

                    /* Create button for each result */
                    char result_id[32];
                    snprintf(result_id, sizeof(result_id), "result_%d", i);
                    const CsButtonStyle result_style = { CS_BTN_DEFAULT, 11, 6, 4, 2, 200 };

                    if (cs_button(cs_hash_id(result_id), r->name, &result_style).clicked) {
                        /* Navigate to result location */
                        g_app.map.lat = r->lat;
                        g_app.map.lon = r->lon;
                        g_app.map.zoom = 16; /* Zoom in */
                        g_app.search.show_results = false;
                        g_app.text.search_len = 0;
                        g_app.text.search[0] = '\0';
                    }
                }
            }
        }

        /* Hide results when input loses focus or is cleared */
        if (search_result.blurred || g_app.text.search_len < 2) {
            g_app.search.show_results = false;
        }
    }
}

static void render_tile_info(void) {
    if (!g_app.panels.show_tile_info) return;

    int tile_x = (int)cs_map_lon_to_tile_x(g_app.map.lon, g_app.map.zoom);
    int tile_y = (int)cs_map_lat_to_tile_y(g_app.map.lat, g_app.map.zoom);
    snprintf(g_app.scratch.tile, sizeof(g_app.scratch.tile), "%d/%d/%d", g_app.map.zoom, tile_x, tile_y);

    CLAY(CLAY_ID("TileInfo"), {
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_BOTTOM, .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM },
            .offset = {16, -16}
        },
        .layout = {
            .padding = CLAY_PADDING_ALL(8),
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
        },
        .backgroundColor = THEME.bg_overlay,
        .cornerRadius = CLAY_CORNER_RADIUS(4)
    }) {
        CLAY_TEXT(((Clay_String){ .chars = g_app.scratch.tile, .length = (int)strlen(g_app.scratch.tile) }),
                  CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text }));
    }
}

static void render_attribution(void) {
    CLAY(CLAY_ID("Attribution"), {
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_RIGHT_BOTTOM, .parent = CLAY_ATTACH_POINT_RIGHT_BOTTOM },
            .offset = {-8, -8}
        },
        .layout = { .padding = CLAY_PADDING_ALL(4) },
        .backgroundColor = (Clay_Color){255, 255, 255, 200},
        .cornerRadius = CLAY_CORNER_RADIUS(2)
    }) {
        CLAY_TEXT(CLAY_STRING("OpenStreetMap contributors"),
                  CLAY_TEXT_CONFIG({ .fontSize = 10, .textColor = THEME.text_dark }));
    }
}

/* Helper to format distance */
static void format_distance(char *buf, size_t size, double meters) {
    if (meters >= 1000.0) {
        snprintf(buf, size, "%.1f km", meters / 1000.0);
    } else {
        snprintf(buf, size, "%.0f m", meters);
    }
}

/* Helper to format duration */
static void format_duration(char *buf, size_t size, double seconds) {
    int mins = (int)(seconds / 60.0);
    if (mins >= 60) {
        int hours = mins / 60;
        mins = mins % 60;
        snprintf(buf, size, "%dh %dm", hours, mins);
    } else {
        snprintf(buf, size, "%d min", mins);
    }
}

static void render_route_panel(void) {
    const CsButtonStyle clear_btn = {
        .variant = CS_BTN_DEFAULT,
        .font_size = 12,
        .corner_radius = 4,
        .padding_x = 8,
        .padding_y = 4,
    };

    CLAY(CLAY_ID("RoutePanel"), {
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_BOTTOM, .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM },
            .offset = {16, -60}
        },
        .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = CLAY_PADDING_ALL(10), .childGap = 6 },
        .backgroundColor = THEME.bg_panel,
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .border = { .width = {1, 1, 1, 1}, .color = THEME.border }
    }) {
        if (g_app.route.loading) {
            CLAY_TEXT(CLAY_STRING("Calculating route..."),
                      CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text }));
        } else if (g_app.route.error) {
            CLAY_TEXT(CLAY_STRING("Route not found"),
                      CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = (Clay_Color){255, 100, 100, 255} }));
            if (cs_button(CS_ID("clear_route"), "Clear", &clear_btn).clicked) {
                g_app.route.has_start = false;
                g_app.route.has_end = false;
                g_app.route.point_count = 0;
                g_app.route.error = false;
            }
        } else if (g_app.route.point_count > 0) {
            /* Show route info */
            char dist_buf[32], time_buf[32];
            format_distance(dist_buf, sizeof(dist_buf), g_app.route.distance_m);
            format_duration(time_buf, sizeof(time_buf), g_app.route.duration_s);
            snprintf(g_app.scratch.route_info, sizeof(g_app.scratch.route_info),
                     "%s - %s", dist_buf, time_buf);

            CLAY_TEXT(((Clay_String){ .chars = g_app.scratch.route_info,
                                       .length = (int)strlen(g_app.scratch.route_info) }),
                      CLAY_TEXT_CONFIG({ .fontSize = 14, .textColor = THEME.text }));

            if (cs_button(CS_ID("clear_route"), "Clear Route", &clear_btn).clicked) {
                g_app.route.has_start = false;
                g_app.route.has_end = false;
                g_app.route.point_count = 0;
                cs_provider_route_clear();
            }
        } else if (!g_app.route.has_start) {
            CLAY_TEXT(CLAY_STRING("Click map to set start"),
                      CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text_muted }));
        } else if (!g_app.route.has_end) {
            CLAY_TEXT(CLAY_STRING("Click map to set end"),
                      CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text_muted }));
            if (cs_button(CS_ID("clear_route"), "Cancel", &clear_btn).clicked) {
                g_app.route.has_start = false;
            }
        }
    }
}

static void render_ui(void) {
    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED((float)g_app.map.width), CLAY_SIZING_FIXED((float)g_app.map.height) } }
    }) {
        /* Map with overlays - using begin/end pattern */
        cs_map_begin(g_app.map.component_id, &g_app.map.lat, &g_app.map.lon, &g_app.map.zoom,
                     (float)g_app.map.width, (float)g_app.map.height, NULL);

        /* Route polyline (only if we have route geometry) */
        if (g_app.route.point_count > 1) {
            cs_polyline(CS_ID("route"), g_app.route.points, g_app.route.point_count, &(CsPolylineStyle){
                .color = {0.2f, 0.5f, 1.0f, 0.9f},
                .width = 5.0f,
            });
        }

        /* Start marker (green) */
        if (g_app.route.has_start) {
            cs_marker(CS_ID("start"), g_app.route.start.lat, g_app.route.start.lon, &(CsMarkerStyle){
                .color = {0.2f, 0.8f, 0.3f, 1.0f},
                .radius = 12.0f,
                .border_color = {1.0f, 1.0f, 1.0f, 1.0f},
                .border_width = 3.0f,
            });
        }

        /* End marker (red) */
        if (g_app.route.has_end) {
            cs_marker(CS_ID("end"), g_app.route.end.lat, g_app.route.end.lon, &(CsMarkerStyle){
                .color = {0.9f, 0.2f, 0.2f, 1.0f},
                .radius = 12.0f,
                .border_color = {1.0f, 1.0f, 1.0f, 1.0f},
                .border_width = 3.0f,
            });
        }

        cs_map_end();

        /* UI overlays */
        render_info_panel();
        render_layer_panel();
        render_zoom_controls();
        render_tile_info();
        render_route_panel();
        render_attribution();
    }
}

/* ============================================================================
 * Domain Exports - Map State
 * ============================================================================ */

EXPORT void map_init(int width, int height) {
    g_app.map.width = width;
    g_app.map.height = height;
    g_app.map.component_id = CS_ID("map");

    CsClayConfig cfg = cs_clay_default_config();
    cs_clay_init(&cfg, width, height);
}

EXPORT void map_resize(int width, int height) {
    g_app.map.width = width;
    g_app.map.height = height;
    cs_clay_resize(width, height);
}

EXPORT void map_set_center(double lat, double lon) {
    g_app.map.lat = lat;
    g_app.map.lon = lon;
}

EXPORT void map_set_zoom(int zoom) {
    g_app.map.zoom = cs_map_scroll(zoom, 0, 0, 19);
}

EXPORT double map_get_lat(void) { return g_app.map.lat; }
EXPORT double map_get_lon(void) { return g_app.map.lon; }
EXPORT int map_get_zoom(void) { return g_app.map.zoom; }
EXPORT double map_get_visual_zoom(void) { return cs_map_get_visual_zoom(g_app.map.component_id); }
EXPORT int map_get_layer(void) { return g_app.panels.layer_type; }

/* ============================================================================
 * Domain Exports - Pointer Handling
 * ============================================================================ */

EXPORT void map_pointer_move(float x, float y) {
    bool dragging = cs_map_is_dragging(g_app.map.component_id);
    cs_clay_set_pointer(x, y, dragging);

    if (dragging) {
        double new_lat, new_lon;
        if (cs_map_pointer_move(g_app.map.component_id, g_app.map.zoom, x, y, &new_lat, &new_lon)) {
            g_app.map.lat = new_lat;
            g_app.map.lon = new_lon;
            if (g_app.map.lat > 85.0) g_app.map.lat = 85.0;
            if (g_app.map.lat < -85.0) g_app.map.lat = -85.0;
            while (g_app.map.lon > 180.0) g_app.map.lon -= 360.0;
            while (g_app.map.lon < -180.0) g_app.map.lon += 360.0;
        }
    }
}

EXPORT void map_pointer_down(float x, float y) {
    cs_map_pointer_down(g_app.map.component_id, g_app.map.lat, g_app.map.lon, x, y);
    cs_clay_set_pointer(x, y, true);
}

EXPORT void map_pointer_up(float x, float y) {
    cs_map_pointer_up(g_app.map.component_id, x, y);
    cs_clay_set_pointer(x, y, false);
}

EXPORT void map_scroll(float delta, float x, float y) {
    (void)x; (void)y;
    g_app.map.zoom = cs_map_scroll(g_app.map.zoom, delta > 0 ? 1 : -1, 0, 19);
}

EXPORT int map_handle_click(float x, float y) {
    bool on_ui = cs_clay_pointer_over("InfoPanel") ||
                 cs_clay_pointer_over("LayerPanel") ||
                 cs_clay_pointer_over("ZoomControls") ||
                 cs_clay_pointer_over("TileInfo") ||
                 cs_clay_pointer_over("RoutePanel") ||
                 cs_clay_pointer_over("Attribution");

    if (cs_focused_id() != 0 && !on_ui) {
        cs_blur();
    }

    /* Handle map click for routing */
    if (!on_ui && !g_app.route.loading) {
        /* Convert screen coords to geo coords using delta from center
         * Note: negate Y because screen Y increases downward but latitude increases upward */
        float cx = (float)g_app.map.width / 2.0f;
        float cy = (float)g_app.map.height / 2.0f;
        double dlat, dlon;
        cs_map_screen_to_geo_delta(g_app.map.lat, g_app.map.zoom, x - cx, -(y - cy), &dlat, &dlon);
        double click_lat = g_app.map.lat + dlat;
        double click_lon = g_app.map.lon + dlon;

        if (!g_app.route.has_start) {
            /* Set start point */
            g_app.route.start.lat = click_lat;
            g_app.route.start.lon = click_lon;
            g_app.route.has_start = true;
            g_app.route.error = false;
        } else if (!g_app.route.has_end) {
            /* Set end point and request route */
            g_app.route.end.lat = click_lat;
            g_app.route.end.lon = click_lon;
            g_app.route.has_end = true;
            g_app.route.loading = true;
            g_app.route.error = false;
            g_app.route.point_count = 0;

            /* Request route from provider */
            cs_provider_route(g_app.route.start, g_app.route.end);
        }
    }

    return on_ui ? 1 : 0;
}

/* ============================================================================
 * Domain Export - Frame
 * ============================================================================ */

/* Check for route completion from provider */
static void update_route_state(void) {
    if (!g_app.route.loading) return;

    if (cs_provider_route_ready()) {
        const CsRouteResult *result = cs_provider_route_result();

        if (result->error) {
            g_app.route.error = true;
            g_app.route.loading = false;
            g_app.route.point_count = 0;
        } else {
            /* Copy route geometry */
            int count = result->count;
            if (count > CS_PROVIDER_MAX_ROUTE_POINTS) {
                count = CS_PROVIDER_MAX_ROUTE_POINTS;
            }
            for (int i = 0; i < count; i++) {
                g_app.route.points[i] = result->points[i];
            }
            g_app.route.point_count = count;
            g_app.route.distance_m = result->distance_m;
            g_app.route.duration_s = result->duration_s;
            g_app.route.loading = false;
            g_app.route.error = false;
        }
    }
}

EXPORT int map_frame(float dt) {
    if (!cs_clay_is_initialized()) return -1;

    /* Update smooth zoom animation */
    cs_map_update_zoom_animation(g_app.map.component_id, g_app.map.zoom, dt);

    /* Check for route completion */
    update_route_state();

    cs_clay_begin_frame();
    render_ui();
    return cs_clay_end_frame(dt);
}
