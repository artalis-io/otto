/**
 * cs_tui_demo.c - ClayShards TUI WebGL Demo
 *
 * Demonstrates ClayShards immediate-mode UI components in a browser:
 *   - cs_button: Clickable buttons
 *   - cs_input: Text input with cursor
 *   - cs_checkbox: Boolean toggle with checkmark
 *   - cs_toggle: On/off switch
 *   - cs_slider: Value slider
 *   - cs_dropdown: Selection dropdown
 *
 * This mirrors the terminal demo but renders to WebGL via the TUI buffer.
 */

#include <emscripten.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#define CLAY_IMPLEMENTATION
#include "clay.h"
#include "cs_immediate.h"
#include "cs_tui.h"
#include "cs_tui_internal.h"

/* ============================================================================
 * Application State
 * ============================================================================ */

typedef struct {
    /* Input fields */
    char name[64];
    int name_len;
    char email[128];
    int email_len;

    /* Checkboxes */
    bool notifications;
    bool darkmode;

    /* Toggle */
    bool enabled;

    /* Slider */
    float volume;

    /* Dropdown */
    int priority;

    /* Counter (button demo) */
    int counter;

    /* Terminal size */
    int width;
    int height;
} AppState;

static AppState g_app = {
    .name = "John",
    .name_len = 4,
    .email = "",
    .email_len = 0,
    .notifications = true,
    .darkmode = false,
    .enabled = true,
    .volume = 0.7f,
    .priority = 1,
    .counter = 0,
    .width = 80,
    .height = 24,
};

static const char *PRIORITY_OPTIONS[] = {"Low", "Medium", "High", "Critical"};

/* Global renderer */
static CsTuiRenderer *g_renderer = NULL;
static void *g_clay_mem = NULL;

/* ============================================================================
 * Theme
 * ============================================================================ */

static const struct {
    Clay_Color bg;
    Clay_Color panel;
    Clay_Color text;
    Clay_Color text_muted;
    Clay_Color border;
    Clay_Color accent;
} THEME = {
    .bg         = {30, 30, 30, 255},
    .panel      = {45, 45, 45, 255},
    .text       = {240, 240, 240, 255},
    .text_muted = {150, 150, 150, 255},
    .border     = {80, 80, 80, 255},
    .accent     = {80, 140, 200, 255},
};

/* ============================================================================
 * UI Layout (same as demo_tui.c)
 * ============================================================================ */

static void render_ui(void) {
    /* Root container */
    CLAY(CLAY_ID("Root"), {
        .layout = {
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .sizing = { CLAY_SIZING_FIXED((float)g_app.width), CLAY_SIZING_FIXED((float)g_app.height) },
            .padding = CLAY_PADDING_ALL(1),
            .childGap = 1
        },
        .backgroundColor = THEME.bg
    }) {
        /* Title */
        CLAY(CLAY_ID("TitleBar"), {
            .layout = {
                .padding = {1, 1, 0, 0},
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) }
            },
            .backgroundColor = THEME.accent
        }) {
            CLAY_TEXT(CLAY_STRING(" ClayShards TUI WebGL Demo "),
                     CLAY_TEXT_CONFIG({ .fontSize = 14, .textColor = THEME.text }));
        }

        /* Main content area */
        CLAY(CLAY_ID("Content"), {
            .layout = {
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                .childGap = 2
            }
        }) {
            /* Left panel - Form inputs */
            CLAY(CLAY_ID("LeftPanel"), {
                .layout = {
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                    .sizing = { CLAY_SIZING_FIXED(30), CLAY_SIZING_GROW(0) },
                    .padding = CLAY_PADDING_ALL(1),
                    .childGap = 1
                },
                .backgroundColor = THEME.panel,
                .border = { .width = {1, 1, 1, 1, 0}, .color = THEME.border }
            }) {
                CLAY_TEXT(CLAY_STRING("Form Inputs"),
                         CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text_muted }));

                /* Name input */
                CLAY_TEXT(CLAY_STRING("Name:"),
                         CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text }));

                const CsInputStyle input_style = {
                    .width = 26,
                    .height = 1,
                    .font_size = 12,
                    .padding = 0,
                    .corner_radius = 0
                };
                cs_input(CS_ID("name_input"),
                    g_app.name, &g_app.name_len, sizeof(g_app.name),
                    "Enter name...", &input_style);

                /* Email input */
                CLAY_TEXT(CLAY_STRING("Email:"),
                         CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text }));

                cs_input(CS_ID("email_input"),
                    g_app.email, &g_app.email_len, sizeof(g_app.email),
                    "user@example.com", &input_style);

                /* Separator */
                CLAY(CLAY_ID("Sep1"), {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) } },
                    .backgroundColor = THEME.border
                }) {}

                /* Checkboxes */
                CLAY_TEXT(CLAY_STRING("Options:"),
                         CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text_muted }));

                const CsCheckboxStyle check_style = {
                    .size = 1,
                    .font_size = 12,
                    .corner_radius = 0,
                    .gap = 1,
                    .border_width = 0
                };
                cs_checkbox(CS_ID("notif_check"), &g_app.notifications, "Notifications", &check_style);
                cs_checkbox(CS_ID("dark_check"), &g_app.darkmode, "Dark Mode", &check_style);
            }

            /* Right panel - Controls */
            CLAY(CLAY_ID("RightPanel"), {
                .layout = {
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                    .padding = CLAY_PADDING_ALL(1),
                    .childGap = 1
                },
                .backgroundColor = THEME.panel,
                .border = { .width = {1, 1, 1, 1, 0}, .color = THEME.border }
            }) {
                CLAY_TEXT(CLAY_STRING("Controls"),
                         CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text_muted }));

                /* Toggle */
                CLAY(CLAY_ID("ToggleRow"), {
                    .layout = { .childGap = 2, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } }
                }) {
                    CLAY_TEXT(CLAY_STRING("Enabled:"),
                             CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text }));
                    const CsToggleStyle toggle_style = {
                        .width = 4,
                        .height = 1,
                        .font_size = 12,
                        .gap = 1
                    };
                    cs_toggle(CS_ID("enable_toggle"), &g_app.enabled, NULL, &toggle_style);
                }

                /* Slider */
                CLAY(CLAY_ID("SliderRow"), {
                    .layout = {
                        .layoutDirection = CLAY_TOP_TO_BOTTOM,
                        .childGap = 0
                    }
                }) {
                    const CsSliderStyle slider_style = {
                        .width = 20,
                        .height = 1,
                        .thumb_size = 1,
                        .font_size = 12,
                        .corner_radius = 0,
                        .step = 0.1f,
                        .show_value = true
                    };
                    cs_slider(CS_ID("volume_slider"), &g_app.volume, 0.0f, 1.0f, "Volume", &slider_style);
                }

                /* Dropdown */
                CLAY(CLAY_ID("DropdownRow"), {
                    .layout = { .childGap = 2, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } }
                }) {
                    CLAY_TEXT(CLAY_STRING("Priority:"),
                             CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text }));
                    const CsDropdownStyle dd_style = {
                        .width = 10,
                        .height = 1,
                        .font_size = 12,
                        .corner_radius = 0,
                        .max_height = 6
                    };
                    cs_dropdown(CS_ID("priority_dd"), &g_app.priority, PRIORITY_OPTIONS, 4, &dd_style);
                }

                /* Space for dropdown list */
                CLAY(CLAY_ID("DropdownSpace"), {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(5) } }
                }) {}

                /* Buttons */
                CLAY_TEXT(CLAY_STRING("Actions:"),
                         CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text_muted }));

                CLAY(CLAY_ID("ButtonRow"), {
                    .layout = { .childGap = 2 }
                }) {
                    const CsButtonStyle btn_style = {
                        .variant = CS_BTN_PRIMARY,
                        .font_size = 12,
                        .corner_radius = 0,
                        .padding_x = 2,
                        .padding_y = 0
                    };
                    const CsButtonStyle btn_default = {
                        .variant = CS_BTN_DEFAULT,
                        .font_size = 12,
                        .corner_radius = 0,
                        .padding_x = 2,
                        .padding_y = 0
                    };

                    if (cs_button(CS_ID("inc_btn"), " + ", &btn_style).clicked) {
                        g_app.counter++;
                    }
                    if (cs_button(CS_ID("dec_btn"), " - ", &btn_default).clicked) {
                        g_app.counter--;
                    }
                    if (cs_button(CS_ID("reset_btn"), " Reset ", &btn_default).clicked) {
                        g_app.counter = 0;
                    }
                }

                /* Counter display - use static buffer to persist until Clay_EndLayout */
                static char counter_str[32];
                int counter_len = snprintf(counter_str, sizeof(counter_str), "Counter: %d", g_app.counter);
                CLAY_TEXT(((Clay_String){ .chars = counter_str, .length = counter_len }),
                         CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text }));
            }
        }

        /* Footer */
        CLAY(CLAY_ID("Footer"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) },
                .padding = {1, 1, 0, 0}
            },
            .backgroundColor = THEME.panel
        }) {
            CLAY_TEXT(CLAY_STRING(" Tab: Navigate | Enter: Activate | Arrow Keys: Adjust "),
                     CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text_muted }));
        }
    }
}

/* ============================================================================
 * WASM Exports
 * ============================================================================ */

/**
 * Initialize the demo.
 */
EMSCRIPTEN_KEEPALIVE
int demo_init(int width, int height) {
    if (width <= 0) width = 80;
    if (height <= 0) height = 24;

    g_app.width = width;
    g_app.height = height;

    /* Initialize Clay */
    g_clay_mem = cs_tui_init_clay(width, height, NULL, NULL);
    if (!g_clay_mem) return 0;

    /* Initialize ClayShards */
    cs_init();

    /* Create headless TUI renderer */
    CsTuiConfig config;
    cs_tui_config_init(&config);
    config.width = width;
    config.height = height;
    config.headless = true;

    g_renderer = cs_tui_create(&config);
    if (!g_renderer) {
        free(g_clay_mem);
        g_clay_mem = NULL;
        return 0;
    }

    return 1;
}

/**
 * Resize the demo.
 */
EMSCRIPTEN_KEEPALIVE
void demo_resize(int width, int height) {
    if (!g_renderer) return;

    if (width <= 0) width = 80;
    if (height <= 0) height = 24;

    g_app.width = width;
    g_app.height = height;

    cs_tui_resize(g_renderer, width, height);
    cs_tui_update_clay_size(width, height);
}

/**
 * Get buffer pointer.
 */
EMSCRIPTEN_KEEPALIVE
void *demo_get_buffer(void) {
    return cs_tui_get_buffer(g_renderer);
}

/**
 * Get buffer width.
 */
EMSCRIPTEN_KEEPALIVE
int demo_get_width(void) {
    return g_app.width;
}

/**
 * Get buffer height.
 */
EMSCRIPTEN_KEEPALIVE
int demo_get_height(void) {
    return g_app.height;
}

/**
 * Get cell size.
 */
EMSCRIPTEN_KEEPALIVE
int demo_get_cell_size(void) {
    return cs_tui_get_cell_size();
}

/**
 * Run a frame.
 */
EMSCRIPTEN_KEEPALIVE
int demo_frame(float dt) {
    if (!g_renderer) return 0;

    /* Run ClayShards frame */
    cs_frame_begin();
    Clay_BeginLayout();
    render_ui();
    Clay_RenderCommandArray commands = Clay_EndLayout();
    cs_frame_end(dt);

    /* Render to TUI buffer */
    cs_tui_begin(g_renderer);
    cs_tui_render_clay_commands(g_renderer, commands.internalArray, commands.length);

    /* Render cursor for text input */
    float fx, fy, fw, fh;
    cs_focused_bounds(&fx, &fy, &fw, &fh);
    if (fw > 0 && fh > 0 && cs_cursor_visible()) {
        int cursor_pos = cs_cursor_pos();
        int cursor_x = (int)fx + cursor_pos;
        int cursor_y = (int)fy;
        /* Render block cursor */
        cs_tui_text(g_renderer, cursor_x, cursor_y, "\xE2\x96\x88", 3,
                   (CsTuiColor){255, 255, 0, 255}, NULL);
    }

    cs_tui_end(g_renderer);

    return commands.length;
}

/**
 * Handle key down.
 */
EMSCRIPTEN_KEEPALIVE
void demo_key_down(int key, int mods) {
    bool ctrl = (mods & 1) != 0;
    bool shift = (mods & 2) != 0;
    (void)ctrl;

    switch (key) {
        case 9:   /* Tab */
            if (shift) {
                cs_focus_prev();
            } else {
                cs_focus_next();
            }
            break;
        case 13:  /* Enter */
            cs_key_down(13, false, false);
            break;
        case 8:   /* Backspace */
        case 46:  /* Delete */
            cs_key_down(8, false, false);
            break;
        case 37:  /* Left */
            cs_key_down(37, false, false);
            break;
        case 38:  /* Up */
            cs_key_down(38, false, false);
            break;
        case 39:  /* Right */
            cs_key_down(39, false, false);
            break;
        case 40:  /* Down */
            cs_key_down(40, false, false);
            break;
        case 36:  /* Home */
            cs_key_down(36, false, false);
            break;
        case 35:  /* End */
            cs_key_down(35, false, false);
            break;
        case 27:  /* Escape */
            cs_key_down(27, false, false);
            break;
    }
}

/**
 * Handle character input.
 */
EMSCRIPTEN_KEEPALIVE
void demo_key_char(uint32_t codepoint) {
    if (codepoint >= 32 && codepoint < 127) {
        cs_key_char(codepoint);
    }
}

/**
 * Handle mouse move.
 */
EMSCRIPTEN_KEEPALIVE
void demo_mouse_move(int x, int y) {
    Clay_SetPointerState((Clay_Vector2){(float)x, (float)y}, cs_is_pointer_down());
    cs_set_pointer((float)x, (float)y);
}

/**
 * Handle mouse click.
 */
EMSCRIPTEN_KEEPALIVE
void demo_mouse_click(int x, int y) {
    Clay_SetPointerState((Clay_Vector2){(float)x, (float)y}, true);
    cs_set_pointer((float)x, (float)y);
    cs_set_pointer_down(true);
    cs_set_pending_click();
}

/**
 * Handle mouse release.
 */
EMSCRIPTEN_KEEPALIVE
void demo_mouse_release(int x, int y) {
    Clay_SetPointerState((Clay_Vector2){(float)x, (float)y}, false);
    cs_set_pointer((float)x, (float)y);
    cs_set_pointer_down(false);
}

/**
 * Get app state for display.
 */
EMSCRIPTEN_KEEPALIVE
int demo_get_counter(void) {
    return g_app.counter;
}

EMSCRIPTEN_KEEPALIVE
float demo_get_volume(void) {
    return g_app.volume;
}

EMSCRIPTEN_KEEPALIVE
int demo_get_priority(void) {
    return g_app.priority;
}

EMSCRIPTEN_KEEPALIVE
int demo_get_enabled(void) {
    return g_app.enabled ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int demo_get_notifications(void) {
    return g_app.notifications ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int demo_get_darkmode(void) {
    return g_app.darkmode ? 1 : 0;
}
