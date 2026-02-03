/**
 * Clay Components - Common Utilities
 *
 * Core API, ID generation, focus management, and shared utilities.
 */

#ifndef CS_COMMON_H
#define CS_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>  /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/* Timing constants */
#define CS_CURSOR_BLINK_PERIOD  0.53f   /* Seconds per blink half-cycle */

/* ============================================================================
 * Layout Types (for component styles)
 * ============================================================================ */

typedef enum {
    CS_ALIGN_AUTO,      /* Inherit from parent */
    CS_ALIGN_START,     /* Top/Left */
    CS_ALIGN_CENTER,    /* Center */
    CS_ALIGN_END,       /* Bottom/Right */
} CsAlign;

typedef struct {
    float top;
    float bottom;
    float left;
    float right;
} CsMargin;

/* Text measurement fallback when no font metrics available */
#define CS_MONOSPACE_WIDTH_RATIO 0.6f   /* char_width = fontSize * ratio */

/* Default colors (RGBA 0-255) - use with Clay_Color{CS_COLOR_*} */
#define CS_COLOR_BG_DEFAULT     50, 50, 50, 255
#define CS_COLOR_BG_FOCUSED     60, 60, 60, 255
#define CS_COLOR_BG_HOVER       70, 70, 70, 255
#define CS_COLOR_BORDER         100, 100, 100, 255
#define CS_COLOR_BORDER_FOCUSED 66, 133, 244, 255
#define CS_COLOR_TEXT           255, 255, 255, 255
#define CS_COLOR_TEXT_MUTED     120, 120, 120, 255
#define CS_COLOR_PRIMARY        66, 133, 244, 255
#define CS_COLOR_PRIMARY_HOVER  100, 160, 255, 255

/* Button colors - Tailwind palette (RGBA) */
#define CS_COLOR_BTN_BLUE       59, 130, 246, 255   /* Blue 500 */
#define CS_COLOR_BTN_BLUE_HOVER 37, 99, 235, 255    /* Blue 600 */
#define CS_COLOR_BTN_RED        239, 68, 68, 255    /* Red 500 */
#define CS_COLOR_BTN_RED_HOVER  220, 38, 38, 255    /* Red 600 */
#define CS_COLOR_BTN_GRAY       75, 85, 99, 255     /* Gray 600 */
#define CS_COLOR_BTN_GRAY_HOVER 55, 65, 81, 255     /* Gray 700 */
#define CS_COLOR_BTN_GHOST_HOVER 55, 65, 81, 128   /* Gray 700 @ 50% */

/* ============================================================================
 * ID Generation
 * ============================================================================ */

/* Generate unique ID from string using FNV-1a hash */
uint32_t cs_hash_id(const char *str);

/* Convenience macro for component IDs */
#define CS_ID(name) cs_hash_id(name)

/* ============================================================================
 * Custom Allocator Support
 * ============================================================================ */

/* Function pointers for custom memory allocation.
 * Allows integration with arena allocators or debugging allocators. */
typedef void* (*CsAllocFn)(size_t size, void *user_data);
typedef void* (*CsReallocFn)(void *ptr, size_t size, void *user_data);
typedef void  (*CsFreeFn)(void *ptr, void *user_data);

/* Allocator configuration.
 * Set all three functions, or set all to NULL for default (malloc/realloc/free). */
typedef struct {
    CsAllocFn alloc;
    CsReallocFn realloc;
    CsFreeFn free;
    void *user_data;  /* Passed to alloc/realloc/free functions */
} CsAllocator;

/* Set custom allocator (call before cs_init, or pass NULL for defaults).
 * Thread-safe: each thread can have its own allocator. */
void cs_set_allocator(const CsAllocator *allocator);

/* Get current allocator (returns internal default if none set) */
const CsAllocator* cs_get_allocator(void);

/* ============================================================================
 * Core API
 * ============================================================================ */

/* Initialize component system (call once at startup, or once per thread if multi-threaded) */
void cs_init(void);

/* Frame lifecycle - call at start/end of each frame */
void cs_frame_begin(void);
void cs_frame_end(float dt);

/* ============================================================================
 * Focus Management
 * ============================================================================ */

/* Get/set focused element ID (0 = none focused) */
uint32_t cs_focused_id(void);
void cs_focus(uint32_t id);
void cs_blur(void);

/* Cursor state for focused text input */
int cs_cursor_pos(void);
int cs_selection_start(void);  /* -1 if no selection */
bool cs_cursor_visible(void);

/* Get focused element bounds (for cursor rendering) */
bool cs_focused_bounds(float *x, float *y, float *w, float *h);
float cs_focused_x(void);
float cs_focused_y(void);
float cs_focused_w(void);
float cs_focused_h(void);

/* Get focused input text (for cursor rendering) */
const char* cs_focused_text(void);
int cs_focused_text_len(void);

/* ============================================================================
 * Input Routing (call from platform event handlers)
 * ============================================================================ */

/* Route keyboard to focused element. Returns true if consumed. */
bool cs_key_down(int key_code, bool shift, bool ctrl);
bool cs_key_char(uint32_t char_code);

/* Set pending click for this frame (call on mousedown before rendering) */
void cs_set_pending_click(void);

/* Set pointer position (called automatically by cs_clay_set_pointer) */
void cs_set_pointer(float x, float y);
void cs_set_pointer_down(bool down);
bool cs_is_pointer_down(void);

/* Drag tracking (for sliders, etc.) */
uint32_t cs_get_dragging_id(void);
void cs_set_dragging_id(uint32_t id);

/* Get pointer position */
float cs_pointer_x(void);
float cs_pointer_y(void);

/* ============================================================================
 * Tab Navigation
 * ============================================================================ */

/* Register a focusable element (called by components during frame) */
void cs_register_focusable(uint32_t id);

/* Focus next/previous element in tab order. Returns true if focus changed. */
bool cs_focus_next(void);
bool cs_focus_prev(void);

/* Get count of registered focusable elements (for testing/debugging) */
int cs_focusable_count(void);

/* ============================================================================
 * Color Utilities
 * ============================================================================ */

/* Pack RGBA color (0-255 each) into uint32_t */
static inline uint32_t cs_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | a;
}

/* Color constants - Tailwind-inspired palette */
#define CS_COLOR_TRANSPARENT cs_color(0, 0, 0, 0)
#define CS_COLOR_WHITE       cs_color(255, 255, 255, 255)
#define CS_COLOR_BLACK       cs_color(0, 0, 0, 255)

/* Grays */
#define CS_COLOR_GRAY_50     cs_color(249, 250, 251, 255)
#define CS_COLOR_GRAY_100    cs_color(243, 244, 246, 255)
#define CS_COLOR_GRAY_200    cs_color(229, 231, 235, 255)
#define CS_COLOR_GRAY_300    cs_color(209, 213, 219, 255)
#define CS_COLOR_GRAY_400    cs_color(156, 163, 175, 255)
#define CS_COLOR_GRAY_500    cs_color(107, 114, 128, 255)
#define CS_COLOR_GRAY_600    cs_color(75, 85, 99, 255)
#define CS_COLOR_GRAY_700    cs_color(55, 65, 81, 255)
#define CS_COLOR_GRAY_800    cs_color(31, 41, 55, 255)
#define CS_COLOR_GRAY_900    cs_color(17, 24, 39, 255)

/* Primary colors */
#define CS_COLOR_RED_500     cs_color(239, 68, 68, 255)
#define CS_COLOR_RED_600     cs_color(220, 38, 38, 255)
#define CS_COLOR_GREEN_500   cs_color(34, 197, 94, 255)
#define CS_COLOR_GREEN_600   cs_color(22, 163, 74, 255)
#define CS_COLOR_BLUE_500    cs_color(59, 130, 246, 255)
#define CS_COLOR_BLUE_600    cs_color(37, 99, 235, 255)
#define CS_COLOR_BLUE_700    cs_color(29, 78, 216, 255)

/* Semantic colors (uint32_t packed) */
#define CS_COLOR_DANGER_U32  CS_COLOR_RED_500
#define CS_COLOR_SUCCESS_U32 CS_COLOR_GREEN_500
#define CS_COLOR_PRIMARY_U32 CS_COLOR_BLUE_500

/* ============================================================================
 * Math Utilities
 * ============================================================================ */

static inline int cs_min_i(int a, int b) { return a < b ? a : b; }
static inline int cs_max_i(int a, int b) { return a > b ? a : b; }
static inline int cs_clamp_i(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float cs_min_f(float a, float b) { return a < b ? a : b; }
static inline float cs_max_f(float a, float b) { return a > b ? a : b; }
static inline float cs_clamp_f(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

#ifdef __cplusplus
}
#endif

#endif /* CS_COMMON_H */
