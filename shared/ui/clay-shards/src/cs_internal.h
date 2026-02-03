/**
 * Clay Components - Internal Header
 *
 * Shared state and utilities for component implementations.
 * Not part of public API.
 */

#ifndef CS_INTERNAL_H
#define CS_INTERNAL_H

#include "cs_common.h"

/* ============================================================================
 * Thread-Local Storage
 * ============================================================================ */

/* Cross-platform thread-local storage macro.
 * Each thread gets its own copy of UI state for thread safety. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
    /* C11 with threads support */
    #define CS_THREAD_LOCAL _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
    /* GCC/Clang extension */
    #define CS_THREAD_LOCAL __thread
#elif defined(_MSC_VER)
    /* MSVC */
    #define CS_THREAD_LOCAL __declspec(thread)
#else
    /* No TLS support - single-threaded only */
    #define CS_THREAD_LOCAL
    #define CS_NO_TLS 1
#endif

/* ============================================================================
 * Internal Constants
 * ============================================================================ */

/* ID offsets for internal wrapper elements to avoid collisions with user IDs.
 * When a component needs internal sub-elements (margin wrapper, text wrapper),
 * it adds these offsets to the user-provided ID. */
#define CS_ID_OFFSET_WRAPPER        0x10000  /* Margin/alignment wrapper */
#define CS_ID_OFFSET_TEXT_WRAPPER   0x20000  /* Text offset wrapper */
#define CS_ID_OFFSET_SLIDER_FILL    0x30000  /* Slider fill track */
#define CS_ID_OFFSET_DROPDOWN_ARROW 0x40000  /* Dropdown arrow indicator */
#define CS_ID_OFFSET_DROPDOWN_LIST  0x50000  /* Dropdown floating list */
#define CS_ID_OFFSET_DROPDOWN_ITEM  0x60000  /* Dropdown list items (+ index) */

/* ============================================================================
 * Helper Macros for Code Deduplication
 * ============================================================================ */

/**
 * Check if a CsMargin has any non-zero values.
 * Used by components to determine if wrapper element is needed.
 */
static inline bool cs_has_margin(CsMargin m) {
    return m.top > 0 || m.bottom > 0 || m.left > 0 || m.right > 0;
}

/**
 * Mark the current focused element as a non-text element.
 * Call this at the end of button/checkbox/toggle/dropdown/slider components
 * so keyboard navigation knows Enter means "activate" not "submit text".
 */
#define CS_MARK_NON_TEXT_IF_FOCUSED(state, focused) do { \
    if (focused) { \
        (state)->active_text = NULL; \
        (state)->active_len = NULL; \
        (state)->active_max_len = 0; \
    } \
} while(0)

/* ============================================================================
 * Widget State Store
 * ============================================================================ */

#define CS_WIDGET_STATE_CAPACITY 256  /* Hash table size (power of 2) */

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
} CsWidgetState;

/* ============================================================================
 * Global State Structure
 * ============================================================================ */

#define CS_MAX_FOCUSABLES 64  /* Max focusable elements per frame */

typedef struct {
    uint32_t focused_id;        /* Currently focused element (0 = none) */

    /* Current frame state */
    uint32_t clicked_id;        /* Element clicked this frame */
    uint32_t hovered_id;        /* Element hovered this frame */
    bool pending_click;         /* Click event pending this frame */
    bool pending_enter;         /* Enter key pressed this frame (for button activation) */
    bool pending_escape;        /* Escape key pressed this frame (for closing dropdowns) */
    bool pending_arrow_up;      /* Up arrow pressed (for dropdown navigation) */
    bool pending_arrow_down;    /* Down arrow pressed (for dropdown navigation) */
    bool pending_arrow_left;    /* Left arrow pressed (for slider decrement) */
    bool pending_arrow_right;   /* Right arrow pressed (for slider increment) */
    bool pending_home;          /* Home key pressed (for slider jump to min) */
    bool pending_end;           /* End key pressed (for slider jump to max) */

    /* Pointer position (set by cs_set_pointer) */
    float pointer_x, pointer_y;
    bool pointer_down;          /* Pointer is currently pressed */
    uint32_t dragging_id;       /* Element currently being dragged (0 = none) */

    /* Focused element bounds (set during render) */
    float focused_x, focused_y, focused_w, focused_h;

    /* Active input buffer reference (only valid during frame) */
    char *active_text;
    int *active_len;
    int active_max_len;

    /* Tab navigation: focusable elements registered this frame */
    uint32_t focusables[CS_MAX_FOCUSABLES];
    int focusable_count;

    /* Widget state store (hash table) */
    CsWidgetState widgets[CS_WIDGET_STATE_CAPACITY];
} CsState;

/* Get pointer to global state (defined in cs_common.c) */
CsState* cs_get_state(void);

/* Get or create widget state for given ID */
CsWidgetState* cs_widget_state(uint32_t id);

/* Shared utilities are in cs_common.h: cs_min_i, cs_max_i, cs_clamp_i, etc. */

/* ============================================================================
 * Internal Allocation Helpers
 * ============================================================================ */

/* Use these instead of malloc/realloc/free to support custom allocators */
void* cs_alloc(size_t size);
void* cs_realloc(void *ptr, size_t size);
void  cs_free(void *ptr);

/* ============================================================================
 * Error Tracking (for debugging silent failures)
 * ============================================================================ */

/* Error codes for internal failures */
typedef enum {
    CS_ERR_NONE = 0,
    CS_ERR_ALLOC_FAILED,        /* Memory allocation failed */
    CS_ERR_CAPACITY_EXCEEDED,   /* Hash table or buffer full */
    CS_ERR_INVALID_ARGUMENT,    /* Invalid argument (negative count, overflow) */
} CsErrorCode;

/* Record an internal error (thread-local in future) */
void cs_record_error(CsErrorCode code);

/* Get and clear the last error code */
CsErrorCode cs_get_last_error(void);

/* Get count of errors since last clear */
int cs_get_error_count(void);

/* Clear error state */
void cs_clear_errors(void);

#endif /* CS_INTERNAL_H */
