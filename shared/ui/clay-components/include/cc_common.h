/**
 * Clay Components - Common Utilities
 *
 * Core API, ID generation, focus management, and shared utilities.
 */

#ifndef CC_COMMON_H
#define CC_COMMON_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/* Timing constants */
#define CC_CURSOR_BLINK_PERIOD  0.53f   /* Seconds per blink half-cycle */

/* ============================================================================
 * Layout Types (for component styles)
 * ============================================================================ */

typedef enum {
    CC_ALIGN_AUTO,      /* Inherit from parent */
    CC_ALIGN_START,     /* Top/Left */
    CC_ALIGN_CENTER,    /* Center */
    CC_ALIGN_END,       /* Bottom/Right */
} CcAlign;

typedef struct {
    float top;
    float bottom;
    float left;
    float right;
} CcMargin;

/* Text measurement fallback when no font metrics available */
#define CC_MONOSPACE_WIDTH_RATIO 0.6f   /* char_width = fontSize * ratio */

/* Default colors (RGBA 0-255) - use with Clay_Color{CC_COLOR_*} */
#define CC_COLOR_BG_DEFAULT     50, 50, 50, 255
#define CC_COLOR_BG_FOCUSED     60, 60, 60, 255
#define CC_COLOR_BG_HOVER       70, 70, 70, 255
#define CC_COLOR_BORDER         100, 100, 100, 255
#define CC_COLOR_BORDER_FOCUSED 66, 133, 244, 255
#define CC_COLOR_TEXT           255, 255, 255, 255
#define CC_COLOR_TEXT_MUTED     120, 120, 120, 255
#define CC_COLOR_PRIMARY        66, 133, 244, 255
#define CC_COLOR_PRIMARY_HOVER  100, 160, 255, 255

/* Button colors - Tailwind palette (RGBA) */
#define CC_COLOR_BTN_BLUE       59, 130, 246, 255   /* Blue 500 */
#define CC_COLOR_BTN_BLUE_HOVER 37, 99, 235, 255    /* Blue 600 */
#define CC_COLOR_BTN_RED        239, 68, 68, 255    /* Red 500 */
#define CC_COLOR_BTN_RED_HOVER  220, 38, 38, 255    /* Red 600 */
#define CC_COLOR_BTN_GRAY       75, 85, 99, 255     /* Gray 600 */
#define CC_COLOR_BTN_GRAY_HOVER 55, 65, 81, 255     /* Gray 700 */
#define CC_COLOR_BTN_GHOST_HOVER 55, 65, 81, 128   /* Gray 700 @ 50% */

/* ============================================================================
 * ID Generation
 * ============================================================================ */

/* Generate unique ID from string using FNV-1a hash */
uint32_t cc_hash_id(const char *str);

/* Convenience macro for component IDs */
#define CC_ID(name) cc_hash_id(name)

/* ============================================================================
 * Core API
 * ============================================================================ */

/* Initialize component system (call once at startup) */
void cc_init(void);

/* Frame lifecycle - call at start/end of each frame */
void cc_frame_begin(void);
void cc_frame_end(float dt);

/* ============================================================================
 * Focus Management
 * ============================================================================ */

/* Get/set focused element ID (0 = none focused) */
uint32_t cc_focused_id(void);
void cc_focus(uint32_t id);
void cc_blur(void);

/* Cursor state for focused text input */
int cc_cursor_pos(void);
int cc_selection_start(void);  /* -1 if no selection */
bool cc_cursor_visible(void);

/* Get focused element bounds (for cursor rendering) */
bool cc_focused_bounds(float *x, float *y, float *w, float *h);
float cc_focused_x(void);
float cc_focused_y(void);
float cc_focused_w(void);
float cc_focused_h(void);

/* Get focused input text (for cursor rendering) */
const char* cc_focused_text(void);
int cc_focused_text_len(void);

/* ============================================================================
 * Input Routing (call from platform event handlers)
 * ============================================================================ */

/* Route keyboard to focused element. Returns true if consumed. */
bool cc_key_down(int key_code, bool shift, bool ctrl);
bool cc_key_char(uint32_t char_code);

/* Set pending click for this frame (call on mousedown before rendering) */
void cc_set_pending_click(void);

/* Set pointer position (called automatically by cc_clay_set_pointer) */
void cc_set_pointer(float x, float y);

/* Get pointer position */
float cc_pointer_x(void);
float cc_pointer_y(void);

/* ============================================================================
 * Tab Navigation
 * ============================================================================ */

/* Register a focusable element (called by components during frame) */
void cc_register_focusable(uint32_t id);

/* Focus next/previous element in tab order. Returns true if focus changed. */
bool cc_focus_next(void);
bool cc_focus_prev(void);

/* Get count of registered focusable elements (for testing/debugging) */
int cc_focusable_count(void);

/* ============================================================================
 * Color Utilities
 * ============================================================================ */

/* Pack RGBA color (0-255 each) into uint32_t */
static inline uint32_t cc_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | a;
}

/* Color constants - Tailwind-inspired palette */
#define CC_COLOR_TRANSPARENT cc_color(0, 0, 0, 0)
#define CC_COLOR_WHITE       cc_color(255, 255, 255, 255)
#define CC_COLOR_BLACK       cc_color(0, 0, 0, 255)

/* Grays */
#define CC_COLOR_GRAY_50     cc_color(249, 250, 251, 255)
#define CC_COLOR_GRAY_100    cc_color(243, 244, 246, 255)
#define CC_COLOR_GRAY_200    cc_color(229, 231, 235, 255)
#define CC_COLOR_GRAY_300    cc_color(209, 213, 219, 255)
#define CC_COLOR_GRAY_400    cc_color(156, 163, 175, 255)
#define CC_COLOR_GRAY_500    cc_color(107, 114, 128, 255)
#define CC_COLOR_GRAY_600    cc_color(75, 85, 99, 255)
#define CC_COLOR_GRAY_700    cc_color(55, 65, 81, 255)
#define CC_COLOR_GRAY_800    cc_color(31, 41, 55, 255)
#define CC_COLOR_GRAY_900    cc_color(17, 24, 39, 255)

/* Primary colors */
#define CC_COLOR_RED_500     cc_color(239, 68, 68, 255)
#define CC_COLOR_RED_600     cc_color(220, 38, 38, 255)
#define CC_COLOR_GREEN_500   cc_color(34, 197, 94, 255)
#define CC_COLOR_GREEN_600   cc_color(22, 163, 74, 255)
#define CC_COLOR_BLUE_500    cc_color(59, 130, 246, 255)
#define CC_COLOR_BLUE_600    cc_color(37, 99, 235, 255)
#define CC_COLOR_BLUE_700    cc_color(29, 78, 216, 255)

/* Semantic colors (uint32_t packed) */
#define CC_COLOR_DANGER_U32  CC_COLOR_RED_500
#define CC_COLOR_SUCCESS_U32 CC_COLOR_GREEN_500
#define CC_COLOR_PRIMARY_U32 CC_COLOR_BLUE_500

/* ============================================================================
 * Math Utilities
 * ============================================================================ */

static inline int cc_min_i(int a, int b) { return a < b ? a : b; }
static inline int cc_max_i(int a, int b) { return a > b ? a : b; }
static inline int cc_clamp_i(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float cc_min_f(float a, float b) { return a < b ? a : b; }
static inline float cc_max_f(float a, float b) { return a > b ? a : b; }
static inline float cc_clamp_f(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

#ifdef __cplusplus
}
#endif

#endif /* CC_COMMON_H */
