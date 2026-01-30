/**
 * Clay Components - Common Implementation
 *
 * Core state, ID generation, focus management, and input routing.
 *
 * WASM exports (add to Makefile EXPORTED_FUNCTIONS):
 *   "_cc_focused_id","_cc_cursor_pos","_cc_selection_start","_cc_cursor_visible",
 *   "_cc_focused_bounds","_cc_focused_text","_cc_focused_text_len",
 *   "_cc_key_down","_cc_key_char","_cc_blur","_cc_set_pending_click"
 */

#include "cc_internal.h"
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define CC_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define CC_EXPORT
#endif

/* ============================================================================
 * Global State
 * ============================================================================ */

static CcState g_cc = {
    .focused_id = 0,
    .cursor = 0,
    .selection_start = -1,
    .cursor_blink = 0.0f,
    .cursor_visible = true,
    .pending_click = false,
};

/* Components need access to global state */
CcState* cc_get_state(void) {
    return &g_cc;
}

/* ============================================================================
 * ID Generation
 * ============================================================================ */

uint32_t cc_hash_id(const char *str) {
    /* FNV-1a hash */
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= 16777619u;
    }
    return hash ? hash : 1; /* Never return 0 */
}

/* ============================================================================
 * Core API
 * ============================================================================ */

void cc_init(void) {
    memset(&g_cc, 0, sizeof(g_cc));
    g_cc.selection_start = -1;
    g_cc.cursor_visible = true;
}

void cc_frame_begin(void) {
    g_cc.clicked_id = 0;
    g_cc.hovered_id = 0;
    /* Don't reset pending_click - it's set by mousedown and consumed during render */
    g_cc.active_text = NULL;
    g_cc.active_len = NULL;
}

void cc_frame_end(float dt) {
    /* Reset pending click after components have had a chance to check it */
    g_cc.pending_click = false;

    /* Update cursor blink */
    if (g_cc.focused_id != 0) {
        g_cc.cursor_blink += dt;
        if (g_cc.cursor_blink >= CC_CURSOR_BLINK_PERIOD) {
            g_cc.cursor_blink = 0.0f;
            g_cc.cursor_visible = !g_cc.cursor_visible;
        }
    }
}

/* ============================================================================
 * Focus Management
 * ============================================================================ */

CC_EXPORT uint32_t cc_focused_id(void) {
    return g_cc.focused_id;
}

void cc_focus(uint32_t id) {
    if (g_cc.focused_id != id) {
        g_cc.focused_id = id;
        g_cc.cursor = 0;
        g_cc.selection_start = -1;
        g_cc.cursor_visible = true;
        g_cc.cursor_blink = 0.0f;
    }
}

CC_EXPORT void cc_blur(void) {
    g_cc.focused_id = 0;
    g_cc.selection_start = -1;
}

CC_EXPORT int cc_cursor_pos(void) {
    return g_cc.cursor;
}

CC_EXPORT int cc_selection_start(void) {
    return g_cc.selection_start;
}

CC_EXPORT bool cc_cursor_visible(void) {
    return g_cc.cursor_visible;
}

CC_EXPORT bool cc_focused_bounds(float *x, float *y, float *w, float *h) {
    if (g_cc.focused_id == 0) return false;
    if (x) *x = g_cc.focused_x;
    if (y) *y = g_cc.focused_y;
    if (w) *w = g_cc.focused_w;
    if (h) *h = g_cc.focused_h;
    return true;
}

/* Simple getters for WASM (no pointer params) */
CC_EXPORT float cc_focused_x(void) { return g_cc.focused_x; }
CC_EXPORT float cc_focused_y(void) { return g_cc.focused_y; }
CC_EXPORT float cc_focused_w(void) { return g_cc.focused_w; }
CC_EXPORT float cc_focused_h(void) { return g_cc.focused_h; }

CC_EXPORT const char* cc_focused_text(void) {
    if (g_cc.focused_id == 0 || !g_cc.active_text) return "";
    return g_cc.active_text;
}

CC_EXPORT int cc_focused_text_len(void) {
    if (g_cc.focused_id == 0 || !g_cc.active_len) return 0;
    return *g_cc.active_len;
}

/* ============================================================================
 * Input Routing
 * ============================================================================ */

static bool has_selection(void) {
    return g_cc.selection_start >= 0 && g_cc.selection_start != g_cc.cursor;
}

static void delete_selection(char *text, int *len) {
    if (!has_selection()) return;

    int start = cc_min_i(g_cc.cursor, g_cc.selection_start);
    int end = cc_max_i(g_cc.cursor, g_cc.selection_start);

    /* Clamp to valid range to prevent underflow */
    if (start < 0) start = 0;
    if (end > *len) end = *len;
    if (start >= end) {
        g_cc.selection_start = -1;
        return;
    }

    memmove(&text[start], &text[end], *len - end + 1);
    *len -= (end - start);
    g_cc.cursor = start;
    g_cc.selection_start = -1;
}

CC_EXPORT bool cc_key_char(uint32_t char_code) {
    if (g_cc.focused_id == 0) return false;
    if (!g_cc.active_text || !g_cc.active_len) return false;
    if (char_code < 32 || char_code > 126) return false;
    if (*g_cc.active_len >= g_cc.active_max_len - 1) return false;

    char *text = g_cc.active_text;
    int *len = g_cc.active_len;

    delete_selection(text, len);

    /* Insert character */
    memmove(&text[g_cc.cursor + 1], &text[g_cc.cursor], *len - g_cc.cursor + 1);
    text[g_cc.cursor] = (char)char_code;
    g_cc.cursor++;
    (*len)++;

    g_cc.cursor_visible = true;
    g_cc.cursor_blink = 0.0f;

    return true;
}

CC_EXPORT bool cc_key_down(int key_code, bool shift, bool ctrl) {
    /* Validate key code range (standard keyboard codes 0-255, extended up to 512) */
    if (key_code < 0 || key_code > 512) return false;

    if (g_cc.focused_id == 0) return false;
    if (!g_cc.active_text || !g_cc.active_len) return false;

    char *text = g_cc.active_text;
    int *len = g_cc.active_len;
    bool sel = has_selection();

    g_cc.cursor_visible = true;
    g_cc.cursor_blink = 0.0f;

    switch (key_code) {
        case 37: /* Left */
            if (ctrl) {
                while (g_cc.cursor > 0 && text[g_cc.cursor - 1] == ' ') g_cc.cursor--;
                while (g_cc.cursor > 0 && text[g_cc.cursor - 1] != ' ') g_cc.cursor--;
            } else if (sel && !shift) {
                g_cc.cursor = cc_min_i(g_cc.cursor, g_cc.selection_start);
            } else if (g_cc.cursor > 0) {
                g_cc.cursor--;
            }
            if (!shift) g_cc.selection_start = -1;
            else if (g_cc.selection_start < 0) g_cc.selection_start = g_cc.cursor + 1;
            return true;

        case 39: /* Right */
            if (ctrl) {
                while (g_cc.cursor < *len && text[g_cc.cursor] != ' ') g_cc.cursor++;
                while (g_cc.cursor < *len && text[g_cc.cursor] == ' ') g_cc.cursor++;
            } else if (sel && !shift) {
                g_cc.cursor = cc_max_i(g_cc.cursor, g_cc.selection_start);
            } else if (g_cc.cursor < *len) {
                g_cc.cursor++;
            }
            if (!shift) g_cc.selection_start = -1;
            else if (g_cc.selection_start < 0) g_cc.selection_start = g_cc.cursor - 1;
            return true;

        case 36: /* Home */
            if (shift && g_cc.selection_start < 0) g_cc.selection_start = g_cc.cursor;
            g_cc.cursor = 0;
            if (!shift) g_cc.selection_start = -1;
            return true;

        case 35: /* End */
            if (shift && g_cc.selection_start < 0) g_cc.selection_start = g_cc.cursor;
            g_cc.cursor = *len;
            if (!shift) g_cc.selection_start = -1;
            return true;

        case 8: /* Backspace */
            if (sel) {
                delete_selection(text, len);
            } else if (g_cc.cursor > 0) {
                if (ctrl) {
                    int start = g_cc.cursor;
                    while (g_cc.cursor > 0 && text[g_cc.cursor - 1] == ' ') g_cc.cursor--;
                    while (g_cc.cursor > 0 && text[g_cc.cursor - 1] != ' ') g_cc.cursor--;
                    memmove(&text[g_cc.cursor], &text[start], *len - start + 1);
                    *len -= (start - g_cc.cursor);
                } else {
                    memmove(&text[g_cc.cursor - 1], &text[g_cc.cursor], *len - g_cc.cursor + 1);
                    g_cc.cursor--;
                    (*len)--;
                }
            }
            return true;

        case 46: /* Delete */
            if (sel) {
                delete_selection(text, len);
            } else if (g_cc.cursor < *len) {
                if (ctrl) {
                    int end = g_cc.cursor;
                    while (end < *len && text[end] == ' ') end++;
                    while (end < *len && text[end] != ' ') end++;
                    memmove(&text[g_cc.cursor], &text[end], *len - end + 1);
                    *len -= (end - g_cc.cursor);
                } else {
                    memmove(&text[g_cc.cursor], &text[g_cc.cursor + 1], *len - g_cc.cursor);
                    (*len)--;
                }
            }
            return true;

        case 65: /* A - select all */
            if (ctrl) {
                g_cc.selection_start = 0;
                g_cc.cursor = *len;
                return true;
            }
            break;

        case 27: /* Escape */
            cc_blur();
            return true;

        case 13: /* Enter */
            return true; /* Handled by component to set submitted flag */
    }

    return false;
}

CC_EXPORT void cc_set_pending_click(void) {
    g_cc.pending_click = true;
}
