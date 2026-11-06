#ifndef SURGE_SG_LOADING_H
#define SURGE_SG_LOADING_H
#include <stdint.h>
#include "sg_types.h"
#ifdef __cplusplus
extern "C" {
#endif
#define SG_LOADING_MAX_UNITS 128
#define SG_LOADING_MAX_SLOTS 128
#define SG_LOADING_MAX_CLASSES 64
#define SG_LOADING_MAX_LAYERS 8
/* P1: one physical truckload, identical full floor footprints, vertical stacks.
 * No conversion from fractional demand, rotation, overhang or reshuffling. */
typedef enum { SG_LOADING_SIDE_FREE, SG_LOADING_REAR_LIFO } SGLoadingAccess;
typedef enum { SG_LOADING_FEASIBLE, SG_LOADING_INFEASIBLE, SG_LOADING_UNKNOWN } SGLoadingOutcome;
typedef enum {
    SG_LOADING_WITNESS, SG_LOADING_CAPACITY, SG_LOADING_NO_ARRANGEMENT,
    SG_LOADING_BUDGET, SG_LOADING_UNSUPPORTED
} SGLoadingReason;
typedef struct {
    uint32_t stop; /* delivery sequence, smaller numbers unload first */
    uint32_t height_mm;
    uint64_t weight_g;
    uint64_t max_weight_above_g;
    uint32_t stack_class;
    uint32_t max_units_above; /* zero means nothing may be stacked on this unit */
} SGLoadingUnit;
typedef struct {
    uint32_t slot_count, max_layers, clearance_mm, class_count;
    uint64_t max_stack_weight_g, payload_g;
    SGLoadingAccess access;
    /* Each slot is one floor position. For REAR_LIFO: same lane + smaller
     * depth = closer to door. Distinct lanes are independently accessible. */
    uint32_t lane[SG_LOADING_MAX_SLOTS], depth[SG_LOADING_MAX_SLOTS];
    /* Directed immediate-support rule: bottom class -> permitted top classes. */
    uint64_t allowed_top[SG_LOADING_MAX_CLASSES];
} SGLoadingVehicle;
typedef struct { uint32_t slot, layer; } SGLoadingPlacement;
typedef struct {
    SGLoadingOutcome outcome;
    SGLoadingReason reason;
    uint64_t nodes;
    uint32_t count;
    SGLoadingPlacement placements[SG_LOADING_MAX_UNITS]; /* original input order */
} SGLoadingResult;
/* Invalid input => INVALID_ARG, no certificate. A zero node budget => UNKNOWN.
 * Exhaustive supported search alone can establish NO_ARRANGEMENT. */
SGStatus sg_loading_check(const SGLoadingVehicle *vehicle,
    const SGLoadingUnit *units, uint32_t count, uint64_t node_budget,
    SGLoadingResult *result);
/* Independent certificate validation. Only OK means the arrangement is valid. */
SGStatus sg_loading_validate(const SGLoadingVehicle *vehicle,
    const SGLoadingUnit *units, uint32_t count,
    const SGLoadingPlacement *placements);
#ifdef __cplusplus
}
#endif
#endif
