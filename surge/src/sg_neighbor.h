#ifndef SURGE_SG_NEIGHBOR_H
#define SURGE_SG_NEIGHBOR_H

#include <stdint.h>

/*
 * SGNeighborIndex — k-nearest location index for insertion pruning.
 *
 * Pre-computes the k closest locations for each location (and optionally
 * per travel profile) so that during ALNS repair we can skip vehicles
 * whose routes contain no stops near the request being inserted.
 *
 * When a travel callback is set, distances are opaque and the index is
 * not built.  All query functions return 1 (= "assume near") so the
 * full scan runs as before.
 *
 * Function prototypes are in sg_internal.h (after SGContext/SGRouteSolution).
 */

typedef struct {
    uint32_t *neighbors;     /* flat array [num_entries * k] — sorted by loc_id asc */
    uint32_t num_entries;    /* = num_locations * (1 + num_profiles) */
    uint32_t num_locations;
    uint32_t k;              /* neighbors per location per layer */
    uint32_t num_profiles;   /* 0 = global-only, >0 = per-profile layers */
} SGNeighborIndex;

#endif /* SURGE_SG_NEIGHBOR_H */
