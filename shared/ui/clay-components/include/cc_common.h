/**
 * Clay Components - Common Utilities
 *
 * Shared types, colors, and utility functions for all components.
 */

#ifndef CC_COMMON_H
#define CC_COMMON_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

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

/* Semantic colors */
#define CC_COLOR_PRIMARY     CC_COLOR_BLUE_500
#define CC_COLOR_PRIMARY_HOVER CC_COLOR_BLUE_600
#define CC_COLOR_DANGER      CC_COLOR_RED_500
#define CC_COLOR_SUCCESS     CC_COLOR_GREEN_500

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
