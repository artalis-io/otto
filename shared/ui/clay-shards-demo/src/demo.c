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

typedef struct {
    char search[256];
    int search_len;
} UIText;

/* Scratch buffers for formatted strings */
typedef struct {
    char coord[64];
    char zoom[32];
    char tile[48];
} Scratch;

typedef struct {
    MapState map;
    UIPanels panels;
    UIText text;
    Scratch scratch;
} AppState;

static AppState g_app = {
    .map = { .lat = 47.4979, .lon = 19.0402, .zoom = 12, .width = 800, .height = 600 },
    .panels = { .show_tile_info = true, .layer_type = 0 },
    .text = { .search = "", .search_len = 0 },
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
    CLAY(CLAY_ID("ZoomControls"), {
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_RIGHT_TOP, .parent = CLAY_ATTACH_POINT_RIGHT_TOP },
            .offset = {-16, 160}
        },
        .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 2 }
    }) {
        if (cs_button(CS_ID("zoom_in"), "+", NULL).clicked) {
            g_app.map.zoom = cs_map_scroll(g_app.map.zoom, 1, 0, 19);
        }
        if (cs_button(CS_ID("zoom_out"), "-", NULL).clicked) {
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

        if (cs_button(CS_ID("layer_osm"), "OSM", g_app.panels.layer_type == 0 ? &sel : &def).clicked)
            g_app.panels.layer_type = 0;
        if (cs_button(CS_ID("layer_carto"), "Carto", g_app.panels.layer_type == 1 ? &sel : &def).clicked)
            g_app.panels.layer_type = 1;
        if (cs_button(CS_ID("layer_terrain"), "Terrain", g_app.panels.layer_type == 2 ? &sel : &def).clicked)
            g_app.panels.layer_type = 2;
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
        CLAY_TEXT(CLAY_STRING("Clay Map Viewer"), CLAY_TEXT_CONFIG({ .fontSize = 16, .textColor = THEME.text }));

        CLAY(CLAY_ID("CoordRow"), { .layout = { .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = 8 } }) {
            CLAY_TEXT(CLAY_STRING("Center:"), CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text_muted }));
            CLAY_TEXT(((Clay_String){ .chars = g_app.scratch.coord, .length = (int)strlen(g_app.scratch.coord) }),
                      CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text }));
        }

        CLAY_TEXT(((Clay_String){ .chars = g_app.scratch.zoom, .length = (int)strlen(g_app.scratch.zoom) }),
                  CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text }));

        const CsInputStyle input_style = { .width = 180, .height = 28, .font_size = 12, .padding = 8, .corner_radius = 4 };
        cs_input(CS_ID("search"), g_app.text.search, &g_app.text.search_len,
                 sizeof(g_app.text.search), "Search location...", &input_style);
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
        .layout = { .padding = CLAY_PADDING_ALL(8) },
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

static void render_ui(void) {
    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED((float)g_app.map.width), CLAY_SIZING_FIXED((float)g_app.map.height) } }
    }) {
        /* Map component - ID set once in map_init() */
        cs_map(g_app.map.component_id, &g_app.map.lat, &g_app.map.lon, &g_app.map.zoom,
               (float)g_app.map.width, (float)g_app.map.height, NULL);

        /* UI overlays */
        render_info_panel();
        render_layer_panel();
        render_zoom_controls();
        render_tile_info();
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
    (void)x; (void)y;

    bool on_ui = cs_clay_pointer_over("InfoPanel") ||
                 cs_clay_pointer_over("LayerPanel") ||
                 cs_clay_pointer_over("ZoomControls") ||
                 cs_clay_pointer_over("TileInfo") ||
                 cs_clay_pointer_over("Attribution");

    if (cs_focused_id() != 0 && !on_ui) {
        cs_blur();
    }

    return on_ui ? 1 : 0;
}

/* ============================================================================
 * Domain Export - Frame
 * ============================================================================ */

EXPORT int map_frame(float dt) {
    if (!cs_clay_is_initialized()) return -1;

    cs_clay_begin_frame();
    render_ui();
    return cs_clay_end_frame(dt);
}
