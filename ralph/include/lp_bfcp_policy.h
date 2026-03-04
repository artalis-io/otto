#ifndef LP_BFCP_POLICY_H
#define LP_BFCP_POLICY_H

#include "lp_policy_glpk_compat.h"

/* BFCP policy module: normalize requested BFCP controls into effective
 * runtime overrides consumed by the solver/LU layer. */
typedef struct {
    int requested_backend;         /* LP_GLPK_BFCP_BACKEND_* */
    int requested_update_limit;    /* <=0 => auto/default */
    double requested_pivot_tol;    /* <=0/non-finite => auto/default */
    double requested_growth_guard; /* <=0/non-finite => auto/default */
} LPBFCPPolicyRequest;

typedef struct {
    int effective_backend;      /* currently clamped to LUF_FT */
    int backend_supported;      /* 1 if requested backend is available */
    int update_limit_override;  /* <=0 => use LU default */
    double pivot_tol_override;  /* <=0 => use LU default */
    double growth_guard_override; /* <=0 => use LU default */
} LPBFCPPolicyEffective;

typedef enum {
    LP_BFCP_REFACTOR_REASON_NONE = 0,
    LP_BFCP_REFACTOR_REASON_MAX_UPDATES = 1,
    LP_BFCP_REFACTOR_REASON_GROWTH_GUARD = 2,
    LP_BFCP_REFACTOR_REASON_AVG_SPIKE_DENSITY = 3,
    LP_BFCP_REFACTOR_REASON_COND_SEVERE = 4,
    LP_BFCP_REFACTOR_REASON_COND_ADAPTIVE_LIMIT = 5,
    LP_BFCP_REFACTOR_REASON_SPIKE_POOL_WARN = 6,
    LP_BFCP_REFACTOR_REASON_SPIKE_WORK = 7
} LPBFCPRefactorReason;

typedef struct {
    int num_updates;
    int max_updates;
    double growth_factor;
    double growth_guard_threshold;

    int use_ft_updates;
    int m;
    int ft_num_updates;
    int spike_pool_used;
    int spike_pool_capacity;
    int update_aged;

    /* Density/work thresholds */
    int min_ft_updates_for_avg_density;
    int spike_dense_reject_m_min;
    double spike_avg_refactor_ratio;
    double spike_avg_refactor_aged_ratio;
    int spike_pool_warn_pct;
    int spike_work_multiplier;

    /* Conditioning thresholds */
    int cond_min_updates;
    double cond_estimate;
    double cond_severe_ratio;
    double cond_adaptive_hi;
    double cond_adaptive_mid;
} LPBFCPRefactorSignals;

void lp_bfcp_policy_request_init(LPBFCPPolicyRequest *req);
void lp_bfcp_policy_effective_init(LPBFCPPolicyEffective *eff);
void lp_bfcp_policy_refactor_signals_init(LPBFCPRefactorSignals *sig);

/* Returns 0 on success, -1 on invalid pointers. */
int lp_bfcp_policy_compute(const LPBFCPPolicyRequest *req,
                           LPBFCPPolicyEffective *eff);
/* Runtime lifecycle helpers used by LU update/reinvert decisions. */
int lp_bfcp_policy_effective_update_limit(const LPBFCPRefactorSignals *sig);
int lp_bfcp_policy_dense_reject_min_updates(const LPBFCPRefactorSignals *sig);
int lp_bfcp_policy_refactor_hard_trigger(const LPBFCPRefactorSignals *sig);
int lp_bfcp_policy_refactor_reason(const LPBFCPRefactorSignals *sig);
const char *lp_bfcp_policy_refactor_reason_string(int reason);

#endif /* LP_BFCP_POLICY_H */
