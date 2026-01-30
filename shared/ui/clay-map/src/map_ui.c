/**
 * Clay Map UI - WASM Map Viewer with Clay UI Layout
 *
 * Demonstrates using Clay for UI layout with immediate mode components.
 * Clay handles layout, WebGL handles rendering.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

#define CLAY_IMPLEMENTATION
#include "clay.h"

/* Immediate mode components */
#include "cc_immediate.h"

/* ============================================================================
 * Map State
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
} MapState;

static MapState g_map = {
    .lat = 47.4979,       /* Budapest */
    .lon = 19.0402,
    .zoom = 12,
    .view_lat = 47.4979,
    .view_lon = 19.0402,
    .view_zoom = 12.0f,
    .width = 800,
    .height = 600,
    .dragging = false,
};

/* UI State - you own the buffers! */
typedef struct {
    bool show_controls;
    bool show_tile_info;
    int layer_type;
    char status_text[128];

    /* Your text input buffers */
    char search_text[256];
    int search_len;
} UIState;

static UIState g_ui = {
    .show_controls = true,
    .show_tile_info = true,
    .layer_type = 0,
    .status_text = "Ready",
    .search_text = "",
    .search_len = 0,
};

/* Clay memory */
static uint8_t g_clay_memory[6 * 1024 * 1024];
static bool g_clay_initialized = false;

/* ============================================================================
 * Web Mercator Projection
 * ============================================================================ */

#define PI 3.14159265358979323846
#define DEG_TO_RAD (PI / 180.0)

static double lon_to_tile_x(double lon, int zoom) {
    return (lon + 180.0) / 360.0 * (1 << zoom);
}

static double lat_to_tile_y(double lat, int zoom) {
    double lat_rad = lat * DEG_TO_RAD;
    return (1.0 - log(tan(lat_rad) + 1.0/cos(lat_rad)) / PI) / 2.0 * (1 << zoom);
}

/* ============================================================================
 * Clay Callbacks
 * ============================================================================ */

static Clay_Dimensions measure_text(
    Clay_StringSlice text,
    Clay_TextElementConfig *config,
    void *userData
) {
    (void)userData;
    float char_width = config->fontSize * 0.6f;
    float char_height = config->fontSize;
    return (Clay_Dimensions){
        .width = text.length * char_width,
        .height = char_height
    };
}

static Clay_Vector2 query_scroll_offset(uint32_t elementId, void *userData) {
    (void)elementId;
    (void)userData;
    return (Clay_Vector2){0, 0};
}

static void handle_clay_error(Clay_ErrorData error) {
    snprintf(g_ui.status_text, sizeof(g_ui.status_text),
             "Clay Error: %.*s", error.errorText.length, error.errorText.chars);
}

/* ============================================================================
 * UI Rendering with Immediate Mode Components
 * ============================================================================ */

static const Clay_Color COLOR_BG_DARK = {40, 40, 40, 230};
static const Clay_Color COLOR_TEXT_LIGHT = {255, 255, 255, 255};
static const Clay_Color COLOR_TEXT_DARK = {40, 40, 40, 255};
static const Clay_Color COLOR_BORDER = {100, 100, 100, 255};

/* Static buffers for dynamic text */
static char g_coord_text[64];
static char g_zoom_text[32];
static char g_tile_text[48];

static void render_zoom_controls(void) {
    CLAY(CLAY_ID("ZoomControls"), {
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = {
                .element = CLAY_ATTACH_POINT_RIGHT_TOP,
                .parent = CLAY_ATTACH_POINT_RIGHT_TOP
            },
            .offset = {-16, 160}
        },
        .layout = {
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .childGap = 2
        }
    }) {
        /* Zoom In */
        if (cc_button(CC_ID("zoom_in"), "+", NULL).clicked) {
            if (g_map.zoom < 19) {
                g_map.zoom++;
                g_map.view_zoom = (float)g_map.zoom;
            }
        }

        /* Zoom Out */
        if (cc_button(CC_ID("zoom_out"), "-", NULL).clicked) {
            if (g_map.zoom > 0) {
                g_map.zoom--;
                g_map.view_zoom = (float)g_map.zoom;
            }
        }
    }
}

static void render_layer_panel(void) {
    CLAY(CLAY_ID("LayerPanel"), {
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = {
                .element = CLAY_ATTACH_POINT_RIGHT_TOP,
                .parent = CLAY_ATTACH_POINT_RIGHT_TOP
            },
            .offset = {-16, 16}
        },
        .layout = {
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = CLAY_PADDING_ALL(8),
            .childGap = 4
        },
        .backgroundColor = COLOR_BG_DARK,
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .border = {
            .width = {1, 1, 1, 1},
            .color = COLOR_BORDER
        }
    }) {
        CLAY_TEXT(CLAY_STRING("Layers"), CLAY_TEXT_CONFIG({
            .fontSize = 12,
            .textColor = (Clay_Color){180, 180, 180, 255}
        }));

        /* Layer buttons - use different styles based on selection */
        CcButtonStyle selected = {CC_BTN_PRIMARY, 14, 8, 8, 4};
        CcButtonStyle normal = {CC_BTN_DEFAULT, 14, 8, 8, 4};

        if (cc_button(CC_ID("layer_osm"), "OSM",
            g_ui.layer_type == 0 ? &selected : &normal).clicked) {
            g_ui.layer_type = 0;
        }

        if (cc_button(CC_ID("layer_carto"), "Carto",
            g_ui.layer_type == 1 ? &selected : &normal).clicked) {
            g_ui.layer_type = 1;
        }

        if (cc_button(CC_ID("layer_terrain"), "Terrain",
            g_ui.layer_type == 2 ? &selected : &normal).clicked) {
            g_ui.layer_type = 2;
        }
    }
}

static void render_info_panel(void) {
    snprintf(g_coord_text, sizeof(g_coord_text), "%.4f, %.4f", g_map.view_lat, g_map.view_lon);
    snprintf(g_zoom_text, sizeof(g_zoom_text), "Zoom: %d", g_map.zoom);

    CLAY(CLAY_ID("InfoPanel"), {
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = {
                .element = CLAY_ATTACH_POINT_LEFT_TOP,
                .parent = CLAY_ATTACH_POINT_LEFT_TOP
            },
            .offset = {16, 16}
        },
        .layout = {
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = CLAY_PADDING_ALL(12),
            .childGap = 8
        },
        .backgroundColor = COLOR_BG_DARK,
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .border = {
            .width = {1, 1, 1, 1},
            .color = COLOR_BORDER
        }
    }) {
        CLAY_TEXT(CLAY_STRING("Clay Map Viewer"), CLAY_TEXT_CONFIG({
            .fontSize = 16,
            .textColor = COLOR_TEXT_LIGHT
        }));

        CLAY(CLAY_ID("CoordRow"), {
            .layout = {
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childGap = 8
            }
        }) {
            CLAY_TEXT(CLAY_STRING("Center:"), CLAY_TEXT_CONFIG({
                .fontSize = 12,
                .textColor = (Clay_Color){180, 180, 180, 255}
            }));
            Clay_String coord_str = {.chars = g_coord_text, .length = (int)strlen(g_coord_text)};
            CLAY_TEXT(coord_str, CLAY_TEXT_CONFIG({
                .fontSize = 12,
                .textColor = COLOR_TEXT_LIGHT
            }));
        }

        Clay_String zoom_str = {.chars = g_zoom_text, .length = (int)strlen(g_zoom_text)};
        CLAY_TEXT(zoom_str, CLAY_TEXT_CONFIG({
            .fontSize = 12,
            .textColor = COLOR_TEXT_LIGHT
        }));

        /* Search input - immediate mode! */
        CcInputStyle input_style = {
            .width = 180,
            .height = 28,
            .font_size = 12,
            .padding = 8,
            .corner_radius = 4
        };

        CcInputResult r = cc_input(
            CC_ID("search"),
            g_ui.search_text,
            &g_ui.search_len,
            sizeof(g_ui.search_text),
            "Search location...",
            &input_style
        );

        if (r.submitted) {
            /* TODO: Actually search for location */
            snprintf(g_ui.status_text, sizeof(g_ui.status_text),
                     "Search: %s", g_ui.search_text);
        }
    }
}

static void render_tile_info(void) {
    if (!g_ui.show_tile_info) return;

    int tile_x = (int)lon_to_tile_x(g_map.view_lon, g_map.zoom);
    int tile_y = (int)lat_to_tile_y(g_map.view_lat, g_map.zoom);

    snprintf(g_tile_text, sizeof(g_tile_text), "%d/%d/%d", g_map.zoom, tile_x, tile_y);

    CLAY(CLAY_ID("TileInfo"), {
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = {
                .element = CLAY_ATTACH_POINT_LEFT_BOTTOM,
                .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM
            },
            .offset = {16, -16}
        },
        .layout = {
            .padding = CLAY_PADDING_ALL(8)
        },
        .backgroundColor = (Clay_Color){0, 0, 0, 180},
        .cornerRadius = CLAY_CORNER_RADIUS(4)
    }) {
        Clay_String tile_str = {.chars = g_tile_text, .length = (int)strlen(g_tile_text)};
        CLAY_TEXT(tile_str, CLAY_TEXT_CONFIG({
            .fontSize = 12,
            .textColor = COLOR_TEXT_LIGHT
        }));
    }
}

static void render_attribution(void) {
    CLAY(CLAY_ID("Attribution"), {
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = {
                .element = CLAY_ATTACH_POINT_RIGHT_BOTTOM,
                .parent = CLAY_ATTACH_POINT_RIGHT_BOTTOM
            },
            .offset = {-8, -8}
        },
        .layout = {
            .padding = CLAY_PADDING_ALL(4)
        },
        .backgroundColor = (Clay_Color){255, 255, 255, 200},
        .cornerRadius = CLAY_CORNER_RADIUS(2)
    }) {
        CLAY_TEXT(CLAY_STRING("OpenStreetMap contributors"), CLAY_TEXT_CONFIG({
            .fontSize = 10,
            .textColor = COLOR_TEXT_DARK
        }));
    }
}

static void render_ui(void) {
    CLAY(CLAY_ID("Root"), {
        .layout = {
            .sizing = {
                .width = CLAY_SIZING_FIXED((float)g_map.width),
                .height = CLAY_SIZING_FIXED((float)g_map.height)
            }
        }
    }) {
        render_info_panel();
        render_layer_panel();
        render_zoom_controls();
        render_tile_info();
        render_attribution();
    }
}

/* ============================================================================
 * Exported WASM Functions
 * ============================================================================ */

EXPORT void map_init(int width, int height) {
    g_map.width = width;
    g_map.height = height;

    /* Initialize immediate mode components */
    cc_init();

    /* Initialize Clay */
    uint32_t min_mem = Clay_MinMemorySize();
    uint32_t mem_size = sizeof(g_clay_memory);
    if (mem_size < min_mem) {
        snprintf(g_ui.status_text, sizeof(g_ui.status_text),
                 "Error: need %u bytes, have %u", min_mem, mem_size);
        return;
    }

    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(mem_size, g_clay_memory);

    Clay_Initialize(
        arena,
        (Clay_Dimensions){(float)width, (float)height},
        (Clay_ErrorHandler){handle_clay_error, NULL}
    );

    Clay_SetMeasureTextFunction(measure_text, NULL);
    Clay_SetQueryScrollOffsetFunction(query_scroll_offset, NULL);

    g_clay_initialized = true;
    snprintf(g_ui.status_text, sizeof(g_ui.status_text), "Initialized %dx%d", width, height);
}

EXPORT void map_resize(int width, int height) {
    g_map.width = width;
    g_map.height = height;
    Clay_SetLayoutDimensions((Clay_Dimensions){(float)width, (float)height});
}

EXPORT void map_set_center(double lat, double lon) {
    g_map.lat = lat;
    g_map.lon = lon;
    g_map.view_lat = lat;
    g_map.view_lon = lon;
}

EXPORT void map_set_zoom(int zoom) {
    if (zoom < 0) zoom = 0;
    if (zoom > 19) zoom = 19;
    g_map.zoom = zoom;
    g_map.view_zoom = (float)zoom;
}

/* Map state getters */
EXPORT double map_get_lat(void) { return g_map.view_lat; }
EXPORT double map_get_lon(void) { return g_map.view_lon; }
EXPORT int map_get_zoom(void) { return g_map.zoom; }
EXPORT int map_get_layer(void) { return g_ui.layer_type; }

/* Pointer handling */
EXPORT void map_pointer_move(float x, float y) {
    Clay_SetPointerState((Clay_Vector2){x, y}, g_map.dragging);

    if (g_map.dragging) {
        double meters_per_pixel = 156543.03392 * cos(g_map.view_lat * DEG_TO_RAD) / (1 << g_map.zoom);
        double dx = (g_map.drag_start_x - x) * meters_per_pixel / 111320.0;
        double dy = (y - g_map.drag_start_y) * meters_per_pixel / 110540.0;

        g_map.view_lon = g_map.drag_start_lon + dx;
        g_map.view_lat = g_map.drag_start_lat + dy;

        if (g_map.view_lat > 85.0) g_map.view_lat = 85.0;
        if (g_map.view_lat < -85.0) g_map.view_lat = -85.0;
        while (g_map.view_lon > 180.0) g_map.view_lon -= 360.0;
        while (g_map.view_lon < -180.0) g_map.view_lon += 360.0;
    }
}

EXPORT void map_pointer_down(float x, float y) {
    g_map.dragging = true;
    g_map.drag_start_x = x;
    g_map.drag_start_y = y;
    g_map.drag_start_lat = g_map.view_lat;
    g_map.drag_start_lon = g_map.view_lon;
    Clay_SetPointerState((Clay_Vector2){x, y}, true);
}

EXPORT void map_pointer_up(float x, float y) {
    g_map.dragging = false;
    Clay_SetPointerState((Clay_Vector2){x, y}, false);
    g_map.lat = g_map.view_lat;
    g_map.lon = g_map.view_lon;
}

EXPORT void map_scroll(float delta, float x, float y) {
    (void)x; (void)y;
    int new_zoom = g_map.zoom + (delta > 0 ? 1 : -1);
    map_set_zoom(new_zoom);
}

/**
 * Handle click - returns 1 if consumed by UI
 */
EXPORT int map_handle_click(float x, float y) {
    (void)x; (void)y;

    /* Check if clicking on any UI element */
    bool on_ui = Clay_PointerOver(CLAY_ID("InfoPanel")) ||
                 Clay_PointerOver(CLAY_ID("LayerPanel")) ||
                 Clay_PointerOver(CLAY_ID("ZoomControls")) ||
                 Clay_PointerOver(CLAY_ID("TileInfo")) ||
                 Clay_PointerOver(CLAY_ID("Attribution"));

    /* Check if clicking outside focused element to blur */
    uint32_t focused = cc_focused_id();
    if (focused != 0 && !on_ui) {
        cc_blur();
    }

    /* Return 1 if click should be consumed by UI (prevents map drag) */
    return on_ui ? 1 : 0;
}

/* ============================================================================
 * Render Commands
 * ============================================================================ */

static Clay_RenderCommandArray g_commands;

EXPORT int map_frame(float dt) {
    if (!g_clay_initialized) return -1;

    cc_frame_begin();

    Clay_BeginLayout();
    render_ui();
    g_commands = Clay_EndLayout();

    cc_frame_end(dt);

    return g_commands.length;
}

/* Command accessors */
EXPORT int map_cmd_type(int index) {
    if (index < 0 || index >= g_commands.length) return -1;
    return (int)g_commands.internalArray[index].commandType;
}

EXPORT float map_cmd_x(int index) {
    if (index < 0 || index >= g_commands.length) return 0;
    return g_commands.internalArray[index].boundingBox.x;
}

EXPORT float map_cmd_y(int index) {
    if (index < 0 || index >= g_commands.length) return 0;
    return g_commands.internalArray[index].boundingBox.y;
}

EXPORT float map_cmd_w(int index) {
    if (index < 0 || index >= g_commands.length) return 0;
    return g_commands.internalArray[index].boundingBox.width;
}

EXPORT float map_cmd_h(int index) {
    if (index < 0 || index >= g_commands.length) return 0;
    return g_commands.internalArray[index].boundingBox.height;
}

EXPORT uint32_t map_cmd_rect_color(int index) {
    if (index < 0 || index >= g_commands.length) return 0;
    Clay_Color c = g_commands.internalArray[index].renderData.rectangle.backgroundColor;
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) | ((uint32_t)c.b << 8) | (uint32_t)c.a;
}

EXPORT float map_cmd_rect_radius(int index) {
    if (index < 0 || index >= g_commands.length) return 0;
    return g_commands.internalArray[index].renderData.rectangle.cornerRadius.topLeft;
}

EXPORT const char* map_cmd_text_str(int index) {
    if (index < 0 || index >= g_commands.length) return "";
    return g_commands.internalArray[index].renderData.text.stringContents.chars;
}

EXPORT int map_cmd_text_len(int index) {
    if (index < 0 || index >= g_commands.length) return 0;
    return g_commands.internalArray[index].renderData.text.stringContents.length;
}

EXPORT uint32_t map_cmd_text_color(int index) {
    if (index < 0 || index >= g_commands.length) return 0;
    Clay_Color c = g_commands.internalArray[index].renderData.text.textColor;
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) | ((uint32_t)c.b << 8) | (uint32_t)c.a;
}

EXPORT int map_cmd_text_size(int index) {
    if (index < 0 || index >= g_commands.length) return 0;
    return g_commands.internalArray[index].renderData.text.fontSize;
}

EXPORT uint32_t map_cmd_border_color(int index) {
    if (index < 0 || index >= g_commands.length) return 0;
    Clay_Color c = g_commands.internalArray[index].renderData.border.color;
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) | ((uint32_t)c.b << 8) | (uint32_t)c.a;
}

EXPORT float map_cmd_border_radius(int index) {
    if (index < 0 || index >= g_commands.length) return 0;
    return g_commands.internalArray[index].renderData.border.cornerRadius.topLeft;
}

EXPORT int map_cmd_border_width(int index) {
    if (index < 0 || index >= g_commands.length) return 0;
    return g_commands.internalArray[index].renderData.border.width.left;
}

/* ============================================================================
 * Component State Exports (generic - not per-component!)
 * ============================================================================ */

EXPORT uint32_t cc_get_focused_id(void) {
    return cc_focused_id();
}

EXPORT int cc_get_cursor_pos(void) {
    return cc_cursor_pos();
}

EXPORT int cc_get_selection_start(void) {
    return cc_selection_start();
}

EXPORT int cc_is_cursor_visible(void) {
    return cc_cursor_visible() ? 1 : 0;
}

EXPORT int cc_get_focused_bounds(float *out) {
    return cc_focused_bounds(&out[0], &out[1], &out[2], &out[3]) ? 1 : 0;
}

EXPORT int cc_handle_key_down(int key, int shift, int ctrl) {
    return cc_key_down(key, shift != 0, ctrl != 0) ? 1 : 0;
}

EXPORT int cc_handle_key_char(int char_code) {
    return cc_key_char((uint32_t)char_code) ? 1 : 0;
}

EXPORT void cc_do_blur(void) {
    cc_blur();
}

EXPORT void cc_set_click(void) {
    cc_set_pending_click();
}

/* Get focused input text for cursor rendering */
EXPORT const char* cc_get_focused_text(void) {
    uint32_t focused = cc_focused_id();
    if (focused == CC_ID("search")) {
        return g_ui.search_text;
    }
    return "";
}

EXPORT int cc_get_focused_text_len(void) {
    uint32_t focused = cc_focused_id();
    if (focused == CC_ID("search")) {
        return g_ui.search_len;
    }
    return 0;
}
