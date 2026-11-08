#ifndef SG_AXLE_H
#define SG_AXLE_H
#include "sg_loading.h"

/* Internal axle kernel. No v2 context/JSON capability is advertised until the
 * complete search, HGS and publication boundaries are integrated. */
#define SG_AXLE_MAX_MASS_G UINT64_C(1000000000)
#define SG_AXLE_MAX_COORD_MM INT32_C(1000000)
typedef struct { int32_t min, max; } SGAxleSignedInterval;
typedef struct { uint64_t min, max; } SGAxleMassInterval;
typedef struct { uint32_t numerator, denominator; } SGAxleFraction;
typedef struct {
    uint32_t wheelbase_mm, footprint_length_mm, slot_count;
    int32_t deck_min_mm, deck_max_mm;
    int32_t slot_centres_mm[SG_LOADING_MAX_SLOTS];
    SGAxleMassInterval front_tare_g, rear_tare_g;
    SGAxleMassInterval front_limits_g, rear_limits_g;
    uint64_t max_gross_g;
    uint8_t has_min_front_fraction;
    SGAxleFraction min_front_fraction;
} SGAxleProfile;
typedef struct {
    int64_t front_min_num, front_max_num, rear_min_num, rear_max_num;
    uint32_t denominator;
    uint64_t gross_min_g, gross_max_g;
} SGAxleState;

SGStatus sg_axle_profile_validate(const SGLoadingVehicle *vehicle, const SGAxleProfile *profile);
/* Complete declared facts and positions are required even for removed units.
 * active[i] is exactly 0/1. Output is zero on malformed input; an INFEASIBLE
 * state retains computed diagnostic bounds, never a feasibility certificate.
 * This checks axles alone; callers must independently validate stack/access. */
SGStatus sg_axle_state(const SGLoadingVehicle *vehicle, const SGAxleProfile *profile,
    const SGLoadingUnit *units, const SGAxleSignedInterval *cg_offsets,
    const uint32_t *slots, const uint8_t *active, uint32_t count, SGAxleState *out);

/* NULL profile + NULL CG selects explicit operation-validated stacking only.
 * Required axle capability must always supply a profile in the context layer. */
typedef struct {
    SGLoadingResult loading;
    uint32_t unload_order[SG_LOADING_MAX_UNITS]; /* indices in input unit array */
    SGAxleState states[SG_LOADING_MAX_UNITS+1]; /* departure, then each removal */
} SGAxleLoadingResult;
SGStatus sg_axle_loading_check(const SGLoadingVehicle *vehicle, const SGAxleProfile *profile,
    const SGLoadingUnit *units, const SGAxleSignedInterval *cg_offsets, uint32_t count,
    uint64_t budget, SGLoadingShouldStop should_stop, void *user_data, SGAxleLoadingResult *out);
SGStatus sg_axle_loading_validate(const SGLoadingVehicle *vehicle, const SGAxleProfile *profile,
    const SGLoadingUnit *units, const SGAxleSignedInterval *cg_offsets, uint32_t count,
    const SGLoadingPlacement *placements, const uint32_t *unload_order, SGAxleState *states);
#endif
