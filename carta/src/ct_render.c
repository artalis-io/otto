/*
 * ct_render.c - Software rasterizer for map tiles
 */

#include "ct_render.h"
#include "ct_tile.h"
#include "ct_pbf.h"
#include "ct_lod.h"
#include "ct_simplify.h"
#include "ct_label.h"
#include "ct_boundary.h"
#include "sh_font.h"
#include "shared.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>

/* SIMD support detection */
#if defined(__AVX2__)
    #include <immintrin.h>
    #define CT_HAVE_AVX2 1
#elif defined(__SSE2__)
    #include <emmintrin.h>
    #define CT_HAVE_SSE2 1
#elif defined(__ARM_NEON) || defined(__aarch64__)
    #include <arm_neon.h>
    #define CT_HAVE_NEON 1
#endif

/* Minimum feature size in pixels for render-time filtering.
 * Lines need at least 1px to be visible.
 * Buildings need at least 3px in both dimensions to be worth rendering.
 * Smaller buildings are filtered out entirely. */
#define MIN_LINE_PIXELS 1.0f
#define MIN_BUILDING_PIXELS 3.0f

/* Minimum render size for buildings in pixels.
 * Buildings smaller than this are scaled up to this size to ensure they
 * appear as recognizable rectangles rather than dots. */
#define MIN_BUILDING_RENDER_SIZE 6.0f

/* Edge structure for scanline polygon fill (defined here for buffer preallocation)
 * Uses double for x and dx to prevent accumulated floating-point error
 * over many scanlines, which could cause edges to drift and cross incorrectly. */
typedef struct {
    int y_min, y_max;
    double x, dx;
} CTEdge;

/* ============================================================================
 * Render Context Management
 * ============================================================================ */

CTRenderContext *ct_render_create(int width, int height)
{
    /* Validate dimensions */
    if (width <= 0 || height <= 0) return NULL;

    /* Check for integer overflow: width * height * 4 */
    if (width > INT_MAX / 4 || height > INT_MAX / width) {
        return NULL;  /* Would overflow */
    }
    size_t pixel_size = (size_t)width * (size_t)height * 4;
    if (pixel_size > SIZE_MAX) {
        return NULL;  /* Would overflow */
    }

    CTRenderContext *ctx = malloc(sizeof(CTRenderContext));
    if (!ctx) return NULL;

    ctx->width = width;
    ctx->height = height;
    ctx->stride = width * 4;  /* RGBA */
    ctx->pixels = calloc(pixel_size, 1);

    if (!ctx->pixels) {
        free(ctx);
        return NULL;
    }

    /* Pre-allocate scaling buffer to avoid per-feature malloc */
    ctx->scale_buffer_capacity = 8192;  /* 8K points handles most features */
    ctx->scale_buffer = malloc(ctx->scale_buffer_capacity * sizeof(CTTilePoint));

    /* Pre-allocate edge buffers for polygon fill (avoids per-polygon malloc)
     * 8K edges handles most polygons; will fall back to malloc for larger ones */
    ctx->edge_buffer_capacity = 8192;
    ctx->edge_buffer = malloc(ctx->edge_buffer_capacity * sizeof(CTEdge));
    ctx->active_buffer = malloc(ctx->edge_buffer_capacity * sizeof(CTEdge));

    ct_default_style(&ctx->style);
    ct_render_options_default(&ctx->options);
    return ctx;
}

void ct_render_free(CTRenderContext *ctx)
{
    if (!ctx) return;
    SAFE_FREE(ctx->pixels);
    SAFE_FREE(ctx->scale_buffer);
    SAFE_FREE(ctx->edge_buffer);
    SAFE_FREE(ctx->active_buffer);
    free(ctx);
}

void ct_render_clear(CTRenderContext *ctx)
{
    CTColor bg = ctx->style.background_color;
    uint8_t r = CT_COLOR_R(bg);
    uint8_t g = CT_COLOR_G(bg);
    uint8_t b = CT_COLOR_B(bg);
    uint8_t a = CT_COLOR_A(bg);

    int num_pixels = ctx->width * ctx->height;

    /* Fast path: if all components are the same, use memset */
    if (r == g && g == b && b == a) {
        memset(ctx->pixels, r, (size_t)num_pixels * 4);
        return;
    }

    /* Use SIMD for non-uniform colors */
    uint32_t rgba = ((uint32_t)a << 24) | ((uint32_t)b << 16) |
                    ((uint32_t)g << 8) | (uint32_t)r;
    uint32_t *pixels32 = (uint32_t *)ctx->pixels;
    int i = 0;

#if defined(CT_HAVE_AVX2)
    /* AVX2: clear 8 pixels at a time */
    __m256i rgba_vec = _mm256_set1_epi32((int)rgba);
    for (; i + 7 < num_pixels; i += 8) {
        _mm256_storeu_si256((__m256i *)&pixels32[i], rgba_vec);
    }
#elif defined(CT_HAVE_SSE2)
    /* SSE2: clear 4 pixels at a time */
    __m128i rgba_vec = _mm_set1_epi32((int)rgba);
    for (; i + 3 < num_pixels; i += 4) {
        _mm_storeu_si128((__m128i *)&pixels32[i], rgba_vec);
    }
#endif

    /* Scalar remainder */
    for (; i < num_pixels; i++) {
        pixels32[i] = rgba;
    }
}

void ct_render_set_style(CTRenderContext *ctx, const CTStyle *style)
{
    ctx->style = *style;
}

void ct_render_set_options(CTRenderContext *ctx, const CTRenderOptions *opts)
{
    ctx->options = *opts;
}

void ct_render_options_default(CTRenderOptions *opts)
{
    /* All layers ON */
    opts->render_water = 1;
    opts->render_landuse = 1;
    opts->render_buildings = 1;
    opts->render_roads = 1;
    opts->render_railways = 1;
    opts->render_boundaries = 1;
    opts->render_labels = 1;

    /* Details ON with zoom gates */
    opts->render_road_casing = 1;
    opts->render_railway_casing = 1;
    opts->render_bridge_outlines = 1;
    opts->render_building_outlines = 1;
    opts->render_label_halos = 1;
    opts->render_boundary_dashes = 1;

    opts->casing_min_zoom = 14;
    opts->building_outlines_min_zoom = 14;
    opts->labels_min_zoom = 8;
}

void ct_render_options_fast(CTRenderOptions *opts)
{
    /* All layers ON except boundaries (expensive relation processing) */
    opts->render_water = 1;
    opts->render_landuse = 1;
    opts->render_buildings = 1;
    opts->render_roads = 1;
    opts->render_railways = 1;
    opts->render_boundaries = 0;       /* OFF - expensive relation processing */
    opts->render_labels = 1;           /* ON - useful for navigation */

    /* Visual details ON - match OSM quality except boundaries */
    opts->render_road_casing = 1;
    opts->render_railway_casing = 1;
    opts->render_bridge_outlines = 1;
    opts->render_building_outlines = 1;
    opts->render_label_halos = 1;
    opts->render_boundary_dashes = 0;  /* OFF - boundaries disabled anyway */

    /* Standard zoom cutoffs */
    opts->casing_min_zoom = 14;
    opts->building_outlines_min_zoom = 14;
    opts->labels_min_zoom = 8;
}

void ct_render_options_quality(CTRenderOptions *opts)
{
    /* All layers ON */
    opts->render_water = 1;
    opts->render_landuse = 1;
    opts->render_buildings = 1;
    opts->render_roads = 1;
    opts->render_railways = 1;
    opts->render_boundaries = 1;
    opts->render_labels = 1;

    /* All details ON */
    opts->render_road_casing = 1;
    opts->render_railway_casing = 1;
    opts->render_bridge_outlines = 1;
    opts->render_building_outlines = 1;
    opts->render_label_halos = 1;
    opts->render_boundary_dashes = 1;

    /* Low zoom cutoffs - enable at most zoom levels */
    opts->casing_min_zoom = 12;
    opts->building_outlines_min_zoom = 13;
    opts->labels_min_zoom = 6;
}

uint8_t *ct_render_pixels(CTRenderContext *ctx)
{
    return ctx->pixels;
}

/* ============================================================================
 * Pixel Operations
 * ============================================================================ */

CTColor ct_render_get_pixel(CTRenderContext *ctx, int x, int y)
{
    if (x < 0 || x >= ctx->width || y < 0 || y >= ctx->height) {
        return 0;
    }
    int offset = (y * ctx->width + x) * 4;
    return CT_RGBA(ctx->pixels[offset], ctx->pixels[offset + 1],
                   ctx->pixels[offset + 2], ctx->pixels[offset + 3]);
}

void ct_render_set_pixel(CTRenderContext *ctx, int x, int y, CTColor color)
{
    if (x < 0 || x >= ctx->width || y < 0 || y >= ctx->height) {
        return;
    }
    int offset = (y * ctx->width + x) * 4;
    ctx->pixels[offset + 0] = CT_COLOR_R(color);
    ctx->pixels[offset + 1] = CT_COLOR_G(color);
    ctx->pixels[offset + 2] = CT_COLOR_B(color);
    ctx->pixels[offset + 3] = CT_COLOR_A(color);
}

void ct_render_blend_pixel(CTRenderContext *ctx, int x, int y, CTColor color)
{
    if (x < 0 || x >= ctx->width || y < 0 || y >= ctx->height) {
        return;
    }

    int offset = (y * ctx->width + x) * 4;
    uint8_t sr = CT_COLOR_R(color);
    uint8_t sg = CT_COLOR_G(color);
    uint8_t sb = CT_COLOR_B(color);
    uint8_t sa = CT_COLOR_A(color);

    if (sa == 255) {
        ctx->pixels[offset + 0] = sr;
        ctx->pixels[offset + 1] = sg;
        ctx->pixels[offset + 2] = sb;
        ctx->pixels[offset + 3] = 255;
        return;
    }

    if (sa == 0) return;

    uint8_t dr = ctx->pixels[offset + 0];
    uint8_t dg = ctx->pixels[offset + 1];
    uint8_t db = ctx->pixels[offset + 2];
    uint8_t da = ctx->pixels[offset + 3];

    /* Alpha blending */
    uint16_t out_a = sa + (da * (255 - sa)) / 255;
    if (out_a == 0) return;

    ctx->pixels[offset + 0] = (sr * sa + dr * da * (255 - sa) / 255) / out_a;
    ctx->pixels[offset + 1] = (sg * sa + dg * da * (255 - sa) / 255) / out_a;
    ctx->pixels[offset + 2] = (sb * sa + db * da * (255 - sa) / 255) / out_a;
    ctx->pixels[offset + 3] = out_a;
}

/*
 * Fast horizontal span fill - does bounds checking once, not per pixel.
 * Uses SIMD when available for maximum throughput.
 */
static void fill_span(CTRenderContext *ctx, int y, int x_start, int x_end, CTColor color)
{
    /* Bounds check y once */
    if (y < 0 || y >= ctx->height) return;

    /* Clip x range to buffer */
    if (x_start < 0) x_start = 0;
    if (x_end >= ctx->width) x_end = ctx->width - 1;
    if (x_start > x_end) return;

    uint8_t sr = CT_COLOR_R(color);
    uint8_t sg = CT_COLOR_G(color);
    uint8_t sb = CT_COLOR_B(color);
    uint8_t sa = CT_COLOR_A(color);

    uint8_t *row = ctx->pixels + y * ctx->width * 4;

    /* Fast path: opaque color - use SIMD when available */
    if (sa == 255) {
        uint32_t rgba = ((uint32_t)255 << 24) | ((uint32_t)sb << 16) |
                        ((uint32_t)sg << 8) | (uint32_t)sr;
        uint32_t *row32 = (uint32_t *)row;
        int x = x_start;

#if defined(CT_HAVE_AVX2)
        /* AVX2: write 8 pixels (32 bytes) at a time */
        int count = x_end - x_start + 1;
        if (count >= 8) {
            __m256i rgba_vec = _mm256_set1_epi32((int)rgba);
            for (; x + 7 <= x_end; x += 8) {
                _mm256_storeu_si256((__m256i *)&row32[x], rgba_vec);
            }
        }
#elif defined(CT_HAVE_SSE2)
        /* SSE2: write 4 pixels (16 bytes) at a time */
        int count = x_end - x_start + 1;
        if (count >= 4) {
            __m128i rgba_vec = _mm_set1_epi32((int)rgba);
            for (; x + 3 <= x_end; x += 4) {
                _mm_storeu_si128((__m128i *)&row32[x], rgba_vec);
            }
        }
#elif defined(CT_HAVE_NEON)
        /* NEON: write 4 pixels (16 bytes) at a time */
        int count = x_end - x_start + 1;
        if (count >= 4) {
            uint32x4_t rgba_vec = vdupq_n_u32(rgba);
            for (; x + 3 <= x_end; x += 4) {
                vst1q_u32(&row32[x], rgba_vec);
            }
        }
#endif
        /* Scalar remainder */
        for (; x <= x_end; x++) {
            row32[x] = rgba;
        }
        return;
    }

    /* Transparent - nothing to do */
    if (sa == 0) return;

    /*
     * Alpha blending using fast integer approximation.
     * Formula: out = (src * sa + dst * inv_sa + 128) >> 8
     * This approximates division by 255 with good accuracy.
     */
    uint16_t inv_sa = 255 - sa;

    /* Pre-multiply source by alpha */
    uint16_t sr_sa = sr * sa;
    uint16_t sg_sa = sg * sa;
    uint16_t sb_sa = sb * sa;

    int x = x_start;

#if defined(CT_HAVE_SSE2)
    /*
     * SIMD alpha blending: process 4 pixels at a time.
     * Each pixel is RGBA (4 bytes), so 4 pixels = 16 bytes = 128 bits.
     */
    int count = x_end - x_start + 1;
    if (count >= 4) {
        /* Broadcast source alpha and inv_sa to all 8 lanes (16-bit) */
        __m128i src_r = _mm_set1_epi16((short)sr_sa);
        __m128i src_g = _mm_set1_epi16((short)sg_sa);
        __m128i src_b = _mm_set1_epi16((short)sb_sa);
        __m128i inv_alpha = _mm_set1_epi16((short)inv_sa);
        __m128i src_alpha = _mm_set1_epi16((short)sa);
        __m128i const_128 = _mm_set1_epi16(128);
        __m128i const_255 = _mm_set1_epi16(255);
        __m128i zero = _mm_setzero_si128();

        for (; x + 3 <= x_end; x += 4) {
            /* Load 4 destination pixels (16 bytes) */
            __m128i dst = _mm_loadu_si128((__m128i *)&row[x * 4]);

            /* Unpack to 16-bit: dst_lo = pixels 0-1, dst_hi = pixels 2-3 */
            __m128i dst_lo = _mm_unpacklo_epi8(dst, zero);
            __m128i dst_hi = _mm_unpackhi_epi8(dst, zero);

            /* Extract R, G, B, A channels (interleaved in RGBA order) */
            /* dst_lo: R0 G0 B0 A0 R1 G1 B1 A1 (16-bit each) */
            /* dst_hi: R2 G2 B2 A2 R3 G3 B3 A3 (16-bit each) */

            /* Blend each channel: out = (src*sa + dst*inv_sa + 128) >> 8 */
            /* Process all 8 components (4 pixels * RGBA) in two vectors */

            /* Multiply destination by inv_alpha */
            __m128i d_inv_lo = _mm_mullo_epi16(dst_lo, inv_alpha);
            __m128i d_inv_hi = _mm_mullo_epi16(dst_hi, inv_alpha);

            /* Add source (premultiplied) - need to expand to 8 components */
            /* src pattern for 2 pixels: sr_sa, sg_sa, sb_sa, sa (repeated) */
            __m128i src_pattern = _mm_set_epi16((short)sa, (short)sb_sa,
                                                 (short)sg_sa, (short)sr_sa,
                                                 (short)sa, (short)sb_sa,
                                                 (short)sg_sa, (short)sr_sa);

            /* Add src + dst*inv + 128 */
            __m128i sum_lo = _mm_add_epi16(d_inv_lo, src_pattern);
            __m128i sum_hi = _mm_add_epi16(d_inv_hi, src_pattern);
            sum_lo = _mm_add_epi16(sum_lo, const_128);
            sum_hi = _mm_add_epi16(sum_hi, const_128);

            /* Shift right by 8 */
            sum_lo = _mm_srli_epi16(sum_lo, 8);
            sum_hi = _mm_srli_epi16(sum_hi, 8);

            /* Fix alpha: out_a = sa + (da * inv_sa + 128) >> 8 */
            /* Alpha is at indices 3, 7 in each vector */
            /* For now, clamp to 255 (saturated add handles overflow) */
            sum_lo = _mm_min_epi16(sum_lo, const_255);
            sum_hi = _mm_min_epi16(sum_hi, const_255);

            /* Pack back to 8-bit */
            __m128i result = _mm_packus_epi16(sum_lo, sum_hi);

            /* Store 4 pixels */
            _mm_storeu_si128((__m128i *)&row[x * 4], result);
        }
    }
#elif defined(CT_HAVE_NEON)
    /*
     * NEON alpha blending: process 4 pixels at a time.
     * Each pixel is RGBA (4 bytes), so 4 pixels = 16 bytes = 128 bits.
     */
    int count = x_end - x_start + 1;
    if (count >= 4) {
        /* Create source pattern for 4 pixels (RGBA repeated) */
        uint16_t src_vals[8] = {sr_sa, sg_sa, sb_sa, sa, sr_sa, sg_sa, sb_sa, sa};
        uint16x8_t src_pattern = vld1q_u16(src_vals);
        uint16x8_t inv_alpha = vdupq_n_u16(inv_sa);
        uint16x8_t const_128 = vdupq_n_u16(128);

        for (; x + 3 <= x_end; x += 4) {
            /* Load 4 destination pixels (16 bytes) */
            uint8x16_t dst = vld1q_u8(&row[x * 4]);

            /* Unpack to 16-bit: lo = pixels 0-1, hi = pixels 2-3 */
            uint16x8_t dst_lo = vmovl_u8(vget_low_u8(dst));
            uint16x8_t dst_hi = vmovl_u8(vget_high_u8(dst));

            /* Multiply destination by inv_alpha */
            uint16x8_t d_inv_lo = vmulq_u16(dst_lo, inv_alpha);
            uint16x8_t d_inv_hi = vmulq_u16(dst_hi, inv_alpha);

            /* Add src + dst*inv + 128 */
            uint16x8_t sum_lo = vaddq_u16(d_inv_lo, src_pattern);
            uint16x8_t sum_hi = vaddq_u16(d_inv_hi, src_pattern);
            sum_lo = vaddq_u16(sum_lo, const_128);
            sum_hi = vaddq_u16(sum_hi, const_128);

            /* Shift right by 8 */
            sum_lo = vshrq_n_u16(sum_lo, 8);
            sum_hi = vshrq_n_u16(sum_hi, 8);

            /* Pack back to 8-bit (saturating narrow) */
            uint8x8_t result_lo = vqmovn_u16(sum_lo);
            uint8x8_t result_hi = vqmovn_u16(sum_hi);
            uint8x16_t result = vcombine_u8(result_lo, result_hi);

            /* Store 4 pixels */
            vst1q_u8(&row[x * 4], result);
        }
    }
#endif

    /* Scalar remainder */
    for (; x <= x_end; x++) {
        int offset = x * 4;
        uint8_t dr = row[offset + 0];
        uint8_t dg = row[offset + 1];
        uint8_t db = row[offset + 2];
        uint8_t da = row[offset + 3];

        /* Fast approximate blend: (src*sa + dst*inv_sa + 128) >> 8 */
        row[offset + 0] = (uint8_t)((sr_sa + dr * inv_sa + 128) >> 8);
        row[offset + 1] = (uint8_t)((sg_sa + dg * inv_sa + 128) >> 8);
        row[offset + 2] = (uint8_t)((sb_sa + db * inv_sa + 128) >> 8);

        /* Output alpha: sa + da * (1 - sa) */
        uint16_t out_a = sa + ((da * inv_sa + 128) >> 8);
        row[offset + 3] = (uint8_t)(out_a > 255 ? 255 : out_a);
    }
}

/* ============================================================================
 * Line Drawing (Xiaolin Wu's Anti-aliased Line)
 * ============================================================================ */

static void plot_aa(CTRenderContext *ctx, int x, int y, float brightness, CTColor color)
{
    if (brightness <= 0) return;
    if (brightness > 1) brightness = 1;

    uint8_t alpha = (uint8_t)(CT_COLOR_A(color) * brightness);
    CTColor c = CT_RGBA(CT_COLOR_R(color), CT_COLOR_G(color),
                        CT_COLOR_B(color), alpha);
    ct_render_blend_pixel(ctx, x, y, c);
}

static float fpart(float x) { return x - floorf(x); }
static float rfpart(float x) { return 1.0f - fpart(x); }

static void draw_line_aa(CTRenderContext *ctx,
                         float x0, float y0, float x1, float y1,
                         CTColor color)
{
    int steep = fabsf(y1 - y0) > fabsf(x1 - x0);

    if (steep) {
        float t = x0; x0 = y0; y0 = t;
        t = x1; x1 = y1; y1 = t;
    }
    if (x0 > x1) {
        float t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }

    float dx = x1 - x0;
    float dy = y1 - y0;
    float gradient = (dx == 0) ? 1.0f : dy / dx;

    /* First endpoint */
    float xend = roundf(x0);
    float yend = y0 + gradient * (xend - x0);
    float xgap = rfpart(x0 + 0.5f);
    int xpxl1 = (int)xend;
    int ypxl1 = (int)floorf(yend);

    if (steep) {
        plot_aa(ctx, ypxl1, xpxl1, rfpart(yend) * xgap, color);
        plot_aa(ctx, ypxl1 + 1, xpxl1, fpart(yend) * xgap, color);
    } else {
        plot_aa(ctx, xpxl1, ypxl1, rfpart(yend) * xgap, color);
        plot_aa(ctx, xpxl1, ypxl1 + 1, fpart(yend) * xgap, color);
    }

    float intery = yend + gradient;

    /* Second endpoint */
    xend = roundf(x1);
    yend = y1 + gradient * (xend - x1);
    xgap = fpart(x1 + 0.5f);
    int xpxl2 = (int)xend;
    int ypxl2 = (int)floorf(yend);

    if (steep) {
        plot_aa(ctx, ypxl2, xpxl2, rfpart(yend) * xgap, color);
        plot_aa(ctx, ypxl2 + 1, xpxl2, fpart(yend) * xgap, color);
    } else {
        plot_aa(ctx, xpxl2, ypxl2, rfpart(yend) * xgap, color);
        plot_aa(ctx, xpxl2, ypxl2 + 1, fpart(yend) * xgap, color);
    }

    /* Main loop */
    for (int x = xpxl1 + 1; x < xpxl2; x++) {
        int y = (int)floorf(intery);
        if (steep) {
            plot_aa(ctx, y, x, rfpart(intery), color);
            plot_aa(ctx, y + 1, x, fpart(intery), color);
        } else {
            plot_aa(ctx, x, y, rfpart(intery), color);
            plot_aa(ctx, x, y + 1, fpart(intery), color);
        }
        intery += gradient;
    }
}

/*
 * Bresenham's line drawing algorithm.
 * Fast integer-based algorithm for 1px lines without anti-aliasing.
 * Use for performance-critical thin lines like road casing.
 */
static void draw_line_bresenham(CTRenderContext *ctx,
                                int x0, int y0, int x1, int y1,
                                CTColor color)
{
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    while (1) {
        ct_render_blend_pixel(ctx, x0, y0, color);

        if (x0 == x1 && y0 == y1) break;

        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void ct_render_line(CTRenderContext *ctx,
                    int x0, int y0, int x1, int y1,
                    CTColor color, float width)
{
    if (width <= 1.0f) {
        /*
         * For very thin lines (< 0.75px), use fast Bresenham.
         * For 0.75-1.0px lines, use anti-aliased Xiaolin Wu.
         */
        if (width < 0.75f) {
            draw_line_bresenham(ctx, x0, y0, x1, y1, color);
        } else {
            draw_line_aa(ctx, (float)x0, (float)y0, (float)x1, (float)y1, color);
        }
        return;
    }

    /* Thick line: draw multiple parallel lines */
    float dx = (float)(x1 - x0);
    float dy = (float)(y1 - y0);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;

    /* Perpendicular direction */
    float px = -dy / len;
    float py = dx / len;

    float half = width / 2.0f;
    int steps = (int)(width + 0.5f);
    if (steps < 1) steps = 1;

    /*
     * Performance optimization: Use fast Bresenham for interior lines,
     * AA only for the two edge lines to maintain smooth appearance.
     */
    for (int i = 0; i <= steps; i++) {
        float offset = -half + (i * width) / steps;
        float ox = px * offset;
        float oy = py * offset;

        if (i == 0 || i == steps) {
            /* Edge lines: use AA for smooth appearance */
            draw_line_aa(ctx, x0 + ox, y0 + oy, x1 + ox, y1 + oy, color);
        } else {
            /* Interior lines: use fast Bresenham */
            draw_line_bresenham(ctx,
                                (int)(x0 + ox + 0.5f), (int)(y0 + oy + 0.5f),
                                (int)(x1 + ox + 0.5f), (int)(y1 + oy + 0.5f),
                                color);
        }
    }
}

/* ============================================================================
 * Polyline Drawing
 * ============================================================================ */

void ct_render_polyline(CTRenderContext *ctx,
                        const CTTilePoint *points, int num_points,
                        CTColor color, float width)
{
    if (num_points < 2) return;

    for (int i = 0; i < num_points - 1; i++) {
        ct_render_line(ctx, points[i].x, points[i].y,
                       points[i + 1].x, points[i + 1].y, color, width);
    }
}

void ct_render_polyline_cased(CTRenderContext *ctx,
                              const CTTilePoint *points, int num_points,
                              CTColor fill_color, CTColor outline_color,
                              float fill_width, float outline_width)
{
    /* Draw outline first (thicker) */
    ct_render_polyline(ctx, points, num_points, outline_color,
                       fill_width + outline_width * 2);
    /* Then fill on top */
    ct_render_polyline(ctx, points, num_points, fill_color, fill_width);
}

/*
 * Render a polyline with dashes.
 * dash_length and gap_length are in pixels.
 */
void ct_render_polyline_dashed(CTRenderContext *ctx,
                               const CTTilePoint *points, int num_points,
                               CTColor color, float width,
                               float dash_length, float gap_length)
{
    if (num_points < 2) return;
    if (dash_length <= 0 || gap_length < 0) {
        /* No dashing - fall back to solid */
        ct_render_polyline(ctx, points, num_points, color, width);
        return;
    }

    float pattern_length = dash_length + gap_length;
    float distance = 0.0f;  /* Distance along polyline */

    for (int i = 0; i < num_points - 1; i++) {
        float x0 = (float)points[i].x;
        float y0 = (float)points[i].y;
        float x1 = (float)points[i + 1].x;
        float y1 = (float)points[i + 1].y;

        float dx = x1 - x0;
        float dy = y1 - y0;
        float seg_length = sqrtf(dx * dx + dy * dy);
        if (seg_length < 0.001f) continue;

        /* Unit vector along segment */
        float ux = dx / seg_length;
        float uy = dy / seg_length;

        float seg_pos = 0.0f;  /* Position along this segment */

        while (seg_pos < seg_length) {
            /* Where are we in the dash pattern? */
            float pattern_pos = fmodf(distance, pattern_length);
            int in_dash = (pattern_pos < dash_length);

            /* How much of current state (dash or gap) remains? */
            float remaining;
            if (in_dash) {
                remaining = dash_length - pattern_pos;
            } else {
                remaining = pattern_length - pattern_pos;
            }

            /* How much can we draw before end of segment or state change? */
            float draw_length = seg_length - seg_pos;
            if (draw_length > remaining) draw_length = remaining;

            if (in_dash) {
                /* Draw this dash segment */
                float sx = x0 + ux * seg_pos;
                float sy = y0 + uy * seg_pos;
                float ex = x0 + ux * (seg_pos + draw_length);
                float ey = y0 + uy * (seg_pos + draw_length);
                ct_render_line(ctx, (int)sx, (int)sy, (int)ex, (int)ey, color, width);
            }

            seg_pos += draw_length;
            distance += draw_length;
        }
    }
}

/* ============================================================================
 * Polygon Filling (Scanline Algorithm)
 * ============================================================================ */

/* CTEdge defined at top of file for buffer preallocation */

static int compare_edges(const void *a, const void *b)
{
    const CTEdge *ea = (const CTEdge *)a;
    const CTEdge *eb = (const CTEdge *)b;
    if (ea->y_min != eb->y_min) return ea->y_min - eb->y_min;
    /* Must return 0 for equal values to satisfy qsort contract */
    if (ea->x < eb->x) return -1;
    if (ea->x > eb->x) return 1;
    return 0;
}

void ct_render_polygon(CTRenderContext *ctx,
                       const CTTilePoint *points, int num_points,
                       CTColor color)
{
    if (num_points < 3) return;

    /* Find bounding box */
    int min_y = points[0].y, max_y = points[0].y;
    int min_x = points[0].x, max_x = points[0].x;
    for (int i = 1; i < num_points; i++) {
        if (points[i].y < min_y) min_y = points[i].y;
        if (points[i].y > max_y) max_y = points[i].y;
        if (points[i].x < min_x) min_x = points[i].x;
        if (points[i].x > max_x) max_x = points[i].x;
    }

    /* Check if polygon is entirely outside visible area */
    if (min_y >= ctx->height || max_y < 0 ||
        min_x >= ctx->width || max_x < 0) {
        return;
    }
    if (min_y < 0) min_y = 0;
    if (max_y >= ctx->height) max_y = ctx->height - 1;

    /* Use pre-allocated buffers if polygon fits, otherwise malloc */
    CTEdge *edges;
    CTEdge *active;
    int edges_allocated = 0;

    if ((size_t)num_points <= ctx->edge_buffer_capacity && ctx->edge_buffer) {
        edges = (CTEdge *)ctx->edge_buffer;
        active = (CTEdge *)ctx->active_buffer;
    } else {
        edges = malloc(num_points * sizeof(CTEdge));
        if (!edges) return;
        active = malloc(num_points * sizeof(CTEdge));
        if (!active) {
            free(edges);
            return;
        }
        edges_allocated = 1;
    }

    /* Build edge table */
    int num_edges = 0;

    for (int i = 0; i < num_points; i++) {
        int j = (i + 1) % num_points;
        int y0 = points[i].y, y1 = points[j].y;
        int x0 = points[i].x, x1 = points[j].x;

        if (y0 == y1) continue;  /* Skip horizontal edges */

        if (y0 > y1) {
            int t = y0; y0 = y1; y1 = t;
            t = x0; x0 = x1; x1 = t;
        }

        edges[num_edges].y_min = y0;
        edges[num_edges].y_max = y1;
        edges[num_edges].x = (double)x0;
        edges[num_edges].dx = (double)(x1 - x0) / (double)(y1 - y0);
        num_edges++;
    }

    if (num_edges < 2) {
        if (edges_allocated) {
            free(edges);
            free(active);
        }
        return;
    }

    qsort(edges, num_edges, sizeof(CTEdge), compare_edges);

    /* Active edge table */
    int num_active = 0;
    int edge_idx = 0;

    /* Scanline fill */
    for (int y = min_y; y <= max_y; y++) {
        /* Add edges starting at or before this scanline (but not already ended) */
        while (edge_idx < num_edges && edges[edge_idx].y_min <= y) {
            CTEdge e = edges[edge_idx++];
            /* Skip edges that ended before the visible area */
            if (e.y_max <= y) continue;
            /* If edge started before visible area, advance x to current scanline */
            if (e.y_min < y) {
                e.x += e.dx * (double)(y - e.y_min);
            }
            active[num_active++] = e;
        }

        /* Remove edges ending at this scanline */
        for (int i = 0; i < num_active; ) {
            if (active[i].y_max <= y) {
                active[i] = active[--num_active];
            } else {
                i++;
            }
        }

        /* Sort active edges by x using insertion sort (O(n) for nearly-sorted).
         * Early exit if already sorted - common when edges don't cross. */
        int needs_sort = 0;
        for (int i = 1; i < num_active; i++) {
            if (active[i].x < active[i - 1].x) {
                needs_sort = 1;
                break;
            }
        }
        if (needs_sort) {
            for (int i = 1; i < num_active; i++) {
                CTEdge key = active[i];
                int j = i - 1;
                while (j >= 0 && active[j].x > key.x) {
                    active[j + 1] = active[j];
                    j--;
                }
                active[j + 1] = key;
            }
        }

        /* Fill between pairs of edges using fast span fill */
        for (int i = 0; i + 1 < num_active; i += 2) {
            int x_start = (int)(active[i].x + 0.5);
            int x_end = (int)(active[i + 1].x + 0.5);
            fill_span(ctx, y, x_start, x_end, color);
        }

        /* Update x for next scanline */
        for (int i = 0; i < num_active; i++) {
            active[i].x += active[i].dx;
        }
    }

    /* Only free if we allocated (not using pre-allocated buffers) */
    if (edges_allocated) {
        free(edges);
        free(active);
    }
}

/*
 * Render a multipolygon with multiple rings (outer + holes).
 * Uses even-odd fill rule: all ring edges are included, and the
 * algorithm will properly exclude hole areas.
 */
void ct_render_multipolygon(CTRenderContext *ctx,
                            const CTTilePoint *points, int num_points,
                            const int *ring_ends, int num_rings,
                            CTColor color)
{
    if (num_points < 3 || num_rings < 1 || !ring_ends) return;

    /* Validate ring_ends: last entry must equal num_points */
    if (ring_ends[num_rings - 1] != num_points) {
#ifndef NDEBUG
        fprintf(stderr, "ct_render_multipolygon: ring_ends mismatch! "
                "ring_ends[%d-1]=%d != num_points=%d\n",
                num_rings, ring_ends[num_rings - 1], num_points);
#endif
        return;
    }

    /* Find bounding box across all points */
    int min_y = points[0].y, max_y = points[0].y;
    int min_x = points[0].x, max_x = points[0].x;
    for (int i = 1; i < num_points; i++) {
        if (points[i].y < min_y) min_y = points[i].y;
        if (points[i].y > max_y) max_y = points[i].y;
        if (points[i].x < min_x) min_x = points[i].x;
        if (points[i].x > max_x) max_x = points[i].x;
    }

    /* Check if polygon is entirely outside visible area */
    if (min_y >= ctx->height || max_y < 0 ||
        min_x >= ctx->width || max_x < 0) {
        return;
    }
    if (min_y < 0) min_y = 0;
    if (max_y >= ctx->height) max_y = ctx->height - 1;

    /* Use pre-allocated buffers if polygon fits, otherwise malloc */
    CTEdge *edges;
    CTEdge *active;
    int edges_allocated = 0;

    if ((size_t)num_points <= ctx->edge_buffer_capacity && ctx->edge_buffer) {
        edges = (CTEdge *)ctx->edge_buffer;
        active = (CTEdge *)ctx->active_buffer;
    } else {
        edges = malloc(num_points * sizeof(CTEdge));
        if (!edges) return;
        active = malloc(num_points * sizeof(CTEdge));
        if (!active) {
            free(edges);
            return;
        }
        edges_allocated = 1;
    }

    /* Build edge table from all rings */
    int num_edges = 0;

    /* Process each ring */
    int ring_start = 0;
    for (int r = 0; r < num_rings; r++) {
        int ring_end = ring_ends[r];
        int ring_points = ring_end - ring_start;
        if (ring_points < 3) {
            ring_start = ring_end;
            continue;
        }

        for (int i = ring_start; i < ring_end; i++) {
            int j = ring_start + ((i - ring_start + 1) % ring_points);
            int y0 = points[i].y, y1 = points[j].y;
            int x0 = points[i].x, x1 = points[j].x;

            if (y0 == y1) continue;  /* Skip horizontal edges */

            if (y0 > y1) {
                int t = y0; y0 = y1; y1 = t;
                t = x0; x0 = x1; x1 = t;
            }

            edges[num_edges].y_min = y0;
            edges[num_edges].y_max = y1;
            edges[num_edges].x = (double)x0;
            edges[num_edges].dx = (double)(x1 - x0) / (double)(y1 - y0);
            num_edges++;
        }

        ring_start = ring_end;
    }

    if (num_edges < 2) {
        if (edges_allocated) {
            free(edges);
            free(active);
        }
        return;
    }

    qsort(edges, num_edges, sizeof(CTEdge), compare_edges);

    /* Active edge table already allocated above */
    int num_active = 0;
    int edge_idx = 0;

    /* Scanline fill using even-odd rule (handles holes naturally) */
    for (int y = min_y; y <= max_y; y++) {
        /* Add edges starting at or before this scanline (but not already ended) */
        while (edge_idx < num_edges && edges[edge_idx].y_min <= y) {
            CTEdge e = edges[edge_idx++];
            /* Skip edges that ended before the visible area */
            if (e.y_max <= y) continue;
            /* If edge started before visible area, advance x to current scanline */
            if (e.y_min < y) {
                e.x += e.dx * (double)(y - e.y_min);
            }
            active[num_active++] = e;
        }

        /* Remove edges ending at this scanline */
        for (int i = 0; i < num_active; ) {
            if (active[i].y_max <= y) {
                active[i] = active[--num_active];
            } else {
                i++;
            }
        }

        /* Sort active edges by x using insertion sort (O(n) for nearly-sorted).
         * Early exit if already sorted - common when edges don't cross. */
        int needs_sort = 0;
        for (int i = 1; i < num_active; i++) {
            if (active[i].x < active[i - 1].x) {
                needs_sort = 1;
                break;
            }
        }
        if (needs_sort) {
            for (int i = 1; i < num_active; i++) {
                CTEdge key = active[i];
                int j = i - 1;
                while (j >= 0 && active[j].x > key.x) {
                    active[j + 1] = active[j];
                    j--;
                }
                active[j + 1] = key;
            }
        }

        /* Fill between pairs of edges (even-odd rule) using fast span fill */
        for (int i = 0; i + 1 < num_active; i += 2) {
            int x_start = (int)(active[i].x + 0.5);
            int x_end = (int)(active[i + 1].x + 0.5);
            fill_span(ctx, y, x_start, x_end, color);
        }

        /* Update x for next scanline */
        for (int i = 0; i < num_active; i++) {
            active[i].x += active[i].dx;
        }
    }

    /* Only free if we allocated (not using pre-allocated buffers) */
    if (edges_allocated) {
        free(edges);
        free(active);
    }
}

void ct_render_polygon_outline(CTRenderContext *ctx,
                               const CTTilePoint *points, int num_points,
                               CTColor color, float width)
{
    if (num_points < 2) return;

    for (int i = 0; i < num_points; i++) {
        int j = (i + 1) % num_points;
        ct_render_line(ctx, points[i].x, points[i].y,
                       points[j].x, points[j].y, color, width);
    }
}

void ct_render_polygon_filled(CTRenderContext *ctx,
                              const CTTilePoint *points, int num_points,
                              CTColor fill_color, CTColor outline_color,
                              float outline_width)
{
    ct_render_polygon(ctx, points, num_points, fill_color);
    ct_render_polygon_outline(ctx, points, num_points, outline_color, outline_width);
}

/* ============================================================================
 * Circle Drawing
 * ============================================================================ */

void ct_render_circle(CTRenderContext *ctx,
                      int cx, int cy, float radius,
                      CTColor color)
{
    int r = (int)(radius + 0.5f);
    if (r < 1) {
        ct_render_blend_pixel(ctx, cx, cy, color);
        return;
    }

    /* Use squared distances to avoid sqrt for most pixels */
    float radius_sq = radius * radius;
    float inner_radius = radius - 1.0f;
    float inner_sq = inner_radius * inner_radius;

    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            float dist_sq = (float)(x * x + y * y);

            /* Outside circle - skip entirely */
            if (dist_sq > radius_sq) {
                continue;
            }

            /* Inside inner region - full opacity, no sqrt needed */
            if (dist_sq <= inner_sq) {
                ct_render_blend_pixel(ctx, cx + x, cy + y, color);
                continue;
            }

            /* Edge region - need sqrt for anti-aliasing */
            float dist = sqrtf(dist_sq);
            float alpha = radius - dist;
            if (alpha > 0) {
                uint8_t a = (uint8_t)(CT_COLOR_A(color) * alpha);
                CTColor c = CT_RGBA(CT_COLOR_R(color), CT_COLOR_G(color),
                                    CT_COLOR_B(color), a);
                ct_render_blend_pixel(ctx, cx + x, cy + y, c);
            }
        }
    }
}

/* ============================================================================
 * Tile Rendering
 * ============================================================================ */

/*
 * Get scaling buffer from render context, growing if needed.
 * Avoids per-feature malloc/free overhead.
 */
static CTTilePoint *get_scale_buffer(CTRenderContext *ctx, size_t needed)
{
    if (needed > ctx->scale_buffer_capacity) {
        /* Grow buffer - double or use needed size, whichever is larger */
        size_t new_capacity = ctx->scale_buffer_capacity * 2;
        if (new_capacity < needed) new_capacity = needed;

        CTTilePoint *new_buf = realloc(ctx->scale_buffer,
                                        new_capacity * sizeof(CTTilePoint));
        if (new_buf) {
            ctx->scale_buffer = new_buf;
            ctx->scale_buffer_capacity = new_capacity;
        }
        /* If realloc fails, continue with existing buffer if large enough */
    }
    return ctx->scale_buffer;
}

void ct_render_tile(CTRenderContext *ctx, const CTTile *tile)
{
    ct_render_clear(ctx);

    /* Calculate scale factor from tile extent to render size */
    float scale = (float)ctx->width / CT_MVT_EXTENT;

    /* Render in order: landuse, water, buildings, roads, railways, boundaries */
    for (int pass = 0; pass < 6; pass++) {
        CTLayer target_layer;
        int layer_enabled;
        switch (pass) {
            case 0: target_layer = CT_LAYER_LANDUSE;    layer_enabled = ctx->options.render_landuse;    break;
            case 1: target_layer = CT_LAYER_WATER;      layer_enabled = ctx->options.render_water;      break;
            case 2: target_layer = CT_LAYER_BUILDINGS;  layer_enabled = ctx->options.render_buildings;  break;
            case 3: target_layer = CT_LAYER_ROADS;      layer_enabled = ctx->options.render_roads;      break;
            case 4: target_layer = CT_LAYER_RAILWAYS;   layer_enabled = ctx->options.render_railways;   break;
            case 5: target_layer = CT_LAYER_BOUNDARIES; layer_enabled = ctx->options.render_boundaries; break;
            default: continue;
        }
        if (!layer_enabled) continue;

        for (size_t i = 0; i < tile->num_features; i++) {
            const CTFeature *f = &tile->features[i];
            if (f->layer != target_layer) continue;

            /* Get pre-allocated buffer instead of malloc */
            CTTilePoint *scaled = get_scale_buffer(ctx, f->num_points);
            if (!scaled) continue;

            for (int j = 0; j < f->num_points; j++) {
                /* Use floor() for consistent rounding toward -infinity.
                 * (int) truncates toward zero, causing coordinates near
                 * tile boundaries to round inconsistently. */
                scaled[j].x = (int)floor(f->points[j].x * scale);
                scaled[j].y = (int)floor(f->points[j].y * scale);
            }

            switch (f->layer) {
                case CT_LAYER_LANDUSE: {
                    /* Select color based on landuse type */
                    CTColor landuse_color;
                    switch (f->feature_type) {
                        case CT_LANDUSE_FOREST:
                            landuse_color = ctx->style.forest_color;
                            break;
                        case CT_LANDUSE_PARK:
                            landuse_color = ctx->style.park_color;
                            break;
                        case CT_LANDUSE_GRASS:
                            landuse_color = ctx->style.grass_color;
                            break;
                        case CT_LANDUSE_RESIDENTIAL:
                            landuse_color = ctx->style.residential_color;
                            break;
                        case CT_LANDUSE_COMMERCIAL:
                            landuse_color = ctx->style.commercial_color;
                            break;
                        case CT_LANDUSE_INDUSTRIAL:
                            landuse_color = ctx->style.industrial_color;
                            break;
                        case CT_LANDUSE_FARMLAND:
                            landuse_color = ctx->style.farmland_color;
                            break;
                        case CT_LANDUSE_CEMETERY:
                            landuse_color = ctx->style.cemetery_color;
                            break;
                        case CT_LANDUSE_MILITARY:
                            landuse_color = ctx->style.military_color;
                            break;
                        default:
                            landuse_color = ctx->style.land_color;
                            break;
                    }
                    if (f->num_rings > 1 && f->ring_ends) {
                        ct_render_multipolygon(ctx, scaled, f->num_points,
                                               f->ring_ends, f->num_rings,
                                               landuse_color);
                    } else {
                        ct_render_polygon(ctx, scaled, f->num_points, landuse_color);
                    }
                    break;
                }

                case CT_LAYER_WATER:
                    if (f->type == CT_GEOM_POLYGON) {
                        if (f->num_rings > 1 && f->ring_ends) {
                            ct_render_multipolygon(ctx, scaled, f->num_points,
                                                   f->ring_ends, f->num_rings,
                                                   ctx->style.water_color);
                        } else {
                            ct_render_polygon(ctx, scaled, f->num_points,
                                              ctx->style.water_color);
                        }
                    } else {
                        /* Use zoom-adaptive width based on waterway type */
                        int waterway_type = f->feature_type;
                        if (waterway_type < 0 || waterway_type >= CT_WATERWAY_TYPE_COUNT) {
                            waterway_type = CT_WATERWAY_OTHER;
                        }
                        float width = ct_style_waterway_width_at_zoom(&ctx->style, waterway_type, tile->coord.z);
                        ct_render_polyline(ctx, scaled, f->num_points,
                                           ctx->style.water_color, width);
                    }
                    break;

                case CT_LAYER_BUILDINGS: {
                    /* Zoom-adaptive building outlines for performance:
                     * z13: no outline (buildings are tiny, outlines add noise)
                     * z14: 1px outline (single AA line, fast)
                     * z15+: 1.5px outline (full quality)
                     */
                    float outline_width = 0.0f;
                    int outline_min_zoom = ctx->options.building_outlines_min_zoom;
                    if (outline_min_zoom == 0) outline_min_zoom = 14;  /* Default */

                    if (ctx->options.render_building_outlines &&
                        tile->coord.z >= outline_min_zoom) {
                        if (tile->coord.z >= 15) {
                            outline_width = 1.5f;
                        } else {
                            outline_width = 1.0f;
                        }
                    }

                    /* Calculate bounding box for minimum size enforcement */
                    int32_t bld_min_x = scaled[0].x, bld_max_x = scaled[0].x;
                    int32_t bld_min_y = scaled[0].y, bld_max_y = scaled[0].y;
                    for (int k = 1; k < f->num_points; k++) {
                        if (scaled[k].x < bld_min_x) bld_min_x = scaled[k].x;
                        if (scaled[k].x > bld_max_x) bld_max_x = scaled[k].x;
                        if (scaled[k].y < bld_min_y) bld_min_y = scaled[k].y;
                        if (scaled[k].y > bld_max_y) bld_max_y = scaled[k].y;
                    }
                    int32_t bld_width = bld_max_x - bld_min_x;
                    int32_t bld_height = bld_max_y - bld_min_y;

                    /* If building is too small, render as minimum-sized rectangle
                     * to ensure it appears as a shape rather than a dot */
                    int min_size = (int)MIN_BUILDING_RENDER_SIZE;
                    if (bld_width < min_size || bld_height < min_size) {
                        int32_t cx = (bld_min_x + bld_max_x) / 2;
                        int32_t cy = (bld_min_y + bld_max_y) / 2;
                        int32_t half_w = (bld_width > min_size ? bld_width : min_size) / 2;
                        int32_t half_h = (bld_height > min_size ? bld_height : min_size) / 2;

                        CTTilePoint rect[4] = {
                            {cx - half_w, cy - half_h},
                            {cx + half_w, cy - half_h},
                            {cx + half_w, cy + half_h},
                            {cx - half_w, cy + half_h}
                        };
                        ct_render_polygon(ctx, rect, 4, ctx->style.building_color);
                        if (outline_width > 0.0f) {
                            ct_render_polygon_outline(ctx, rect, 4,
                                                      ctx->style.building_outline_color, outline_width);
                        }
                    } else if (f->num_rings > 1 && f->ring_ends) {
                        /* Multipolygon buildings */
                        ct_render_multipolygon(ctx, scaled, f->num_points,
                                               f->ring_ends, f->num_rings,
                                               ctx->style.building_color);
                        if (outline_width > 0.0f) {
                            ct_render_polygon_outline(ctx, scaled, f->ring_ends[0],
                                                      ctx->style.building_outline_color, outline_width);
                        }
                    } else {
                        /* Simple buildings */
                        ct_render_polygon(ctx, scaled, f->num_points, ctx->style.building_color);
                        if (outline_width > 0.0f) {
                            ct_render_polygon_outline(ctx, scaled, f->num_points,
                                                      ctx->style.building_outline_color, outline_width);
                        }
                    }
                    break;
                }

                case CT_LAYER_ROADS: {
                    int road_type = f->feature_type;
                    if (road_type < 0 || road_type >= CT_ROAD_TYPE_COUNT) {
                        road_type = CT_ROAD_OTHER;
                    }
                    /* Use zoom-adaptive road width */
                    float width = ct_style_road_width(&ctx->style, road_type, tile->coord.z);

                    /* Get zoom-adaptive casing from style module, respecting options */
                    int casing_min_zoom = ctx->options.casing_min_zoom;
                    if (casing_min_zoom == 0) casing_min_zoom = 14;  /* Default */

                    float casing = 0.0f;
                    if (ctx->options.render_road_casing &&
                        tile->coord.z >= casing_min_zoom) {
                        casing = ct_style_road_casing(tile->coord.z);
                    }

                    /* Add bridge outline for elevated roads */
                    if ((f->flags & CT_FLAG_BRIDGE) && ctx->options.render_bridge_outlines) {
                        ct_render_polyline(ctx, scaled, f->num_points,
                                           ctx->style.bridge_outline_color,
                                           width + ctx->style.bridge_outline_width * 2 + 2.0f);
                    }

                    if (casing > 0.0f) {
                        ct_render_polyline_cased(ctx, scaled, f->num_points,
                                                 ctx->style.road_colors[road_type],
                                                 ctx->style.road_outline_colors[road_type],
                                                 width, casing);
                    } else {
                        /* No casing - just draw the road fill */
                        ct_render_polyline(ctx, scaled, f->num_points,
                                           ctx->style.road_colors[road_type], width);
                    }
                    break;
                }

                case CT_LAYER_RAILWAYS: {
                    int railway_type = f->feature_type;
                    if (railway_type < 0 || railway_type >= CT_RAILWAY_TYPE_COUNT) {
                        railway_type = CT_RAILWAY_OTHER;
                    }
                    float width = ct_style_railway_width(&ctx->style, railway_type, tile->coord.z);
                    CTColor color = ctx->style.railway_colors[railway_type];
                    CTColor outline = ctx->style.railway_outline_colors[railway_type];

                    /* Get zoom-adaptive casing from style module, respecting options */
                    int rw_casing_min_zoom = ctx->options.casing_min_zoom;
                    if (rw_casing_min_zoom == 0) rw_casing_min_zoom = 14;  /* Default */

                    float casing = 0.0f;
                    if (ctx->options.render_railway_casing &&
                        tile->coord.z >= rw_casing_min_zoom) {
                        casing = ct_style_railway_casing(tile->coord.z);
                    }

                    if (casing > 0.0f) {
                        /* Render railway with casing (tick marks effect) */
                        ct_render_polyline_cased(ctx, scaled, f->num_points,
                                                 color, outline, width, casing);
                    } else {
                        /* No casing at low zoom for performance */
                        ct_render_polyline(ctx, scaled, f->num_points, color, width);
                    }

                    /* Add extra casing for bridges (only if casing enabled) */
                    if ((f->flags & CT_FLAG_BRIDGE) && casing > 0.0f &&
                        ctx->options.render_bridge_outlines) {
                        ct_render_polyline(ctx, scaled, f->num_points,
                                           ctx->style.bridge_outline_color,
                                           width + ctx->style.bridge_outline_width * 2);
                        ct_render_polyline_cased(ctx, scaled, f->num_points,
                                                 color, outline, width, casing);
                    }
                    break;
                }

                case CT_LAYER_BOUNDARIES:
                    /* Render admin boundaries as semi-transparent lines */
                    ct_render_polyline(ctx, scaled, f->num_points,
                                       ctx->style.boundary_color,
                                       ctx->style.boundary_width);
                    break;

                default:
                    break;
            }

            /* No free needed - buffer is reused */
        }
    }
}

/*
 * Check if a feature is large enough to be visible.
 * Returns 1 if visible, 0 if too small.
 *
 * Buildings need at least MIN_BUILDING_PIXELS (3px) in both dimensions.
 * Smaller buildings are filtered out entirely.
 * Buildings that pass are scaled up to MIN_BUILDING_RENDER_SIZE (6px) if needed.
 * Lines only need MIN_LINE_PIXELS (1px) to be visible.
 */
static int feature_is_visible(const CTFeature *f, float scale)
{
    if (f->num_points < 2) return 0;

    /* Calculate bounding box */
    int min_x = f->points[0].x, max_x = f->points[0].x;
    int min_y = f->points[0].y, max_y = f->points[0].y;

    for (int i = 1; i < f->num_points; i++) {
        if (f->points[i].x < min_x) min_x = f->points[i].x;
        if (f->points[i].x > max_x) max_x = f->points[i].x;
        if (f->points[i].y < min_y) min_y = f->points[i].y;
        if (f->points[i].y > max_y) max_y = f->points[i].y;
    }

    float width = (max_x - min_x) * scale;
    float height = (max_y - min_y) * scale;

    /* Lines: check length */
    if (f->type == CT_GEOM_LINESTRING) {
        float diag = sqrtf(width * width + height * height);
        return diag >= MIN_LINE_PIXELS;
    }

    /* Polygons: need minimum size to avoid rendering as dots */
    if (f->type == CT_GEOM_POLYGON) {
        /* Buildings need BOTH dimensions >= MIN_BUILDING_PIXELS (3px).
         * Buildings that pass this filter but are smaller than
         * MIN_BUILDING_RENDER_SIZE (6px) will be scaled up during rendering.
         * Other polygons (water, landuse) only need one dimension >= 1px. */
        if (f->layer == CT_LAYER_BUILDINGS) {
            return width >= MIN_BUILDING_PIXELS && height >= MIN_BUILDING_PIXELS;
        }
        return width >= MIN_LINE_PIXELS || height >= MIN_LINE_PIXELS;
    }

    return 1;
}

void ct_render_from_pbf(CTRenderContext *ctx, const CTPBFContext *pbf,
                        CTTileCoord coord)
{
    /* Get features for this tile */
    CTBBox bbox = ct_tile_bounds(coord);
    CTFeature *features;
    size_t count;

    if (ct_pbf_get_bbox_features(pbf, bbox, &features, &count) != CT_OK) {
        return;
    }

    /* Create tile and convert coordinates */
    CTTile tile;
    ct_tile_init(&tile, coord);

    for (size_t i = 0; i < count; i++) {
        CTFeature *f = &features[i];

        /* Fast batch coordinate transformation */
        ct_batch_transform_points(coord, CT_MVT_EXTENT, f->points, f->num_points);

        ct_tile_add_feature(&tile, f);
    }

    /* Render features */
    ct_render_tile(ctx, &tile);

    /* Render labels on top (if enabled) */
    int labels_min_zoom = ctx->options.labels_min_zoom;
    if (labels_min_zoom == 0) labels_min_zoom = 8;  /* Default */

    if (ctx->options.render_labels && coord.z >= labels_min_zoom) {
        const SHFont *font = sh_font_get_default();
        if (font) {
            CTLabelPlacer *placer = ct_label_placer_create(ctx->width, ctx->height);
            if (placer) {
                ct_label_place_points(placer, pbf, coord, font, 16.0f);
                float halo_width = ctx->options.render_label_halos ? 1.5f : 0.0f;
                ct_render_labels(ctx, placer, font,
                                CT_RGB(51, 51, 51),
                                CT_RGB(255, 255, 255),
                                halo_width);
                ct_label_placer_free(placer);
            }
        }
    }

    /* Cleanup - ct_tile_free handles freeing the points arrays
     * since ct_tile_add_feature took ownership via shallow copy */
    free(features);
    ct_tile_free(&tile);
}

void ct_render_from_pbf_lod(CTRenderContext *ctx, const CTPBFContext *pbf,
                            CTTileCoord coord, const CTLODConfig *lod)
{
    /* Get features for this tile with LOD filtering */
    CTFeature *features;
    size_t count;

    if (ct_pbf_get_tile_features_lod(pbf, coord, lod, &features, &count) != CT_OK) {
        return;
    }

    /* Create tile and convert coordinates */
    CTTile tile;
    ct_tile_init(&tile, coord);

    float scale = (float)ctx->width / CT_MVT_EXTENT;

    /* Clipping buffer: allow 64 pixels overshoot to avoid edge artifacts */
    int clip_buffer = 64;

    for (size_t i = 0; i < count; i++) {
        CTFeature *f = &features[i];

        /* Fast batch coordinate transformation */
        ct_batch_transform_points(coord, CT_MVT_EXTENT, f->points, f->num_points);

        /* Clip polygons to tile bounds to prevent scanline fill artifacts.
         * Large polygons that span multiple tiles cause precision issues
         * when their edges extend thousands of pixels beyond the visible area. */
        if (f->type == CT_GEOM_POLYGON && f->num_points >= 3) {
            if (f->num_rings > 1 && f->ring_ends) {
                /* Multipolygon with holes */
                CTTilePoint *clipped_pts = NULL;
                int clipped_count = 0;
                int *clipped_ring_ends = NULL;
                int clipped_num_rings = 0;

                ct_clip_multipolygon(f->points, f->num_points,
                                     f->ring_ends, f->num_rings,
                                     CT_MVT_EXTENT, clip_buffer,
                                     &clipped_pts, &clipped_count,
                                     &clipped_ring_ends, &clipped_num_rings);

                if (clipped_pts && clipped_count >= 3 && clipped_num_rings > 0) {
#ifndef NDEBUG
                    /* Validate clipped multipolygon structure */
                    int valid = 1;
                    if (clipped_ring_ends[clipped_num_rings - 1] != clipped_count) {
                        fprintf(stderr, "CLIP BUG: ring_ends[%d]=%d != count=%d\n",
                                clipped_num_rings - 1, clipped_ring_ends[clipped_num_rings - 1], clipped_count);
                        valid = 0;
                    }
                    int prev_end = 0;
                    for (int r = 0; r < clipped_num_rings; r++) {
                        int ring_pts = clipped_ring_ends[r] - prev_end;
                        if (ring_pts < 3) {
                            fprintf(stderr, "CLIP BUG: ring %d has only %d points\n", r, ring_pts);
                            valid = 0;
                        }
                        if (clipped_ring_ends[r] <= prev_end && r > 0) {
                            fprintf(stderr, "CLIP BUG: ring_ends not monotonic at %d\n", r);
                            valid = 0;
                        }
                        prev_end = clipped_ring_ends[r];
                    }
                    if (!valid) {
                        fprintf(stderr, "  original: %d pts, %d rings\n", f->num_points, f->num_rings);
                    }
#endif
                    /* Replace original with clipped geometry */
                    free(f->points);
                    free(f->ring_ends);
                    f->points = clipped_pts;
                    f->num_points = clipped_count;
                    f->ring_ends = clipped_ring_ends;
                    f->num_rings = clipped_num_rings;
                } else {
                    /* Polygon was clipped away entirely */
                    free(clipped_pts);
                    free(clipped_ring_ends);
                    free(f->points);
                    free(f->ring_ends);
                    f->points = NULL;
                    f->ring_ends = NULL;
                    continue;
                }
            } else {
                /* Simple polygon without holes */
                CTTilePoint *clipped_pts = NULL;
                int clipped_count = 0;

                ct_clip_polygon(f->points, f->num_points,
                               CT_MVT_EXTENT, clip_buffer,
                               &clipped_pts, &clipped_count);

                if (clipped_pts && clipped_count >= 3) {
                    /* Replace original with clipped geometry */
                    free(f->points);
                    f->points = clipped_pts;
                    f->num_points = clipped_count;
                } else {
                    /* Polygon was clipped away entirely */
                    free(clipped_pts);
                    free(f->points);
                    free(f->ring_ends);
                    f->points = NULL;
                    f->ring_ends = NULL;
                    continue;
                }
            }
        }

        /* NOTE: Simplification disabled for PNG rendering.
         *
         * Rationale:
         * 1. Rasterization already "simplifies" at the pixel level
         * 2. Aggressive simplification (64 units at z10) causes visible
         *    polygon degradation - angular/blocky shapes vs OSM's smooth curves
         * 3. Simplification has CPU overhead that may offset rendering gains
         * 4. Visual quality is prioritized for PNG output
         *
         * Simplification is still applied for MVT output in ct_mvt.c where
         * it reduces file size without visible quality loss.
         */

        /* Skip features too small to see */
        if (!feature_is_visible(f, scale)) {
            free(f->points);
            free(f->ring_ends);
            f->points = NULL;
            f->ring_ends = NULL;
            continue;
        }

        ct_tile_add_feature(&tile, f);
    }

    /* Render features */
    ct_render_tile(ctx, &tile);

    /* Render boundaries on top of base map but below labels (if enabled) */
    if (ctx->options.render_boundaries &&
        pbf->num_boundaries > 0 && pbf->boundary_rtree) {
        CTBBox tile_bbox = ct_tile_bounds(coord);
        size_t *boundary_indices = NULL;
        size_t boundary_count = 0;

        if (ct_boundary_query(pbf, tile_bbox, &boundary_indices, &boundary_count) == CT_OK &&
            boundary_count > 0) {

            /* LOD filtering: only show appropriate admin levels at each zoom */
            int min_admin = CT_BOUNDARY_OTHER;  /* Default: show all */
            if (coord.z <= 6) {
                min_admin = CT_BOUNDARY_COUNTRY;  /* z0-6: country borders only */
            } else if (coord.z <= 8) {
                min_admin = CT_BOUNDARY_STATE;    /* z7-8: country + state */
            } else if (coord.z <= 10) {
                min_admin = CT_BOUNDARY_COUNTY;   /* z9-10: country + state + county */
            }
            /* z11+: show all configured admin levels */

            for (size_t i = 0; i < boundary_count; i++) {
                size_t idx = boundary_indices[i];
                if (idx >= pbf->num_boundaries) continue;

                const CTAssembledBoundary *b = &pbf->boundaries[idx];

                /* LOD filter: skip boundaries below visibility threshold */
                if (b->boundary_type == CT_BOUNDARY_TYPE_ADMIN &&
                    b->admin_level > min_admin) {
                    continue;
                }

                /* Protected areas: only show at z8+ */
                if (b->boundary_type == CT_BOUNDARY_TYPE_PROTECTED && coord.z < 8) {
                    continue;
                }

                /* Get boundary styling */
                CTColor color;
                float width, dash, gap;
                ct_style_boundary(&ctx->style, b->boundary_type,
                                  b->admin_level, coord.z,
                                  &color, &width, &dash, &gap);

                /* Allocate tile points for this boundary */
                CTTilePoint *pts = malloc(b->num_coords * sizeof(CTTilePoint));
                if (!pts) continue;

                /* Transform lat/lon to tile pixel coordinates */
                for (int j = 0; j < b->num_coords; j++) {
                    int px, py;
                    ct_latlon_to_tile_pixel(b->coords[j].lat, b->coords[j].lon,
                                           coord, ctx->width, &px, &py);
                    pts[j].x = px;
                    pts[j].y = py;
                }

                /* Render the boundary - dashed or solid based on options */
                if (ctx->options.render_boundary_dashes && dash > 0.0f) {
                    ct_render_polyline_dashed(ctx, pts, b->num_coords,
                                              color, width, dash, gap);
                } else {
                    ct_render_polyline(ctx, pts, b->num_coords, color, width);
                }

                free(pts);
            }
            free(boundary_indices);
        }
    }

    /* Render labels on top (if enabled) */
    int lod_labels_min_zoom = ctx->options.labels_min_zoom;
    if (lod_labels_min_zoom == 0) lod_labels_min_zoom = 8;  /* Default */

    if (ctx->options.render_labels && coord.z >= lod_labels_min_zoom) {
        const SHFont *font = sh_font_get_default();
        if (font) {
            CTLabelPlacer *placer = ct_label_placer_create(ctx->width, ctx->height);
            if (placer) {
                /* Place point labels (cities, towns, etc.) */
                ct_label_place_points(placer, pbf, coord, font, 16.0f);

                /* Render with halo for readability (if enabled) */
                float halo_width = ctx->options.render_label_halos ? 1.5f : 0.0f;
                ct_render_labels(ctx, placer, font,
                                CT_RGB(51, 51, 51),      /* Dark gray text */
                                CT_RGB(255, 255, 255),   /* White halo */
                                halo_width);

                ct_label_placer_free(placer);
            }
        }
    }

    /* Cleanup */
    free(features);
    ct_tile_free(&tile);
}

/* ============================================================================
 * Text Rendering
 *
 * Uses shared MSDF sampling functions from sh_font.h for high-quality
 * text rendering with bilinear interpolation and smoothstep anti-aliasing.
 * ============================================================================ */

void ct_render_glyph(CTRenderContext *ctx,
                     const SHGlyph *glyph,
                     int x, int y,
                     const SHFont *font, float font_size,
                     CTColor color, float threshold)
{
    if (!ctx || !glyph || !font || font_size <= 0.0f) {
        return;
    }

    /* Calculate glyph dimensions in screen pixels */
    float glyph_width = (glyph->plane.right - glyph->plane.left) * font_size;
    float glyph_height = (glyph->plane.top - glyph->plane.bottom) * font_size;

    if (glyph_width <= 0.0f || glyph_height <= 0.0f) {
        return;
    }

    int px_width = (int)ceilf(glyph_width);
    int px_height = (int)ceilf(glyph_height);

    /* Early bounds check */
    if (x + px_width < 0 || x >= ctx->width ||
        y + px_height < 0 || y >= ctx->height) {
        return;
    }

    uint8_t sr = CT_COLOR_R(color);
    uint8_t sg = CT_COLOR_G(color);
    uint8_t sb = CT_COLOR_B(color);
    uint8_t base_alpha = CT_COLOR_A(color);

    /* Sample each pixel in the glyph bounding box */
    for (int py = 0; py < px_height; py++) {
        int screen_y = y + py;
        if (screen_y < 0 || screen_y >= ctx->height) continue;

        for (int px = 0; px < px_width; px++) {
            int screen_x = x + px;
            if (screen_x < 0 || screen_x >= ctx->width) continue;

            /* Map screen pixel to local glyph coordinates [0, 1]
             * local_y=0 -> atlas.bottom (low row = visual top in PNG)
             * local_y=1 -> atlas.top (high row = visual bottom in PNG) */
            float local_x = ((float)px + 0.5f) / glyph_width;
            float local_y = ((float)py + 0.5f) / glyph_height;

            /* Get MSDF coverage with threshold (uses bilinear sampling) */
            float coverage = sh_font_msdf_coverage_threshold(font, glyph, local_x, local_y,
                                                              font_size, threshold);

            if (coverage <= 0.0f) continue;

            /* Apply coverage to alpha */
            uint8_t alpha = (uint8_t)(base_alpha * coverage);
            if (alpha == 0) continue;

            /* Blend pixel */
            CTColor pixel_color = CT_RGBA(sr, sg, sb, alpha);
            ct_render_blend_pixel(ctx, screen_x, screen_y, pixel_color);
        }
    }
}

void ct_render_text(CTRenderContext *ctx,
                    const char *text, int x, int y,
                    const SHFont *font, float font_size,
                    CTColor color)
{
    if (!ctx || !text || !font || font_size <= 0.0f) {
        return;
    }

    float cursor_x = (float)x;
    float baseline_y = (float)y + sh_font_ascent(font, font_size);
    const char *p = text;

    while (*p) {
        uint32_t codepoint;
        int len = sh_utf8_decode(p, &codepoint);
        if (len == 0 || codepoint == 0) break;
        p += len;

        const SHGlyph *glyph = sh_font_get_glyph(font, codepoint);
        if (!glyph) {
            cursor_x += 0.5f * font_size;  /* Default advance */
            continue;
        }

        /* Calculate glyph position */
        float glyph_x = cursor_x + glyph->plane.left * font_size;
        float glyph_y = baseline_y - glyph->plane.top * font_size;

        /* Render glyph (threshold 0.5 = normal) */
        ct_render_glyph(ctx, glyph, (int)glyph_x, (int)glyph_y,
                        font, font_size, color, 0.5f);

        cursor_x += glyph->advance * font_size;
    }
}

void ct_render_text_halo(CTRenderContext *ctx,
                         const char *text, int x, int y,
                         const SHFont *font, float font_size,
                         CTColor fill_color, CTColor halo_color,
                         float halo_width)
{
    if (!ctx || !text || !font || font_size <= 0.0f) {
        return;
    }

    /* Calculate halo threshold.
     * halo_width determines how much to expand the glyph.
     * Each pixel of halo corresponds to ~0.05 threshold reduction.
     */
    float halo_threshold = 0.5f - (halo_width * 0.08f);
    if (halo_threshold < 0.1f) halo_threshold = 0.1f;

    /* First pass: render halo (expanded glyph) */
    float cursor_x = (float)x;
    float baseline_y = (float)y + sh_font_ascent(font, font_size);
    const char *p = text;

    while (*p) {
        uint32_t codepoint;
        int len = sh_utf8_decode(p, &codepoint);
        if (len == 0 || codepoint == 0) break;
        p += len;

        const SHGlyph *glyph = sh_font_get_glyph(font, codepoint);
        if (!glyph) {
            cursor_x += 0.5f * font_size;
            continue;
        }

        float glyph_x = cursor_x + glyph->plane.left * font_size;
        float glyph_y = baseline_y - glyph->plane.top * font_size;

        /* Render halo (lower threshold = expanded) */
        ct_render_glyph(ctx, glyph, (int)glyph_x, (int)glyph_y,
                        font, font_size, halo_color, halo_threshold);

        cursor_x += glyph->advance * font_size;
    }

    /* Second pass: render fill on top */
    cursor_x = (float)x;
    p = text;

    while (*p) {
        uint32_t codepoint;
        int len = sh_utf8_decode(p, &codepoint);
        if (len == 0 || codepoint == 0) break;
        p += len;

        const SHGlyph *glyph = sh_font_get_glyph(font, codepoint);
        if (!glyph) {
            cursor_x += 0.5f * font_size;
            continue;
        }

        float glyph_x = cursor_x + glyph->plane.left * font_size;
        float glyph_y = baseline_y - glyph->plane.top * font_size;

        /* Render fill (normal threshold) */
        ct_render_glyph(ctx, glyph, (int)glyph_x, (int)glyph_y,
                        font, font_size, fill_color, 0.5f);

        cursor_x += glyph->advance * font_size;
    }
}

int ct_render_labels(CTRenderContext *ctx,
                     const CTLabelPlacer *placer,
                     const SHFont *font,
                     CTColor fill_color, CTColor halo_color,
                     float halo_width)
{
    if (!ctx || !placer || !font) {
        return 0;
    }

    int rendered = 0;

    for (size_t i = 0; i < placer->num_placements; i++) {
        const CTLabelPlacement *p = &placer->placements[i];
        if (!p->point || !p->point->name) continue;

        /* Render text with halo at the placement position */
        ct_render_text_halo(ctx, p->point->name, p->x, p->y,
                            font, p->font_size,
                            fill_color, halo_color, halo_width);
        rendered++;
    }

    return rendered;
}
