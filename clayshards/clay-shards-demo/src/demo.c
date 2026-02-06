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
#include <math.h>

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
    bool smooth_zoom;
    int layer_type;
    float route_line_width;
    int route_profile_idx;  /* 0=Car, 1=Truck */
} UIPanels;

/* Which endpoint we're reverse geocoding */
typedef enum {
    REVERSE_NONE = 0,
    REVERSE_START,
    REVERSE_END
} ReversePending;

/* Mock turn-by-turn instruction (placeholder until Velo provides real instructions) */
#define MAX_INSTRUCTIONS 32
#define INSTRUCTION_TEXT_LEN 64

typedef struct {
    char text[INSTRUCTION_TEXT_LEN];
    double distance_m;           /* Distance for this segment */
} RouteInstruction;

/* Route state for click-to-route */
typedef struct {
    bool has_start;
    bool has_end;
    CsGeoPoint start;
    CsGeoPoint end;
    /* Route geometry - pointer to provider's buffer (no copy needed) */
    const CsGeoPoint *points;
    int point_count;
    double distance_m;
    double duration_s;
    double calc_time_ms;        /* Server calculation time */
    bool loading;
    bool error;
    /* Drag-to-reroute debouncing */
    float reroute_cooldown;      /* Time until next reroute allowed (seconds) */
    bool dragging_marker;        /* Currently dragging a route marker */
    /* Cached display values (prevent flicker during recalculation) */
    char cached_info[96];        /* Cached formatted string */
    bool has_cached_info;
    /* Reverse geocoded addresses */
    char start_address[128];
    char end_address[128];
    ReversePending reverse_pending;
    bool start_needs_geocode;    /* Start marker moved, needs new address */
    bool end_needs_geocode;      /* End marker moved, needs new address */
    /* Mock turn-by-turn instructions */
    RouteInstruction instructions[MAX_INSTRUCTIONS];
    int instruction_count;
} RouteState;

/* Minimum time between route requests during drag (seconds) */
#define DRAG_REROUTE_INTERVAL 0.4f

/* Max zoom per layer (layer_type: 0=Carta, 1=OSM) */
#define LAYER_CARTA 0
#define LAYER_OSM   1
#define MAX_ZOOM_CARTA 18
#define MAX_ZOOM_OSM   19

static int get_max_zoom(int layer_type) {
    return (layer_type == LAYER_CARTA) ? MAX_ZOOM_CARTA : MAX_ZOOM_OSM;
}

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

/* Scrollbar drag state */
typedef struct {
    bool dragging;              /* Currently dragging scrollbar thumb */
    uint32_t scroll_id;         /* ID of scroll container being dragged */
    float drag_start_y;         /* Mouse Y when drag started */
    float scroll_start_y;       /* Scroll position when drag started */
    float track_height;         /* Height of scrollbar track */
    float content_height;       /* Total content height */
    float view_height;          /* Visible view height */
    /* Track bounds for hit testing (set during render) */
    float track_x, track_y, track_w, track_h;
    bool track_visible;         /* Whether scrollbar is currently visible */
} ScrollbarDrag;

typedef struct {
    MapState map;
    UIPanels panels;
    UIText text;
    Scratch scratch;
    RouteState route;
    SearchState search;
    ScrollbarDrag scrollbar;
    bool attribution_clicked;  /* Set when attribution link is clicked */
} AppState;

static AppState g_app = {
    .map = { .lat = 47.4979, .lon = 19.0402, .zoom = 12, .width = 800, .height = 600 },
    .panels = { .show_tile_info = true, .smooth_zoom = true, .layer_type = 0, .route_line_width = 5.0f, .route_profile_idx = 0 },
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

static void render_layer_panel(void) {
    /* Single floating container for layers, settings, and zoom controls */
    CLAY(CLAY_ID("LayerPanel"), {
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_RIGHT_TOP, .parent = CLAY_ATTACH_POINT_RIGHT_TOP },
            .offset = {-16, 16}
        },
        .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = CLAY_PADDING_ALL(8), .childGap = 6 },
        .backgroundColor = THEME.bg_panel,
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .border = { .width = {1, 1, 1, 1, 0}, .color = THEME.border }
    }) {
        CLAY_TEXT(CLAY_STRING("Layers"), CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text_muted }));

        /* Layer buttons in same row */
        CLAY(CLAY_ID("LayerButtons"), {
            .layout = { .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = 4 }
        }) {
            const CsButtonStyle sel = {CS_BTN_PRIMARY, 14, 8, 8, 4, 70};
            const CsButtonStyle def = {CS_BTN_DEFAULT, 14, 8, 8, 4, 70};

            if (cs_button(CS_ID("layer_carta"), "Carta", g_app.panels.layer_type == LAYER_CARTA ? &sel : &def).clicked) {
                g_app.panels.layer_type = LAYER_CARTA;
                /* Clamp zoom to Carta's max if needed */
                if (g_app.map.zoom > MAX_ZOOM_CARTA) {
                    g_app.map.zoom = MAX_ZOOM_CARTA;
                }
            }
            if (cs_button(CS_ID("layer_osm"), "OSM", g_app.panels.layer_type == LAYER_OSM ? &sel : &def).clicked)
                g_app.panels.layer_type = LAYER_OSM;
        }

        /* Divider */
        CLAY(CLAY_ID("LayerDivider"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) } },
            .backgroundColor = THEME.border
        }) {}

        CLAY_TEXT(CLAY_STRING("Settings"), CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text_muted }));

        /* Checkbox and toggle in same row */
        CLAY(CLAY_ID("SettingsRow"), {
            .layout = { .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = 12, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } }
        }) {
            const CsCheckboxStyle check_style = { .size = 16, .font_size = 11, .corner_radius = 3, .gap = 6 };
            cs_checkbox(CS_ID("show_tile_info"), &g_app.panels.show_tile_info, "Tile info", &check_style);

            const CsToggleStyle toggle_style = { .width = 36, .height = 18, .font_size = 11, .gap = 6 };
            cs_toggle(CS_ID("smooth_zoom"), &g_app.panels.smooth_zoom, "Smooth", &toggle_style);
        }

        /* Divider before zoom */
        CLAY(CLAY_ID("ZoomDivider"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) } },
            .backgroundColor = THEME.border
        }) {}

        /* Zoom controls */
        CLAY_TEXT(CLAY_STRING("Zoom"), CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text_muted }));

        CLAY(CLAY_ID("ZoomButtons"), {
            .layout = { .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = 4 }
        }) {
            const CsButtonStyle zoom_btn = {
                .variant = CS_BTN_DEFAULT,
                .font_size = 16,
                .corner_radius = 4,
                .width = 32,
                .height = 32,
                .text_offset_y = -2
            };
            if (cs_button(CS_ID("zoom_out"), "-", &zoom_btn).clicked) {
                g_app.map.zoom = cs_map_scroll(g_app.map.zoom, -1, 0, get_max_zoom(g_app.panels.layer_type));
            }
            if (cs_button(CS_ID("zoom_in"), "+", &zoom_btn).clicked) {
                g_app.map.zoom = cs_map_scroll(g_app.map.zoom, 1, 0, get_max_zoom(g_app.panels.layer_type));
            }
        }
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
        .border = { .width = {1, 1, 1, 1, 0}, .color = THEME.border }
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
            g_app.search.prev_query[sizeof(g_app.search.prev_query) - 1] = '\0';
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
    /* Check hover state from previous frame */
    bool attr_hovered = cs_clay_pointer_over("Attribution");

    /* Link colors: blue when hovered, dark gray otherwise */
    Clay_Color link_color = attr_hovered
        ? (Clay_Color){0, 102, 204, 255}    /* Blue on hover */
        : (Clay_Color){64, 64, 64, 255};    /* Dark gray */

    CLAY(CLAY_ID("Attribution"), {
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_RIGHT_BOTTOM, .parent = CLAY_ATTACH_POINT_RIGHT_BOTTOM },
            .offset = {-8, -8}
        },
        .layout = { .padding = CLAY_PADDING_ALL(4) },
        .backgroundColor = (Clay_Color){255, 255, 255, 220},
        .cornerRadius = CLAY_CORNER_RADIUS(2)
    }) {
        /* Text with underline on hover */
        CLAY(CLAY_ID("AttrLinkWrapper"), {
            .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 2 }
        }) {
            /* Use ASCII (c) since MSDF font may not have copyright symbol */
            CLAY_TEXT(CLAY_STRING("(c) OpenStreetMap contributors"),
                      CLAY_TEXT_CONFIG({ .fontSize = 10, .textColor = link_color }));

            /* Underline - always present to maintain size, transparent when not hovered */
            CLAY(CLAY_ID("AttrUnderline"), {
                .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(1) } },
                .backgroundColor = attr_hovered ? link_color : (Clay_Color){0, 0, 0, 0}
            }) {}
        }
    }

    /* Note: Click handling is done in map_handle_click via is_pointer_over_ui */
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

/* Calculate bearing between two points (degrees, 0 = north, clockwise) */
static double calc_bearing(double lat1, double lon1, double lat2, double lon2) {
    double dlon = (lon2 - lon1) * 0.0174533;  /* deg to rad */
    double lat1_rad = lat1 * 0.0174533;
    double lat2_rad = lat2 * 0.0174533;

    double x = sin(dlon) * cos(lat2_rad);
    double y = cos(lat1_rad) * sin(lat2_rad) - sin(lat1_rad) * cos(lat2_rad) * cos(dlon);

    double bearing = atan2(x, y) * 57.2958;  /* rad to deg */
    if (bearing < 0) bearing += 360.0;
    return bearing;
}

/* Haversine distance in meters */
static double calc_distance(double lat1, double lon1, double lat2, double lon2) {
    double dlat = (lat2 - lat1) * 0.0174533;
    double dlon = (lon2 - lon1) * 0.0174533;
    double lat1_rad = lat1 * 0.0174533;
    double lat2_rad = lat2 * 0.0174533;

    double a = sin(dlat/2) * sin(dlat/2) +
               cos(lat1_rad) * cos(lat2_rad) * sin(dlon/2) * sin(dlon/2);
    double c = 2 * atan2(sqrt(a), sqrt(1-a));
    return 6371000.0 * c;  /* Earth radius in meters */
}

/* Generate mock turn-by-turn instructions from route geometry */
static void generate_mock_instructions(RouteState *route) {
    route->instruction_count = 0;

    if (route->point_count < 2) return;

    const CsGeoPoint *pts = route->points;
    int n = route->point_count;
    int idx = 0;

    /* Start instruction */
    if (idx < MAX_INSTRUCTIONS) {
        snprintf(route->instructions[idx].text, INSTRUCTION_TEXT_LEN,
                 "Depart from origin");
        route->instructions[idx].distance_m = 0;
        idx++;
    }

    /* Process route - sample more frequently for more instructions */
    int step = n > 200 ? n / 25 : (n > 50 ? 2 : 1);
    double prev_bearing = -1;
    double accumulated_dist = 0;
    double last_instruction_dist = 0;

    for (int i = 1; i < n && idx < MAX_INSTRUCTIONS - 1; i += step) {
        int prev_i = (i - step >= 0) ? i - step : 0;

        /* Calculate distance for this segment */
        double seg_dist = 0;
        for (int j = prev_i; j < i && j < n - 1; j++) {
            seg_dist += calc_distance(pts[j].lat, pts[j].lon, pts[j+1].lat, pts[j+1].lon);
        }
        accumulated_dist += seg_dist;

        /* Calculate bearing */
        double bearing = calc_bearing(pts[prev_i].lat, pts[prev_i].lon, pts[i].lat, pts[i].lon);

        /* Determine turn type based on bearing change */
        if (prev_bearing >= 0) {
            double turn = bearing - prev_bearing;
            if (turn > 180) turn -= 360;
            if (turn < -180) turn += 360;

            const char *turn_type = NULL;
            if (turn > 70) turn_type = "Sharp right";
            else if (turn < -70) turn_type = "Sharp left";
            else if (turn > 35) turn_type = "Turn right";
            else if (turn < -35) turn_type = "Turn left";
            else if (turn > 15) turn_type = "Bear right";
            else if (turn < -15) turn_type = "Bear left";

            /* Add turn instruction if significant turn and enough distance */
            if (turn_type && accumulated_dist > 50) {
                char dist_str[32];
                format_distance(dist_str, sizeof(dist_str), accumulated_dist);
                snprintf(route->instructions[idx].text, INSTRUCTION_TEXT_LEN,
                         "%s in %s", turn_type, dist_str);
                route->instructions[idx].distance_m = accumulated_dist;
                idx++;
                last_instruction_dist = 0;
                accumulated_dist = 0;
            }
            /* Add "Continue straight" for long straight sections */
            else if (accumulated_dist > 500 && last_instruction_dist > 400) {
                char dist_str[32];
                format_distance(dist_str, sizeof(dist_str), accumulated_dist);
                snprintf(route->instructions[idx].text, INSTRUCTION_TEXT_LEN,
                         "Continue straight for %s", dist_str);
                route->instructions[idx].distance_m = accumulated_dist;
                idx++;
                last_instruction_dist = 0;
                accumulated_dist = 0;
            }
        }

        last_instruction_dist += seg_dist;
        prev_bearing = bearing;
    }

    /* Continue instruction for remaining distance */
    if (accumulated_dist > 30 && idx < MAX_INSTRUCTIONS - 1) {
        char dist_str[32];
        format_distance(dist_str, sizeof(dist_str), accumulated_dist);
        snprintf(route->instructions[idx].text, INSTRUCTION_TEXT_LEN,
                 "Continue for %s", dist_str);
        route->instructions[idx].distance_m = accumulated_dist;
        idx++;
    }

    /* Arrival instruction */
    if (idx < MAX_INSTRUCTIONS) {
        snprintf(route->instructions[idx].text, INSTRUCTION_TEXT_LEN,
                 "Arrive at destination");
        route->instructions[idx].distance_m = 0;
        idx++;
    }

    route->instruction_count = idx;
}

/* Route profile options for dropdown */
static const char *ROUTE_PROFILES[] = {"Car", "Truck"};

static void render_route_panel(void) {
    const CsButtonStyle small_btn = {
        .variant = CS_BTN_DEFAULT,
        .font_size = 11,
        .corner_radius = 4,
        .padding_x = 6,
        .padding_y = 3,
    };
    const CsButtonStyle small_btn_active = {
        .variant = CS_BTN_PRIMARY,
        .font_size = 11,
        .corner_radius = 4,
        .padding_x = 6,
        .padding_y = 3,
    };
    const CsButtonStyle clear_btn = {
        .variant = CS_BTN_DEFAULT,
        .font_size = 12,
        .corner_radius = 4,
        .padding_x = 8,
        .padding_y = 4,
    };

    /* Move route panel closer to bottom when tile info is hidden */
    float route_panel_offset_y = g_app.panels.show_tile_info ? -60.0f : -16.0f;

    CLAY(CLAY_ID("RoutePanel"), {
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_BOTTOM, .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM },
            .offset = {16, route_panel_offset_y}
        },
        .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = CLAY_PADDING_ALL(10), .childGap = 6 },
        .backgroundColor = THEME.bg_panel,
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .border = { .width = {1, 1, 1, 1, 0}, .color = THEME.border }
    }) {
        /* Profile dropdown and mode selectors row */
        CLAY(CLAY_ID("RouteOptions"), {
            .layout = { .childGap = 4, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } }
        }) {
            /* Profile selector dropdown: Car / Truck */
            const CsDropdownStyle profile_dd_style = {
                .width = 80,
                .height = 26,
                .font_size = 11,
                .corner_radius = 4,
            };
            CsDropdownResult profile_result = cs_dropdown(
                CS_ID("profile_dropdown"),
                &g_app.panels.route_profile_idx,
                ROUTE_PROFILES, 2,
                &profile_dd_style
            );
            if (profile_result.changed) {
                /* Update provider profile */
                cs_provider_set_route_profile(g_app.panels.route_profile_idx == 0 ? CS_PROFILE_CAR : CS_PROFILE_TRUCK);
                /* Trigger reroute if we have both endpoints */
                if (g_app.route.has_start && g_app.route.has_end) {
                    cs_provider_route(g_app.route.start, g_app.route.end);
                    g_app.route.loading = true;
                }
            }

            /* Spacer */
            CLAY(CLAY_ID("RouteSpacer"), { .layout = { .sizing = { CLAY_SIZING_FIXED(8), CLAY_SIZING_FIXED(1) } } }) {}

            /* Mode selector: Fastest / Shortest */
            CsRouteMode mode = cs_provider_get_route_mode();
            if (cs_button(CS_ID("mode_fastest"), "Fastest",
                          mode == CS_MODE_FASTEST ? &small_btn_active : &small_btn).clicked) {
                cs_provider_set_route_mode(CS_MODE_FASTEST);
                if (g_app.route.has_start && g_app.route.has_end) {
                    cs_provider_route(g_app.route.start, g_app.route.end);
                    g_app.route.loading = true;
                }
            }
            if (cs_button(CS_ID("mode_shortest"), "Shortest",
                          mode == CS_MODE_SHORTEST ? &small_btn_active : &small_btn).clicked) {
                cs_provider_set_route_mode(CS_MODE_SHORTEST);
                if (g_app.route.has_start && g_app.route.has_end) {
                    cs_provider_route(g_app.route.start, g_app.route.end);
                    g_app.route.loading = true;
                }
            }
        }

        /* Route line width slider */
        const CsSliderStyle line_width_style = {
            .width = 120,
            .height = 6,
            .thumb_size = 14,
            .font_size = 11,
            .corner_radius = 3,
            .step = 1.0f,
            .show_value = true,
        };
        cs_slider(CS_ID("line_width"), &g_app.panels.route_line_width, 1.0f, 10.0f, "Width", &line_width_style);

        /* Route status/info - use consistent structure to prevent flicker */
        bool has_route = g_app.route.point_count > 0 || g_app.route.has_cached_info;

        if (g_app.route.error) {
            /* Error state */
            CLAY_TEXT(CLAY_STRING("Route not found"),
                      CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = (Clay_Color){255, 100, 100, 255} }));
            if (cs_button(CS_ID("clear_route"), "Clear", &clear_btn).clicked) {
                g_app.route.has_start = false;
                g_app.route.has_end = false;
                g_app.route.point_count = 0; g_app.route.points = NULL;
                g_app.route.error = false;
                g_app.route.has_cached_info = false;
                g_app.route.instruction_count = 0;
            }
        } else if (has_route || g_app.route.loading) {
            /* Route info - always show cached info during loading to prevent flicker */
            if (g_app.route.point_count > 0 && !g_app.route.loading) {
                /* Update cache with new data */
                char dist_buf[32], time_buf[32];
                format_distance(dist_buf, sizeof(dist_buf), g_app.route.distance_m);
                format_duration(time_buf, sizeof(time_buf), g_app.route.duration_s);
                snprintf(g_app.route.cached_info, sizeof(g_app.route.cached_info),
                         "%s - %s (%.0fms)", dist_buf, time_buf, g_app.route.calc_time_ms);
                g_app.route.has_cached_info = true;
            }

            /* Show route info (cached or current) */
            if (g_app.route.has_cached_info) {
                Clay_Color info_color = g_app.route.loading ? THEME.text_muted : THEME.text;
                CLAY_TEXT(((Clay_String){ .chars = g_app.route.cached_info,
                                           .length = (int)strlen(g_app.route.cached_info) }),
                          CLAY_TEXT_CONFIG({ .fontSize = 14, .textColor = info_color }));
            } else {
                CLAY_TEXT(CLAY_STRING("Calculating route..."),
                          CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text_muted }));
            }

            /* Always show address rows (empty if no address yet) - prevents height changes */
            CLAY(CLAY_ID("FromAddr"), { .layout = { .childGap = 4, .sizing = { .height = CLAY_SIZING_FIXED(16) } } }) {
                if (g_app.route.start_address[0] != '\0') {
                    CLAY_TEXT(CLAY_STRING("From:"),
                              CLAY_TEXT_CONFIG({ .fontSize = 11, .textColor = THEME.text_muted }));
                    CLAY_TEXT(((Clay_String){ .chars = g_app.route.start_address,
                                               .length = (int)strlen(g_app.route.start_address) }),
                              CLAY_TEXT_CONFIG({ .fontSize = 11, .textColor = THEME.text }));
                }
            }
            CLAY(CLAY_ID("ToAddr"), { .layout = { .childGap = 4, .sizing = { .height = CLAY_SIZING_FIXED(16) } } }) {
                if (g_app.route.end_address[0] != '\0') {
                    CLAY_TEXT(CLAY_STRING("To:"),
                              CLAY_TEXT_CONFIG({ .fontSize = 11, .textColor = THEME.text_muted }));
                    CLAY_TEXT(((Clay_String){ .chars = g_app.route.end_address,
                                               .length = (int)strlen(g_app.route.end_address) }),
                              CLAY_TEXT_CONFIG({ .fontSize = 11, .textColor = THEME.text }));
                }
            }

            /* Scrollable turn-by-turn instructions panel */
            if (g_app.route.instruction_count > 0 && !g_app.route.loading) {
                CLAY(CLAY_ID("InstructionsDivider"), {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) } },
                    .backgroundColor = THEME.border
                }) {}

                CLAY_TEXT(CLAY_STRING("Turn-by-turn"),
                          CLAY_TEXT_CONFIG({ .fontSize = 11, .textColor = THEME.text_muted }));

                /* Container for scroll content + scrollbar */
                CLAY(CLAY_ID("InstructionsContainer"), {
                    .layout = { .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = 4 }
                }) {
                    const CsScrollStyle scroll_style = {
                        .width = 200,
                        .vertical = true,
                        .corner_radius = 4,
                    };
                    CS_SCROLL(CS_ID("instructions"), 120.0f, &scroll_style) {
                        CLAY(CLAY_ID("InstructionsList"), {
                            .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 4, .padding = CLAY_PADDING_ALL(4) }
                        }) {
                            for (int i = 0; i < g_app.route.instruction_count; i++) {
                                CLAY_TEXT(((Clay_String){
                                    .chars = g_app.route.instructions[i].text,
                                    .length = (int)strlen(g_app.route.instructions[i].text)
                                }), CLAY_TEXT_CONFIG({ .fontSize = 11, .textColor = THEME.text }));
                            }
                        }
                    }

                    /* Custom scrollbar */
                    CsScrollInfo scroll_info = cs_scroll_info(CS_ID("instructions"));
                    if (scroll_info.found && scroll_info.content_height > scroll_info.view_height) {
                        float track_height = 120.0f;
                        float content_ratio = scroll_info.view_height / scroll_info.content_height;
                        float thumb_height = track_height * content_ratio;
                        if (thumb_height < 20.0f) thumb_height = 20.0f;  /* Minimum thumb size */

                        float max_scroll = scroll_info.content_height - scroll_info.view_height;
                        float scroll_ratio = (max_scroll > 0) ? (scroll_info.scroll_y / max_scroll) : 0;
                        float thumb_offset = scroll_ratio * (track_height - thumb_height);

                        /* Scrollbar track */
                        CLAY(CLAY_ID("ScrollTrack"), {
                            .layout = { .sizing = { CLAY_SIZING_FIXED(6), CLAY_SIZING_FIXED(track_height) } },
                            .backgroundColor = (Clay_Color){60, 60, 60, 200},
                            .cornerRadius = CLAY_CORNER_RADIUS(3)
                        }) {
                            /* Scrollbar thumb */
                            CLAY(CLAY_ID("ScrollThumb"), {
                                .floating = {
                                    .attachTo = CLAY_ATTACH_TO_PARENT,
                                    .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_TOP },
                                    .offset = {0, thumb_offset}
                                },
                                .layout = { .sizing = { CLAY_SIZING_FIXED(6), CLAY_SIZING_FIXED(thumb_height) } },
                                .backgroundColor = scroll_info.hovered
                                    ? (Clay_Color){150, 150, 150, 255}
                                    : (Clay_Color){100, 100, 100, 255},
                                .cornerRadius = CLAY_CORNER_RADIUS(3)
                            }) {}
                        }

                        /* Register hit targets for unified hit testing (uses previous frame's bounds) */
                        uint32_t scroll_id = CS_ID("instructions");
                        Clay_BoundingBox track_box = Clay_GetElementData(CLAY_ID("ScrollTrack")).boundingBox;
                        Clay_BoundingBox thumb_box = Clay_GetElementData(CLAY_ID("ScrollThumb")).boundingBox;
                        cs_scroll_register_track(scroll_id, track_box.x, track_box.y, track_box.width, track_box.height);
                        cs_scroll_register_thumb(scroll_id, thumb_box.x, thumb_box.y, thumb_box.width, thumb_box.height);

                        /* Store dimensions for drag handling */
                        g_app.scrollbar.track_visible = true;
                        g_app.scrollbar.content_height = scroll_info.content_height;
                        g_app.scrollbar.view_height = scroll_info.view_height;
                        g_app.scrollbar.track_height = track_height;
                        g_app.scrollbar.track_x = track_box.x;
                        g_app.scrollbar.track_y = track_box.y;
                        g_app.scrollbar.track_w = track_box.width;
                        g_app.scrollbar.track_h = track_box.height;
                    } else {
                        g_app.scrollbar.track_visible = false;
                    }
                }
            }

            if (cs_button(CS_ID("clear_route"), "Clear Route", &clear_btn).clicked) {
                g_app.route.has_start = false;
                g_app.route.has_end = false;
                g_app.route.point_count = 0; g_app.route.points = NULL;
                g_app.route.has_cached_info = false;
                g_app.route.start_address[0] = '\0';
                g_app.route.end_address[0] = '\0';
                g_app.route.instruction_count = 0;
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
        CsMapResult map_result = cs_map_begin(g_app.map.component_id, &g_app.map.lat, &g_app.map.lon, &g_app.map.zoom,
                     (float)g_app.map.width, (float)g_app.map.height, NULL);

        /* Route polyline (only if we have route geometry) */
        if (g_app.route.point_count > 1) {
            cs_polyline(CS_ID("route"), g_app.route.points, g_app.route.point_count, &(CsPolylineStyle){
                .color = {0.2f, 0.5f, 1.0f, 0.9f},
                .width = g_app.panels.route_line_width,
            });
        }

        /* Determine marker positions - use drag position during active drag */
        double start_lat = g_app.route.start.lat;
        double start_lon = g_app.route.start.lon;
        double end_lat = g_app.route.end.lat;
        double end_lon = g_app.route.end.lon;

        /* During active drag, use the drag position for the dragged marker */
        if (map_result.dragged_marker_id == CS_ID("start")) {
            start_lat = map_result.dragged_marker_lat;
            start_lon = map_result.dragged_marker_lon;
        } else if (map_result.dragged_marker_id == CS_ID("end")) {
            end_lat = map_result.dragged_marker_lat;
            end_lon = map_result.dragged_marker_lon;
        }

        /* Start marker (green, draggable) */
        if (g_app.route.has_start) {
            cs_marker(CS_ID("start"), start_lat, start_lon, &(CsMarkerStyle){
                .color = {0.2f, 0.8f, 0.3f, 1.0f},
                .radius = 12.0f,
                .border_color = {1.0f, 1.0f, 1.0f, 1.0f},
                .border_width = 3.0f,
                .draggable = true,
            });
        }

        /* End marker (red, draggable) */
        if (g_app.route.has_end) {
            cs_marker(CS_ID("end"), end_lat, end_lon, &(CsMarkerStyle){
                .color = {0.9f, 0.2f, 0.2f, 1.0f},
                .radius = 12.0f,
                .border_color = {1.0f, 1.0f, 1.0f, 1.0f},
                .border_width = 3.0f,
                .draggable = true,
            });
        }

        cs_map_end();

        /* Track active marker drag state */
        bool is_dragging_route_marker = (map_result.dragged_marker_id == CS_ID("start") ||
                                         map_result.dragged_marker_id == CS_ID("end"));

        /* Handle marker drag completion - update app-owned state and re-route */
        if (map_result.drag_ended && map_result.dragged_marker_id != 0) {
            if (map_result.dragged_marker_id == CS_ID("start")) {
                /* Update start position from drag result */
                g_app.route.start.lat = map_result.dragged_marker_lat;
                g_app.route.start.lon = map_result.dragged_marker_lon;
                /* Mark for re-geocoding */
                g_app.route.start_address[0] = '\0';
                g_app.route.start_needs_geocode = true;
            } else if (map_result.dragged_marker_id == CS_ID("end")) {
                /* Update end position from drag result */
                g_app.route.end.lat = map_result.dragged_marker_lat;
                g_app.route.end.lon = map_result.dragged_marker_lon;
                /* Mark for re-geocoding */
                g_app.route.end_address[0] = '\0';
                g_app.route.end_needs_geocode = true;
            }

            /* Trigger final re-route on drag end */
            if (g_app.route.has_start && g_app.route.has_end) {
                g_app.route.loading = true;
                g_app.route.error = false;
                g_app.route.point_count = 0; g_app.route.points = NULL;
                cs_provider_route(g_app.route.start, g_app.route.end);
            }
            g_app.route.dragging_marker = false;
            g_app.route.reroute_cooldown = 0;
        }
        /* Handle debounced route updates during drag */
        else if (is_dragging_route_marker && g_app.route.has_start && g_app.route.has_end) {
            g_app.route.dragging_marker = true;

            /* Request intermediate route when cooldown expires */
            if (g_app.route.reroute_cooldown <= 0 && !g_app.route.loading) {
                /* Use current drag positions for the route request */
                CsGeoPoint from = g_app.route.start;
                CsGeoPoint to = g_app.route.end;

                if (map_result.dragged_marker_id == CS_ID("start")) {
                    from.lat = map_result.dragged_marker_lat;
                    from.lon = map_result.dragged_marker_lon;
                } else if (map_result.dragged_marker_id == CS_ID("end")) {
                    to.lat = map_result.dragged_marker_lat;
                    to.lon = map_result.dragged_marker_lon;
                }

                g_app.route.loading = true;
                g_app.route.error = false;
                cs_provider_route(from, to);
                g_app.route.reroute_cooldown = DRAG_REROUTE_INTERVAL;
            }
        }

        /* UI overlays */
        render_info_panel();
        render_layer_panel();
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
    g_app.map.zoom = cs_map_scroll(zoom, 0, 0, get_max_zoom(g_app.panels.layer_type));
}

EXPORT double map_get_lat(void) { return g_app.map.lat; }
EXPORT double map_get_lon(void) { return g_app.map.lon; }
EXPORT int map_get_zoom(void) { return g_app.map.zoom; }
EXPORT double map_get_visual_zoom(void) { return cs_map_get_visual_zoom(g_app.map.component_id); }
EXPORT int map_get_layer(void) { return g_app.panels.layer_type; }
EXPORT uint32_t map_get_component_id(void) { return g_app.map.component_id; }
EXPORT int map_get_width(void) { return g_app.map.width; }
EXPORT int map_get_height(void) { return g_app.map.height; }
EXPORT int map_get_show_tile_info(void) { return g_app.panels.show_tile_info ? 1 : 0; }

/* Attribution link - returns true once when clicked, then clears */
EXPORT int map_get_attribution_clicked(void) {
    if (g_app.attribution_clicked) {
        g_app.attribution_clicked = false;
        return 1;
    }
    return 0;
}

/* Debug exports for route markers */
EXPORT int map_debug_has_start(void) { return g_app.route.has_start ? 1 : 0; }
EXPORT double map_debug_start_lat(void) { return g_app.route.start.lat; }
EXPORT double map_debug_start_lon(void) { return g_app.route.start.lon; }

/* ============================================================================
 * Domain Exports - Pointer Handling
 * ============================================================================ */

/* Check if pointer is over any UI panel (not the map) */
static bool is_pointer_over_ui(void) {
    return cs_clay_pointer_over("InfoPanel") ||
           cs_clay_pointer_over("LayerPanel") ||
           cs_clay_pointer_over("TileInfo") ||
           cs_clay_pointer_over("RoutePanel") ||
           cs_clay_pointer_over("Attribution");
}

EXPORT void map_pointer_move(float x, float y) {
    bool dragging_map = cs_map_is_dragging(g_app.map.component_id);
    bool dragging_marker = cs_map_is_dragging_marker(g_app.map.component_id);
    bool dragging = dragging_map || dragging_marker;
    cs_clay_set_pointer(x, y, dragging);

    if (dragging) {
        double new_lat, new_lon;
        double visual_zoom = cs_map_get_visual_zoom(g_app.map.component_id);
        if (cs_map_pointer_move(g_app.map.component_id, visual_zoom, x, y,
                                (float)g_app.map.width, (float)g_app.map.height,
                                &new_lat, &new_lon)) {
            /* Only update map position if dragging map (not marker) */
            if (dragging_map) {
                g_app.map.lat = new_lat;
                g_app.map.lon = new_lon;
                if (g_app.map.lat > 85.0) g_app.map.lat = 85.0;
                if (g_app.map.lat < -85.0) g_app.map.lat = -85.0;
                while (g_app.map.lon > 180.0) g_app.map.lon -= 360.0;
                while (g_app.map.lon < -180.0) g_app.map.lon += 360.0;
            }
        }
    }
}

EXPORT void map_pointer_down(float x, float y) {
    /* Don't start map drag if clicking on UI */
    if (is_pointer_over_ui()) {
        cs_set_pending_click();
        return;
    }

    double visual_zoom = cs_map_get_visual_zoom(g_app.map.component_id);
    cs_map_pointer_down(g_app.map.component_id, g_app.map.lat, g_app.map.lon, x, y,
                        (float)g_app.map.width, (float)g_app.map.height, visual_zoom);
    cs_clay_set_pointer(x, y, true);
}

EXPORT void map_pointer_up(float x, float y) {
    cs_map_pointer_up(g_app.map.component_id, x, y);
    cs_clay_set_pointer(x, y, false);
}

/* ============================================================================
 * Domain Exports - Scrollbar Drag Handling
 * ============================================================================ */

EXPORT void scrollbar_start_drag(uint32_t scroll_id, float mouse_y,
                                  float track_height, float content_height, float view_height) {
    CsScrollInfo info = cs_scroll_info(scroll_id);
    g_app.scrollbar.dragging = true;
    g_app.scrollbar.scroll_id = scroll_id;
    g_app.scrollbar.drag_start_y = mouse_y;
    g_app.scrollbar.scroll_start_y = info.found ? info.scroll_y : 0;
    g_app.scrollbar.track_height = track_height;
    g_app.scrollbar.content_height = content_height;
    g_app.scrollbar.view_height = view_height;
}

EXPORT void scrollbar_move(float mouse_y) {
    if (!g_app.scrollbar.dragging) return;

    float delta_y = mouse_y - g_app.scrollbar.drag_start_y;

    /* Convert pixel delta to scroll delta */
    float max_scroll = g_app.scrollbar.content_height - g_app.scrollbar.view_height;
    if (max_scroll <= 0) return;

    float content_ratio = g_app.scrollbar.view_height / g_app.scrollbar.content_height;
    float thumb_height = g_app.scrollbar.track_height * content_ratio;
    if (thumb_height < 20.0f) thumb_height = 20.0f;

    float track_range = g_app.scrollbar.track_height - thumb_height;
    if (track_range <= 0) return;

    float scroll_per_pixel = max_scroll / track_range;
    float new_scroll_y = g_app.scrollbar.scroll_start_y + (delta_y * scroll_per_pixel);

    cs_scroll_set_position(g_app.scrollbar.scroll_id, 0, new_scroll_y);
}

EXPORT void scrollbar_end_drag(void) {
    g_app.scrollbar.dragging = false;
    g_app.scrollbar.scroll_id = 0;
}

EXPORT int scrollbar_is_dragging(void) {
    return g_app.scrollbar.dragging ? 1 : 0;
}

/* Check if click coordinates are over the scrollbar track or thumb
 * Uses unified hit testing system */
EXPORT int scrollbar_hit_test_xy(float x, float y) {
    if (!g_app.scrollbar.track_visible) return 0;

    /* Use unified hit testing */
    CsHitResult hit = cs_hit_test(x, y);
    if (hit.id == CS_ID("instructions") &&
        (hit.zone == CS_HIT_TRACK || hit.zone == CS_HIT_THUMB)) {
        return 1;
    }
    return 0;
}

/* Debug exports for scrollbar bounds */
EXPORT int scrollbar_debug_visible(void) { return g_app.scrollbar.track_visible ? 1 : 0; }
EXPORT float scrollbar_debug_x(void) { return g_app.scrollbar.track_x; }
EXPORT float scrollbar_debug_y(void) { return g_app.scrollbar.track_y; }
EXPORT float scrollbar_debug_w(void) { return g_app.scrollbar.track_w; }
EXPORT float scrollbar_debug_h(void) { return g_app.scrollbar.track_h; }

EXPORT float scrollbar_content_height(void) { return g_app.scrollbar.content_height; }
EXPORT float scrollbar_view_height(void) { return g_app.scrollbar.view_height; }
EXPORT float scrollbar_track_height(void) { return g_app.scrollbar.track_height; }
EXPORT uint32_t scrollbar_scroll_id(void) { return CS_ID("instructions"); }

/* Update scrollbar bounds - now handled inline during rendering.
 * This function is kept for backwards compatibility but is a no-op.
 * Scrollbar bounds and hit targets are registered during render_route_panel(). */
static void update_scrollbar_bounds(void) {
    /* Bounds are now updated inline during rendering */
    (void)0;
}

EXPORT void map_scroll(float delta, float x, float y) {
    (void)x; (void)y;
    /* Don't zoom map if scrolling over UI */
    if (is_pointer_over_ui()) return;

    g_app.map.zoom = cs_map_scroll(g_app.map.zoom, delta > 0 ? 1 : -1, 0, get_max_zoom(g_app.panels.layer_type));
}

EXPORT int map_handle_click(float x, float y) {
    bool on_ui = is_pointer_over_ui();

    /* Check if attribution link was clicked */
    if (cs_clay_pointer_over("Attribution")) {
        g_app.attribution_clicked = true;
        return 1;  /* Handled - don't propagate to map */
    }

    if (cs_focused_id() != 0 && !on_ui) {
        cs_blur();
    }

    /* Handle map click for routing */
    if (!on_ui && !g_app.route.loading) {
        /* Convert screen coords to geo coords using delta from center
         * Use visual_zoom for consistency with overlay/tile rendering
         * Note: negate Y because screen Y increases downward but latitude increases upward */
        float cx = (float)g_app.map.width / 2.0f;
        float cy = (float)g_app.map.height / 2.0f;
        double visual_zoom = cs_map_get_visual_zoom(g_app.map.component_id);
        double dlat, dlon;
        cs_map_screen_to_geo_delta(g_app.map.lat, visual_zoom, x - cx, -(y - cy), &dlat, &dlon);
        double click_lat = g_app.map.lat + dlat;
        double click_lon = g_app.map.lon + dlon;

        if (!g_app.route.has_start) {
            /* Set start point */
            g_app.route.start.lat = click_lat;
            g_app.route.start.lon = click_lon;
            g_app.route.has_start = true;
            g_app.route.error = false;
            g_app.route.start_address[0] = '\0';
            /* Request reverse geocode for start */
            g_app.route.start_needs_geocode = true;
            if (g_app.route.reverse_pending == REVERSE_NONE) {
                cs_provider_reverse(click_lat, click_lon);
                g_app.route.reverse_pending = REVERSE_START;
                g_app.route.start_needs_geocode = false;
            }
        } else if (!g_app.route.has_end) {
            /* Set end point and request route */
            g_app.route.end.lat = click_lat;
            g_app.route.end.lon = click_lon;
            g_app.route.has_end = true;
            g_app.route.loading = true;
            g_app.route.error = false;
            g_app.route.point_count = 0; g_app.route.points = NULL;
            g_app.route.end_address[0] = '\0';
            /* Request reverse geocode for end */
            g_app.route.end_needs_geocode = true;
            if (g_app.route.reverse_pending == REVERSE_NONE) {
                cs_provider_reverse(click_lat, click_lon);
                g_app.route.reverse_pending = REVERSE_END;
                g_app.route.end_needs_geocode = false;
            }

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
            g_app.route.point_count = 0; g_app.route.points = NULL;
            g_app.route.points = NULL;
            g_app.route.instruction_count = 0;
        } else {
            /* Use provider's buffer directly (no copy) */
            g_app.route.points = result->points;
            g_app.route.point_count = result->count;
            g_app.route.distance_m = result->distance_m;
            g_app.route.duration_s = result->duration_s;
            g_app.route.calc_time_ms = result->calc_time_ms;
            g_app.route.loading = false;
            g_app.route.error = false;

            /* Generate mock turn-by-turn instructions */
            generate_mock_instructions(&g_app.route);
        }
    }
}

/* Format address from reverse geocode result */
static void format_address(char *buf, size_t size, const CsReverseResult *r) {
    /* Prefer display_name (stored in name field) as it's already formatted */
    if (r->name[0]) {
        snprintf(buf, size, "%s", r->name);
    } else if (r->street[0] && r->city[0]) {
        snprintf(buf, size, "%s, %s", r->street, r->city);
    } else if (r->street[0]) {
        snprintf(buf, size, "%s", r->street);
    } else if (r->city[0]) {
        snprintf(buf, size, "%s", r->city);
    } else {
        buf[0] = '\0';
    }
}

/* Check for reverse geocode completion from provider */
static void update_reverse_geocode_state(void) {
    if (g_app.route.reverse_pending == REVERSE_NONE) {
        /* Check if there's a pending geocode request to start */
        if (g_app.route.start_needs_geocode && g_app.route.has_start) {
            cs_provider_reverse(g_app.route.start.lat, g_app.route.start.lon);
            g_app.route.reverse_pending = REVERSE_START;
            g_app.route.start_needs_geocode = false;
        } else if (g_app.route.end_needs_geocode && g_app.route.has_end) {
            cs_provider_reverse(g_app.route.end.lat, g_app.route.end.lon);
            g_app.route.reverse_pending = REVERSE_END;
            g_app.route.end_needs_geocode = false;
        }
        return;
    }

    if (!cs_provider_reverse_ready()) return;

    const CsReverseResult *result = cs_provider_reverse_result();

    if (!result->error) {
        if (g_app.route.reverse_pending == REVERSE_START) {
            format_address(g_app.route.start_address, sizeof(g_app.route.start_address), result);
        } else if (g_app.route.reverse_pending == REVERSE_END) {
            format_address(g_app.route.end_address, sizeof(g_app.route.end_address), result);
        }
    }

    cs_provider_reverse_clear();
    g_app.route.reverse_pending = REVERSE_NONE;

    /* Check if there's another pending request */
    if (g_app.route.start_needs_geocode && g_app.route.has_start) {
        cs_provider_reverse(g_app.route.start.lat, g_app.route.start.lon);
        g_app.route.reverse_pending = REVERSE_START;
        g_app.route.start_needs_geocode = false;
    } else if (g_app.route.end_needs_geocode && g_app.route.has_end) {
        cs_provider_reverse(g_app.route.end.lat, g_app.route.end.lon);
        g_app.route.reverse_pending = REVERSE_END;
        g_app.route.end_needs_geocode = false;
    }
}

EXPORT int map_frame(float dt) {
    if (!cs_clay_is_initialized()) return -1;

    /* Update zoom animation - use large dt to snap instantly when smooth zoom is disabled */
    float zoom_dt = g_app.panels.smooth_zoom ? dt : 100.0f;
    cs_map_update_zoom_animation(g_app.map.component_id, g_app.map.zoom, zoom_dt);

    /* Check for route completion */
    update_route_state();

    /* Check for reverse geocode completion */
    update_reverse_geocode_state();

    /* Decrement reroute cooldown */
    if (g_app.route.reroute_cooldown > 0) {
        g_app.route.reroute_cooldown -= dt;
    }

    /* Update scroll containers with wheel delta */
    cs_update_scroll_containers(dt);

    cs_clay_begin_frame();
    render_ui();
    int result = cs_clay_end_frame(dt);

    /* Update scrollbar bounds after layout for hit testing */
    update_scrollbar_bounds();

    return result;
}
