#ifndef SG_LOADING_V2_INTERNAL_H
#define SG_LOADING_V2_INTERNAL_H
#include "sg_internal.h"
typedef struct {
    const SGRouteStop *stops;
    uint32_t count, insert_pos;
    SGRouteStop inserted;
} SGLoadingV2View;
SGStatus sg_loading_v2_round_view(const SGContext *ctx, uint32_t vehicle,
                                  const SGLoadingV2View *view, uint32_t trip,
                                  SGLoadingResult *result, uint32_t *requests, uint32_t *indices,
                                  int certificate, SGAxleLoadingResult *full);
void sg_loading_v2_clear_error(SGContext *ctx);
SGStatus sg_loading_v2_model_validate(SGContext *ctx);
SGStatus sg_loading_v2_check_units(const SGContext *ctx, uint32_t vehicle,
                                   const SGLoadingUnit *units, const SGAxleSignedInterval *cg,
                                   uint32_t count, int certificate, SGLoadingResult *result,
                                   SGAxleLoadingResult *full);
SGStatus sg_loading_round_v2(const SGContext *ctx, uint32_t vehicle, const SGRouteStop *stops,
                             uint32_t count, uint32_t trip, SGAxleLoadingResult *result,
                             uint32_t *requests, uint32_t *indices);
#endif
