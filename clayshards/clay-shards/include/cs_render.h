/**
 * cs_render.h - Shared Rendering Utilities for ClayShards Backends
 *
 * Common utilities used by all ClayShards renderers (TUI, WebGL, software).
 * Provides scissor stack management and color utilities.
 *
 * Usage:
 *   CsScissorStack stack;
 *   cs_scissor_init(&stack, screen_width, screen_height);
 *
 *   cs_scissor_push(&stack, x, y, w, h);
 *   // ... draw within clip region ...
 *   cs_scissor_pop(&stack);
 */

#ifndef CS_RENDER_H
#define CS_RENDER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

#define CS_MAX_SCISSOR_DEPTH 16

/* ============================================================================
 * Scissor Stack
 * ============================================================================ */

/**
 * Scissor region (clipping rectangle).
 */
typedef struct {
    int x, y, w, h;
} CsScissor;

/**
 * Scissor stack for hierarchical clipping.
 * Each push intersects with the current clip region.
 */
typedef struct {
    CsScissor stack[CS_MAX_SCISSOR_DEPTH];
    int depth;
    int screen_w;
    int screen_h;
} CsScissorStack;

/**
 * Initialize a scissor stack with screen dimensions.
 *
 * @param s Scissor stack to initialize
 * @param screen_w Screen/buffer width
 * @param screen_h Screen/buffer height
 */
void cs_scissor_init(CsScissorStack *s, int screen_w, int screen_h);

/**
 * Push a scissor region onto the stack.
 * The new region is intersected with the current clip region.
 *
 * @param s Scissor stack
 * @param x Region x coordinate
 * @param y Region y coordinate
 * @param w Region width
 * @param h Region height
 */
void cs_scissor_push(CsScissorStack *s, int x, int y, int w, int h);

/**
 * Pop the current scissor region from the stack.
 *
 * @param s Scissor stack
 */
void cs_scissor_pop(CsScissorStack *s);

/**
 * Get the current scissor region.
 *
 * @param s Scissor stack
 * @return Current clip region, or full screen if stack is empty
 */
CsScissor cs_scissor_current(const CsScissorStack *s);

/**
 * Test if a point is inside the current scissor region.
 *
 * @param s Scissor stack
 * @param x Point x coordinate
 * @param y Point y coordinate
 * @return true if point is inside clip region
 */
bool cs_scissor_test_point(const CsScissorStack *s, int x, int y);

/**
 * Test if a rectangle overlaps with the current scissor region.
 *
 * @param s Scissor stack
 * @param x Rectangle x coordinate
 * @param y Rectangle y coordinate
 * @param w Rectangle width
 * @param h Rectangle height
 * @return true if rectangle overlaps clip region
 */
bool cs_scissor_test_rect(const CsScissorStack *s, int x, int y, int w, int h);

/**
 * Clip a rectangle to the current scissor region.
 * Modifies the rectangle in place.
 *
 * @param s Scissor stack
 * @param x Pointer to rectangle x (modified)
 * @param y Pointer to rectangle y (modified)
 * @param w Pointer to rectangle width (modified)
 * @param h Pointer to rectangle height (modified)
 * @return true if rectangle has non-zero area after clipping
 */
bool cs_scissor_clip_rect(const CsScissorStack *s, int *x, int *y, int *w, int *h);

/* ============================================================================
 * Color Utilities
 * ============================================================================ */

/**
 * Unpack a uint32_t color into RGBA components.
 * Assumes color is packed as: (r << 24) | (g << 16) | (b << 8) | a
 * (Same format as cs_color() in cs_common.h)
 *
 * @param color Packed RGBA color
 * @param r Output red (0-255)
 * @param g Output green (0-255)
 * @param b Output blue (0-255)
 * @param a Output alpha (0-255)
 */
static inline void cs_unpack_color(uint32_t color,
                                   uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a)
{
    *r = (color >> 24) & 0xFF;
    *g = (color >> 16) & 0xFF;
    *b = (color >> 8) & 0xFF;
    *a = color & 0xFF;
}

/**
 * Pack RGBA components into a uint32_t color.
 * Same as cs_color() in cs_common.h but available here for convenience.
 *
 * @param r Red (0-255)
 * @param g Green (0-255)
 * @param b Blue (0-255)
 * @param a Alpha (0-255)
 * @return Packed color
 */
static inline uint32_t cs_pack_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | a;
}

/**
 * Blend two colors using alpha compositing (source over).
 *
 * @param src Source color (foreground)
 * @param dst Destination color (background)
 * @return Blended color
 */
uint32_t cs_blend_color(uint32_t src, uint32_t dst);

/**
 * Lerp between two colors.
 *
 * @param a First color
 * @param b Second color
 * @param t Interpolation factor (0.0 = a, 1.0 = b)
 * @return Interpolated color
 */
uint32_t cs_lerp_color(uint32_t a, uint32_t b, float t);

#ifdef __cplusplus
}
#endif

#endif /* CS_RENDER_H */
