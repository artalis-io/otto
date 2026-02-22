#ifndef ARBOR_AR_TYPES_H
#define ARBOR_AR_TYPES_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AR_STATUS_OK = 0,
    AR_STATUS_INVALID_ARG,
    AR_STATUS_OUT_OF_MEMORY,
    AR_STATUS_NO_OPERATORS,
    AR_STATUS_LIMIT,
    AR_STATUS_ERROR
} ARStatus;

typedef enum {
    AR_ACCEPT_SA = 0,
    AR_ACCEPT_RRT,
    AR_ACCEPT_GD,
    AR_ACCEPT_IMPROVING
} ARAcceptType;

typedef enum {
    AR_STOP_NONE = 0,
    AR_STOP_MAX_ITERATIONS,
    AR_STOP_TIME_LIMIT,
    AR_STOP_STAGNATION,
    AR_STOP_TARGET_COST,
    AR_STOP_ERROR
} ARStopReason;

typedef struct {
    int max_iterations;
    int max_time_seconds;
    int max_stagnation_iterations;

    int segment_size;
    int q_min;
    int q_max;

    ARAcceptType accept_type;
    double initial_temp;
    double cooling_rate;
    double threshold;
    double target_cost;
    double water_level;
    double gd_decay;

    double reaction_factor;
    double reward_best;
    double reward_better;
    double reward_accepted;
    double reward_rejected;

    int restart_threshold;        /* Stagnation iters before restart from best (0=disabled) */
    double restart_temp_ratio;    /* SA temperature ratio for reheat on restart (0..1) */
} ARALNSParams;

typedef struct {
    int64_t iterations;
    int64_t improvements;
    int64_t accepted;
    int64_t rejected;
    int64_t invalid_candidates;
    int64_t restarts;
    double best_cost;
    double elapsed_seconds;
    ARStopReason stop_reason;
} ARALNSStats;

typedef struct {
    char name[32];
    double weight;
    int64_t selected;
    int64_t accepted;
    int64_t improvements;
    double total_seconds;
} ARALNSOperatorStats;

#endif /* ARBOR_AR_TYPES_H */
