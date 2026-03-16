/*
 * Ralph - LP Basis Governor (G0 shadow mode)
 */

#include <string.h>
#include "lp_basis_governor.h"

static int phase_is_primal(int phase) {
    return (phase == LP_BASIS_GOV_PHASE1 || phase == LP_BASIS_GOV_PHASE2);
}

int lp_basis_governor_mode_is_valid(int mode) {
    return mode == LP_BASIS_GOV_MODE_OFF ||
           mode == LP_BASIS_GOV_MODE_SHADOW ||
           mode == LP_BASIS_GOV_MODE_CONTROL_PHASE2;
}

int lp_basis_governor_get_mode(const LPBasisGovernorState *state) {
    if (!state) return LP_BASIS_GOV_MODE_OFF;
    if (!lp_basis_governor_mode_is_valid(state->mode)) {
        return LP_BASIS_GOV_MODE_OFF;
    }
    return state->mode;
}

void lp_basis_governor_set_mode(LPBasisGovernorState *state, int mode) {
    if (!state) return;
    if (!lp_basis_governor_mode_is_valid(mode)) {
        state->mode = LP_BASIS_GOV_MODE_OFF;
        return;
    }
    state->mode = mode;
}

void lp_basis_governor_begin_solve(LPBasisGovernorState *state) {
    int mode = LP_BASIS_GOV_MODE_OFF;
    if (!state) return;
    mode = lp_basis_governor_get_mode(state);
    memset(state, 0, sizeof(*state));
    state->mode = mode;
}

int lp_basis_governor_shadow_decide(int phase,
                                    int lu_health_refactor_now,
                                    int periodic_policy_refactor_now) {
    if (phase != LP_BASIS_GOV_PHASE1 &&
        phase != LP_BASIS_GOV_PHASE2 &&
        phase != LP_BASIS_GOV_PHASE_DUAL) {
        return 0;
    }
    return (lu_health_refactor_now || periodic_policy_refactor_now) ? 1 : 0;
}

int lp_basis_governor_decide_refactor(const LPBasisGovernorState *state,
                                      int phase,
                                      int lu_health_refactor_now,
                                      int periodic_policy_refactor_now,
                                      int actual_refactor_now) {
    int mode = lp_basis_governor_get_mode(state);
    int shadow_refactor = lp_basis_governor_shadow_decide(phase,
                                                          lu_health_refactor_now,
                                                          periodic_policy_refactor_now);
    int actual = actual_refactor_now ? 1 : 0;

    if (mode == LP_BASIS_GOV_MODE_CONTROL_PHASE2 &&
        phase == LP_BASIS_GOV_PHASE2) {
        return shadow_refactor;
    }
    return actual;
}

void lp_basis_governor_observe_iter(LPBasisGovernorState *state,
                                    int phase,
                                    int shadow_refactor_now,
                                    int actual_refactor_now) {
    lp_basis_governor_observe_refactor(state,
                                       phase,
                                       shadow_refactor_now,
                                       actual_refactor_now);
}

void lp_basis_governor_observe_refactor(LPBasisGovernorState *state,
                                        int phase,
                                        int shadow_refactor_now,
                                        int actual_refactor_now) {
    int mode = LP_BASIS_GOV_MODE_OFF;
    if (!state) return;
    mode = lp_basis_governor_get_mode(state);
    if (mode == LP_BASIS_GOV_MODE_OFF) return;

    if (shadow_refactor_now) {
        if (phase == LP_BASIS_GOV_PHASE1) state->shadow_refactor_yes_phase1++;
        else if (phase == LP_BASIS_GOV_PHASE2) state->shadow_refactor_yes_phase2++;
        else if (phase == LP_BASIS_GOV_PHASE_DUAL) state->shadow_refactor_yes_dual++;
    } else {
        if (phase == LP_BASIS_GOV_PHASE1) state->shadow_refactor_no_phase1++;
        else if (phase == LP_BASIS_GOV_PHASE2) state->shadow_refactor_no_phase2++;
        else if (phase == LP_BASIS_GOV_PHASE_DUAL) state->shadow_refactor_no_dual++;
    }

    if (shadow_refactor_now != actual_refactor_now) {
        if (phase_is_primal(phase)) {
            state->shadow_disagree_primal_refactor++;
        } else if (phase == LP_BASIS_GOV_PHASE_DUAL) {
            state->shadow_disagree_dual_refactor++;
        }
    }
}

int lp_basis_governor_shadow_decide_lu_backend(int markowitz_eligible,
                                               int supernode_eligible) {
    if (markowitz_eligible) return LP_BASIS_GOV_BACKEND_MARKOWITZ;
    if (supernode_eligible) return LP_BASIS_GOV_BACKEND_SUPERNODE;
    return LP_BASIS_GOV_BACKEND_DENSE;
}

void lp_basis_governor_observe_lu_backend(LPBasisGovernorState *state,
                                          int shadow_backend_pick,
                                          int actual_backend_used) {
    int mode = LP_BASIS_GOV_MODE_OFF;
    if (!state) return;
    mode = lp_basis_governor_get_mode(state);
    if (mode == LP_BASIS_GOV_MODE_OFF) return;

    if (shadow_backend_pick == LP_BASIS_GOV_BACKEND_MARKOWITZ) {
        state->shadow_backend_pick_markowitz++;
    } else if (shadow_backend_pick == LP_BASIS_GOV_BACKEND_SUPERNODE) {
        state->shadow_backend_pick_supernode++;
    } else if (shadow_backend_pick == LP_BASIS_GOV_BACKEND_DENSE) {
        state->shadow_backend_pick_dense++;
    }

    if (shadow_backend_pick != actual_backend_used &&
        actual_backend_used != LP_BASIS_GOV_BACKEND_NONE) {
        state->shadow_disagree_lu_backend++;
    }
}
