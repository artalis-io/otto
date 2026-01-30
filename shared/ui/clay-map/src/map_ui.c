/**
 * Clay Map UI - WASM Map Viewer with Clay UI Layout
 *
 * This demonstrates using Clay for UI layout in a Leaflet-like map viewer.
 * Clay handles the UI controls overlay, WebGL handles tile rendering.
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

/* ============================================================================
 * Map State
 * ============================================================================ */

typedef struct {
    double lat;
    double lon;
    int zoom;
    double view_lat;      /* Current view center latitude */
    double view_lon;      /* Current view center longitude */
    float view_zoom;      /* Current zoom (can be fractional during animation) */
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

/* Text Input State */
#define TEXT_INPUT_MAX_LEN 256

typedef struct {
    char text[TEXT_INPUT_MAX_LEN];
    int length;
    int cursor;
    int selection_start;  /* -1 = no selection */
    bool focused;
    float cursor_blink;
    bool cursor_visible;
} TextInputState;

/* UI State */
typedef struct {
    bool show_controls;
    bool show_tile_info;
    int layer_type;       /* 0 = OSM, 1 = Carto Light, 2 = Stamen Terrain */
    char status_text[128];
    char tile_info_text[64];
    TextInputState search_input;
} UIState;

static UIState g_ui = {
    .show_controls = true,
    .show_tile_info = true,
    .layer_type = 0,
    .status_text = "Ready",
    .tile_info_text = "",
    .search_input = {
        .text = "",
        .length = 0,
        .cursor = 0,
        .selection_start = -1,
        .focused = false,
        .cursor_blink = 0.0f,
        .cursor_visible = true,
    },
};

/* Clay memory arena - Clay_MinMemorySize() returns ~5MB with our config */
static uint8_t g_clay_memory[6 * 1024 * 1024];  /* 6MB for Clay */
static bool g_clay_initialized = false;

/* ============================================================================
 * Web Mercator Projection
 * ============================================================================ */

#define PI 3.14159265358979323846
#define DEG_TO_RAD (PI / 180.0)
#define RAD_TO_DEG (180.0 / PI)

static double lon_to_tile_x(double lon, int zoom) {
    return (lon + 180.0) / 360.0 * (1 << zoom);
}

static double lat_to_tile_y(double lat, int zoom) {
    double lat_rad = lat * DEG_TO_RAD;
    return (1.0 - log(tan(lat_rad) + 1.0/cos(lat_rad)) / PI) / 2.0 * (1 << zoom);
}

/* Inverse projection - used by JS, kept here for reference */
static double __attribute__((unused)) tile_x_to_lon(double x, int zoom) {
    return x / (1 << zoom) * 360.0 - 180.0;
}

static double __attribute__((unused)) tile_y_to_lat(double y, int zoom) {
    double n = PI - 2.0 * PI * y / (1 << zoom);
    return RAD_TO_DEG * atan(0.5 * (exp(n) - exp(-n)));
}

/* ============================================================================
 * Text Measurement (callback for Clay)
 * ============================================================================ */

/* Simple fixed-width font measurement for WASM */
static Clay_Dimensions measure_text(
    Clay_StringSlice text,
    Clay_TextElementConfig *config,
    void *userData
) {
    (void)userData;
    /* Approximate character dimensions based on font size */
    float char_width = config->fontSize * 0.6f;
    float char_height = config->fontSize;
    return (Clay_Dimensions){
        .width = text.length * char_width,
        .height = char_height
    };
}

/* Scroll offset query - required by Clay */
static Clay_Vector2 query_scroll_offset(uint32_t elementId, void *userData) {
    (void)elementId;
    (void)userData;
    return (Clay_Vector2){0, 0};
}

/* ============================================================================
 * Clay Error Handler
 * ============================================================================ */

static void handle_clay_error(Clay_ErrorData error) {
    snprintf(g_ui.status_text, sizeof(g_ui.status_text),
             "Clay Error: %.*s", error.errorText.length, error.errorText.chars);
}

/* ============================================================================
 * UI Components
 * ============================================================================ */

/* Colors */
static const Clay_Color COLOR_BG_DARK = {40, 40, 40, 230};
/* Unused for now but available for light theme */
static const Clay_Color COLOR_BG_LIGHT __attribute__((unused)) = {255, 255, 255, 240};
static const Clay_Color COLOR_ACCENT = {66, 133, 244, 255};
static const Clay_Color COLOR_TEXT_LIGHT = {255, 255, 255, 255};
static const Clay_Color COLOR_TEXT_DARK = {40, 40, 40, 255};
static const Clay_Color COLOR_BORDER = {100, 100, 100, 255};

/* Helper to create Clay_String from C string */
static Clay_String make_string(const char *str) {
    return (Clay_String){
        .isStaticallyAllocated = false,
        .length = (int32_t)strlen(str),
        .chars = str
    };
}

/* Button component - uses string literal labels */
static void render_button_osm(Clay_ElementId id, bool selected) {
    Clay_Color bg = selected ? COLOR_ACCENT : (Clay_Color){80, 80, 80, 255};
    if (Clay_Hovered()) {
        bg = selected ? (Clay_Color){86, 153, 255, 255} : (Clay_Color){100, 100, 100, 255};
    }

    CLAY(id, {
        .layout = {
            .padding = CLAY_PADDING_ALL(8),
            .sizing = { .width = CLAY_SIZING_FIT(60, 200) }
        },
        .backgroundColor = bg,
        .cornerRadius = CLAY_CORNER_RADIUS(4)
    }) {
        CLAY_TEXT(CLAY_STRING("OSM"), CLAY_TEXT_CONFIG({
            .fontSize = 14,
            .textColor = COLOR_TEXT_LIGHT
        }));
    }
}

static void render_button_carto(Clay_ElementId id, bool selected) {
    Clay_Color bg = selected ? COLOR_ACCENT : (Clay_Color){80, 80, 80, 255};
    if (Clay_Hovered()) {
        bg = selected ? (Clay_Color){86, 153, 255, 255} : (Clay_Color){100, 100, 100, 255};
    }

    CLAY(id, {
        .layout = {
            .padding = CLAY_PADDING_ALL(8),
            .sizing = { .width = CLAY_SIZING_FIT(60, 200) }
        },
        .backgroundColor = bg,
        .cornerRadius = CLAY_CORNER_RADIUS(4)
    }) {
        CLAY_TEXT(CLAY_STRING("Carto"), CLAY_TEXT_CONFIG({
            .fontSize = 14,
            .textColor = COLOR_TEXT_LIGHT
        }));
    }
}

static void render_button_terrain(Clay_ElementId id, bool selected) {
    Clay_Color bg = selected ? COLOR_ACCENT : (Clay_Color){80, 80, 80, 255};
    if (Clay_Hovered()) {
        bg = selected ? (Clay_Color){86, 153, 255, 255} : (Clay_Color){100, 100, 100, 255};
    }

    CLAY(id, {
        .layout = {
            .padding = CLAY_PADDING_ALL(8),
            .sizing = { .width = CLAY_SIZING_FIT(60, 200) }
        },
        .backgroundColor = bg,
        .cornerRadius = CLAY_CORNER_RADIUS(4)
    }) {
        CLAY_TEXT(CLAY_STRING("Terrain"), CLAY_TEXT_CONFIG({
            .fontSize = 14,
            .textColor = COLOR_TEXT_LIGHT
        }));
    }
}

/* Zoom control buttons */
static void render_zoom_controls(void) {
    CLAY(CLAY_ID("ZoomControls"), {
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = {
                .element = CLAY_ATTACH_POINT_RIGHT_TOP,
                .parent = CLAY_ATTACH_POINT_RIGHT_TOP
            },
            .offset = {-16, 160}  /* Move down to avoid layer panel */
        },
        .layout = {
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .childGap = 2
        }
    }) {
        /* Zoom In */
        CLAY(CLAY_ID("ZoomIn"), {
            .layout = {
                .padding = CLAY_PADDING_ALL(8),
                .sizing = {
                    .width = CLAY_SIZING_FIXED(36),
                    .height = CLAY_SIZING_FIXED(36)
                },
                .childAlignment = {
                    .x = CLAY_ALIGN_X_CENTER,
                    .y = CLAY_ALIGN_Y_CENTER
                }
            },
            .backgroundColor = Clay_Hovered() ? (Clay_Color){60, 60, 60, 240} : COLOR_BG_DARK,
            .cornerRadius = CLAY_CORNER_RADIUS(4),
            .border = {
                .width = {1, 1, 1, 1},
                .color = COLOR_BORDER
            }
        }) {
            CLAY_TEXT(CLAY_STRING("+"), CLAY_TEXT_CONFIG({
                .fontSize = 20,
                .textColor = COLOR_TEXT_LIGHT
            }));
        }

        /* Zoom Out */
        CLAY(CLAY_ID("ZoomOut"), {
            .layout = {
                .padding = CLAY_PADDING_ALL(8),
                .sizing = {
                    .width = CLAY_SIZING_FIXED(36),
                    .height = CLAY_SIZING_FIXED(36)
                },
                .childAlignment = {
                    .x = CLAY_ALIGN_X_CENTER,
                    .y = CLAY_ALIGN_Y_CENTER
                }
            },
            .backgroundColor = Clay_Hovered() ? (Clay_Color){60, 60, 60, 240} : COLOR_BG_DARK,
            .cornerRadius = CLAY_CORNER_RADIUS(4),
            .border = {
                .width = {1, 1, 1, 1},
                .color = COLOR_BORDER
            }
        }) {
            CLAY_TEXT(CLAY_STRING("-"), CLAY_TEXT_CONFIG({
                .fontSize = 20,
                .textColor = COLOR_TEXT_LIGHT
            }));
        }
    }
}

/* Layer selector panel */
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

        render_button_osm(CLAY_ID("LayerOSM"), g_ui.layer_type == 0);
        render_button_carto(CLAY_ID("LayerCarto"), g_ui.layer_type == 1);
        render_button_terrain(CLAY_ID("LayerTerrain"), g_ui.layer_type == 2);
    }
}

/* Static buffers for dynamic text (persisted across frames) */
static char g_coord_text[64];
static char g_zoom_text[32];
static char g_tile_text[48];
static char g_search_display[TEXT_INPUT_MAX_LEN + 2];  /* Extra space for cursor */

/* Render a text input field */
static void render_search_input(void) {
    TextInputState *input = &g_ui.search_input;

    /* Prepare display text with cursor indicator for focused state */
    if (input->length == 0 && !input->focused) {
        strcpy(g_search_display, "Search location...");
    } else {
        strncpy(g_search_display, input->text, sizeof(g_search_display) - 1);
        g_search_display[input->length] = '\0';
    }

    Clay_Color bg = input->focused ? (Clay_Color){60, 60, 60, 255} : (Clay_Color){50, 50, 50, 255};
    Clay_Color border = input->focused ? COLOR_ACCENT : COLOR_BORDER;
    Clay_Color text_color = (input->length == 0 && !input->focused)
        ? (Clay_Color){120, 120, 120, 255}
        : COLOR_TEXT_LIGHT;

    CLAY(CLAY_ID("SearchInput"), {
        .layout = {
            .sizing = {
                .width = CLAY_SIZING_FIXED(180),
                .height = CLAY_SIZING_FIXED(28)
            },
            .padding = { .left = 8, .right = 8, .top = 4, .bottom = 4 },
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
        },
        .backgroundColor = bg,
        .cornerRadius = CLAY_CORNER_RADIUS(4),
        .border = {
            .width = {1, 1, 1, 1},
            .color = border
        }
    }) {
        CLAY_TEXT(make_string(g_search_display), CLAY_TEXT_CONFIG({
            .fontSize = 12,
            .textColor = text_color
        }));
    }
}

/* Info panel showing coordinates */
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
            CLAY_TEXT(make_string(g_coord_text), CLAY_TEXT_CONFIG({
                .fontSize = 12,
                .textColor = COLOR_TEXT_LIGHT
            }));
        }

        CLAY(CLAY_ID("ZoomRow"), {
            .layout = {
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childGap = 8
            }
        }) {
            CLAY_TEXT(make_string(g_zoom_text), CLAY_TEXT_CONFIG({
                .fontSize = 12,
                .textColor = COLOR_TEXT_LIGHT
            }));
        }

        /* Search input */
        render_search_input();
    }
}

/* Tile info overlay */
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
        CLAY_TEXT(make_string(g_tile_text), CLAY_TEXT_CONFIG({
            .fontSize = 12,
            .textColor = COLOR_TEXT_LIGHT
        }));
    }
}

/* Attribution */
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

/* Forward declarations for text input functions */
EXPORT void map_search_focus(void);
EXPORT void map_search_blur(void);

/* ============================================================================
 * Main Render Function
 * ============================================================================ */

static void render_ui(void) {
    /* Root container (invisible, just for layout) */
    CLAY(CLAY_ID("Root"), {
        .layout = {
            .sizing = {
                .width = CLAY_SIZING_FIXED((float)g_map.width),
                .height = CLAY_SIZING_FIXED((float)g_map.height)
            }
        }
    }) {
        /* All floating UI elements */
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

/**
 * Initialize Clay and map state
 */
EXPORT void map_init(int width, int height) {
    g_map.width = width;
    g_map.height = height;

    /* Initialize Clay */
    uint32_t min_mem = Clay_MinMemorySize();
    uint32_t mem_size = sizeof(g_clay_memory);
    if (mem_size < min_mem) {
        /* Not enough memory - this shouldn't happen with 4MB */
        snprintf(g_ui.status_text, sizeof(g_ui.status_text),
                 "Error: need %u bytes, have %u", min_mem, mem_size);
        return;
    }

    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(
        mem_size,
        g_clay_memory
    );

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

/**
 * Update viewport size
 */
EXPORT void map_resize(int width, int height) {
    g_map.width = width;
    g_map.height = height;
    Clay_SetLayoutDimensions((Clay_Dimensions){(float)width, (float)height});
}

/**
 * Set map center
 */
EXPORT void map_set_center(double lat, double lon) {
    g_map.lat = lat;
    g_map.lon = lon;
    g_map.view_lat = lat;
    g_map.view_lon = lon;
}

/**
 * Set zoom level
 */
EXPORT void map_set_zoom(int zoom) {
    if (zoom < 0) zoom = 0;
    if (zoom > 19) zoom = 19;
    g_map.zoom = zoom;
    g_map.view_zoom = (float)zoom;
}

/**
 * Get current map state (for JS tile rendering)
 */
EXPORT double map_get_lat(void) { return g_map.view_lat; }
EXPORT double map_get_lon(void) { return g_map.view_lon; }
EXPORT int map_get_zoom(void) { return g_map.zoom; }
EXPORT int map_get_layer(void) { return g_ui.layer_type; }

/**
 * Handle mouse/pointer input
 */
EXPORT void map_pointer_move(float x, float y) {
    Clay_SetPointerState((Clay_Vector2){x, y}, g_map.dragging);

    if (g_map.dragging) {
        /* Calculate movement in degrees */
        double meters_per_pixel = 156543.03392 * cos(g_map.view_lat * DEG_TO_RAD) / (1 << g_map.zoom);
        double dx = (g_map.drag_start_x - x) * meters_per_pixel / 111320.0;  /* lon degrees */
        double dy = (y - g_map.drag_start_y) * meters_per_pixel / 110540.0;  /* lat degrees */

        g_map.view_lon = g_map.drag_start_lon + dx;
        g_map.view_lat = g_map.drag_start_lat + dy;

        /* Clamp latitude */
        if (g_map.view_lat > 85.0) g_map.view_lat = 85.0;
        if (g_map.view_lat < -85.0) g_map.view_lat = -85.0;

        /* Wrap longitude */
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

    /* Update canonical position */
    g_map.lat = g_map.view_lat;
    g_map.lon = g_map.view_lon;
}

/**
 * Handle scroll/zoom
 */
EXPORT void map_scroll(float delta, float x, float y) {
    (void)x; (void)y;  /* TODO: Zoom towards cursor */

    int new_zoom = g_map.zoom;
    if (delta > 0) {
        new_zoom++;
    } else if (delta < 0) {
        new_zoom--;
    }

    map_set_zoom(new_zoom);
}

/**
 * Check if UI element was clicked and handle it
 * Returns: 1 if click was handled by UI, 0 if it should go to map
 */
EXPORT int map_handle_click(float x, float y) {
    (void)x; (void)y;

    /* Check search input */
    if (Clay_PointerOver(CLAY_ID("SearchInput"))) {
        map_search_focus();
        return 1;
    } else if (g_ui.search_input.focused) {
        /* Click outside search input - blur it */
        map_search_blur();
    }

    /* Check zoom buttons */
    if (Clay_PointerOver(CLAY_ID("ZoomIn"))) {
        map_set_zoom(g_map.zoom + 1);
        return 1;
    }
    if (Clay_PointerOver(CLAY_ID("ZoomOut"))) {
        map_set_zoom(g_map.zoom - 1);
        return 1;
    }

    /* Check layer buttons */
    if (Clay_PointerOver(CLAY_ID("LayerOSM"))) {
        g_ui.layer_type = 0;
        return 1;
    }
    if (Clay_PointerOver(CLAY_ID("LayerCarto"))) {
        g_ui.layer_type = 1;
        return 1;
    }
    if (Clay_PointerOver(CLAY_ID("LayerTerrain"))) {
        g_ui.layer_type = 2;
        return 1;
    }

    return 0;
}

/* ============================================================================
 * Render Command Access - for JS interop
 * ============================================================================ */

static Clay_RenderCommandArray g_commands;

/**
 * Run layout and store render commands
 * Returns the number of commands
 */
EXPORT int map_frame(void) {
    if (!g_clay_initialized) {
        return -1;  /* Return -1 to indicate not initialized */
    }
    Clay_BeginLayout();
    render_ui();
    g_commands = Clay_EndLayout();
    return g_commands.length;
}

/* Debug: check if Clay is initialized */
EXPORT int map_is_initialized(void) {
    return g_clay_initialized ? 1 : 0;
}

/* Debug: get min memory size */
EXPORT uint32_t map_debug_min_mem(void) {
    return Clay_MinMemorySize();
}

/* Debug: get our memory size */
EXPORT uint32_t map_debug_our_mem(void) {
    return sizeof(g_clay_memory);
}

/**
 * Get command type at index
 */
EXPORT int map_cmd_type(int index) {
    if (index < 0 || index >= g_commands.length) return -1;
    return (int)g_commands.internalArray[index].commandType;
}

/**
 * Get bounding box at index
 */
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

/**
 * Get rectangle color (returns packed RGBA as uint32)
 */
EXPORT uint32_t map_cmd_rect_color(int index) {
    if (index < 0 || index >= g_commands.length) return 0;
    Clay_Color c = g_commands.internalArray[index].renderData.rectangle.backgroundColor;
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) | ((uint32_t)c.b << 8) | (uint32_t)c.a;
}

EXPORT float map_cmd_rect_radius(int index) {
    if (index < 0 || index >= g_commands.length) return 0;
    return g_commands.internalArray[index].renderData.rectangle.cornerRadius.topLeft;
}

/**
 * Get text data
 */
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

/**
 * Get border data
 */
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
 * Text Input Exports - for JS keyboard handling
 * ============================================================================ */

/**
 * Focus the search input
 */
EXPORT void map_search_focus(void) {
    g_ui.search_input.focused = true;
    g_ui.search_input.cursor_visible = true;
    g_ui.search_input.cursor_blink = 0.0f;
}

/**
 * Blur the search input
 */
EXPORT void map_search_blur(void) {
    g_ui.search_input.focused = false;
    g_ui.search_input.selection_start = -1;
}

/**
 * Check if search input is focused
 */
EXPORT int map_search_is_focused(void) {
    return g_ui.search_input.focused ? 1 : 0;
}

/**
 * Get search input text
 */
EXPORT const char* map_search_get_text(void) {
    return g_ui.search_input.text;
}

/**
 * Get search input text length
 */
EXPORT int map_search_get_length(void) {
    return g_ui.search_input.length;
}

/**
 * Get cursor position
 */
EXPORT int map_search_get_cursor(void) {
    return g_ui.search_input.cursor;
}

/**
 * Get selection start (-1 if no selection)
 */
EXPORT int map_search_get_selection(void) {
    return g_ui.search_input.selection_start;
}

/**
 * Check if cursor should be visible (for blinking)
 */
EXPORT int map_search_cursor_visible(void) {
    return g_ui.search_input.cursor_visible ? 1 : 0;
}

/**
 * Update cursor blink timer
 */
EXPORT void map_search_update(float dt) {
    if (!g_ui.search_input.focused) return;

    g_ui.search_input.cursor_blink += dt;
    if (g_ui.search_input.cursor_blink >= 0.5f) {
        g_ui.search_input.cursor_blink = 0.0f;
        g_ui.search_input.cursor_visible = !g_ui.search_input.cursor_visible;
    }
}

/**
 * Insert a character at cursor
 */
EXPORT void map_search_insert_char(int char_code) {
    TextInputState *input = &g_ui.search_input;
    if (!input->focused) return;
    if (char_code < 32 || char_code > 126) return;
    if (input->length >= TEXT_INPUT_MAX_LEN - 1) return;

    /* Delete selection if any */
    if (input->selection_start >= 0 && input->selection_start != input->cursor) {
        int start = input->selection_start < input->cursor ? input->selection_start : input->cursor;
        int end = input->selection_start < input->cursor ? input->cursor : input->selection_start;
        memmove(&input->text[start], &input->text[end], input->length - end + 1);
        input->length -= (end - start);
        input->cursor = start;
        input->selection_start = -1;
    }

    /* Insert character */
    memmove(&input->text[input->cursor + 1], &input->text[input->cursor],
            input->length - input->cursor + 1);
    input->text[input->cursor] = (char)char_code;
    input->cursor++;
    input->length++;

    input->cursor_visible = true;
    input->cursor_blink = 0.0f;
}

/**
 * Handle special keys
 * Returns 1 if handled
 */
/**
 * Get search input bounding box (for cursor rendering in JS)
 */
EXPORT float map_search_get_x(void) {
    Clay_BoundingBox box = Clay_GetElementData(CLAY_ID("SearchInput")).boundingBox;
    return box.x;
}

EXPORT float map_search_get_y(void) {
    Clay_BoundingBox box = Clay_GetElementData(CLAY_ID("SearchInput")).boundingBox;
    return box.y;
}

EXPORT float map_search_get_width(void) {
    Clay_BoundingBox box = Clay_GetElementData(CLAY_ID("SearchInput")).boundingBox;
    return box.width;
}

EXPORT float map_search_get_height(void) {
    Clay_BoundingBox box = Clay_GetElementData(CLAY_ID("SearchInput")).boundingBox;
    return box.height;
}

EXPORT int map_search_key_down(int key_code, int shift, int ctrl) {
    TextInputState *input = &g_ui.search_input;
    if (!input->focused) return 0;

    input->cursor_visible = true;
    input->cursor_blink = 0.0f;

    int has_sel = input->selection_start >= 0 && input->selection_start != input->cursor;

    switch (key_code) {
        case 37:  /* Left arrow */
            if (has_sel && !shift) {
                input->cursor = input->selection_start < input->cursor
                    ? input->selection_start : input->cursor;
                input->selection_start = -1;
            } else if (input->cursor > 0) {
                if (shift && input->selection_start < 0) {
                    input->selection_start = input->cursor;
                }
                input->cursor--;
            }
            if (!shift) input->selection_start = -1;
            return 1;

        case 39:  /* Right arrow */
            if (has_sel && !shift) {
                input->cursor = input->selection_start > input->cursor
                    ? input->selection_start : input->cursor;
                input->selection_start = -1;
            } else if (input->cursor < input->length) {
                if (shift && input->selection_start < 0) {
                    input->selection_start = input->cursor;
                }
                input->cursor++;
            }
            if (!shift) input->selection_start = -1;
            return 1;

        case 36:  /* Home */
            if (shift && input->selection_start < 0) {
                input->selection_start = input->cursor;
            }
            input->cursor = 0;
            if (!shift) input->selection_start = -1;
            return 1;

        case 35:  /* End */
            if (shift && input->selection_start < 0) {
                input->selection_start = input->cursor;
            }
            input->cursor = input->length;
            if (!shift) input->selection_start = -1;
            return 1;

        case 8:   /* Backspace */
            if (has_sel) {
                int start = input->selection_start < input->cursor ? input->selection_start : input->cursor;
                int end = input->selection_start < input->cursor ? input->cursor : input->selection_start;
                memmove(&input->text[start], &input->text[end], input->length - end + 1);
                input->length -= (end - start);
                input->cursor = start;
                input->selection_start = -1;
            } else if (input->cursor > 0) {
                memmove(&input->text[input->cursor - 1], &input->text[input->cursor],
                        input->length - input->cursor + 1);
                input->cursor--;
                input->length--;
            }
            return 1;

        case 46:  /* Delete */
            if (has_sel) {
                int start = input->selection_start < input->cursor ? input->selection_start : input->cursor;
                int end = input->selection_start < input->cursor ? input->cursor : input->selection_start;
                memmove(&input->text[start], &input->text[end], input->length - end + 1);
                input->length -= (end - start);
                input->cursor = start;
                input->selection_start = -1;
            } else if (input->cursor < input->length) {
                memmove(&input->text[input->cursor], &input->text[input->cursor + 1],
                        input->length - input->cursor);
                input->length--;
            }
            return 1;

        case 65:  /* A - select all with Ctrl */
            if (ctrl) {
                input->selection_start = 0;
                input->cursor = input->length;
                return 1;
            }
            break;

        case 27:  /* Escape */
            map_search_blur();
            return 1;
    }

    return 0;
}
