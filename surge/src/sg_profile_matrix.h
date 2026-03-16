#ifndef SURGE_SG_PROFILE_MATRIX_H
#define SURGE_SG_PROFILE_MATRIX_H

#include "sg_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SGContext SGContext;

/* One cell of the profile × scale matrix.
   Each cell contains the iteration/time budget and all tune params
   (no sentinels — every field is set). */
typedef struct {
    int        max_iterations;
    int        max_time_seconds;
    SGTuneParams tune;
} SGProfileCell;

/* Static const matrix: k_profile_matrix[SG_PROFILE_COUNT][SG_SCALE_COUNT] */
extern const SGProfileCell k_profile_matrix[SG_PROFILE_COUNT][SG_SCALE_COUNT];

/* Determine scale from request count.
   Snaps to the smallest column that covers the count:
   1-100 → SMALL, 101-200 → MEDIUM, 201-400 → LARGE,
   401-800 → XLARGE, 801+ → MASSIVE. */
SGScale sg_scale_from_count(uint32_t num_requests);

/* Apply matrix cell to context (sets config iterations/time + tune_params).
   Returns SG_STATUS_INVALID_ARG if profile or scale is out of range. */
SGStatus sg_profile_matrix_apply(SGContext *ctx, SGProfile profile, SGScale scale);

#ifdef __cplusplus
}
#endif

#endif /* SURGE_SG_PROFILE_MATRIX_H */
