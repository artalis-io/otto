/*
 * ct_boundary.h - Boundary relation assembly
 *
 * Stitches way segments from boundary relations into continuous linestrings.
 */

#ifndef CT_BOUNDARY_H
#define CT_BOUNDARY_H

#include "ct_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Assemble boundaries from parsed boundary relations.
 * Stitches way segments into continuous linestrings.
 *
 * Call after ct_pbf_load() and before tile generation.
 */
CTStatus ct_assemble_boundaries(CTPBFContext *ctx);

/*
 * Build R-tree index for assembled boundaries.
 * Call after ct_assemble_boundaries().
 */
CTStatus ct_build_boundary_rtree(CTPBFContext *ctx);

/*
 * Query boundaries intersecting a bounding box.
 *
 * Returns array of boundary indices (into ctx->boundaries).
 * Caller must free the returned array.
 */
CTStatus ct_boundary_query(const CTPBFContext *ctx, CTBBox bbox,
                           size_t **indices_out, size_t *count_out);

/*
 * Initialize boundary configuration with defaults.
 */
void ct_boundary_config_init(CTBoundaryConfig *config);

/*
 * Set boundary configuration before loading PBF.
 * Call before ct_pbf_load().
 */
void ct_pbf_set_boundary_config(CTPBFContext *ctx, const CTBoundaryConfig *config);

#ifdef __cplusplus
}
#endif

#endif /* CT_BOUNDARY_H */
