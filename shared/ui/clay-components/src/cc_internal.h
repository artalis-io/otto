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
 * Global State Structure
 * ============================================================================ */

typedef struct {
    uint32_t focused_id;        /* Currently focused element (0 = none) */

    /* Text input state for focused element */
    int cursor;                 /* Cursor position */
    int selection_start;        /* Selection anchor (-1 = no selection) */
    float cursor_blink;         /* Blink timer */
    bool cursor_visible;        /* Current blink state */

    /* Current frame state */
    uint32_t clicked_id;        /* Element clicked this frame */
    uint32_t hovered_id;        /* Element hovered this frame */
    bool pending_click;         /* Click event pending this frame */

    /* Focused element bounds (set during render) */
    float focused_x, focused_y, focused_w, focused_h;

    /* Active input buffer reference (only valid during frame) */
    char *active_text;
    int *active_len;
    int active_max_len;
} CcState;

/* Get pointer to global state (defined in cc_common.c) */
CcState* cc_get_state(void);

/* Shared utilities are in cc_common.h: cc_min_i, cc_max_i, cc_clamp_i, etc. */

#endif /* CC_INTERNAL_H */
