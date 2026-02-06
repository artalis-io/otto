/**
 * cs_tui_wasm.c - WASM bridge for ClayShards TUI
 *
 * Provides Emscripten exports for WebGL renderer to access cell buffer
 * and forward input events to the TUI application.
 */

#include <emscripten.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "cs_tui.h"
#include "cs_tui_internal.h"
#include "cs_immediate.h"
#include "clay.h"

/* ============================================================================
 * Global State
 * ============================================================================ */

static CsTuiRenderer *g_renderer = NULL;
static void *g_clay_mem = NULL;
static int g_width = 80;
static int g_height = 24;

/* Input state */
static int g_mouse_x = 0;
static int g_mouse_y = 0;
static bool g_mouse_down = false;

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * Initialize the TUI renderer for WASM.
 * @param width  Terminal width in cells (0 = 80)
 * @param height Terminal height in cells (0 = 24)
 * @return 1 on success, 0 on failure
 */
EMSCRIPTEN_KEEPALIVE
int wasm_tui_init(int width, int height) {
    if (g_renderer) {
        /* Already initialized */
        return 1;
    }

    /* Default dimensions */
    if (width <= 0) width = 80;
    if (height <= 0) height = 24;

    g_width = width;
    g_height = height;

    /* Initialize Clay with TUI text measurement */
    g_clay_mem = cs_tui_init_clay(width, height, NULL, NULL);
    if (!g_clay_mem) {
        return 0;
    }

    /* Initialize ClayShards immediate mode */
    cs_init();

    /* Create headless TUI renderer (no terminal I/O) */
    CsTuiConfig config;
    cs_tui_config_init(&config);
    config.width = width;
    config.height = height;
    config.headless = true;  /* No terminal output, buffer only */

    g_renderer = cs_tui_create(&config);
    if (!g_renderer) {
        free(g_clay_mem);
        g_clay_mem = NULL;
        return 0;
    }

    return 1;
}

/**
 * Resize the TUI renderer.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_tui_resize(int width, int height) {
    if (!g_renderer) return;

    if (width <= 0) width = 80;
    if (height <= 0) height = 24;

    g_width = width;
    g_height = height;

    cs_tui_resize(g_renderer, width, height);
    cs_tui_update_clay_size(width, height);
}

/**
 * Clean up resources.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_tui_destroy(void) {
    if (g_renderer) {
        cs_tui_free(g_renderer);
        g_renderer = NULL;
    }
    if (g_clay_mem) {
        free(g_clay_mem);
        g_clay_mem = NULL;
    }
}

/* ============================================================================
 * Buffer Access
 * ============================================================================ */

/**
 * Get pointer to the front (displayed) cell buffer.
 * Each cell is exactly 16 bytes.
 */
EMSCRIPTEN_KEEPALIVE
void *wasm_tui_get_buffer(void) {
    return cs_tui_get_buffer(g_renderer);
}

/**
 * Get buffer width in cells.
 */
EMSCRIPTEN_KEEPALIVE
int wasm_tui_get_width(void) {
    return cs_tui_get_buffer_width(g_renderer);
}

/**
 * Get buffer height in cells.
 */
EMSCRIPTEN_KEEPALIVE
int wasm_tui_get_height(void) {
    return cs_tui_get_buffer_height(g_renderer);
}

/**
 * Get size of a single cell in bytes (always 16).
 */
EMSCRIPTEN_KEEPALIVE
int wasm_tui_get_cell_size(void) {
    return cs_tui_get_cell_size();
}

/**
 * Check if buffer has changed since last call.
 */
EMSCRIPTEN_KEEPALIVE
int wasm_tui_buffer_dirty(void) {
    return cs_tui_buffer_dirty(g_renderer) ? 1 : 0;
}

/* ============================================================================
 * Input Events
 * ============================================================================ */

/**
 * Handle keyboard event.
 * @param key  Key code (e.g., 13 for Enter, 27 for Escape)
 * @param mods Modifier flags: 1=Ctrl, 2=Shift, 4=Alt
 * @param down true for keydown, false for keyup
 */
EMSCRIPTEN_KEEPALIVE
void wasm_tui_key_event(int key, int mods, int down) {
    if (!down) return;  /* Only handle key down for now */

    bool ctrl = (mods & 1) != 0;
    bool shift = (mods & 2) != 0;
    (void)shift;  /* Not used yet */

    /* Map common key codes to ClayShards input */
    switch (key) {
        case 8:   /* Backspace */
        case 46:  /* Delete */
            cs_key_down(CS_KEY_BACKSPACE);
            break;
        case 13:  /* Enter */
            cs_key_down(CS_KEY_ENTER);
            break;
        case 27:  /* Escape */
            cs_key_down(CS_KEY_ESCAPE);
            break;
        case 9:   /* Tab */
            if (shift) {
                cs_focus_prev();
            } else {
                cs_focus_next();
            }
            break;
        case 37:  /* Left arrow */
            cs_key_down(CS_KEY_LEFT);
            break;
        case 39:  /* Right arrow */
            cs_key_down(CS_KEY_RIGHT);
            break;
        case 38:  /* Up arrow */
            cs_key_down(CS_KEY_UP);
            break;
        case 40:  /* Down arrow */
            cs_key_down(CS_KEY_DOWN);
            break;
        case 36:  /* Home */
            cs_key_down(CS_KEY_HOME);
            break;
        case 35:  /* End */
            cs_key_down(CS_KEY_END);
            break;
        default:
            /* Handle Ctrl+key combinations */
            if (ctrl) {
                if (key == 65) {  /* Ctrl+A: select all */
                    cs_key_down(CS_KEY_HOME);
                    /* TODO: select all */
                } else if (key == 67) {  /* Ctrl+C: copy */
                    /* TODO: copy */
                } else if (key == 86) {  /* Ctrl+V: paste */
                    /* TODO: paste */
                }
            }
            break;
    }
}

/**
 * Handle character input (printable characters).
 * @param codepoint Unicode codepoint
 */
EMSCRIPTEN_KEEPALIVE
void wasm_tui_char_event(uint32_t codepoint) {
    /* Convert codepoint to UTF-8 string for ClayShards */
    char buf[5] = {0};
    if (codepoint < 0x80) {
        buf[0] = (char)codepoint;
    } else if (codepoint < 0x800) {
        buf[0] = (char)(0xC0 | (codepoint >> 6));
        buf[1] = (char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        buf[0] = (char)(0xE0 | (codepoint >> 12));
        buf[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x110000) {
        buf[0] = (char)(0xF0 | (codepoint >> 18));
        buf[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (codepoint & 0x3F));
    }

    cs_key_char(buf);
}

/**
 * Handle mouse movement.
 * @param cell_x X position in cells
 * @param cell_y Y position in cells
 */
EMSCRIPTEN_KEEPALIVE
void wasm_tui_mouse_move(int cell_x, int cell_y) {
    g_mouse_x = cell_x;
    g_mouse_y = cell_y;

    /* Update Clay pointer position (in cell coordinates) */
    Clay_SetPointerState((Clay_Vector2){(float)cell_x, (float)cell_y}, g_mouse_down);
}

/**
 * Handle mouse button event.
 * @param cell_x X position in cells
 * @param cell_y Y position in cells
 * @param button Button number (0 = left, 1 = middle, 2 = right)
 * @param down   true for button down, false for up
 */
EMSCRIPTEN_KEEPALIVE
void wasm_tui_mouse_event(int cell_x, int cell_y, int button, int down) {
    if (button != 0) return;  /* Only handle left button for now */

    g_mouse_x = cell_x;
    g_mouse_y = cell_y;
    g_mouse_down = (down != 0);

    Clay_SetPointerState((Clay_Vector2){(float)cell_x, (float)cell_y}, g_mouse_down);

    if (down) {
        /* Signal pending click for ClayShards button handling */
        cs_set_pending_click();
    }
}

/**
 * Handle mouse scroll.
 * @param delta_y Scroll delta (positive = down, negative = up)
 */
EMSCRIPTEN_KEEPALIVE
void wasm_tui_scroll(float delta_y) {
    /* Forward scroll to ClayShards */
    cs_set_scroll_delta(delta_y);
    Clay_UpdateScrollContainers(true, (Clay_Vector2){0, delta_y}, 0.016f);
}

/* ============================================================================
 * Frame Lifecycle
 * ============================================================================ */

/**
 * Run a frame.
 * This should be called by the app's frame function after updating UI state.
 *
 * @param commands Clay render command array
 * @param count    Number of commands
 * @return 1 if buffer changed, 0 otherwise
 */
EMSCRIPTEN_KEEPALIVE
int wasm_tui_render(const void *commands, int count) {
    if (!g_renderer || !commands || count <= 0) return 0;

    /* Render frame */
    cs_tui_begin(g_renderer);
    cs_tui_clear(g_renderer, (CsTuiColor){30, 30, 30, 255});
    cs_tui_render_clay_commands(g_renderer, commands, count);
    cs_tui_end(g_renderer);

    return 1;  /* Buffer always changes after render */
}

/**
 * Begin a new frame for ClayShards.
 * Call this at the start of each frame.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_tui_frame_begin(void) {
    cs_frame_begin();
    Clay_BeginLayout();
}

/**
 * End the ClayShards frame and render.
 * @param dt Delta time in seconds
 * @return Number of render commands
 */
EMSCRIPTEN_KEEPALIVE
int wasm_tui_frame_end(float dt) {
    Clay_RenderCommandArray commands = Clay_EndLayout();
    cs_frame_end(dt);

    /* Render to TUI buffer */
    cs_tui_begin(g_renderer);
    cs_tui_clear(g_renderer, (CsTuiColor){30, 30, 30, 255});
    cs_tui_render_clay_commands(g_renderer, commands.internalArray, commands.length);
    cs_tui_end(g_renderer);

    return commands.length;
}

/* ============================================================================
 * Cell Accessors (for JS fallback if direct memory access doesn't work)
 * ============================================================================ */

/**
 * Get cell codepoint at position.
 */
EMSCRIPTEN_KEEPALIVE
uint32_t wasm_tui_cell_codepoint(int x, int y) {
    return cs_tui_get_cell_char(g_renderer, x, y);
}

/**
 * Get cell foreground color at position (packed RGB).
 */
EMSCRIPTEN_KEEPALIVE
uint32_t wasm_tui_cell_fg(int x, int y) {
    CsTuiColor c = cs_tui_get_cell_fg(g_renderer, x, y);
    return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | (uint32_t)c.b;
}

/**
 * Get cell background color at position (packed RGB).
 */
EMSCRIPTEN_KEEPALIVE
uint32_t wasm_tui_cell_bg(int x, int y) {
    CsTuiColor c = cs_tui_get_cell_bg(g_renderer, x, y);
    return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | (uint32_t)c.b;
}
