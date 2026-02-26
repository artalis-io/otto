#ifndef SURGE_SG_TYPES_H
#define SURGE_SG_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#define SG_MAX_COMPARTMENTS_PER_VEHICLE 8

#ifdef __cplusplus
extern "C" {
#endif

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
    SG_PD_POLICY_NONE = 0,   /* Default: no stacking constraint */
    SG_PD_POLICY_LIFO = 1,   /* Nested: last picked up, first delivered */
    SG_PD_POLICY_FIFO = 2    /* Same-order: first picked up, first delivered */
} SGPDPolicy;

typedef enum {
    SG_LOCK_NONE      = 0,   /* Default: can be dropped or reassigned */
    SG_LOCK_COMMITTED = 1,   /* Must-serve: cannot be dropped, CAN be reassigned */
    SG_LOCK_FROZEN    = 2    /* Must-serve + locked to vehicle: cannot be dropped or reassigned */
} SGRequestLock;

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

/* Solve phase identifiers */
typedef enum {
    SG_PHASE_CONSTRUCTION = 0,
    SG_PHASE_1_VEHICLE_MIN = 1,
    SG_PHASE_1_5_CRUNCH = 2,
    SG_PHASE_2_POLISH = 3,
    SG_PHASE_POSTPROCESS = 4
} SGSolvePhase;

/* Penalty constraint types (public mirror of internal SG_PENALTY_* enum) */
typedef enum {
    SG_PENALTY_TYPE_TIME_WARP = 0,
    SG_PENALTY_TYPE_CAPACITY = 1,
    SG_PENALTY_TYPE_DURATION = 2,
    SG_PENALTY_TYPE_RIDE_TIME = 3,
    SG_PENALTY_TYPE_DISTANCE = 4,
    SG_PENALTY_TYPE_TOTAL_WORK = 5,
    SG_PENALTY_TYPE_COUNT = 6
} SGPenaltyTypePublic;

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
    double elapsed_seconds; /* wall-clock since solve start */
    SGSolvePhase phase;     /* current solve phase */
} SGStats;

/* Convergence history entry */
typedef struct {
    int64_t iteration;
    double cost;
    double elapsed_seconds;
    uint32_t vehicles_used;
    uint32_t unassigned;
    double total_distance;
    SGSolvePhase phase;
    uint8_t is_new_best;    /* 1 = triggered by improvement, 0 = segment sample */
} SGConvergenceEntry;

/* Per-phase stats */
typedef struct {
    SGSolvePhase phase;
    int64_t iterations;
    double elapsed_seconds;
    double start_cost;
    double end_cost;
    uint32_t start_vehicles;
    uint32_t end_vehicles;
    uint32_t start_unassigned;
    uint32_t end_unassigned;
} SGPhaseStats;

/* Snapshot of penalty weights at end of solve */
typedef struct {
    double weight[6];  /* indexed by SGPenaltyTypePublic (0..5) */
} SGPenaltySnapshot;

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
    SG_VIOLATION_DUPLICATE_TASK,
    SG_VIOLATION_PD_POLICY,
    SG_VIOLATION_BACKHAUL,
    SG_VIOLATION_FROZEN_ASSIGNMENT,
    SG_VIOLATION_COMMITTED_UNASSIGNED,
    SG_VIOLATION_COMPARTMENT_CAPACITY,
    SG_VIOLATION_PRECEDENCE
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

#ifdef __cplusplus
}
#endif

#endif /* SURGE_SG_TYPES_H */
