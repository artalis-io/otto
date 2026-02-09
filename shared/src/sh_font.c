/*
 * sh_font.c - MSDF Font Implementation
 *
 * Provides text measurement and MSDF sampling for map label rendering.
 * Font data is embedded at build time via sh_font_data.c.
 */

#include "sh_font.h"
#include <string.h>
#include <math.h>

/* ============================================================================
 * External Font Data (generated at build time)
 * ============================================================================ */

/* Defined in sh_font_data.c (generated) */
extern const SHFont sh_font_ui;

/* ============================================================================
 * Font Access
 * ============================================================================ */

const SHFont *sh_font_get_default(void)
{
    /* Return embedded font if available */
    if (sh_font_ui.glyph_count > 0) {
        return &sh_font_ui;
    }
    return NULL;
}

/* ============================================================================
 * Glyph Lookup
 * ============================================================================ */

const SHGlyph *sh_font_get_glyph(const SHFont *font, uint32_t codepoint)
{
    if (!font || !font->glyphs) {
        return NULL;
    }

    /* Fast path for ASCII */
    if (codepoint < 128 && font->ascii[codepoint]) {
        return font->ascii[codepoint];
    }

    /* Binary search for non-ASCII (glyphs sorted by unicode) */
    int lo = 0;
    int hi = font->glyph_count - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint32_t mid_cp = font->glyphs[mid].unicode;

        if (mid_cp == codepoint) {
            return &font->glyphs[mid];
        } else if (mid_cp < codepoint) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    return NULL;
}

float sh_font_get_advance(const SHFont *font, uint32_t codepoint)
{
    const SHGlyph *g = sh_font_get_glyph(font, codepoint);
    if (g) {
        return g->advance;
    }

    /* Default advance for missing glyphs (roughly half em-width) */
    return 0.5f;
}

/* ============================================================================
 * UTF-8 Decoding
 * ============================================================================ */

/*
 * UTF-8 byte sequence lengths based on leading byte.
 * 0 = invalid leading byte.
 */
static const uint8_t utf8_lengths[256] = {
    /* 0x00-0x7F: ASCII (1 byte) */
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    /* 0x80-0xBF: continuation bytes (invalid as leading) */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0xC0-0xDF: 2-byte sequences */
    2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    /* 0xE0-0xEF: 3-byte sequences */
    3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,
    /* 0xF0-0xF7: 4-byte sequences */
    4,4,4,4,4,4,4,4,
    /* 0xF8-0xFF: invalid */
    0,0,0,0,0,0,0,0
};

/* Unicode replacement character for invalid sequences */
#define REPLACEMENT_CHAR 0xFFFD

int sh_utf8_decode(const char *str, uint32_t *codepoint)
{
    if (!str || !codepoint) {
        if (codepoint) *codepoint = REPLACEMENT_CHAR;
        return 1;
    }

    const uint8_t *s = (const uint8_t *)str;
    uint8_t lead = s[0];

    /* Handle NULL terminator */
    if (lead == 0) {
        *codepoint = 0;
        return 0;
    }

    int len = utf8_lengths[lead];

    if (len == 0) {
        /* Invalid leading byte */
        *codepoint = REPLACEMENT_CHAR;
        return 1;
    }

    if (len == 1) {
        /* ASCII */
        *codepoint = lead;
        return 1;
    }

    /* Multi-byte sequence */
    uint32_t cp = 0;

    switch (len) {
        case 2:
            /* Check for valid continuation byte */
            if ((s[1] & 0xC0) != 0x80) {
                *codepoint = REPLACEMENT_CHAR;
                return 1;
            }
            cp = ((lead & 0x1F) << 6) | (s[1] & 0x3F);
            /* Reject overlong encoding */
            if (cp < 0x80) {
                *codepoint = REPLACEMENT_CHAR;
                return 1;
            }
            break;

        case 3:
            if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) {
                *codepoint = REPLACEMENT_CHAR;
                return 1;
            }
            cp = ((lead & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
            /* Reject overlong and surrogate range */
            if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) {
                *codepoint = REPLACEMENT_CHAR;
                return 1;
            }
            break;

        case 4:
            if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 ||
                (s[3] & 0xC0) != 0x80) {
                *codepoint = REPLACEMENT_CHAR;
                return 1;
            }
            cp = ((lead & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
                 ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
            /* Reject overlong and out-of-range */
            if (cp < 0x10000 || cp > 0x10FFFF) {
                *codepoint = REPLACEMENT_CHAR;
                return 1;
            }
            break;

        default:
            *codepoint = REPLACEMENT_CHAR;
            return 1;
    }

    *codepoint = cp;
    return len;
}

int sh_utf8_strlen(const char *str)
{
    if (!str) return 0;

    int count = 0;
    const char *p = str;

    while (*p) {
        uint32_t cp;
        int len = sh_utf8_decode(p, &cp);
        if (len == 0) break;
        p += len;
        count++;
    }

    return count;
}

/* ============================================================================
 * Text Measurement
 * ============================================================================ */

float sh_font_text_width(const SHFont *font, const char *text, float font_size)
{
    return sh_font_text_width_n(font, text, -1, font_size);
}

float sh_font_text_width_n(const SHFont *font, const char *text,
                           int max_chars, float font_size)
{
    if (!font || !text || font_size <= 0.0f) {
        return 0.0f;
    }

    float width = 0.0f;
    const char *p = text;
    int chars = 0;

    while (*p && (max_chars < 0 || chars < max_chars)) {
        uint32_t cp;
        int len = sh_utf8_decode(p, &cp);
        if (len == 0 || cp == 0) break;

        width += sh_font_get_advance(font, cp);
        p += len;
        chars++;
    }

    return width * font_size;
}

float sh_font_line_height(const SHFont *font, float font_size)
{
    (void)font;  /* Currently fixed ratio */
    return font_size * 1.2f;
}

float sh_font_ascent(const SHFont *font, float font_size)
{
    (void)font;
    return font_size * 0.8f;  /* Typical ascent ratio */
}

float sh_font_descent(const SHFont *font, float font_size)
{
    (void)font;
    return font_size * 0.2f;  /* Typical descent ratio */
}

/* ============================================================================
 * MSDF Sampling
 * ============================================================================ */

/*
 * Get median of three values (used for MSDF).
 */
static inline uint8_t median3(uint8_t a, uint8_t b, uint8_t c)
{
    if (a > b) { uint8_t t = a; a = b; b = t; }
    if (b > c) { uint8_t t = b; b = c; c = t; }
    if (a > b) { uint8_t t = a; a = b; b = t; }
    return b;
}

uint8_t sh_font_sample_msdf(const SHFont *font, int x, int y)
{
    if (!font || !font->atlas_data) {
        return 0;
    }

    /* Bounds check */
    if (x < 0 || x >= font->atlas_width ||
        y < 0 || y >= font->atlas_height) {
        return 0;
    }

    /* Atlas is RGBA, row-major */
    size_t idx = ((size_t)y * (size_t)font->atlas_width + (size_t)x) * 4;

    /* Safety check for atlas bounds */
    if (idx + 2 >= font->atlas_data_size) {
        return 0;
    }

    uint8_t r = font->atlas_data[idx + 0];
    uint8_t g = font->atlas_data[idx + 1];
    uint8_t b = font->atlas_data[idx + 2];

    return median3(r, g, b);
}

int sh_font_msdf_inside(const SHFont *font, const SHGlyph *glyph,
                        float local_x, float local_y)
{
    if (!font || !glyph) {
        return 0;
    }

    /* Map local coordinates [0,1] to atlas coordinates */
    float atlas_x = glyph->atlas.left + local_x * (glyph->atlas.right - glyph->atlas.left);
    float atlas_y = glyph->atlas.bottom + local_y * (glyph->atlas.top - glyph->atlas.bottom);

    uint8_t dist = sh_font_sample_msdf(font, (int)atlas_x, (int)atlas_y);

    /* MSDF: 128 is the edge, >128 is inside */
    return dist >= 128;
}

float sh_font_msdf_coverage(const SHFont *font, const SHGlyph *glyph,
                            float local_x, float local_y, float font_size)
{
    if (!font || !glyph || font_size <= 0.0f) {
        return 0.0f;
    }

    /* Map local coordinates [0,1] to atlas coordinates */
    float atlas_x = glyph->atlas.left + local_x * (glyph->atlas.right - glyph->atlas.left);
    float atlas_y = glyph->atlas.bottom + local_y * (glyph->atlas.top - glyph->atlas.bottom);

    uint8_t dist_byte = sh_font_sample_msdf(font, (int)atlas_x, (int)atlas_y);

    /* Convert to signed distance [-1, 1] range */
    float dist = (dist_byte - 128.0f) / 128.0f;

    /* Scale by font size and distance range for proper anti-aliasing.
     * Larger fonts = sharper edges, smaller fonts = more blur.
     * The distance_range tells us how many pixels the SDF spans.
     */
    float screen_px_range = font->distance_range * (font_size / font->em_size);
    if (screen_px_range < 1.0f) screen_px_range = 1.0f;

    /* Apply smoothstep for anti-aliasing */
    float edge = 0.5f / screen_px_range;
    float coverage = (dist + edge) / (2.0f * edge);

    /* Clamp to [0, 1] */
    if (coverage < 0.0f) coverage = 0.0f;
    if (coverage > 1.0f) coverage = 1.0f;

    return coverage;
}

/* ============================================================================
 * Bilinear Sampling and High-Quality Coverage
 * ============================================================================ */

/*
 * Clamp float to range [a, b].
 */
static inline float clampf(float x, float a, float b)
{
    return x < a ? a : (x > b ? b : x);
}

/*
 * Linear interpolation.
 */
static inline float lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

/*
 * Smoothstep for anti-aliased edges.
 * Returns smooth interpolation between 0 and 1.
 */
static inline float smoothstepf(float edge0, float edge1, float x)
{
    float t = clampf((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/*
 * Median of three floats (for MSDF).
 */
static inline float median3f(float a, float b, float c)
{
    float max_ab = a > b ? a : b;
    float min_ab = a < b ? a : b;
    float max_bc = b > c ? b : c;
    float min_max = max_ab < c ? max_ab : c;
    return min_ab > max_bc ? min_ab : (min_max > max_bc ? max_bc : min_max);
}

float sh_font_sample_msdf_bilinear(const SHFont *font, float atlas_x, float atlas_y)
{
    if (!font || !font->atlas_data) {
        return 0.0f;
    }

    /* Clamp to atlas bounds */
    float x = clampf(atlas_x, 0.0f, (float)(font->atlas_width - 1));
    float y = clampf(atlas_y, 0.0f, (float)(font->atlas_height - 1));

    /* Get integer coordinates and fractional parts */
    int x0 = (int)x;
    int y0 = (int)y;
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    /* Clamp to bounds */
    if (x1 >= font->atlas_width) x1 = font->atlas_width - 1;
    if (y1 >= font->atlas_height) y1 = font->atlas_height - 1;

    float tx = x - (float)x0;
    float ty = y - (float)y0;

    /* Sample four corners (RGBA atlas, we need RGB for MSDF) */
    size_t idx00 = ((size_t)y0 * (size_t)font->atlas_width + (size_t)x0) * 4;
    size_t idx10 = ((size_t)y0 * (size_t)font->atlas_width + (size_t)x1) * 4;
    size_t idx01 = ((size_t)y1 * (size_t)font->atlas_width + (size_t)x0) * 4;
    size_t idx11 = ((size_t)y1 * (size_t)font->atlas_width + (size_t)x1) * 4;

    /* Bounds check */
    if (idx11 + 2 >= font->atlas_data_size) {
        return 0.0f;
    }

    /* Convert to float [0, 1] */
    const float inv255 = 1.0f / 255.0f;

    float r00 = font->atlas_data[idx00 + 0] * inv255;
    float g00 = font->atlas_data[idx00 + 1] * inv255;
    float b00 = font->atlas_data[idx00 + 2] * inv255;

    float r10 = font->atlas_data[idx10 + 0] * inv255;
    float g10 = font->atlas_data[idx10 + 1] * inv255;
    float b10 = font->atlas_data[idx10 + 2] * inv255;

    float r01 = font->atlas_data[idx01 + 0] * inv255;
    float g01 = font->atlas_data[idx01 + 1] * inv255;
    float b01 = font->atlas_data[idx01 + 2] * inv255;

    float r11 = font->atlas_data[idx11 + 0] * inv255;
    float g11 = font->atlas_data[idx11 + 1] * inv255;
    float b11 = font->atlas_data[idx11 + 2] * inv255;

    /* Bilinearly interpolate each channel FIRST (matches GPU texture filtering)
     * This is critical for MSDF - must interpolate RGB, then compute median */
    float r0 = lerpf(r00, r10, tx);
    float r1 = lerpf(r01, r11, tx);
    float r = lerpf(r0, r1, ty);

    float g0 = lerpf(g00, g10, tx);
    float g1 = lerpf(g01, g11, tx);
    float g = lerpf(g0, g1, ty);

    float b0 = lerpf(b00, b10, tx);
    float b1 = lerpf(b01, b11, tx);
    float b = lerpf(b0, b1, ty);

    /* THEN compute median of the interpolated values */
    return median3f(r, g, b);
}

float sh_font_msdf_coverage_bilinear(const SHFont *font, const SHGlyph *glyph,
                                      float local_x, float local_y, float font_size)
{
    /* Just use the threshold version with threshold = 0.5 (normal edge) */
    return sh_font_msdf_coverage_threshold(font, glyph, local_x, local_y, font_size, 0.5f);
}

float sh_font_msdf_coverage_threshold(const SHFont *font, const SHGlyph *glyph,
                                       float local_x, float local_y,
                                       float font_size, float threshold)
{
    if (!font || !glyph || font_size <= 0.0f) {
        return 0.0f;
    }

    /* Map local coordinates [0,1] to atlas coordinates
     * Note: despite yOrigin=bottom in the atlas JSON, the actual bounds are
     * in PNG row coordinates where atlas.bottom < atlas.top numerically,
     * and lower values = closer to top of image = visual top of glyph */
    float atlas_x = glyph->atlas.left + local_x * (glyph->atlas.right - glyph->atlas.left);
    float atlas_y = glyph->atlas.bottom + local_y * (glyph->atlas.top - glyph->atlas.bottom);

    /* Sample with bilinear interpolation */
    float sd = sh_font_sample_msdf_bilinear(font, atlas_x, atlas_y);

    /* Calculate screen pixel range (same formula as WebGL renderer)
     * pxRange = distanceRange * (fontSize / emSize)
     */
    float pxRange = font->distance_range * (font_size / font->em_size);

    /* Match WebGL shader formula exactly:
     * screenPxDistance = pxRange * (sd - 0.5)
     * opacity = clamp(screenPxDistance + 0.5, 0.0, 1.0)
     *
     * For threshold != 0.5, adjust the edge position
     */
    float screenPxDist = pxRange * (sd - threshold);

    /* Linear clamp for anti-aliased edge (matches WebGL shader exactly) */
    return clampf(screenPxDist + 0.5f, 0.0f, 1.0f);
}

/* ============================================================================
 * Glyph Rendering
 * ============================================================================ */

#include "sh_render.h"
#include <math.h>

/* Helper: test if point is inside scissor rectangle */
static inline int scissor_test(const SHScissor *s, int x, int y)
{
    if (!s) return 1;  /* No scissor = always inside */
    return x >= s->x && x < s->x + s->w &&
           y >= s->y && y < s->y + s->h;
}

void sh_font_render_glyph_clipped(uint8_t *pixels, int buf_width, int buf_height,
                                   const SHFont *font, const SHGlyph *glyph,
                                   int x, int y, float font_size,
                                   uint8_t r, uint8_t g, uint8_t b, uint8_t alpha,
                                   float threshold, const SHScissor *scissor)
{
    if (!pixels || !font || !glyph || font_size <= 0.0f) {
        return;
    }

    /* Calculate glyph dimensions in screen pixels */
    float glyph_width = (glyph->plane.right - glyph->plane.left) * font_size;
    float glyph_height = (glyph->plane.top - glyph->plane.bottom) * font_size;

    if (glyph_width <= 0.0f || glyph_height <= 0.0f) {
        return;
    }

    /* Extend by 1 pixel on each side to capture MSDF anti-aliasing at edges
     * (WebGL renders a quad that covers the full UV range; we need to match) */
    int gx = x - 1;
    int gy = y - 1;
    int px_width = (int)ceilf(glyph_width) + 2;
    int px_height = (int)ceilf(glyph_height) + 2;

    /* Early bounds check against buffer */
    if (gx + px_width < 0 || gx >= buf_width ||
        gy + px_height < 0 || gy >= buf_height) {
        return;
    }

    /* Early scissor rejection: check if glyph bbox is completely outside scissor */
    if (scissor) {
        if (gx + px_width <= scissor->x || gx >= scissor->x + scissor->w ||
            gy + px_height <= scissor->y || gy >= scissor->y + scissor->h) {
            return;
        }
    }

    /* Sample each pixel in the glyph bounding box */
    for (int py = 0; py < px_height; py++) {
        int screen_y = gy + py;
        if (screen_y < 0 || screen_y >= buf_height) continue;

        for (int px = 0; px < px_width; px++) {
            int screen_x = gx + px;
            if (screen_x < 0 || screen_x >= buf_width) continue;

            /* Scissor test */
            if (!scissor_test(scissor, screen_x, screen_y)) continue;

            /* Map screen pixel to local glyph coordinates [0, 1]
             * Account for the 1-pixel extension on each side */
            float local_x = ((float)px - 0.5f) / glyph_width;
            float local_y = ((float)py - 0.5f) / glyph_height;

            /* Get MSDF coverage with threshold (uses bilinear sampling) */
            float coverage = sh_font_msdf_coverage_threshold(font, glyph,
                                                              local_x, local_y,
                                                              font_size, threshold);

            if (coverage <= 0.0f) continue;

            /* Apply coverage to alpha */
            uint8_t pixel_alpha = (uint8_t)(alpha * coverage);
            if (pixel_alpha == 0) continue;

            /* Blend pixel using sh_render */
            uint32_t color = SH_RGBA(r, g, b, pixel_alpha);
            sh_blend_pixel(pixels, buf_width, buf_height, screen_x, screen_y, color);
        }
    }
}

void sh_font_render_glyph(uint8_t *pixels, int buf_width, int buf_height,
                          const SHFont *font, const SHGlyph *glyph,
                          int x, int y, float font_size,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t alpha,
                          float threshold)
{
    sh_font_render_glyph_clipped(pixels, buf_width, buf_height,
                                  font, glyph, x, y, font_size,
                                  r, g, b, alpha, threshold, NULL);
}

void sh_font_render_text_clipped(uint8_t *pixels, int buf_width, int buf_height,
                                  const SHFont *font, const char *text, int len,
                                  float x, float y, float font_size,
                                  uint8_t r, uint8_t g, uint8_t b, uint8_t alpha,
                                  const SHScissor *scissor)
{
    if (!pixels || !font || !text || font_size <= 0.0f) {
        return;
    }

    if (len < 0) {
        len = 0;
        const char *p = text;
        while (*p++) len++;
    }

    /* y is top of text bounding box - calculate baseline */
    float baseline_y = y + sh_font_ascent(font, font_size);
    float cursor_x = x;

    for (int i = 0; i < len; ) {
        uint32_t codepoint;
        int bytes = sh_utf8_decode(text + i, &codepoint);

        const SHGlyph *glyph = sh_font_get_glyph(font, codepoint);
        if (!glyph) {
            i += bytes;
            continue;
        }

        /* Calculate glyph dimensions in screen pixels */
        float glyph_w = (glyph->plane.right - glyph->plane.left) * font_size;
        float glyph_h = (glyph->plane.top - glyph->plane.bottom) * font_size;

        if (glyph_w > 0.0f && glyph_h > 0.0f) {
            /* Calculate glyph position */
            float glyph_x = cursor_x + glyph->plane.left * font_size;
            float glyph_y = baseline_y - glyph->plane.top * font_size;

            sh_font_render_glyph_clipped(pixels, buf_width, buf_height,
                                          font, glyph,
                                          (int)glyph_x, (int)glyph_y, font_size,
                                          r, g, b, alpha, 0.5f, scissor);
        }

        cursor_x += glyph->advance * font_size;
        i += bytes;
    }
}

void sh_font_render_text(uint8_t *pixels, int buf_width, int buf_height,
                         const SHFont *font, const char *text, int len,
                         float x, float y, float font_size,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t alpha)
{
    sh_font_render_text_clipped(pixels, buf_width, buf_height,
                                 font, text, len, x, y, font_size,
                                 r, g, b, alpha, NULL);
}
