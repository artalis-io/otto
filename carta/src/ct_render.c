/*
 * ct_render.c - Software rasterizer for map tiles
 */

#include "ct_render.h"
#include "ct_tile.h"
#include "ct_pbf.h"
#include "ct_lod.h"
#include "ct_simplify.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

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

    /* Otherwise use 32-bit writes instead of 4 separate byte writes */
    uint32_t rgba = ((uint32_t)a << 24) | ((uint32_t)b << 16) |
                    ((uint32_t)g << 8) | (uint32_t)r;
    uint32_t *pixels32 = (uint32_t *)ctx->pixels;

    for (int i = 0; i < num_pixels; i++) {
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

        /* Sort active edges by x using insertion sort (O(n) for nearly-sorted) */
        for (int i = 1; i < num_active; i++) {
            CTEdge key = active[i];
            int j = i - 1;
            while (j >= 0 && active[j].x > key.x) {
                active[j + 1] = active[j];
                j--;
            }
            active[j + 1] = key;
        }

        /* Fill between pairs of edges */
        for (int i = 0; i + 1 < num_active; i += 2) {
            int x_start = (int)(active[i].x + 0.5f);
            int x_end = (int)(active[i + 1].x + 0.5f);

            if (x_start < 0) x_start = 0;
            if (x_end >= ctx->width) x_end = ctx->width - 1;

            for (int x = x_start; x <= x_end; x++) {
                ct_render_blend_pixel(ctx, x, y, color);
            }
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

        /* Sort active edges by x using insertion sort (O(n) for nearly-sorted) */
        for (int i = 1; i < num_active; i++) {
            CTEdge key = active[i];
            int j = i - 1;
            while (j >= 0 && active[j].x > key.x) {
                active[j + 1] = active[j];
                j--;
            }
            active[j + 1] = key;
        }

        /* Fill between pairs of edges (even-odd rule) */
        for (int i = 0; i + 1 < num_active; i += 2) {
            int x_start = (int)(active[i].x + 0.5f);
            int x_end = (int)(active[i + 1].x + 0.5f);

            if (x_start < 0) x_start = 0;
            if (x_end >= ctx->width) x_end = ctx->width - 1;

            for (int x = x_start; x <= x_end; x++) {
                ct_render_blend_pixel(ctx, x, y, color);
            }
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

    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            float dist = sqrtf((float)(x * x + y * y));
            if (dist <= radius) {
                float alpha = 1.0f;
                if (dist > radius - 1.0f) {
                    alpha = radius - dist;
                }
                if (alpha > 0) {
                    uint8_t a = (uint8_t)(CT_COLOR_A(color) * alpha);
                    CTColor c = CT_RGBA(CT_COLOR_R(color), CT_COLOR_G(color),
                                        CT_COLOR_B(color), a);
                    ct_render_blend_pixel(ctx, cx + x, cy + y, c);
                }
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

        /* Convert from fixed-point lat/lon to tile pixel coords */
        for (int j = 0; j < f->num_points; j++) {
            double lon = f->points[j].x * 1e-7;
            double lat = f->points[j].y * 1e-7;

            int px, py;
            ct_latlon_to_tile_pixel(lat, lon, coord, CT_MVT_EXTENT, &px, &py);

            f->points[j].x = px;
            f->points[j].y = py;
        }

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

        /* Convert from fixed-point lat/lon to tile pixel coords */
        for (int j = 0; j < f->num_points; j++) {
            double lon = f->points[j].x * 1e-7;
            double lat = f->points[j].y * 1e-7;

            int px, py;
            ct_latlon_to_tile_pixel(lat, lon, coord, CT_MVT_EXTENT, &px, &py);

            f->points[j].x = px;
            f->points[j].y = py;
        }

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
