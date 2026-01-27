/*
 * ct_render.h - Software rasterizer for map tiles
 *
 * Renders map features to RGBA pixel buffers for PNG output.
 */

#ifndef CT_RENDER_H
#define CT_RENDER_H

#include "ct_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Render Context Management
 * ============================================================================ */

/*
 * Create a new render context.
 *
 * @param width  Tile width in pixels (usually 256 or 512)
 * @param height Tile height in pixels
 * @return New render context, or NULL on error
 */
CTRenderContext *ct_render_create(int width, int height);

/*
 * Free a render context.
 */
void ct_render_free(CTRenderContext *ctx);

/*
 * Clear the render buffer to background color.
 */
void ct_render_clear(CTRenderContext *ctx);

/* ============================================================================
 * Tile Rendering
 * ============================================================================ */

/*
 * Render all features in a tile.
 *
 * @param ctx  Render context
 * @param tile Tile data with features
 */
void ct_render_tile(CTRenderContext *ctx, const CTTile *tile);

/*
 * Render tile directly from PBF context.
 * Convenience function that combines feature extraction and rendering.
 *
 * @param ctx    Render context
 * @param pbf    PBF context with parsed data
 * @param coord  Tile coordinates
 */
void ct_render_from_pbf(CTRenderContext *ctx, const CTPBFContext *pbf,
                        CTTileCoord coord);

/* ============================================================================
 * Primitive Drawing
 * ============================================================================ */

/*
 * Draw a line with anti-aliasing.
 *
 * @param ctx   Render context
 * @param x0,y0 Start point
 * @param x1,y1 End point
 * @param color Line color (RGBA)
 * @param width Line width in pixels
 */
void ct_render_line(CTRenderContext *ctx,
                    int x0, int y0, int x1, int y1,
                    CTColor color, float width);

/*
 * Draw a polyline (connected line segments).
 *
 * @param ctx        Render context
 * @param points     Array of points
 * @param num_points Number of points
 * @param color      Line color
 * @param width      Line width
 */
void ct_render_polyline(CTRenderContext *ctx,
                        const CTTilePoint *points, int num_points,
                        CTColor color, float width);

/*
 * Draw a polyline with outline (casing).
 * Draws outline first, then fill, for road-style rendering.
 */
void ct_render_polyline_cased(CTRenderContext *ctx,
                              const CTTilePoint *points, int num_points,
                              CTColor fill_color, CTColor outline_color,
                              float fill_width, float outline_width);

/*
 * Fill a polygon.
 *
 * @param ctx        Render context
 * @param points     Array of points (closed polygon)
 * @param num_points Number of points
 * @param color      Fill color
 */
void ct_render_polygon(CTRenderContext *ctx,
                       const CTTilePoint *points, int num_points,
                       CTColor color);

/*
 * Draw a polygon outline.
 */
void ct_render_polygon_outline(CTRenderContext *ctx,
                               const CTTilePoint *points, int num_points,
                               CTColor color, float width);

/*
 * Fill a polygon with outline.
 */
void ct_render_polygon_filled(CTRenderContext *ctx,
                              const CTTilePoint *points, int num_points,
                              CTColor fill_color, CTColor outline_color,
                              float outline_width);

/*
 * Draw a circle (for point features).
 *
 * @param ctx    Render context
 * @param cx, cy Center point
 * @param radius Circle radius in pixels
 * @param color  Fill color
 */
void ct_render_circle(CTRenderContext *ctx,
                      int cx, int cy, float radius,
                      CTColor color);

/* ============================================================================
 * Styling
 * ============================================================================ */

/*
 * Get default map style.
 */
void ct_default_style(CTStyle *style);

/*
 * Set render context style.
 */
void ct_render_set_style(CTRenderContext *ctx, const CTStyle *style);

/*
 * Calculate line width for a given zoom level.
 * Scales width from reference zoom (z=14) to actual zoom.
 *
 * @param base_width Width at reference zoom
 * @param zoom       Actual zoom level
 * @param ref_zoom   Reference zoom level
 * @return Scaled width
 */
float ct_scale_width(float base_width, int zoom, int ref_zoom);

/* ============================================================================
 * Pixel Access
 * ============================================================================ */

/*
 * Get pointer to pixel buffer.
 * Format is RGBA, 4 bytes per pixel, row-major order.
 */
uint8_t *ct_render_pixels(CTRenderContext *ctx);

/*
 * Get a single pixel color.
 */
CTColor ct_render_get_pixel(CTRenderContext *ctx, int x, int y);

/*
 * Set a single pixel color.
 */
void ct_render_set_pixel(CTRenderContext *ctx, int x, int y, CTColor color);

/*
 * Blend a color onto a pixel (alpha compositing).
 */
void ct_render_blend_pixel(CTRenderContext *ctx, int x, int y, CTColor color);

#ifdef __cplusplus
}
#endif

#endif /* CT_RENDER_H */
