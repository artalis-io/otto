/**
 * Clay Components - Common Implementation
 *
 * Core state, ID generation, focus management, and input routing.
 *
 * WASM exports (add to Makefile EXPORTED_FUNCTIONS):
 *   "_cs_focused_id","_cs_cursor_pos","_cs_selection_start","_cs_cursor_visible",
 *   "_cs_focused_bounds","_cs_focused_text","_cs_focused_text_len",
 *   "_cs_key_down","_cs_key_char","_cs_blur","_cs_set_pending_click",
 *   "_cs_push_id","_cs_pop_id"
 */

#include "cs_internal.h"
#include <string.h>
#include <stdlib.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define CS_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define CS_EXPORT
#endif

/* ============================================================================
 * Custom Allocator
 * ============================================================================ */

/* Default allocator wrappers */
static void* default_alloc(size_t size, void *user_data) {
    (void)user_data;
    return malloc(size);
}

static void* default_realloc(void *ptr, size_t size, void *user_data) {
    (void)user_data;
    return realloc(ptr, size);
}

static void default_free(void *ptr, void *user_data) {
    (void)user_data;
    free(ptr);
}

static const CsAllocator g_default_allocator = {
    .alloc = default_alloc,
    .realloc = default_realloc,
    .free = default_free,
    .user_data = NULL
};

/* Thread-local allocator (NULL means use default) */
static CS_THREAD_LOCAL const CsAllocator *tls_allocator = NULL;

void cs_set_allocator(const CsAllocator *allocator) {
    tls_allocator = allocator;
}

const CsAllocator* cs_get_allocator(void) {
    return tls_allocator ? tls_allocator : &g_default_allocator;
}

/* Internal allocation helpers */
void* cs_alloc(size_t size) {
    const CsAllocator *a = cs_get_allocator();
    void *ptr = a->alloc(size, a->user_data);
    if (!ptr && size > 0) {
        cs_record_error(CS_ERR_ALLOC_FAILED);
    }
    return ptr;
}

void* cs_realloc(void *ptr, size_t size) {
    const CsAllocator *a = cs_get_allocator();
    void *new_ptr = a->realloc(ptr, size, a->user_data);
    if (!new_ptr && size > 0) {
        cs_record_error(CS_ERR_ALLOC_FAILED);
    }
    return new_ptr;
}

void cs_free(void *ptr) {
    if (!ptr) return;
    const CsAllocator *a = cs_get_allocator();
    a->free(ptr, a->user_data);
}

/* ============================================================================
 * Global State (Thread-Local)
 * ============================================================================ */

static CS_THREAD_LOCAL CsState tls_state = {
    .focused_id = 0,
    .pending_click = false,
};

/* Components need access to global state */
CsState* cs_get_state(void) {
    return &tls_state;
}

/* ============================================================================
 * Widget State Store (Hash Table)
 * ============================================================================ */

/**
 * Get or create widget state for given ID.
 * Uses open-addressed hash table with linear probing.
 */
CsWidgetState* cs_widget_state(uint32_t id) {
    if (id == 0) return NULL;

    uint32_t mask = CS_WIDGET_STATE_CAPACITY - 1;
    uint32_t slot = id & mask;

    /* Linear probe to find existing or empty slot */
    for (int i = 0; i < CS_WIDGET_STATE_CAPACITY; i++) {
        uint32_t idx = (slot + i) & mask;
        CsWidgetState *w = &tls_state.widgets[idx];

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

    /* Table full - return NULL to signal error (shouldn't happen with 256 slots) */
    cs_record_error(CS_ERR_CAPACITY_EXCEEDED);
    return NULL;
}

/* ============================================================================
 * ID Generation
 * ============================================================================ */

uint32_t cs_hash_id(const char *str) {
    if (!str) return 1;  /* Return valid ID for NULL input */

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

void cs_init(void) {
    memset(&tls_state, 0, sizeof(tls_state));
}

void cs_frame_begin(void) {
    tls_state.clicked_id = 0;
    tls_state.hovered_id = 0;
    /* Don't reset pending_click - it's set by mousedown and consumed during render */
    /* Don't reset pending_enter - it's set by keydown and consumed during render */
    tls_state.active_text = NULL;
    tls_state.active_len = NULL;

    /* Clear focused bounds - components will set these if they have an active text buffer */
    tls_state.focused_x = 0;
    tls_state.focused_y = 0;
    tls_state.focused_w = 0;
    tls_state.focused_h = 0;

    /* Reset focusable registry for this frame */
    tls_state.focusable_count = 0;
}

void cs_frame_end(float dt) {
    /* Reset pending input events after components have had a chance to check them */
    tls_state.pending_click = false;
    tls_state.pending_enter = false;
    tls_state.pending_escape = false;
    tls_state.pending_arrow_up = false;
    tls_state.pending_arrow_down = false;
    tls_state.pending_arrow_left = false;
    tls_state.pending_arrow_right = false;

    /* Update cursor blink for focused widget */
    if (tls_state.focused_id != 0) {
        CsWidgetState *w = cs_widget_state(tls_state.focused_id);
        if (w) {
            w->cursor_blink += dt;
            if (w->cursor_blink >= CS_CURSOR_BLINK_PERIOD) {
                w->cursor_blink = 0.0f;
                w->cursor_visible = !w->cursor_visible;
            }
        }
    }
}

/* ============================================================================
 * Focus Management
 * ============================================================================ */

CS_EXPORT uint32_t cs_focused_id(void) {
    return tls_state.focused_id;
}

void cs_focus(uint32_t id) {
    if (tls_state.focused_id != id) {
        tls_state.focused_id = id;
        /* Reset blink timer (but preserve cursor position from widget state) */
        if (id != 0) {
            CsWidgetState *w = cs_widget_state(id);
            if (w) {
                w->cursor_visible = true;
                w->cursor_blink = 0.0f;
                /* Don't reset cursor/selection - widget state preserves these */
            }
        }
    }
}

CS_EXPORT void cs_blur(void) {
    tls_state.focused_id = 0;
}

CS_EXPORT int cs_cursor_pos(void) {
    if (tls_state.focused_id == 0) return 0;
    CsWidgetState *w = cs_widget_state(tls_state.focused_id);
    return w ? w->cursor : 0;
}

CS_EXPORT int cs_selection_start(void) {
    if (tls_state.focused_id == 0) return -1;
    CsWidgetState *w = cs_widget_state(tls_state.focused_id);
    return w ? w->selection_start : -1;
}

CS_EXPORT bool cs_cursor_visible(void) {
    if (tls_state.focused_id == 0) return true;
    CsWidgetState *w = cs_widget_state(tls_state.focused_id);
    return w ? w->cursor_visible : true;
}

CS_EXPORT bool cs_focused_bounds(float *x, float *y, float *w, float *h) {
    if (tls_state.focused_id == 0) return false;
    if (x) *x = tls_state.focused_x;
    if (y) *y = tls_state.focused_y;
    if (w) *w = tls_state.focused_w;
    if (h) *h = tls_state.focused_h;
    return true;
}

/* Simple getters for WASM (no pointer params) */
CS_EXPORT float cs_focused_x(void) { return tls_state.focused_x; }
CS_EXPORT float cs_focused_y(void) { return tls_state.focused_y; }
CS_EXPORT float cs_focused_w(void) { return tls_state.focused_w; }
CS_EXPORT float cs_focused_h(void) { return tls_state.focused_h; }

CS_EXPORT const char* cs_focused_text(void) {
    if (tls_state.focused_id == 0 || !tls_state.active_text) return "";
    return tls_state.active_text;
}

CS_EXPORT int cs_focused_text_len(void) {
    if (tls_state.focused_id == 0 || !tls_state.active_len) return 0;
    return *tls_state.active_len;
}

/* ============================================================================
 * Input Routing
 * ============================================================================ */

static bool has_selection(CsWidgetState *w) {
    return w && w->selection_start >= 0 && w->selection_start != w->cursor;
}

static void delete_selection(CsWidgetState *w, char *text, int *len) {
    if (!has_selection(w)) return;

    int start = cs_min_i(w->cursor, w->selection_start);
    int end = cs_max_i(w->cursor, w->selection_start);

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

CS_EXPORT bool cs_key_char(uint32_t char_code) {
    if (tls_state.focused_id == 0) return false;
    if (!tls_state.active_text || !tls_state.active_len) return false;
    if (char_code < 32 || char_code > 126) return false;
    if (*tls_state.active_len >= tls_state.active_max_len - 1) return false;

    CsWidgetState *w = cs_widget_state(tls_state.focused_id);
    if (!w) return false;

    char *text = tls_state.active_text;
    int *len = tls_state.active_len;

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

CS_EXPORT bool cs_key_down(int key_code, bool shift, bool ctrl) {
    /* Validate key code range (standard keyboard codes 0-255, extended up to 512) */
    if (key_code < 0 || key_code > 512) return false;

    /* Tab navigation works globally (even with no focus) */
    if (key_code == 9) { /* Tab */
        if (shift) {
            return cs_focus_prev();
        } else {
            return cs_focus_next();
        }
    }

    /* Non-text focused element: handle Enter, Escape, and arrow keys */
    if (tls_state.focused_id != 0 && (!tls_state.active_text || !tls_state.active_len)) {
        switch (key_code) {
            case 13: /* Enter */
                tls_state.pending_enter = true;
                return true;
            case 27: /* Escape */
                tls_state.pending_escape = true;
                return true;
            case 37: /* Left arrow */
                tls_state.pending_arrow_left = true;
                return true;
            case 38: /* Up arrow */
                tls_state.pending_arrow_up = true;
                return true;
            case 39: /* Right arrow */
                tls_state.pending_arrow_right = true;
                return true;
            case 40: /* Down arrow */
                tls_state.pending_arrow_down = true;
                return true;
        }
    }

    if (tls_state.focused_id == 0) return false;
    if (!tls_state.active_text || !tls_state.active_len) return false;

    CsWidgetState *w = cs_widget_state(tls_state.focused_id);
    if (!w) return false;

    char *text = tls_state.active_text;
    int *len = tls_state.active_len;
    bool sel = has_selection(w);

    w->cursor_visible = true;
    w->cursor_blink = 0.0f;

    switch (key_code) {
        case 37: /* Left */
            if (ctrl) {
                while (w->cursor > 0 && text[w->cursor - 1] == ' ') w->cursor--;
                while (w->cursor > 0 && text[w->cursor - 1] != ' ') w->cursor--;
            } else if (sel && !shift) {
                w->cursor = cs_min_i(w->cursor, w->selection_start);
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
                w->cursor = cs_max_i(w->cursor, w->selection_start);
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
            cs_blur();
            return true;

        case 13: /* Enter */
            return true; /* Handled by component to set submitted flag */
    }

    return false;
}

CS_EXPORT void cs_set_pending_click(void) {
    tls_state.pending_click = true;
}

CS_EXPORT void cs_set_pointer(float x, float y) {
    tls_state.pointer_x = x;
    tls_state.pointer_y = y;
}

CS_EXPORT void cs_set_pointer_down(bool down) {
    tls_state.pointer_down = down;
    /* Clear dragging when pointer is released */
    if (!down) {
        tls_state.dragging_id = 0;
    }
}

CS_EXPORT bool cs_is_pointer_down(void) {
    return tls_state.pointer_down;
}

CS_EXPORT uint32_t cs_get_dragging_id(void) {
    return tls_state.dragging_id;
}

CS_EXPORT void cs_set_dragging_id(uint32_t id) {
    tls_state.dragging_id = id;
}

CS_EXPORT float cs_pointer_x(void) {
    return tls_state.pointer_x;
}

CS_EXPORT float cs_pointer_y(void) {
    return tls_state.pointer_y;
}

/* ============================================================================
 * Tab Navigation
 * ============================================================================ */

CS_EXPORT void cs_register_focusable(uint32_t id) {
    if (id == 0) return;
    if (tls_state.focusable_count >= CS_MAX_FOCUSABLES) return;

    /* Avoid duplicates */
    for (int i = 0; i < tls_state.focusable_count; i++) {
        if (tls_state.focusables[i] == id) return;
    }

    tls_state.focusables[tls_state.focusable_count++] = id;
}

CS_EXPORT bool cs_focus_next(void) {
    if (tls_state.focusable_count == 0) return false;

    /* Find current focused element index */
    int current_idx = -1;
    for (int i = 0; i < tls_state.focusable_count; i++) {
        if (tls_state.focusables[i] == tls_state.focused_id) {
            current_idx = i;
            break;
        }
    }

    /* Move to next (or first if nothing focused) */
    int next_idx = (current_idx + 1) % tls_state.focusable_count;
    uint32_t next_id = tls_state.focusables[next_idx];

    if (next_id != tls_state.focused_id) {
        cs_focus(next_id);
        return true;
    }
    return false;
}

CS_EXPORT bool cs_focus_prev(void) {
    if (tls_state.focusable_count == 0) return false;

    /* Find current focused element index */
    int current_idx = -1;
    for (int i = 0; i < tls_state.focusable_count; i++) {
        if (tls_state.focusables[i] == tls_state.focused_id) {
            current_idx = i;
            break;
        }
    }

    /* Move to previous (or last if nothing focused) */
    int prev_idx;
    if (current_idx <= 0) {
        prev_idx = tls_state.focusable_count - 1;
    } else {
        prev_idx = current_idx - 1;
    }

    uint32_t prev_id = tls_state.focusables[prev_idx];

    if (prev_id != tls_state.focused_id) {
        cs_focus(prev_id);
        return true;
    }
    return false;
}

CS_EXPORT int cs_focusable_count(void) {
    return tls_state.focusable_count;
}

/* ============================================================================
 * Error Tracking (Thread-Local)
 * ============================================================================ */

static CS_THREAD_LOCAL CsErrorCode tls_last_error = CS_ERR_NONE;
static CS_THREAD_LOCAL int tls_error_count = 0;

void cs_record_error(CsErrorCode code) {
    tls_last_error = code;
    tls_error_count++;
}

CS_EXPORT CsErrorCode cs_get_last_error(void) {
    return tls_last_error;
}

CS_EXPORT int cs_get_error_count(void) {
    return tls_error_count;
}

CS_EXPORT void cs_clear_errors(void) {
    tls_last_error = CS_ERR_NONE;
    tls_error_count = 0;
}
