/*
 * ct_render.h - Software rasterizer for map tiles
 *
 * Renders map features to RGBA pixel buffers for PNG output.
 */

#ifndef CT_RENDER_H
#define CT_RENDER_H

#include "ct_types.h"
#include "ct_label.h"
#include "sh_font.h"

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

/* Forward declaration for LOD config */
struct CTLODConfig;

/*
 * Render tile from PBF context with LOD filtering.
 *
 * This is the recommended function for tile generation. It applies:
 * - LOD filtering (skip features not visible at this zoom)
 * - Geometry simplification (reduce points at lower zooms)
 * - Render-time size filtering (skip features too small to see)
 *
 * @param ctx    Render context
 * @param pbf    PBF context with parsed data
 * @param coord  Tile coordinates
 * @param lod    LOD configuration (NULL = no filtering)
 */
void ct_render_from_pbf_lod(CTRenderContext *ctx, const CTPBFContext *pbf,
                            CTTileCoord coord, const struct CTLODConfig *lod);

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
 * Fill a multipolygon with multiple rings (outer boundary + holes).
 * Uses even-odd fill rule: holes are properly excluded.
 *
 * @param ctx        Render context
 * @param points     Array of all points from all rings
 * @param num_points Total number of points
 * @param ring_ends  Array of ring end indices (exclusive)
 * @param num_rings  Number of rings
 * @param color      Fill color
 */
void ct_render_multipolygon(CTRenderContext *ctx,
                            const CTTilePoint *points, int num_points,
                            const int *ring_ends, int num_rings,
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

/*
 * Get road width at a specific zoom level.
 * Uses linear interpolation between z10, z14, and z18 reference points.
 *
 * @param rw   Road width specification
 * @param zoom Zoom level
 * @return Width in pixels at the given zoom level
 */
float ct_road_width_at_zoom(const CTRoadWidth *rw, int zoom);

/*
 * Get width for a road type at a specific zoom level.
 * Convenience function that looks up the road width spec from the style.
 *
 * @param style     Style configuration
 * @param road_type Road type (motorway, trunk, etc.)
 * @param zoom      Zoom level
 * @return Width in pixels
 */
float ct_style_road_width(const CTStyle *style, CTRoadType road_type, int zoom);

/*
 * Get width for a waterway type.
 * Returns data-driven width based on waterway class (river, stream, canal, etc.).
 * Widths are fixed per type, not zoom-dependent, to match real-world scale.
 *
 * @param style         Style configuration
 * @param waterway_type Waterway type (river, stream, canal, etc.)
 * @return Width in pixels
 */
float ct_style_waterway_width(const CTStyle *style, CTWaterwayType waterway_type);

/* ============================================================================
 * Text Rendering
 * ============================================================================ */

/*
 * Render text at a position.
 *
 * @param ctx       Render context
 * @param text      UTF-8 text to render
 * @param x, y      Top-left position in pixels
 * @param font      MSDF font
 * @param font_size Font size in pixels
 * @param color     Text color
 */
void ct_render_text(CTRenderContext *ctx,
                    const char *text, int x, int y,
                    const SHFont *font, float font_size,
                    CTColor color);

/*
 * Render text with halo (outline).
 * Renders halo first (darker outline), then fill on top.
 *
 * @param ctx         Render context
 * @param text        UTF-8 text to render
 * @param x, y        Top-left position in pixels
 * @param font        MSDF font
 * @param font_size   Font size in pixels
 * @param fill_color  Text fill color
 * @param halo_color  Halo (outline) color
 * @param halo_width  Halo width in pixels (typically 1-2)
 */
void ct_render_text_halo(CTRenderContext *ctx,
                         const char *text, int x, int y,
                         const SHFont *font, float font_size,
                         CTColor fill_color, CTColor halo_color,
                         float halo_width);

/*
 * Render a single glyph at a position.
 * Used internally by text rendering functions.
 *
 * @param ctx       Render context
 * @param glyph     Glyph to render (SHGlyph pointer)
 * @param x, y      Top-left position of glyph bounding box
 * @param font      MSDF font
 * @param font_size Font size in pixels
 * @param color     Glyph color
 * @param threshold MSDF threshold (0.5 = normal, lower = expanded/halo)
 */
void ct_render_glyph(CTRenderContext *ctx,
                     const SHGlyph *glyph,
                     int x, int y,
                     const SHFont *font, float font_size,
                     CTColor color, float threshold);

/*
 * Render all placed labels from a label placer.
 *
 * @param ctx         Render context
 * @param placer      Label placer with placed labels
 * @param font        MSDF font
 * @param fill_color  Text fill color
 * @param halo_color  Halo color
 * @param halo_width  Halo width in pixels
 * @return            Number of labels rendered
 */
int ct_render_labels(CTRenderContext *ctx,
                     const CTLabelPlacer *placer,
                     const SHFont *font,
                     CTColor fill_color, CTColor halo_color,
                     float halo_width);

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
