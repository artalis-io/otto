#ifndef SG_LOADING_SEARCH_H
#define SG_LOADING_SEARCH_H
#include "sg_loading.h"
/* Internal joint-search hook. The same global budget covers physical placement
 * and all additional witness search; an exhausted leaf cannot prove no fit. */
typedef struct {
    uint64_t nodes, budget;
    int exhausted, interrupted;
    SGLoadingShouldStop should_stop;
    void *user_data;
} SGLoadingWork;
static inline int sg_loading_work_enter(SGLoadingWork *work) {
    if (work->should_stop && work->should_stop(work->user_data)) {
        work->exhausted = work->interrupted = 1;
        return 0;
    }
    if (work->nodes == work->budget) {
        work->exhausted = 1;
        return 0;
    }
    ++work->nodes;
    return 1;
}
typedef int (*SGLoadingLeafCheck)(const SGLoadingPlacement *placements,
                                 SGLoadingWork *work, void *data);
SGStatus sg_loading_check_joint(const SGLoadingVehicle *vehicle,
    const SGLoadingUnit *units, uint32_t count, uint64_t node_budget,
    SGLoadingShouldStop should_stop, void *stop_data,
    SGLoadingLeafCheck leaf_check, void *leaf_data, SGLoadingResult *result);
SGStatus sg_loading_validate_joint(const SGLoadingVehicle *vehicle,
    const SGLoadingUnit *units, uint32_t count, const SGLoadingPlacement *placements);
#endif
