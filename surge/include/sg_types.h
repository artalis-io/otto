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

typedef enum {
    SG_ACCEPT_SA = 0,
    SG_ACCEPT_RRT = 1,
    SG_ACCEPT_IMPROVING = 2
} SGAcceptType;

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
    bool lexicographic_objective;
    SGAcceptType accept_type;
    bool adaptive_q;
} SGConfig;

typedef struct {
    int64_t iterations;
    double total_cost;
    double total_distance;
    uint32_t unassigned;
    uint32_t vehicles_used;
    double total_waiting;
    double total_overtime;
    double total_tw_penalty;
    double duration_span;   /* max_duration - min_duration across active routes */
    double distance_span;   /* max_distance - min_distance across active routes */
} SGStats;

typedef enum {
    SG_STOP_TYPE_PICKUP = 0,
    SG_STOP_TYPE_DELIVERY = 1,
    SG_STOP_TYPE_SERVICE = 2
} SGStopType;

typedef struct {
    uint32_t request_id;
    uint32_t task_id;
    SGStopType stop_type;
    double arrival;
    double service_start;
    double departure;
    uint32_t trip_index;    /* 0-based trip number within the vehicle's shift */
} SGSolutionStop;

/* Plan validation */
typedef enum {
    SG_VIOLATION_HARD_TW = 1,
    SG_VIOLATION_CAPACITY,
    SG_VIOLATION_PD_ORDER,
    SG_VIOLATION_RIDE_TIME,
    SG_VIOLATION_MAX_DURATION,
    SG_VIOLATION_MAX_DISTANCE,
    SG_VIOLATION_MAX_TASKS,
    SG_VIOLATION_FORBIDDEN_VEHICLE,
    SG_VIOLATION_QUALIFICATION,
    SG_VIOLATION_UNKNOWN_TASK,
    SG_VIOLATION_DUPLICATE_TASK
} SGViolationType;

typedef struct {
    SGViolationType type;
    uint32_t vehicle_id;
    uint32_t stop_index;
    uint32_t request_id;
    uint32_t task_id;
    double actual;
    double limit;
} SGViolation;

typedef struct {
    uint32_t vehicle_id;
    const uint32_t *task_ids;
    uint32_t task_count;
} SGPlanRoute;

#endif /* SURGE_SG_TYPES_H */
