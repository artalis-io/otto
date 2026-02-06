/*
 * ct_png.h - PNG encoding for raster tiles
 *
 * Encodes RGBA pixel buffers to PNG format using miniz for compression.
 */

#ifndef CT_PNG_H
#define CT_PNG_H

#include "ct_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * PNG Encoding
 * ============================================================================ */

/*
 * Get default PNG encoding options.
 */
void ct_png_default_options(CTPNGOptions *opts);

/*
 * Encode RGBA pixels to PNG format.
 *
 * @param pixels   RGBA pixel buffer (width * height * 4 bytes)
 * @param width    Image width
 * @param height   Image height
 * @param opts     Encoding options (NULL for defaults)
 * @param buffer   Output buffer
 * @param capacity Buffer capacity in bytes
 * @return Number of bytes written, or 0 on error
 */
size_t ct_encode_png(const uint8_t *pixels, int width, int height,
                     const CTPNGOptions *opts,
                     uint8_t *buffer, size_t capacity);

/*
 * Generate PNG tile directly from PBF context.
 * Convenience function that combines rendering and encoding.
 *
 * @param ctx      PBF context with parsed data
 * @param coord    Tile coordinates
 * @param style    Rendering style (NULL for defaults)
 * @param opts     PNG options (NULL for defaults)
 * @param buffer   Output buffer
 * @param capacity Buffer capacity in bytes
 * @return Number of bytes written, or 0 on error
 */
size_t ct_generate_png(const CTPBFContext *ctx, CTTileCoord coord,
                       const CTStyle *style, const CTPNGOptions *opts,
                       uint8_t *buffer, size_t capacity);

/* Forward declaration for LOD config */
struct CTLODConfig;

/*
 * Generate PNG tile with LOD filtering.
 *
 * This is the recommended function for tile generation. It applies:
 * - LOD filtering (skip features not visible at this zoom)
 * - Geometry simplification (reduce points at lower zooms)
 * - Render-time size filtering (skip features too small to see)
 *
 * @param ctx      PBF context with parsed data
 * @param coord    Tile coordinates
 * @param style    Rendering style (NULL for defaults)
 * @param lod      LOD configuration (NULL = no filtering)
 * @param opts     PNG options (NULL for defaults)
 * @param buffer   Output buffer
 * @param capacity Buffer capacity in bytes
 * @return Number of bytes written, or 0 on error
 */
size_t ct_generate_png_lod(const CTPBFContext *ctx, CTTileCoord coord,
                           const CTStyle *style, const struct CTLODConfig *lod,
                           const CTPNGOptions *opts,
                           uint8_t *buffer, size_t capacity);

/*
 * Generate PNG tile with LOD filtering and custom render options.
 *
 * Extended version that allows control over render quality/performance.
 *
 * @param ctx           PBF context with parsed data
 * @param coord         Tile coordinates
 * @param style         Rendering style (NULL for defaults)
 * @param lod           LOD configuration (NULL = no filtering)
 * @param render_opts   Render options (NULL for defaults)
 * @param opts          PNG options (NULL for defaults)
 * @param buffer        Output buffer
 * @param capacity      Buffer capacity in bytes
 * @return Number of bytes written, or 0 on error
 */
size_t ct_generate_png_lod_opts(const CTPBFContext *ctx, CTTileCoord coord,
                                const CTStyle *style, const struct CTLODConfig *lod,
                                const CTRenderOptions *render_opts,
                                const CTPNGOptions *opts,
                                uint8_t *buffer, size_t capacity);

/* ============================================================================
 * Low-Level PNG Functions
 * ============================================================================ */

/*
 * Calculate maximum PNG output size for given dimensions.
 * Use this to allocate output buffer.
 */
size_t ct_png_max_size(int width, int height);

/*
 * Encode PNG with custom filter and compression.
 *
 * @param pixels           RGBA pixel buffer
 * @param width            Image width
 * @param height           Image height
 * @param filter_type      PNG filter (0=None, 1=Sub, 2=Up, 3=Average, 4=Paeth)
 * @param compression_level 0-9 (0=none, 9=max)
 * @param buffer           Output buffer
 * @param capacity         Buffer capacity
 * @return Bytes written or 0 on error
 */
size_t ct_encode_png_ex(const uint8_t *pixels, int width, int height,
                        int filter_type, int compression_level,
                        uint8_t *buffer, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* CT_PNG_H */
