/*
 * ct_multipolygon.h - Multipolygon assembly from OSM relations
 *
 * Assembles OSM multipolygon relations into renderable polygon geometries
 * by chaining member ways into closed rings.
 */

#ifndef CT_MULTIPOLYGON_H
#define CT_MULTIPOLYGON_H

#include "ct_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Assemble all multipolygon relations in the context.
 *
 * For each CTOSMRelation marked as is_multipolygon:
 * 1. Look up member ways by ID
 * 2. Chain ways into closed rings based on endpoint matching
 * 3. Classify rings as outer (CCW) or inner (CW) based on signed area
 * 4. Store assembled CTAssembledMultipolygon in ctx->multipolygons
 *
 * Call this after ct_load_pbf() to populate ctx->multipolygons.
 *
 * Returns CT_OK on success, error code on failure.
 */
CTStatus ct_assemble_multipolygons(CTPBFContext *ctx);

/*
 * Build R-Tree index for assembled multipolygons.
 *
 * After calling ct_assemble_multipolygons(), call this to build
 * a spatial index (ctx->mp_rtree) for efficient tile queries.
 *
 * Returns CT_OK on success, error code on failure.
 */
CTStatus ct_build_multipolygon_rtree(CTPBFContext *ctx);

/*
 * Get the role string for a relation member.
 *
 * Returns the role string from the context's role_strings pool,
 * or empty string if role_idx is invalid.
 */
const char *ct_get_role_string(const CTPBFContext *ctx, uint32_t role_idx);

#ifdef __cplusplus
}
#endif

#endif /* CT_MULTIPOLYGON_H */
