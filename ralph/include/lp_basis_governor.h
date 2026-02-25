/*
 * Ralph - LP Basis Governor (G0 shadow mode)
 *
 * Shadow-only basis-governor contract:
 * - records what a unified governor would decide
 * - does not change solver behavior
 */

#ifndef RALPH_LP_BASIS_GOVERNOR_H
#define RALPH_LP_BASIS_GOVERNOR_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LP_BASIS_GOV_PHASE_INVALID = 0,
    LP_BASIS_GOV_PHASE1 = 1,
    LP_BASIS_GOV_PHASE2 = 2,
    LP_BASIS_GOV_PHASE_DUAL = 3
} LPBasisGovernorPhase;

typedef enum {
    LP_BASIS_GOV_BACKEND_NONE = 0,
    LP_BASIS_GOV_BACKEND_MARKOWITZ = 1,
    LP_BASIS_GOV_BACKEND_SUPERNODE = 2,
    LP_BASIS_GOV_BACKEND_DENSE = 3
} LPBasisGovernorBackend;

typedef struct {
    int shadow_refactor_yes_phase1;
    int shadow_refactor_yes_phase2;
    int shadow_refactor_yes_dual;
    int shadow_refactor_no_phase1;
    int shadow_refactor_no_phase2;
    int shadow_refactor_no_dual;
    int shadow_backend_pick_markowitz;
    int shadow_backend_pick_supernode;
    int shadow_backend_pick_dense;
    int shadow_disagree_primal_refactor;
    int shadow_disagree_dual_refactor;
    int shadow_disagree_lu_backend;
} LPBasisGovernorState;

void lp_basis_governor_begin_solve(LPBasisGovernorState *state);

int lp_basis_governor_shadow_decide(int phase,
                                    int lu_health_refactor_now,
                                    int periodic_policy_refactor_now);

void lp_basis_governor_observe_iter(LPBasisGovernorState *state,
                                    int phase,
                                    int shadow_refactor_now,
                                    int actual_refactor_now);

void lp_basis_governor_observe_refactor(LPBasisGovernorState *state,
                                        int phase,
                                        int shadow_refactor_now,
                                        int actual_refactor_now);

int lp_basis_governor_shadow_decide_lu_backend(int markowitz_eligible,
                                               int supernode_eligible);

void lp_basis_governor_observe_lu_backend(LPBasisGovernorState *state,
                                          int shadow_backend_pick,
                                          int actual_backend_used);

#ifdef __cplusplus
}
#endif

#endif /* RALPH_LP_BASIS_GOVERNOR_H */
