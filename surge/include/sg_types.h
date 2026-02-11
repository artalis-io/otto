#ifndef SURGE_SG_TYPES_H
#define SURGE_SG_TYPES_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SG_STATUS_OK = 0,
    SG_STATUS_INVALID_ARG,
    SG_STATUS_OUT_OF_MEMORY,
    SG_STATUS_INFEASIBLE,
    SG_STATUS_LIMIT,
    SG_STATUS_NOT_IMPLEMENTED,
    SG_STATUS_ERROR
} SGStatus;

typedef enum {
    SG_PRIORITY_REMOVE_LOWER_FIRST = 0,
    SG_PRIORITY_REMOVE_HIGHER_FIRST = 1
} SGPriorityRemovalPolicy;

typedef enum {
    SG_TASK_PICKUP = 0,
    SG_TASK_DELIVERY = 1,
    SG_TASK_SERVICE = 2
} SGTaskType;

typedef enum {
    SG_REQUEST_KIND_UNBOUND = 0,
    SG_REQUEST_KIND_DELIVERY_ONLY = 1,
    SG_REQUEST_KIND_PICKUP_DELIVERY = 2
} SGRequestKind;

typedef enum {
    SG_DEMAND_PICKUP_POSITIVE_DELIVERY_NEGATIVE = 0,
    SG_DEMAND_PICKUP_NEGATIVE_DELIVERY_POSITIVE = 1
} SGDemandSignConvention;

typedef struct {
    double x;
    double y;
} SGLocation;

typedef struct {
    int32_t early;
    int32_t late;
} SGTimeWindow;

typedef struct {
    int max_iterations;
    int max_time_seconds;
    int segment_size;
    int q_min;
    int q_max;
    uint64_t seed;
    bool deterministic;
    bool require_bound_requests_at_solve;
    SGPriorityRemovalPolicy priority_removal_policy;
} SGConfig;

typedef struct {
    int64_t iterations;
    double total_cost;
    double total_distance;
    uint32_t unassigned;
    uint32_t vehicles_used;
} SGStats;

#endif /* SURGE_SG_TYPES_H */
