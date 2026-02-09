/*
 * sh_font.h - MSDF Font API
 *
 * Multi-channel Signed Distance Field font rendering for map labels.
 * Fonts are embedded at build time for zero runtime dependencies.
 *
 * Usage:
 *   const SHFont *font = sh_font_get_default();
 *   float width = sh_font_text_width(font, "Hello", 16.0f);
 *   const SHGlyph *g = sh_font_get_glyph(font, 'A');
 */

#ifndef SH_FONT_H
#define SH_FONT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

/* Glyph bounding box (normalized coordinates, relative to em-size) */
typedef struct {
    float left;
    float bottom;
    float right;
    float top;
} SHFontBounds;

/* Single glyph metrics */
typedef struct {
    uint32_t unicode;        /* Unicode codepoint */
    float advance;           /* Horizontal advance (normalized) */
    SHFontBounds plane;      /* Glyph bounds in em-space */
    SHFontBounds atlas;      /* Glyph bounds in atlas (pixels) */
} SHGlyph;

/* MSDF Font */
typedef struct {
    /* Atlas info */
    int atlas_width;
    int atlas_height;
    float distance_range;    /* MSDF distance field range */
    float em_size;           /* Font size used to generate atlas */

    /* Glyph table */
    const SHGlyph *glyphs;
    int glyph_count;

    /* Fast ASCII lookup (NULL for non-ASCII) */
    const SHGlyph *ascii[128];

    /* Atlas pixel data (RGBA, row-major) */
    const uint8_t *atlas_data;
    size_t atlas_data_size;
} SHFont;

/* ============================================================================
 * Font Access
 * ============================================================================ */

/*
 * Get the default embedded UI font.
 * Returns NULL if no font is embedded (build configuration).
 * The returned pointer is static and should not be freed.
 */
const SHFont *sh_font_get_default(void);

/* ============================================================================
 * Glyph Lookup
 * ============================================================================ */

/*
 * Get glyph for a Unicode codepoint.
 * Returns NULL if glyph not found.
 */
const SHGlyph *sh_font_get_glyph(const SHFont *font, uint32_t codepoint);

/*
 * Get glyph advance (width) for a codepoint.
 * Returns default advance if glyph not found.
 */
float sh_font_get_advance(const SHFont *font, uint32_t codepoint);

/* ============================================================================
 * Text Measurement
 * ============================================================================ */

/*
 * Measure text width at given font size.
 * Handles UTF-8 encoded strings.
 *
 * @param font      Font to use
 * @param text      UTF-8 encoded text (NULL-terminated)
 * @param font_size Desired font size in pixels
 * @return          Width in pixels
 */
float sh_font_text_width(const SHFont *font, const char *text, float font_size);

/*
 * Measure text width with character limit.
 *
 * @param font      Font to use
 * @param text      UTF-8 encoded text
 * @param max_chars Maximum characters to measure (or -1 for all)
 * @param font_size Desired font size in pixels
 * @return          Width in pixels
 */
float sh_font_text_width_n(const SHFont *font, const char *text,
                           int max_chars, float font_size);

/*
 * Get line height for a font size.
 * Typically 1.2x the font size.
 */
float sh_font_line_height(const SHFont *font, float font_size);

/*
 * Get the ascent (height above baseline) for a font size.
 */
float sh_font_ascent(const SHFont *font, float font_size);

/*
 * Get the descent (depth below baseline) for a font size.
 */
float sh_font_descent(const SHFont *font, float font_size);

/* ============================================================================
 * UTF-8 Utilities
 * ============================================================================ */

/*
 * Decode a single UTF-8 codepoint from a string.
 *
 * @param str       Pointer to UTF-8 string
 * @param codepoint Output codepoint (set even on error, to replacement char)
 * @return          Number of bytes consumed (1-4), or 1 on error
 */
int sh_utf8_decode(const char *str, uint32_t *codepoint);

/*
 * Get the number of UTF-8 codepoints in a string.
 */
int sh_utf8_strlen(const char *str);

/* ============================================================================
 * MSDF Sampling (for software rendering)
 * ============================================================================ */

/*
 * Sample the MSDF atlas at given coordinates.
 * Returns the median of RGB channels (the signed distance).
 *
 * @param font  Font with atlas data
 * @param x     Atlas X coordinate (pixels)
 * @param y     Atlas Y coordinate (pixels)
 * @return      Signed distance value [0, 255]
 */
uint8_t sh_font_sample_msdf(const SHFont *font, int x, int y);

/*
 * Check if a point is inside a glyph using MSDF.
 *
 * @param font      Font with atlas data
 * @param glyph     Glyph to sample
 * @param local_x   X position within glyph bounds [0, 1]
 * @param local_y   Y position within glyph bounds [0, 1]
 * @return          1 if inside (should draw), 0 if outside
 */
int sh_font_msdf_inside(const SHFont *font, const SHGlyph *glyph,
                        float local_x, float local_y);

/*
 * Get coverage (alpha) for anti-aliased MSDF rendering.
 * Uses nearest-neighbor sampling.
 *
 * @param font      Font with atlas data
 * @param glyph     Glyph to sample
 * @param local_x   X position within glyph bounds [0, 1]
 * @param local_y   Y position within glyph bounds [0, 1]
 * @param font_size Font size (affects edge sharpness)
 * @return          Coverage value [0.0, 1.0]
 */
float sh_font_msdf_coverage(const SHFont *font, const SHGlyph *glyph,
                            float local_x, float local_y, float font_size);

/*
 * Get coverage with bilinear sampling for higher quality.
 * Smoother results than sh_font_msdf_coverage, especially at larger sizes.
 *
 * @param font      Font with atlas data
 * @param glyph     Glyph to sample
 * @param local_x   X position within glyph bounds [0, 1]
 * @param local_y   Y position within glyph bounds [0, 1]
 * @param font_size Font size (affects edge sharpness)
 * @return          Coverage value [0.0, 1.0]
 */
float sh_font_msdf_coverage_bilinear(const SHFont *font, const SHGlyph *glyph,
                                      float local_x, float local_y, float font_size);

/*
 * Get coverage with adjustable threshold for halo/outline effects.
 * Uses bilinear sampling and smoothstep for high quality.
 *
 * @param font      Font with atlas data
 * @param glyph     Glyph to sample
 * @param local_x   X position within glyph bounds [0, 1]
 * @param local_y   Y position within glyph bounds [0, 1]
 * @param font_size Font size (affects edge sharpness)
 * @param threshold Edge threshold (0.5 = normal, lower = expanded for halo)
 * @return          Coverage value [0.0, 1.0]
 */
float sh_font_msdf_coverage_threshold(const SHFont *font, const SHGlyph *glyph,
                                       float local_x, float local_y,
                                       float font_size, float threshold);

/*
 * Sample MSDF atlas with bilinear interpolation.
 * Returns the median of RGB channels as a normalized float [0, 1].
 *
 * @param font    Font with atlas data
 * @param atlas_x Atlas X coordinate (can be fractional)
 * @param atlas_y Atlas Y coordinate (can be fractional)
 * @return        Signed distance value [0.0, 1.0] (0.5 = edge)
 */
float sh_font_sample_msdf_bilinear(const SHFont *font, float atlas_x, float atlas_y);

/* ============================================================================
 * Glyph Rendering (Software Rasterization)
 * ============================================================================ */

/* Scissor rectangle for clipping (NULL = no clipping) */
typedef struct {
    int x, y, w, h;
} SHScissor;

/*
 * Render a single glyph to a pixel buffer.
 *
 * Uses MSDF sampling with bilinear interpolation and proper edge coverage.
 * Extends the glyph bounding box by 1 pixel on each side to capture
 * anti-aliasing at the edges (matching WebGL behavior).
 *
 * @param pixels     RGBA pixel buffer (row-major, 4 bytes/pixel)
 * @param buf_width  Buffer width in pixels
 * @param buf_height Buffer height in pixels
 * @param font       Font with atlas data
 * @param glyph      Glyph to render
 * @param x          X position of glyph left edge (screen pixels)
 * @param y          Y position of glyph top edge (screen pixels)
 * @param font_size  Font size in pixels
 * @param r, g, b    Color components (0-255)
 * @param alpha      Alpha/opacity (0-255)
 * @param threshold  Edge threshold (0.5 = normal, lower = expanded for halo)
 */
void sh_font_render_glyph(uint8_t *pixels, int buf_width, int buf_height,
                          const SHFont *font, const SHGlyph *glyph,
                          int x, int y, float font_size,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t alpha,
                          float threshold);

/*
 * Render a single glyph with scissor clipping.
 *
 * Same as sh_font_render_glyph but with optional scissor rectangle.
 * Pixels outside the scissor region are not drawn.
 *
 * @param scissor    Scissor rectangle (NULL = no clipping)
 */
void sh_font_render_glyph_clipped(uint8_t *pixels, int buf_width, int buf_height,
                                   const SHFont *font, const SHGlyph *glyph,
                                   int x, int y, float font_size,
                                   uint8_t r, uint8_t g, uint8_t b, uint8_t alpha,
                                   float threshold, const SHScissor *scissor);

/*
 * Render a text string to a pixel buffer.
 *
 * Convenience function that iterates UTF-8 codepoints and renders each glyph.
 *
 * @param pixels     RGBA pixel buffer (row-major, 4 bytes/pixel)
 * @param buf_width  Buffer width in pixels
 * @param buf_height Buffer height in pixels
 * @param font       Font with atlas data
 * @param text       UTF-8 encoded text
 * @param len        Text length in bytes (-1 for null-terminated)
 * @param x          X position of text left edge (screen pixels)
 * @param y          Y position of text top edge (screen pixels, NOT baseline)
 * @param font_size  Font size in pixels
 * @param r, g, b    Color components (0-255)
 * @param alpha      Alpha/opacity (0-255)
 */
void sh_font_render_text(uint8_t *pixels, int buf_width, int buf_height,
                         const SHFont *font, const char *text, int len,
                         float x, float y, float font_size,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t alpha);

/*
 * Render a text string with scissor clipping.
 *
 * Same as sh_font_render_text but with optional scissor rectangle.
 *
 * @param scissor    Scissor rectangle (NULL = no clipping)
 */
void sh_font_render_text_clipped(uint8_t *pixels, int buf_width, int buf_height,
                                  const SHFont *font, const char *text, int len,
                                  float x, float y, float font_size,
                                  uint8_t r, uint8_t g, uint8_t b, uint8_t alpha,
                                  const SHScissor *scissor);

#ifdef __cplusplus
}
#endif

#endif /* SH_FONT_H */
