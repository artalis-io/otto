/**
 * Clay Components - Common Implementation
 *
 * Core state, ID generation, focus management, and input routing.
 *
 * WASM exports (add to Makefile EXPORTED_FUNCTIONS):
 *   "_cc_focused_id","_cc_cursor_pos","_cc_selection_start","_cc_cursor_visible",
 *   "_cc_focused_bounds","_cc_focused_text","_cc_focused_text_len",
 *   "_cc_key_down","_cc_key_char","_cc_blur","_cc_set_pending_click",
 *   "_cc_push_id","_cc_pop_id"
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
    .pending_click = false,
};

/* Components need access to global state */
CcState* cc_get_state(void) {
    return &g_cc;
}

/* ============================================================================
 * Widget State Store (Hash Table)
 * ============================================================================ */

/**
 * Get or create widget state for given ID.
 * Uses open-addressed hash table with linear probing.
 */
CcWidgetState* cc_widget_state(uint32_t id) {
    if (id == 0) return NULL;

    uint32_t mask = CC_WIDGET_STORE_SIZE - 1;
    uint32_t slot = id & mask;

    /* Linear probe to find existing or empty slot */
    for (int i = 0; i < CC_WIDGET_STORE_SIZE; i++) {
        uint32_t idx = (slot + i) & mask;
        CcWidgetState *w = &g_cc.widgets[idx];

        if (w->id == id) {
            return w;  /* Found existing */
        }
        if (w->id == 0) {
            /* Empty slot - initialize and return */
            w->id = id;
            w->cursor = 0;
            w->selection_start = -1;
            w->cursor_blink = 0.0f;
            w->cursor_visible = true;
            w->scroll_x = 0.0f;
            w->scroll_y = 0.0f;
            w->open = false;
            return w;
        }
    }

    /* Table full - return first slot as fallback (shouldn't happen with 256 slots) */
    return &g_cc.widgets[slot & mask];
}

/* ============================================================================
 * ID Generation (Dear ImGui-style ID Stack)
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

static uint32_t cc_hash_combine(uint32_t seed, uint32_t value) {
    /* Combine two hashes (FNV-style) */
    seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    return seed ? seed : 1;
}

CC_EXPORT void cc_push_id(int int_id) {
    if (g_cc.id_stack_depth >= CC_ID_STACK_SIZE) return;
    g_cc.id_stack[g_cc.id_stack_depth++] = (uint32_t)int_id;
}

void cc_push_id_str(const char *str_id) {
    if (g_cc.id_stack_depth >= CC_ID_STACK_SIZE) return;
    g_cc.id_stack[g_cc.id_stack_depth++] = cc_hash_id(str_id);
}

CC_EXPORT void cc_pop_id(void) {
    if (g_cc.id_stack_depth > 0) {
        g_cc.id_stack_depth--;
    }
}

uint32_t cc_get_id(const char *str) {
    uint32_t id = cc_hash_id(str);

    /* Combine with ID stack */
    for (int i = 0; i < g_cc.id_stack_depth; i++) {
        id = cc_hash_combine(id, g_cc.id_stack[i]);
    }

    return id;
}

/* ============================================================================
 * Core API
 * ============================================================================ */

void cc_init(void) {
    memset(&g_cc, 0, sizeof(g_cc));
}

void cc_frame_begin(void) {
    g_cc.clicked_id = 0;
    g_cc.hovered_id = 0;
    /* Don't reset pending_click - it's set by mousedown and consumed during render */
    /* Don't reset pending_enter - it's set by keydown and consumed during render */
    g_cc.active_text = NULL;
    g_cc.active_len = NULL;

    /* Reset focusable registry for this frame */
    g_cc.focusable_count = 0;

    /* Reset ID stack each frame (safety) */
    g_cc.id_stack_depth = 0;
}

void cc_frame_end(float dt) {
    /* Reset pending click/enter after components have had a chance to check it */
    g_cc.pending_click = false;
    g_cc.pending_enter = false;

    /* Update cursor blink for focused widget */
    if (g_cc.focused_id != 0) {
        CcWidgetState *w = cc_widget_state(g_cc.focused_id);
        if (w) {
            w->cursor_blink += dt;
            if (w->cursor_blink >= CC_CURSOR_BLINK_PERIOD) {
                w->cursor_blink = 0.0f;
                w->cursor_visible = !w->cursor_visible;
            }
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
        /* Reset cursor state for newly focused widget */
        if (id != 0) {
            CcWidgetState *w = cc_widget_state(id);
            if (w) {
                w->cursor = 0;
                w->selection_start = -1;
                w->cursor_visible = true;
                w->cursor_blink = 0.0f;
            }
        }
    }
}

CC_EXPORT void cc_blur(void) {
    g_cc.focused_id = 0;
}

CC_EXPORT int cc_cursor_pos(void) {
    if (g_cc.focused_id == 0) return 0;
    CcWidgetState *w = cc_widget_state(g_cc.focused_id);
    return w ? w->cursor : 0;
}

CC_EXPORT int cc_selection_start(void) {
    if (g_cc.focused_id == 0) return -1;
    CcWidgetState *w = cc_widget_state(g_cc.focused_id);
    return w ? w->selection_start : -1;
}

CC_EXPORT bool cc_cursor_visible(void) {
    if (g_cc.focused_id == 0) return true;
    CcWidgetState *w = cc_widget_state(g_cc.focused_id);
    return w ? w->cursor_visible : true;
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

static bool has_selection(CcWidgetState *w) {
    return w && w->selection_start >= 0 && w->selection_start != w->cursor;
}

static void delete_selection(CcWidgetState *w, char *text, int *len) {
    if (!has_selection(w)) return;

    int start = cc_min_i(w->cursor, w->selection_start);
    int end = cc_max_i(w->cursor, w->selection_start);

    /* Clamp to valid range to prevent underflow */
    if (start < 0) start = 0;
    if (end > *len) end = *len;
    if (start >= end) {
        w->selection_start = -1;
        return;
    }

    memmove(&text[start], &text[end], *len - end + 1);
    *len -= (end - start);
    w->cursor = start;
    w->selection_start = -1;
}

CC_EXPORT bool cc_key_char(uint32_t char_code) {
    if (g_cc.focused_id == 0) return false;
    if (!g_cc.active_text || !g_cc.active_len) return false;
    if (char_code < 32 || char_code > 126) return false;
    if (*g_cc.active_len >= g_cc.active_max_len - 1) return false;

    CcWidgetState *w = cc_widget_state(g_cc.focused_id);
    if (!w) return false;

    char *text = g_cc.active_text;
    int *len = g_cc.active_len;

    delete_selection(w, text, len);

    /* Insert character */
    memmove(&text[w->cursor + 1], &text[w->cursor], *len - w->cursor + 1);
    text[w->cursor] = (char)char_code;
    w->cursor++;
    (*len)++;

    w->cursor_visible = true;
    w->cursor_blink = 0.0f;

    return true;
}

CC_EXPORT bool cc_key_down(int key_code, bool shift, bool ctrl) {
    /* Validate key code range (standard keyboard codes 0-255, extended up to 512) */
    if (key_code < 0 || key_code > 512) return false;

    /* Tab navigation works globally (even with no focus) */
    if (key_code == 9) { /* Tab */
        if (shift) {
            return cc_focus_prev();
        } else {
            return cc_focus_next();
        }
    }

    /* Enter key on focused button triggers click */
    if (key_code == 13 && g_cc.focused_id != 0) { /* Enter */
        /* If we have a focused element but no active text buffer,
         * it's a button - set pending_enter for it to detect */
        if (!g_cc.active_text || !g_cc.active_len) {
            g_cc.pending_enter = true;
            return true;
        }
    }

    if (g_cc.focused_id == 0) return false;
    if (!g_cc.active_text || !g_cc.active_len) return false;

    CcWidgetState *w = cc_widget_state(g_cc.focused_id);
    if (!w) return false;

    char *text = g_cc.active_text;
    int *len = g_cc.active_len;
    bool sel = has_selection(w);

    w->cursor_visible = true;
    w->cursor_blink = 0.0f;

    switch (key_code) {
        case 37: /* Left */
            if (ctrl) {
                while (w->cursor > 0 && text[w->cursor - 1] == ' ') w->cursor--;
                while (w->cursor > 0 && text[w->cursor - 1] != ' ') w->cursor--;
            } else if (sel && !shift) {
                w->cursor = cc_min_i(w->cursor, w->selection_start);
            } else if (w->cursor > 0) {
                w->cursor--;
            }
            if (!shift) w->selection_start = -1;
            else if (w->selection_start < 0) w->selection_start = w->cursor + 1;
            return true;

        case 39: /* Right */
            if (ctrl) {
                while (w->cursor < *len && text[w->cursor] != ' ') w->cursor++;
                while (w->cursor < *len && text[w->cursor] == ' ') w->cursor++;
            } else if (sel && !shift) {
                w->cursor = cc_max_i(w->cursor, w->selection_start);
            } else if (w->cursor < *len) {
                w->cursor++;
            }
            if (!shift) w->selection_start = -1;
            else if (w->selection_start < 0) w->selection_start = w->cursor - 1;
            return true;

        case 36: /* Home */
            if (shift && w->selection_start < 0) w->selection_start = w->cursor;
            w->cursor = 0;
            if (!shift) w->selection_start = -1;
            return true;

        case 35: /* End */
            if (shift && w->selection_start < 0) w->selection_start = w->cursor;
            w->cursor = *len;
            if (!shift) w->selection_start = -1;
            return true;

        case 8: /* Backspace */
            if (sel) {
                delete_selection(w, text, len);
            } else if (w->cursor > 0) {
                if (ctrl) {
                    int start = w->cursor;
                    while (w->cursor > 0 && text[w->cursor - 1] == ' ') w->cursor--;
                    while (w->cursor > 0 && text[w->cursor - 1] != ' ') w->cursor--;
                    memmove(&text[w->cursor], &text[start], *len - start + 1);
                    *len -= (start - w->cursor);
                } else {
                    memmove(&text[w->cursor - 1], &text[w->cursor], *len - w->cursor + 1);
                    w->cursor--;
                    (*len)--;
                }
            }
            return true;

        case 46: /* Delete */
            if (sel) {
                delete_selection(w, text, len);
            } else if (w->cursor < *len) {
                if (ctrl) {
                    int end = w->cursor;
                    while (end < *len && text[end] == ' ') end++;
                    while (end < *len && text[end] != ' ') end++;
                    memmove(&text[w->cursor], &text[end], *len - end + 1);
                    *len -= (end - w->cursor);
                } else {
                    memmove(&text[w->cursor], &text[w->cursor + 1], *len - w->cursor);
                    (*len)--;
                }
            }
            return true;

        case 65: /* A - select all */
            if (ctrl) {
                w->selection_start = 0;
                w->cursor = *len;
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

/* ============================================================================
 * Tab Navigation
 * ============================================================================ */

CC_EXPORT void cc_register_focusable(uint32_t id) {
    if (id == 0) return;
    if (g_cc.focusable_count >= CC_MAX_FOCUSABLES) return;

    /* Avoid duplicates */
    for (int i = 0; i < g_cc.focusable_count; i++) {
        if (g_cc.focusables[i] == id) return;
    }

    g_cc.focusables[g_cc.focusable_count++] = id;
}

CC_EXPORT bool cc_focus_next(void) {
    if (g_cc.focusable_count == 0) return false;

    /* Find current focused element index */
    int current_idx = -1;
    for (int i = 0; i < g_cc.focusable_count; i++) {
        if (g_cc.focusables[i] == g_cc.focused_id) {
            current_idx = i;
            break;
        }
    }

    /* Move to next (or first if nothing focused) */
    int next_idx = (current_idx + 1) % g_cc.focusable_count;
    uint32_t next_id = g_cc.focusables[next_idx];

    if (next_id != g_cc.focused_id) {
        cc_focus(next_id);
        return true;
    }
    return false;
}

CC_EXPORT bool cc_focus_prev(void) {
    if (g_cc.focusable_count == 0) return false;

    /* Find current focused element index */
    int current_idx = -1;
    for (int i = 0; i < g_cc.focusable_count; i++) {
        if (g_cc.focusables[i] == g_cc.focused_id) {
            current_idx = i;
            break;
        }
    }

    /* Move to previous (or last if nothing focused) */
    int prev_idx;
    if (current_idx <= 0) {
        prev_idx = g_cc.focusable_count - 1;
    } else {
        prev_idx = current_idx - 1;
    }

    uint32_t prev_id = g_cc.focusables[prev_idx];

    if (prev_id != g_cc.focused_id) {
        cc_focus(prev_id);
        return true;
    }
    return false;
}

CC_EXPORT int cc_focusable_count(void) {
    return g_cc.focusable_count;
}
