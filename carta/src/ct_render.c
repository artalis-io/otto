/*
 * ct_render.c - Software rasterizer for map tiles
 */

#include "ct_render.h"
#include "ct_tile.h"
#include "ct_pbf.h"
#include "ct_lod.h"
#include "ct_simplify.h"
#include "ct_label.h"
#include "sh_font.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* SIMD support detection */
#if defined(__AVX2__)
    #include <immintrin.h>
    #define CT_HAVE_AVX2 1
#elif defined(__SSE2__)
    #include <emmintrin.h>
    #define CT_HAVE_SSE2 1
#endif

/* Minimum feature size in pixels for render-time filtering */
#define MIN_FEATURE_PIXELS 2.0f

/* ============================================================================
 * Render Context Management
 * ============================================================================ */

CTRenderContext *ct_render_create(int width, int height)
{
    CTRenderContext *ctx = malloc(sizeof(CTRenderContext));
    if (!ctx) return NULL;

    ctx->width = width;
    ctx->height = height;
    ctx->stride = width * 4;  /* RGBA */
    ctx->pixels = calloc(width * height * 4, 1);

    if (!ctx->pixels) {
        free(ctx);
        return NULL;
    }

    /* Pre-allocate scaling buffer to avoid per-feature malloc */
    ctx->scale_buffer_capacity = 8192;  /* 8K points handles most features */
    ctx->scale_buffer = malloc(ctx->scale_buffer_capacity * sizeof(CTTilePoint));

    ct_default_style(&ctx->style);
    return ctx;
}

void ct_render_free(CTRenderContext *ctx)
{
    if (!ctx) return;
    free(ctx->pixels);
    free(ctx->scale_buffer);
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
        int count = x_end - x_start + 1;

#if defined(CT_HAVE_AVX2)
        /* AVX2: write 8 pixels (32 bytes) at a time */
        if (count >= 8) {
            __m256i rgba_vec = _mm256_set1_epi32((int)rgba);
            for (; x + 7 <= x_end; x += 8) {
                _mm256_storeu_si256((__m256i *)&row32[x], rgba_vec);
            }
        }
#elif defined(CT_HAVE_SSE2)
        /* SSE2: write 4 pixels (16 bytes) at a time */
        if (count >= 4) {
            __m128i rgba_vec = _mm_set1_epi32((int)rgba);
            for (; x + 3 <= x_end; x += 4) {
                _mm_storeu_si128((__m128i *)&row32[x], rgba_vec);
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

    for (int x = x_start; x <= x_end; x++) {
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

void ct_render_line(CTRenderContext *ctx,
                    int x0, int y0, int x1, int y1,
                    CTColor color, float width)
{
    if (width <= 1.0f) {
        draw_line_aa(ctx, (float)x0, (float)y0, (float)x1, (float)y1, color);
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

    for (int i = 0; i <= steps; i++) {
        float offset = -half + (i * width) / steps;
        float ox = px * offset;
        float oy = py * offset;
        draw_line_aa(ctx, x0 + ox, y0 + oy, x1 + ox, y1 + oy, color);
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

/* ============================================================================
 * Polygon Filling (Scanline Algorithm)
 * ============================================================================ */

typedef struct {
    int y_min, y_max;
    float x, dx;
} CTEdge;

static int compare_edges(const void *a, const void *b)
{
    const CTEdge *ea = (const CTEdge *)a;
    const CTEdge *eb = (const CTEdge *)b;
    if (ea->y_min != eb->y_min) return ea->y_min - eb->y_min;
    return (ea->x < eb->x) ? -1 : 1;
}

void ct_render_polygon(CTRenderContext *ctx,
                       const CTTilePoint *points, int num_points,
                       CTColor color)
{
    if (num_points < 3) return;

    /* Find bounding box */
    int min_y = points[0].y, max_y = points[0].y;
    for (int i = 1; i < num_points; i++) {
        if (points[i].y < min_y) min_y = points[i].y;
        if (points[i].y > max_y) max_y = points[i].y;
    }

    if (min_y >= ctx->height || max_y < 0) return;
    if (min_y < 0) min_y = 0;
    if (max_y >= ctx->height) max_y = ctx->height - 1;

    /* Build edge table */
    CTEdge *edges = malloc(num_points * sizeof(CTEdge));
    if (!edges) return;
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
        edges[num_edges].x = (float)x0;
        edges[num_edges].dx = (float)(x1 - x0) / (float)(y1 - y0);
        num_edges++;
    }

    qsort(edges, num_edges, sizeof(CTEdge), compare_edges);

    /* Active edge table */
    CTEdge *active = malloc(num_edges * sizeof(CTEdge));
    if (!active) {
        free(edges);
        return;
    }
    int num_active = 0;
    int edge_idx = 0;

    /* Scanline fill */
    for (int y = min_y; y <= max_y; y++) {
        /* Add edges starting at this scanline */
        while (edge_idx < num_edges && edges[edge_idx].y_min <= y) {
            active[num_active++] = edges[edge_idx++];
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
            int x_start = (int)(active[i].x + 0.5f);
            int x_end = (int)(active[i + 1].x + 0.5f);
            fill_span(ctx, y, x_start, x_end, color);
        }

        /* Update x for next scanline */
        for (int i = 0; i < num_active; i++) {
            active[i].x += active[i].dx;
        }
    }

    free(edges);
    free(active);
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
    if (num_points < 3 || num_rings < 1) return;

    /* Find bounding box across all points */
    int min_y = points[0].y, max_y = points[0].y;
    for (int i = 1; i < num_points; i++) {
        if (points[i].y < min_y) min_y = points[i].y;
        if (points[i].y > max_y) max_y = points[i].y;
    }

    if (min_y >= ctx->height || max_y < 0) return;
    if (min_y < 0) min_y = 0;
    if (max_y >= ctx->height) max_y = ctx->height - 1;

    /* Build edge table from all rings */
    CTEdge *edges = malloc(num_points * sizeof(CTEdge));
    if (!edges) return;
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
            edges[num_edges].x = (float)x0;
            edges[num_edges].dx = (float)(x1 - x0) / (float)(y1 - y0);
            num_edges++;
        }

        ring_start = ring_end;
    }

    if (num_edges < 2) {
        free(edges);
        return;
    }

    qsort(edges, num_edges, sizeof(CTEdge), compare_edges);

    /* Active edge table */
    CTEdge *active = malloc(num_edges * sizeof(CTEdge));
    if (!active) {
        free(edges);
        return;
    }
    int num_active = 0;
    int edge_idx = 0;

    /* Scanline fill using even-odd rule (handles holes naturally) */
    for (int y = min_y; y <= max_y; y++) {
        /* Add edges starting at this scanline */
        while (edge_idx < num_edges && edges[edge_idx].y_min <= y) {
            active[num_active++] = edges[edge_idx++];
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
            int x_start = (int)(active[i].x + 0.5f);
            int x_end = (int)(active[i + 1].x + 0.5f);
            fill_span(ctx, y, x_start, x_end, color);
        }

        /* Update x for next scanline */
        for (int i = 0; i < num_active; i++) {
            active[i].x += active[i].dx;
        }
    }

    free(edges);
    free(active);
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

    /* Render in order: landuse, water, buildings, roads, railways */
    for (int pass = 0; pass < 5; pass++) {
        CTLayer target_layer;
        switch (pass) {
            case 0: target_layer = CT_LAYER_LANDUSE; break;
            case 1: target_layer = CT_LAYER_WATER; break;
            case 2: target_layer = CT_LAYER_BUILDINGS; break;
            case 3: target_layer = CT_LAYER_ROADS; break;
            case 4: target_layer = CT_LAYER_RAILWAYS; break;
            default: continue;
        }

        for (size_t i = 0; i < tile->num_features; i++) {
            const CTFeature *f = &tile->features[i];
            if (f->layer != target_layer) continue;

            /* Get pre-allocated buffer instead of malloc */
            CTTilePoint *scaled = get_scale_buffer(ctx, f->num_points);
            if (!scaled) continue;

            for (int j = 0; j < f->num_points; j++) {
                scaled[j].x = (int)(f->points[j].x * scale);
                scaled[j].y = (int)(f->points[j].y * scale);
            }

            switch (f->layer) {
                case CT_LAYER_LANDUSE:
                    if (f->num_rings > 1 && f->ring_ends) {
                        ct_render_multipolygon(ctx, scaled, f->num_points,
                                               f->ring_ends, f->num_rings,
                                               ctx->style.grass_color);
                    } else {
                        ct_render_polygon(ctx, scaled, f->num_points,
                                          ctx->style.grass_color);
                    }
                    break;

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
                        /* Use data-driven width based on waterway type */
                        int waterway_type = f->feature_type;
                        if (waterway_type < 0 || waterway_type >= CT_WATERWAY_TYPE_COUNT) {
                            waterway_type = CT_WATERWAY_OTHER;
                        }
                        float width = ct_style_waterway_width(&ctx->style, waterway_type);
                        ct_render_polyline(ctx, scaled, f->num_points,
                                           ctx->style.water_color, width);
                    }
                    break;

                case CT_LAYER_BUILDINGS:
                    if (f->num_rings > 1 && f->ring_ends) {
                        ct_render_multipolygon(ctx, scaled, f->num_points,
                                               f->ring_ends, f->num_rings,
                                               ctx->style.building_color);
                    } else {
                        ct_render_polygon_filled(ctx, scaled, f->num_points,
                                                 ctx->style.building_color,
                                                 ctx->style.building_outline_color,
                                                 1.0f);
                    }
                    break;

                case CT_LAYER_ROADS: {
                    int road_type = f->feature_type;
                    if (road_type < 0 || road_type >= CT_ROAD_TYPE_COUNT) {
                        road_type = CT_ROAD_OTHER;
                    }
                    /* Use zoom-adaptive road width */
                    float width = ct_style_road_width(&ctx->style, road_type, tile->coord.z);
                    ct_render_polyline_cased(ctx, scaled, f->num_points,
                                             ctx->style.road_colors[road_type],
                                             ctx->style.road_outline_colors[road_type],
                                             width, 1.0f);
                    break;
                }

                case CT_LAYER_RAILWAYS:
                    ct_render_polyline(ctx, scaled, f->num_points,
                                       ctx->style.railway_color,
                                       ctx->style.railway_width);
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
        return diag >= MIN_FEATURE_PIXELS;
    }

    /* Polygons: check area (width * height) */
    if (f->type == CT_GEOM_POLYGON) {
        return width >= MIN_FEATURE_PIXELS || height >= MIN_FEATURE_PIXELS;
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

    /* Render */
    ct_render_tile(ctx, &tile);

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

    /* Get simplification tolerance for this zoom level */
    float tolerance = ct_simplify_tolerance(coord.z);
    float scale = (float)ctx->width / CT_MVT_EXTENT;

    for (size_t i = 0; i < count; i++) {
        CTFeature *f = &features[i];

        /* Fast batch coordinate transformation */
        ct_batch_transform_points(coord, CT_MVT_EXTENT, f->points, f->num_points);

        /* Apply geometry simplification */
        if (f->num_points > 4) {
            if (f->type == CT_GEOM_POLYGON) {
                ct_simplify_poly_inplace(f->points, &f->num_points, tolerance);
            } else if (f->type == CT_GEOM_LINESTRING) {
                ct_simplify_line_inplace(f->points, &f->num_points, tolerance);
            }
        }

        /* Skip features too small to see */
        if (!feature_is_visible(f, scale)) {
            free(f->points);
            f->points = NULL;
            continue;
        }

        ct_tile_add_feature(&tile, f);
    }

    /* Render */
    ct_render_tile(ctx, &tile);

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

            /* Map screen pixel to local glyph coordinates [0, 1] */
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
