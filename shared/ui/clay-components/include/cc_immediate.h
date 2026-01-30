/**
 * Clay Components - Immediate Mode API
 *
 * Thin immediate mode components on top of Clay layout.
 * You own the state, components are just functions that return events.
 *
 * Usage:
 *   static char search[256];
 *   static int search_len = 0;
 *
 *   CcResult r = cc_input(CC_ID("search"), search, &search_len, 256, "Search...");
 *   if (r.submitted) do_search(search);
 *
 *   if (cc_button(CC_ID("go"), "Go").clicked) do_search(search);
 */

#ifndef CC_IMMEDIATE_H
#define CC_IMMEDIATE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * ID Generation
 * ============================================================================ */

/* Generate unique ID from string (compile-time hash would be better but this works) */
uint32_t cc_hash_id(const char *str);

/* Convenience macro - use file:line for unique IDs */
#define CC_ID(name) cc_hash_id(name)

/* ============================================================================
 * Result Types
 * ============================================================================ */

typedef struct {
    bool changed;      /* Text was modified this frame */
    bool submitted;    /* Enter was pressed */
    bool focused;      /* Gained focus this frame */
    bool blurred;      /* Lost focus this frame */
} CcInputResult;

typedef struct {
    bool clicked;      /* Was clicked this frame */
    bool hovered;      /* Is hovered this frame */
} CcButtonResult;

/* ============================================================================
 * Styles
 * ============================================================================ */

typedef struct {
    float width;           /* 0 = auto/grow */
    float height;          /* 0 = auto */
    float font_size;
    float padding;
    float corner_radius;
} CcInputStyle;

typedef enum {
    CC_BTN_DEFAULT,
    CC_BTN_PRIMARY,
    CC_BTN_SECONDARY,
    CC_BTN_DANGER,
    CC_BTN_GHOST,
} CcButtonVariant;

typedef struct {
    CcButtonVariant variant;
    float font_size;
    float padding_x;
    float padding_y;
    float corner_radius;
} CcButtonStyle;

/* Default styles */
extern const CcInputStyle CC_INPUT_STYLE_DEFAULT;
extern const CcInputStyle CC_INPUT_STYLE_DARK;
extern const CcButtonStyle CC_BUTTON_STYLE_DEFAULT;

/* ============================================================================
 * Core API
 * ============================================================================ */

/* Initialize component system (call once at startup) */
void cc_init(void);

/* Update per frame (handles cursor blink, etc.) */
void cc_frame_begin(void);
void cc_frame_end(float dt);

/* ============================================================================
 * Input Routing (call from JS event handlers)
 * ============================================================================ */

/* Route keyboard to focused element. Returns true if consumed. */
bool cc_key_down(int key_code, bool shift, bool ctrl);
bool cc_key_char(uint32_t char_code);

/* Route click to element. Returns true if consumed by UI. */
bool cc_click(float x, float y);

/* Set pending click for this frame (call before rendering) */
void cc_set_pending_click(void);

/* ============================================================================
 * Focus State (for JS to render cursor/selection)
 * ============================================================================ */

uint32_t cc_focused_id(void);
void cc_focus(uint32_t id);
void cc_blur(void);

/* Cursor state for focused text input */
int cc_cursor_pos(void);
int cc_selection_start(void);  /* -1 if no selection */
bool cc_cursor_visible(void);

/* Get focused element bounds (for cursor rendering) */
bool cc_focused_bounds(float *x, float *y, float *w, float *h);

/* ============================================================================
 * Components
 * ============================================================================ */

/**
 * Text input - immediate mode
 *
 * @param id          Unique identifier (use CC_ID("name"))
 * @param text        Your text buffer (modified in place)
 * @param len         Pointer to current length (modified in place)
 * @param max_len     Buffer capacity
 * @param placeholder Placeholder text when empty (can be NULL)
 * @param style       Style or NULL for default
 * @return            Result with event flags
 */
CcInputResult cc_input(
    uint32_t id,
    char *text,
    int *len,
    int max_len,
    const char *placeholder,
    const CcInputStyle *style
);

/* Convenience wrapper with default style */
#define cc_input_simple(id, text, len, max_len, placeholder) \
    cc_input(id, text, len, max_len, placeholder, NULL)

/**
 * Button - immediate mode
 *
 * @param id    Unique identifier
 * @param label Button text
 * @param style Style or NULL for default
 * @return      Result with event flags
 */
CcButtonResult cc_button(
    uint32_t id,
    const char *label,
    const CcButtonStyle *style
);

/* Convenience wrapper with default style */
#define cc_button_simple(id, label) cc_button(id, label, NULL)

/* Styled button variants */
#define cc_button_primary(id, label) \
    cc_button(id, label, &(CcButtonStyle){CC_BTN_PRIMARY, 14, 16, 8, 4})
#define cc_button_danger(id, label) \
    cc_button(id, label, &(CcButtonStyle){CC_BTN_DANGER, 14, 16, 8, 4})

#ifdef __cplusplus
}
#endif

#endif /* CC_IMMEDIATE_H */
