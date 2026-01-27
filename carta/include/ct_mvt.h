/*
 * ct_mvt.h - Mapbox Vector Tile encoding
 *
 * Encodes tile data to MVT format (Protocol Buffers with specific schema).
 * See: https://github.com/mapbox/vector-tile-spec
 */

#ifndef CT_MVT_H
#define CT_MVT_H

#include "ct_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MVT Encoding
 * ============================================================================ */

/*
 * Get default MVT encoding options.
 */
void ct_mvt_default_options(CTMVTOptions *opts);

/*
 * Encode a tile to MVT format.
 *
 * @param tile     Tile data with features
 * @param opts     Encoding options (NULL for defaults)
 * @param buffer   Output buffer
 * @param capacity Buffer capacity in bytes
 * @return Number of bytes written, or 0 on error
 */
size_t ct_encode_mvt(const CTTile *tile, const CTMVTOptions *opts,
                     uint8_t *buffer, size_t capacity);

/*
 * Encode tile data directly from PBF context.
 * Convenience function that combines feature extraction and encoding.
 *
 * @param ctx      PBF context with parsed data
 * @param coord    Tile coordinates
 * @param opts     Encoding options (NULL for defaults)
 * @param buffer   Output buffer
 * @param capacity Buffer capacity in bytes
 * @return Number of bytes written, or 0 on error
 */
size_t ct_generate_mvt(const CTPBFContext *ctx, CTTileCoord coord,
                       const CTMVTOptions *opts,
                       uint8_t *buffer, size_t capacity);

/* ============================================================================
 * Layer Names
 * ============================================================================ */

/*
 * Get the MVT layer name for a layer type.
 */
const char *ct_mvt_layer_name(CTLayer layer);

/* ============================================================================
 * Low-Level Encoding (for custom usage)
 * ============================================================================ */

/* MVT protobuf encoder context */
typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t offset;
    int error;
} CTMVTEncoder;

/*
 * Initialize an encoder.
 */
void ct_mvt_encoder_init(CTMVTEncoder *enc, uint8_t *buffer, size_t capacity);

/*
 * Begin a new layer.
 */
void ct_mvt_begin_layer(CTMVTEncoder *enc, const char *name, int extent);

/*
 * Add a feature to the current layer.
 */
void ct_mvt_add_feature(CTMVTEncoder *enc, const CTFeature *feature);

/*
 * End the current layer.
 */
void ct_mvt_end_layer(CTMVTEncoder *enc);

/*
 * Finalize encoding and return total size.
 */
size_t ct_mvt_encoder_finish(CTMVTEncoder *enc);

#ifdef __cplusplus
}
#endif

#endif /* CT_MVT_H */
