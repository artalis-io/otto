#ifndef LP_REINVERT_CONTROLLER_H
#define LP_REINVERT_CONTROLLER_H

typedef enum {
    LP_REINVERT_DECISION_ALLOW = 0,
    LP_REINVERT_DECISION_DEFER = 1,
    LP_REINVERT_DECISION_FORCE = 2
} LPReinvertDecision;

typedef enum {
    LP_REINVERT_REASON_NONE = 0,
    LP_REINVERT_REASON_INVALID_INPUTS = 1,
    LP_REINVERT_REASON_WARMUP = 2,
    LP_REINVERT_REASON_COOLDOWN = 3,
    LP_REINVERT_REASON_HARD_LU_HEALTH = 4,
    LP_REINVERT_REASON_SOFT_LU_HEALTH = 5,
    LP_REINVERT_REASON_UPDATE_AGE = 6,
    LP_REINVERT_REASON_COST_DAMPEN = 7,
    LP_REINVERT_REASON_DENSITY_PRESSURE = 8,
    LP_REINVERT_REASON_PERIODIC_CADENCE = 9
} LPReinvertReason;

typedef struct {
    double iter_cost_ewma_ms;
    double refactor_cost_ewma_ms;
    double solve_density_ewma;
    double update_age_ratio_ewma;
    int soft_lu_breach_streak;
    int cooldown_updates_remaining;
    int iter_cost_samples;
    int refactor_cost_samples;
    int density_samples;
    int update_age_samples;
} LPReinvertControllerState;

typedef struct {
    int phase;
    int iter;
    int m;
    int num_updates;
    int max_updates;
    int periodic_due;
    int min_update_age;
    int cooldown_updates;
    int hard_lu_trigger;
    int soft_lu_trigger;
    double ftran_density;
    double btran_density;
} LPReinvertControllerSignals;

typedef struct {
    LPReinvertDecision decision;
    LPReinvertReason reason;
    double refactor_to_iter_cost_ratio;
    double solve_density;
    int cooldown_updates_next;
    int soft_lu_breach_streak_next;
} LPReinvertControllerDecision;

void lp_reinvert_controller_state_reset(LPReinvertControllerState *state);
void lp_reinvert_controller_state_record_iter_cost(LPReinvertControllerState *state,
                                                   double iter_ms);
void lp_reinvert_controller_state_record_refactor_cost(
    LPReinvertControllerState *state,
    double refactor_ms);
void lp_reinvert_controller_state_record_solve_density(LPReinvertControllerState *state,
                                                       double ftran_density,
                                                       double btran_density);
void lp_reinvert_controller_state_record_update_age_ratio(
    LPReinvertControllerState *state,
    int num_updates,
    int max_updates);
void lp_reinvert_controller_state_set_cooldown(LPReinvertControllerState *state,
                                               int cooldown_updates);
void lp_reinvert_controller_state_apply_decision(
    LPReinvertControllerState *state,
    const LPReinvertControllerDecision *decision);

LPReinvertControllerDecision lp_reinvert_controller_decide(
    const LPReinvertControllerState *state,
    const LPReinvertControllerSignals *signals);

const char *lp_reinvert_controller_reason_string(LPReinvertReason reason);

#endif
