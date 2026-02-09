/*
 * sh_render.h - Shared Rendering Utilities
 *
 * SIMD-optimized span fill and alpha blending for software rasterizers.
 * Used by Carta (map tiles) and ClayShards software renderer.
 *
 * Features:
 *   - SIMD-accelerated horizontal span fill (AVX2/SSE2/NEON)
 *   - Fast approximate alpha blending
 *   - Portable with scalar fallback
 *
 * Usage:
 *   uint8_t *pixels = ...;  // RGBA buffer, row-major
 *   int width = 800;
 *   sh_fill_span(pixels, width, 100, 10, 200, 0xFF3366FF);  // Fill span
 *   sh_blend_pixel(pixels, width, 50, 20, 0x8833FF66);      // Blend single pixel
 */

#ifndef SH_RENDER_H
#define SH_RENDER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Color Macros
 * ============================================================================ */

/* Pack RGBA color components (each 0-255) */
#define SH_RGBA(r, g, b, a) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                             ((uint32_t)(g) << 8) | (uint32_t)(r))
#define SH_RGB(r, g, b)     SH_RGBA(r, g, b, 255)

/* Extract color components from packed uint32_t */
#define SH_COLOR_R(c)  ((uint8_t)((c) & 0xFF))
#define SH_COLOR_G(c)  ((uint8_t)(((c) >> 8) & 0xFF))
#define SH_COLOR_B(c)  ((uint8_t)(((c) >> 16) & 0xFF))
#define SH_COLOR_A(c)  ((uint8_t)(((c) >> 24) & 0xFF))

/* ============================================================================
 * Span Fill (SIMD-optimized)
 * ============================================================================ */

/*
 * Fill a horizontal span of pixels with a color.
 *
 * Fast horizontal span fill with SIMD acceleration when available.
 * Handles bounds checking and alpha blending.
 *
 * @param pixels    RGBA pixel buffer (row-major, 4 bytes/pixel)
 * @param buf_width Buffer width in pixels
 * @param buf_height Buffer height in pixels
 * @param y         Row index (0-based)
 * @param x_start   Start column (inclusive, will be clamped)
 * @param x_end     End column (inclusive, will be clamped)
 * @param color     Packed RGBA color (use SH_RGBA macro)
 *
 * Notes:
 *   - If y is out of bounds, returns immediately
 *   - x_start/x_end are clamped to buffer bounds
 *   - Uses SIMD for opaque colors (alpha = 255)
 *   - Uses alpha blending for semi-transparent colors
 */
void sh_fill_span(uint8_t *pixels, int buf_width, int buf_height,
                  int y, int x_start, int x_end, uint32_t color);

/*
 * Fill a horizontal span without bounds checking.
 *
 * Faster version that assumes all coordinates are valid.
 * Use when you've already validated bounds.
 *
 * @param row       Pointer to start of row (pixels + y * width * 4)
 * @param x_start   Start column (must be >= 0)
 * @param x_end     End column (must be < buffer width)
 * @param color     Packed RGBA color
 */
void sh_fill_span_unchecked(uint8_t *row, int x_start, int x_end, uint32_t color);

/* ============================================================================
 * Pixel Blending
 * ============================================================================ */

/*
 * Blend a single pixel with alpha compositing.
 *
 * Uses standard Porter-Duff source-over blending:
 *   out = src * src_a + dst * (1 - src_a)
 *
 * @param pixels    RGBA pixel buffer
 * @param buf_width Buffer width in pixels
 * @param x         Column index
 * @param y         Row index
 * @param color     Packed RGBA color
 *
 * Notes:
 *   - Returns immediately if (x, y) is out of bounds
 *   - Fast path for fully opaque (alpha = 255) and transparent (alpha = 0)
 */
void sh_blend_pixel(uint8_t *pixels, int buf_width, int buf_height,
                    int x, int y, uint32_t color);

/*
 * Blend a single pixel without bounds checking.
 *
 * @param pixel     Pointer to pixel (pixels + (y * width + x) * 4)
 * @param color     Packed RGBA color
 */
void sh_blend_pixel_unchecked(uint8_t *pixel, uint32_t color);

/*
 * Get a pixel value from a buffer.
 *
 * @param pixels    RGBA pixel buffer
 * @param buf_width Buffer width in pixels
 * @param buf_height Buffer height in pixels
 * @param x         Column index
 * @param y         Row index
 * @return          Packed RGBA color, or 0 if out of bounds
 */
uint32_t sh_get_pixel(const uint8_t *pixels, int buf_width, int buf_height,
                      int x, int y);

/*
 * Set a pixel value without alpha blending.
 *
 * @param pixels    RGBA pixel buffer
 * @param buf_width Buffer width in pixels
 * @param buf_height Buffer height in pixels
 * @param x         Column index
 * @param y         Row index
 * @param color     Packed RGBA color
 */
void sh_set_pixel(uint8_t *pixels, int buf_width, int buf_height,
                  int x, int y, uint32_t color);

/* ============================================================================
 * Buffer Operations
 * ============================================================================ */

/*
 * Clear entire buffer to a solid color.
 *
 * Uses SIMD when available for maximum throughput.
 *
 * @param pixels    RGBA pixel buffer
 * @param buf_width Buffer width in pixels
 * @param buf_height Buffer height in pixels
 * @param color     Packed RGBA color
 */
void sh_clear_buffer(uint8_t *pixels, int buf_width, int buf_height,
                     uint32_t color);

/*
 * Clear a rectangular region to a solid color.
 *
 * @param pixels    RGBA pixel buffer
 * @param buf_width Buffer width in pixels (stride = buf_width * 4)
 * @param buf_height Buffer height in pixels
 * @param x, y      Top-left corner of rectangle
 * @param w, h      Width and height of rectangle
 * @param color     Packed RGBA color
 */
void sh_clear_rect(uint8_t *pixels, int buf_width, int buf_height,
                   int x, int y, int w, int h, uint32_t color);

#ifdef __cplusplus
}
#endif

#endif /* SH_RENDER_H */
