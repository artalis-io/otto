/**
 * Clay Components - Internal Header
 *
 * Shared state and utilities for component implementations.
 * Not part of public API.
 */

#ifndef CC_INTERNAL_H
#define CC_INTERNAL_H

#include "cc_common.h"

/* ============================================================================
 * Widget State Store
 * ============================================================================ */

#define CC_WIDGET_STATE_CAPACITY 256  /* Hash table size (power of 2) */

/**
 * Per-widget persistent state.
 * Stored in hash table keyed by widget ID.
 */
typedef struct {
    uint32_t id;                /* Widget ID (0 = empty slot) */

    /* Text input state */
    int cursor;                 /* Cursor position */
    int selection_start;        /* Selection anchor (-1 = no selection) */
    float cursor_blink;         /* Blink timer */
    bool cursor_visible;        /* Current blink state */

    /* Scroll state (for future scroll containers) */
    float scroll_x;
    float scroll_y;

    /* Generic state */
    bool open;                  /* For collapsibles, dropdowns, etc. */
} CcWidgetState;

/* ============================================================================
 * Global State Structure
 * ============================================================================ */

#define CC_MAX_FOCUSABLES 64  /* Max focusable elements per frame */

typedef struct {
    uint32_t focused_id;        /* Currently focused element (0 = none) */

    /* Current frame state */
    uint32_t clicked_id;        /* Element clicked this frame */
    uint32_t hovered_id;        /* Element hovered this frame */
    bool pending_click;         /* Click event pending this frame */
    bool pending_enter;         /* Enter key pressed this frame (for button activation) */

    /* Pointer position (set by cc_set_pointer) */
    float pointer_x, pointer_y;

    /* Focused element bounds (set during render) */
    float focused_x, focused_y, focused_w, focused_h;

    /* Active input buffer reference (only valid during frame) */
    char *active_text;
    int *active_len;
    int active_max_len;

    /* Tab navigation: focusable elements registered this frame */
    uint32_t focusables[CC_MAX_FOCUSABLES];
    int focusable_count;

    /* Widget state store (hash table) */
    CcWidgetState widgets[CC_WIDGET_STATE_CAPACITY];
} CcState;

/* Get pointer to global state (defined in cc_common.c) */
CcState* cc_get_state(void);

/* Get or create widget state for given ID */
CcWidgetState* cc_widget_state(uint32_t id);

/* Shared utilities are in cc_common.h: cc_min_i, cc_max_i, cc_clamp_i, etc. */

#endif /* CC_INTERNAL_H */
