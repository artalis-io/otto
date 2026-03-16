#include <math.h>
#include "lp_bfcp_policy.h"
#include "lp_glpk_strict.h"

static int bfcp_backend_supported(int backend) {
    return backend >= LP_GLPK_BFCP_BACKEND_LUF_FT &&
           backend <= LP_GLPK_BFCP_BACKEND_CGR;
}

void lp_bfcp_policy_request_init(LPBFCPPolicyRequest *req) {
    if (!req) return;
    req->requested_backend = LP_GLPK_BFCP_BACKEND_LUF_FT;
    req->requested_update_limit = -1;
    req->requested_pivot_tol = 0.0;
    req->requested_growth_guard = 0.0;
}

void lp_bfcp_policy_effective_init(LPBFCPPolicyEffective *eff) {
    if (!eff) return;
    eff->effective_backend = LP_GLPK_BFCP_BACKEND_LUF_FT;
    eff->backend_supported = 1;
    eff->update_limit_override = -1;
    eff->pivot_tol_override = 0.0;
    eff->growth_guard_override = 0.0;
}

void lp_bfcp_policy_refactor_signals_init(LPBFCPRefactorSignals *sig) {
    if (!sig) return;

    sig->strict_mode = 0;
    sig->num_updates = 0;
    sig->max_updates = 0;
    sig->growth_factor = 1.0;
    sig->growth_guard_threshold = 1e8;

    sig->use_ft_updates = 1;
    sig->m = 0;
    sig->ft_num_updates = 0;
    sig->spike_pool_used = 0;
    sig->spike_pool_capacity = 0;
    sig->update_aged = 0;

    sig->min_ft_updates_for_avg_density = 8;
    sig->spike_dense_reject_m_min = 300;
    sig->spike_avg_refactor_ratio = 0.45;
    sig->spike_avg_refactor_aged_ratio = 0.35;
    sig->spike_pool_warn_pct = 85;
    sig->spike_work_multiplier = 8;

    sig->cond_min_updates = 10;
    sig->cond_estimate = 1.0;
    sig->cond_severe_ratio = 1e10;
    sig->cond_adaptive_hi = 1e8;
    sig->cond_adaptive_mid = 1e6;
}

int lp_bfcp_policy_compute(const LPBFCPPolicyRequest *req,
                           LPBFCPPolicyEffective *eff) {
    int requested_backend = LP_GLPK_BFCP_BACKEND_LUF_FT;

    if (!req || !eff) return -1;
    lp_bfcp_policy_effective_init(eff);

    requested_backend = req->requested_backend;
    eff->backend_supported = bfcp_backend_supported(requested_backend);
    eff->effective_backend = eff->backend_supported
        ? requested_backend
        : LP_GLPK_BFCP_BACKEND_LUF_FT;

    if (req->requested_update_limit > 0) {
        eff->update_limit_override = req->requested_update_limit;
    }
    if (isfinite(req->requested_pivot_tol) && req->requested_pivot_tol > 0.0) {
        eff->pivot_tol_override = req->requested_pivot_tol;
    }
    if (isfinite(req->requested_growth_guard) && req->requested_growth_guard > 0.0) {
        eff->growth_guard_override = req->requested_growth_guard;
    }

    return 0;
}

int lp_bfcp_policy_effective_update_limit(const LPBFCPRefactorSignals *sig) {
    int adaptive_limit;

    if (!sig) return 0;
    adaptive_limit = sig->max_updates;
    if (adaptive_limit <= 0) return 0;
    if (!lp_glpk_strict_allow_bfcp_adaptive_reasons(sig->strict_mode)) {
        return adaptive_limit;
    }

    /* Keep base update budget until enough updates are accumulated to estimate
     * conditioning drift reliably. */
    if (sig->num_updates < sig->cond_min_updates) {
        return adaptive_limit;
    }

    if (sig->cond_estimate > sig->cond_adaptive_hi) {
        adaptive_limit = sig->max_updates / 4;
    } else if (sig->cond_estimate > sig->cond_adaptive_mid) {
        adaptive_limit = sig->max_updates / 2;
    }

    if (adaptive_limit < 1) adaptive_limit = 1;
    return adaptive_limit;
}

int lp_bfcp_policy_dense_reject_min_updates(const LPBFCPRefactorSignals *sig) {
    int min_updates;

    if (!sig) return 8;
    min_updates = sig->min_ft_updates_for_avg_density;
    if (min_updates < 1) min_updates = 1;
    return min_updates;
}

int lp_bfcp_policy_refactor_hard_trigger(const LPBFCPRefactorSignals *sig) {
    if (!sig) return 0;

    if (sig->max_updates > 0 && sig->num_updates >= sig->max_updates) {
        return 1;
    }
    if (isfinite(sig->growth_factor) &&
        sig->growth_factor > sig->growth_guard_threshold * 3.0) {
        return 1;
    }
    if (!lp_glpk_strict_allow_bfcp_adaptive_reasons(sig->strict_mode)) {
        return 0;
    }
    if (isfinite(sig->cond_estimate) && sig->cond_estimate > 1e10) {
        return 1;
    }
    if (sig->use_ft_updates &&
        sig->spike_pool_capacity > 0 &&
        sig->spike_pool_used >= (sig->spike_pool_capacity * 95) / 100) {
        return 1;
    }
    return 0;
}

int lp_bfcp_policy_refactor_reason(const LPBFCPRefactorSignals *sig) {
    int adaptive_limit;
    int min_dense_updates;
    int allow_adaptive;

    if (!sig) return LP_BFCP_REFACTOR_REASON_NONE;
    allow_adaptive = lp_glpk_strict_allow_bfcp_adaptive_reasons(sig->strict_mode);

    if (sig->max_updates > 0 && sig->num_updates >= sig->max_updates) {
        return LP_BFCP_REFACTOR_REASON_MAX_UPDATES;
    }

    if (sig->growth_factor > sig->growth_guard_threshold) {
        return LP_BFCP_REFACTOR_REASON_GROWTH_GUARD;
    }
    if (!allow_adaptive) {
        return LP_BFCP_REFACTOR_REASON_NONE;
    }

    min_dense_updates = lp_bfcp_policy_dense_reject_min_updates(sig);
    if (sig->use_ft_updates &&
        sig->m >= sig->spike_dense_reject_m_min &&
        sig->ft_num_updates >= min_dense_updates &&
        sig->spike_pool_used > 0) {
        double avg_spike_ratio = ((double)sig->spike_pool_used /
                                  (double)sig->ft_num_updates) / (double)sig->m;
        double avg_spike_limit = sig->update_aged
            ? sig->spike_avg_refactor_aged_ratio
            : sig->spike_avg_refactor_ratio;
        if (avg_spike_ratio > avg_spike_limit) {
            return LP_BFCP_REFACTOR_REASON_AVG_SPIKE_DENSITY;
        }
    }

    if (sig->num_updates >= sig->cond_min_updates) {
        double cond_ratio = sig->growth_factor * sig->cond_estimate;
        if (cond_ratio > sig->cond_severe_ratio) {
            return LP_BFCP_REFACTOR_REASON_COND_SEVERE;
        }

        adaptive_limit = lp_bfcp_policy_effective_update_limit(sig);
        if (adaptive_limit > 0 &&
            adaptive_limit < sig->max_updates &&
            sig->num_updates >= adaptive_limit) {
            return LP_BFCP_REFACTOR_REASON_COND_ADAPTIVE_LIMIT;
        }
    }

    if (sig->use_ft_updates &&
        sig->spike_pool_capacity > 0 &&
        sig->spike_pool_used >
            (sig->spike_pool_capacity * sig->spike_pool_warn_pct) / 100) {
        return LP_BFCP_REFACTOR_REASON_SPIKE_POOL_WARN;
    }

    if (sig->use_ft_updates &&
        sig->m >= 500 &&
        sig->spike_pool_used > sig->m * sig->spike_work_multiplier) {
        return LP_BFCP_REFACTOR_REASON_SPIKE_WORK;
    }

    return LP_BFCP_REFACTOR_REASON_NONE;
}

const char *lp_bfcp_policy_refactor_reason_string(int reason) {
    switch ((LPBFCPRefactorReason)reason) {
        case LP_BFCP_REFACTOR_REASON_NONE: return "none";
        case LP_BFCP_REFACTOR_REASON_MAX_UPDATES: return "max_updates";
        case LP_BFCP_REFACTOR_REASON_GROWTH_GUARD: return "growth_guard";
        case LP_BFCP_REFACTOR_REASON_AVG_SPIKE_DENSITY: return "avg_spike_density";
        case LP_BFCP_REFACTOR_REASON_COND_SEVERE: return "cond_severe";
        case LP_BFCP_REFACTOR_REASON_COND_ADAPTIVE_LIMIT: return "cond_adaptive_limit";
        case LP_BFCP_REFACTOR_REASON_SPIKE_POOL_WARN: return "spike_pool_warn";
        case LP_BFCP_REFACTOR_REASON_SPIKE_WORK: return "spike_work";
        default: return "unknown";
    }
}
