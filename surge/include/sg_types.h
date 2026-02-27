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

/* Solver profiles for hyperparameter tuning.
   Each profile targets a different time/quality tradeoff. */
typedef enum {
    SG_PROFILE_REALTIME = 0,      /* ~500 iters, <1s  (100 req) */
    SG_PROFILE_FAST = 1,          /* ~2500 iters, <5s (100 req) */
    SG_PROFILE_NEAR_OPTIMAL = 2,  /* ~10000 iters, <15s (100 req) */
    SG_PROFILE_BEST = 3,          /* ~50000 iters, <60s (100 req) */
    SG_PROFILE_COUNT = 4
} SGProfile;

/*
 * Tunable ALNS parameters for hyperparameter optimization.
 * All fields use a sentinel value (-1.0 for doubles, -1 for ints)
 * to indicate "use default". Set only fields you want to override.
 * Initialize with sg_tune_params_default() to set all to sentinel.
 */
typedef struct {
    /* Phase budget split */
    double phase1_fraction;       /* [0.4, 0.8], default 0.60 — Phase 1 budget as fraction of total */
    int    phase15_iters;         /* [100, 2000], default 500 — Phase 1.5 vehicle crunch iterations */

    /* SA temperature / cooling */
    double sa_accept_pct;         /* [0.01, 0.20], default 0.05 — fraction of cost accepted at 50% prob */
    double p1_final_temp_ratio;   /* [0.005, 0.20], default 0.05 — Phase 1 cools to this fraction of T0 */
    double p2_final_temp_ratio;   /* [0.0001, 0.05], default ~0.001 (via calibrate_sa) */

    /* Penalty parameters (progressive + adaptive) */
    double pen_target_start;      /* [0.10, 0.50], default 0.25 — Phase 1 initial feasibility target */
    double pen_target_end;        /* [0.05, 0.30], default 0.15 — Phase 1 final feasibility target */
    double pen_tolerance;         /* [0.02, 0.15], default 0.08 */
    double pen_increase;          /* [1.05, 2.0], default 1.3 */
    double pen_decrease;          /* [0.50, 0.95], default 0.80 */
    double pen_p15_target;        /* [0.10, 0.40], default 0.20 — Phase 1.5 adaptive target */
    double pen_p15_tolerance;     /* [0.02, 0.15], default 0.05 */
    double pen_p15_increase;      /* [1.05, 2.0], default 1.2 */
    double pen_p15_decrease;      /* [0.50, 0.95], default 0.85 */

    /* ALNS adaptive weight parameters */
    double reaction_factor;       /* [0.01, 0.5], default 0.1 */
    double reward_best;           /* [3.0, 50.0], default 8.0 */
    double reward_better;         /* [1.0, 20.0], default 4.0 */
    double reward_accepted;       /* [0.5, 10.0], default 2.0 */
    int    segment_size;          /* [25, 500], default 100 — override config.segment_size */

    /* Destruction operator parameters */
    double worst_randomness;      /* [1.0, 10.0], default 4.0 */
    double shaw_randomness;       /* [1.0, 10.0], default 4.0 */
    double route_cluster_randomness; /* [1.0, 10.0], default 2.5 */
    double time_cluster_randomness;  /* [1.0, 10.0], default 2.0 */
    double pd_shaw_randomness;    /* [1.0, 10.0], default 3.0 */
    double route_shaw_randomness; /* [1.0, 10.0], default 4.0 */
    int    string_l_max;          /* [4, 30], default 10 — max string removal length */

    /* Insertion pruning */
    int    neighbor_k;            /* [5, 50], default 20 — k-nearest neighbors for vehicle pruning */
} SGTuneParams;

#define SG_TUNE_SENTINEL_D (-1.0)
#define SG_TUNE_SENTINEL_I (-1)

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
