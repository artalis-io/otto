#include <stdio.h>
#include <math.h>
#include "lp.h"

/* Internal dual policy hooks from dual_simplex.c */
void dual_ratio_adaptive_config_for_test(int m,
                                         double lu_pivot_tol,
                                         double cond_estimate,
                                         double growth_factor,
                                         double *strict_pivot_floor_out,
                                         double *base_pivot_floor_out,
                                         double *strict_theta_floor_out,
                                         double *hard_refactor_floor_out,
                                         int *flip_round_cap_out);
int dual_ratio_passes_theta_floor_for_test(double ratio, double theta_floor);
int dual_ratio_candidate_value_for_test(const SimplexTableau *tab,
                                        int dir,
                                        int var,
                                        double alpha_j,
                                        double pivot_floor,
                                        double *ratio_out);
double dual_max_reduced_cost_violation_for_test(const SimplexTableau *tab);
int dual_sparse_pressure_force_refactor_for_test(int m,
                                                 int num_updates,
                                                 int max_updates,
                                                 int spike_pool_used,
                                                 int spike_pool_capacity,
                                                 int ftran_nnz,
                                                 int btran_nnz);
int dual_smcp_shift_allows_perturb_for_test(int smcp_shift);
int dual_ratio_scan_direction_for_test(int smcp_aorn);
int dual_ratio_use_at_kernel_for_test(int smcp_aorn, int has_row_scatter);
void dual_cadence_intervals_for_test(int requested_base,
                                     int requested_rc,
                                     int *base_out,
                                     int *rc_out);
void dual_reinvert_hard_trigger_safety_step_for_test(int iter,
                                                     int hard_trigger_total,
                                                     int *last_total_io,
                                                     int *last_iter_io,
                                                     int *burst_io,
                                                     int *demoted_io);
int dual_reinvert_effective_mode_for_test(int configured_mode, int demoted);
int dual_phase1_rescue_progress_limit_for_test(int m);
int dual_phase1_rescue_progress_update_for_test(int current_rows,
                                                double current_max,
                                                double current_sum,
                                                int stall_limit,
                                                int *best_rows_io,
                                                double *best_max_io,
                                                double *best_sum_io,
                                                int *stall_count_io);

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT_TRUE(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s\n", msg); \
    } \
} while (0)

static int build_flip_fixture(SimplexSolver **solver_out,
                              SimplexTableau **tab_out,
                              LPModel **model_out) {
    LPModel *model = NULL;
    SimplexSolver *solver = NULL;
    SimplexTableau *tab = NULL;
    int idx[2] = {0, 1};
    double val[2] = {-1.0, -1.0};

    if (!solver_out || !tab_out || !model_out) return -1;
    *solver_out = NULL;
    *tab_out = NULL;
    *model_out = NULL;

    model = lp_model_create();
    if (!model) return -1;
    model->obj_sense = RALPH_MINIMIZE;

    if (lp_model_add_var(model, 0.0, 1.0, 1e-8, 'C') < 0) goto fail;
    if (lp_model_add_var(model, 0.0, 1.0, 2.0, 'C') < 0) goto fail;
    if (lp_model_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, -1.0) < 0) goto fail;
    if (lp_model_finalize(model) != 0) goto fail;

    solver = simplex_create(model);
    if (!solver) goto fail;
    tab = tableau_create(model);
    if (!tab) goto fail;

    solver->tableau = tab;
    tab->owner = solver;
    solver->telemetry_enabled = 1;
    solver->use_dual_bound_flip = 1;
    solver->dual_ratio_test_mode = LP_DUAL_RATIO_TEST_FLIP;

    if (tableau_refactorize(tab) != 0) goto fail;
    if (tableau_compute_solution(tab) != 0) goto fail;
    if (tableau_compute_reduced_costs(tab) != 0) goto fail;

    /* Seed a deterministic dual-ratio scenario:
     * - Leaving basic row is infeasible below its lower bound.
     * - Both structural vars stay non-basic at lower bounds.
     * - rc[0] is tiny-positive (eligible for flip window), rc[1] is large.
     */
    {
        int leaving_var = tab->basis[0];
        tab->ub_ext[leaving_var] = 0.0;
        tab->x[leaving_var] = tab->ub_ext[leaving_var] + 0.5;
    }
    tab->var_status[0] = RALPH_NONBASIC_LOWER;
    tab->var_status[1] = RALPH_NONBASIC_LOWER;
    tab->x[0] = tab->lb_ext[0];
    tab->x[1] = tab->lb_ext[1];
    tab->rc[0] = 1e-8;
    tab->rc[1] = 2.0;

    *solver_out = solver;
    *tab_out = tab;
    *model_out = model;
    return 0;

fail:
    if (solver) simplex_free(solver);
    else if (tab) tableau_free(tab);
    if (model) lp_model_free(model);
    return -1;
}

static void free_flip_fixture(SimplexSolver *solver, LPModel *model) {
    if (solver) simplex_free(solver);
    if (model) lp_model_free(model);
}

static void test_flip_mode_applies_flip_only_step(void) {
    SimplexSolver *solver = NULL;
    SimplexTableau *tab = NULL;
    LPModel *model = NULL;
    int entering = -99;
    double theta = -1.0;
    int rc;

    if (build_flip_fixture(&solver, &tab, &model) != 0) {
        ASSERT_TRUE(0, "fixture build");
        return;
    }

    /* One constraint => leaving row is position 0. */
    rc = dual_ratio_test(tab, 0, &entering, &theta);
    ASSERT_TRUE(rc == 0, "dual_ratio_test succeeds in flip mode");
    ASSERT_TRUE(entering == -2, "flip mode returns entering=-2 for flip-only step");
    ASSERT_TRUE(fabs(theta) < 1e-12, "flip-only step returns theta=0");
    ASSERT_TRUE(tab->var_status[0] == RALPH_NONBASIC_UPPER, "boxed candidate flips to upper bound");
    ASSERT_TRUE(fabs(tab->x[0] - tab->ub_ext[0]) < 1e-12, "flipped candidate value snapped to upper bound");
    ASSERT_TRUE(solver->telemetry.perf_dual_bound_flip_applied >= 1,
                "dual flip telemetry increments");
    ASSERT_TRUE(solver->telemetry.perf_dual_bound_flip_iterative >= 1,
                "dual iterative flip telemetry increments");
    ASSERT_TRUE(solver->telemetry.perf_dual_bound_flip_startup == 0,
                "dual startup flip telemetry unchanged in iterative-only path");

    free_flip_fixture(solver, model);
}

static void test_harris_mode_keeps_regular_entering(void) {
    SimplexSolver *solver = NULL;
    SimplexTableau *tab = NULL;
    LPModel *model = NULL;
    int entering = -99;
    double theta = -1.0;
    int rc;

    if (build_flip_fixture(&solver, &tab, &model) != 0) {
        ASSERT_TRUE(0, "fixture build");
        return;
    }

    solver->dual_ratio_test_mode = LP_DUAL_RATIO_TEST_HARRIS;
    solver->use_dual_bound_flip = 1;
    solver->telemetry.perf_dual_bound_flip_applied = 0;
    solver->telemetry.perf_dual_bound_flip_startup = 0;
    solver->telemetry.perf_dual_bound_flip_iterative = 0;

    rc = dual_ratio_test(tab, 0, &entering, &theta);
    ASSERT_TRUE(rc == 0, "dual_ratio_test succeeds in Harris mode");
    ASSERT_TRUE(entering >= 0, "Harris mode returns a normal entering column");
    ASSERT_TRUE(entering != -2, "Harris mode does not produce flip-only step");
    ASSERT_TRUE(solver->telemetry.perf_dual_bound_flip_applied == 0,
                "Harris mode leaves dual flip telemetry unchanged");
    ASSERT_TRUE(solver->telemetry.perf_dual_bound_flip_startup == 0,
                "Harris mode leaves dual startup flip telemetry unchanged");
    ASSERT_TRUE(solver->telemetry.perf_dual_bound_flip_iterative == 0,
                "Harris mode leaves dual iterative flip telemetry unchanged");

    free_flip_fixture(solver, model);
}

static void test_harris_mode_accepts_zero_ratio_candidate(void) {
    SimplexSolver *solver = NULL;
    SimplexTableau *tab = NULL;
    LPModel *model = NULL;
    int entering = -99;
    double theta = -1.0;
    int rc;

    if (build_flip_fixture(&solver, &tab, &model) != 0) {
        ASSERT_TRUE(0, "fixture build");
        return;
    }

    solver->dual_ratio_test_mode = LP_DUAL_RATIO_TEST_HARRIS;
    solver->use_dual_bound_flip = 1;
    tab->rc[0] = 0.0;

    rc = dual_ratio_test(tab, 0, &entering, &theta);
    ASSERT_TRUE(rc == 0, "dual_ratio_test accepts zero-ratio Harris candidate");
    ASSERT_TRUE(entering == 0, "zero-ratio candidate is selected before positive ratios");
    ASSERT_TRUE(fabs(theta) < 1e-12, "zero-ratio candidate returns theta=0");

    free_flip_fixture(solver, model);
}

static void test_harris_mode_clamps_tiny_negative_ratio_candidate(void) {
    SimplexSolver *solver = NULL;
    SimplexTableau *tab = NULL;
    LPModel *model = NULL;
    int entering = -99;
    double theta = -1.0;
    int rc;

    if (build_flip_fixture(&solver, &tab, &model) != 0) {
        ASSERT_TRUE(0, "fixture build");
        return;
    }

    solver->dual_ratio_test_mode = LP_DUAL_RATIO_TEST_HARRIS;
    solver->use_dual_bound_flip = 1;
    tab->rc[0] = -1e-12;

    rc = dual_ratio_test(tab, 0, &entering, &theta);
    ASSERT_TRUE(rc == 0, "dual_ratio_test accepts tiny-negative ratio Harris candidate");
    ASSERT_TRUE(entering == 0, "tiny-negative ratio candidate is clamped and selected");
    ASSERT_TRUE(fabs(theta) < 1e-12, "tiny-negative ratio candidate returns theta=0");

    free_flip_fixture(solver, model);
}

static void test_flip_mode_respects_runtime_disable(void) {
    SimplexSolver *solver = NULL;
    SimplexTableau *tab = NULL;
    LPModel *model = NULL;
    int entering = -99;
    double theta = -1.0;
    int rc;

    if (build_flip_fixture(&solver, &tab, &model) != 0) {
        ASSERT_TRUE(0, "fixture build");
        return;
    }

    solver->dual_ratio_test_mode = LP_DUAL_RATIO_TEST_FLIP;
    solver->use_dual_bound_flip = 0;
    solver->telemetry.perf_dual_bound_flip_applied = 0;
    solver->telemetry.perf_dual_bound_flip_startup = 0;
    solver->telemetry.perf_dual_bound_flip_iterative = 0;

    rc = dual_ratio_test(tab, 0, &entering, &theta);
    ASSERT_TRUE(rc == 0, "dual_ratio_test succeeds when flip runtime is disabled");
    ASSERT_TRUE(entering >= 0, "flip runtime disable falls back to regular entering");
    ASSERT_TRUE(entering != -2, "flip runtime disable avoids flip-only step");
    ASSERT_TRUE(solver->telemetry.perf_dual_bound_flip_applied == 0,
                "runtime-disabled flip mode does not apply flips");
    ASSERT_TRUE(solver->telemetry.perf_dual_bound_flip_startup == 0,
                "runtime-disabled flip mode does not apply startup flips");
    ASSERT_TRUE(solver->telemetry.perf_dual_bound_flip_iterative == 0,
                "runtime-disabled flip mode does not apply iterative flips");

    free_flip_fixture(solver, model);
}

static void test_adaptive_ratio_thresholds_scale_with_lu_health(void) {
    double strict_good = 0.0, base_good = 0.0, theta_good = 0.0, hard_good = 0.0;
    double strict_bad = 0.0, base_bad = 0.0, theta_bad = 0.0, hard_bad = 0.0;
    int rounds_good = 0, rounds_bad = 0;

    dual_ratio_adaptive_config_for_test(800,
                                        1e-9,
                                        1e5,
                                        10.0,
                                        &strict_good,
                                        &base_good,
                                        &theta_good,
                                        &hard_good,
                                        &rounds_good);
    dual_ratio_adaptive_config_for_test(800,
                                        1e-9,
                                        1e9,
                                        1e5,
                                        &strict_bad,
                                        &base_bad,
                                        &theta_bad,
                                        &hard_bad,
                                        &rounds_bad);

    ASSERT_TRUE(strict_good > 0.0 && base_good > 0.0, "adaptive thresholds produce positive healthy floors");
    ASSERT_TRUE(strict_bad > strict_good, "adaptive thresholds tighten strict pivot floor for unhealthy LU");
    ASSERT_TRUE(base_bad > base_good, "adaptive thresholds tighten base pivot floor for unhealthy LU");
    ASSERT_TRUE(theta_bad > theta_good, "adaptive thresholds tighten theta floor for unhealthy LU");
    ASSERT_TRUE(hard_bad >= hard_good, "adaptive thresholds tighten hard refactor floor for unhealthy LU");
    ASSERT_TRUE(rounds_bad >= rounds_good, "adaptive thresholds keep or increase flip rounds under stress");
}

static void test_theta_floor_keeps_degenerate_limiter(void) {
    ASSERT_TRUE(dual_ratio_passes_theta_floor_for_test(0.0, 1e-10) == 1,
                "theta floor keeps exact zero dual limiter");
    ASSERT_TRUE(dual_ratio_passes_theta_floor_for_test(0.5e-10, 1e-10) == 1,
                "theta floor keeps tolerance-sized dual limiter");
    ASSERT_TRUE(dual_ratio_passes_theta_floor_for_test(2.0 * RALPH_OPT_TOL, 1e-3) == 0,
                "theta floor still rejects material positive progress below strict floor");
    ASSERT_TRUE(dual_ratio_passes_theta_floor_for_test(2e-10, 1e-10) == 1,
                "theta floor accepts positive progress above strict floor");
}

static void test_candidate_value_rejects_ineligible_sign_status(void) {
    SimplexSolver *solver = NULL;
    SimplexTableau *tab = NULL;
    LPModel *model = NULL;
    double ratio = -1.0;

    if (build_flip_fixture(&solver, &tab, &model) != 0) {
        ASSERT_TRUE(0, "fixture build");
        return;
    }

    tab->var_status[0] = RALPH_NONBASIC_UPPER;
    tab->rc[0] = -3.0;
    ASSERT_TRUE(dual_ratio_candidate_value_for_test(tab, -1, 0, 2.0,
                                                    RALPH_PIVOT_TOL, &ratio) == 0,
                "dual candidate value rejects dir/status/sign mismatch");
    ASSERT_TRUE(ratio == -1.0, "rejected dual candidate leaves ratio output unchanged");

    tab->var_status[0] = RALPH_NONBASIC_LOWER;
    tab->rc[0] = 3.0;
    ASSERT_TRUE(dual_ratio_candidate_value_for_test(tab, -1, 0, 2.0,
                                                    RALPH_PIVOT_TOL, &ratio) == 1,
                "dual candidate value accepts eligible sign/status");
    ASSERT_TRUE(fabs(ratio - 1.5) < 1e-12, "eligible dual candidate returns finite ratio");

    free_flip_fixture(solver, model);
}

static void test_dual_violation_ignores_fixed_excluded_nonbasic(void) {
    SimplexSolver *solver = NULL;
    SimplexTableau *tab = NULL;
    LPModel *model = NULL;

    if (build_flip_fixture(&solver, &tab, &model) != 0) {
        ASSERT_TRUE(0, "fixture build");
        return;
    }

    solver->smcp_excl = 1;
    solver->smcp_shift = 1;
    solver->smcp_tol_bnd = 1e-12;
    tab->var_status[0] = RALPH_NONBASIC_UPPER;
    tab->lb_ext[0] = 0.0;
    tab->ub_ext[0] = 0.0;
    tab->rc[0] = 1.0;
    ASSERT_TRUE(fabs(dual_max_reduced_cost_violation_for_test(tab)) < 1e-12,
                "dual violation check ignores excluded fixed nonbasic column");

    tab->ub_ext[0] = 1.0;
    ASSERT_TRUE(dual_max_reduced_cost_violation_for_test(tab) > 0.5,
                "dual violation check still catches movable upper-bound sign violation");

    free_flip_fixture(solver, model);
}

static void test_sparse_pressure_refactor_gate(void) {
    int trigger_small = dual_sparse_pressure_force_refactor_for_test(
        120, 80, 100, 0, 0, 80, 90);
    int trigger_healthy = dual_sparse_pressure_force_refactor_for_test(
        1200, 10, 100, 50, 4000, 200, 220);
    int trigger_dense_old = dual_sparse_pressure_force_refactor_for_test(
        1200, 80, 100, 1000, 4000, 800, 900);
    int trigger_pool = dual_sparse_pressure_force_refactor_for_test(
        1500, 30, 100, 7000, 8000, 700, 600);

    ASSERT_TRUE(trigger_small == 0, "sparse pressure gate disabled for small systems");
    ASSERT_TRUE(trigger_healthy == 0, "sparse pressure gate stays off for healthy sparse solves");
    ASSERT_TRUE(trigger_dense_old == 1, "sparse pressure gate triggers for aged dense solves");
    ASSERT_TRUE(trigger_pool == 1, "sparse pressure gate triggers under spike-pool pressure");
}

static void test_dual_smcp_shift_toggle(void) {
    ASSERT_TRUE(dual_smcp_shift_allows_perturb_for_test(0) == 0,
                "dual shift off disables perturbation");
    ASSERT_TRUE(dual_smcp_shift_allows_perturb_for_test(1) == 1,
                "dual shift on enables perturbation");
}

static void test_dual_aorn_scan_direction(void) {
    ASSERT_TRUE(dual_ratio_scan_direction_for_test(2) == 1,
                "dual aorn N^T keeps legacy forward scan");
    ASSERT_TRUE(dual_ratio_scan_direction_for_test(1) == -1,
                "dual aorn A^T scans reverse");
}

static void test_dual_aorn_kernel_selection(void) {
    ASSERT_TRUE(dual_ratio_use_at_kernel_for_test(2, 1) == 0,
                "dual aorn N^T keeps column-kernel");
    ASSERT_TRUE(dual_ratio_use_at_kernel_for_test(1, 0) == 0,
                "dual aorn A^T falls back when row-kernel unavailable");
    ASSERT_TRUE(dual_ratio_use_at_kernel_for_test(1, 1) == 1,
                "dual aorn A^T enables row-kernel when available");
}

static void test_dual_cadence_clamp(void) {
    int base = -1;
    int rc = -1;

    dual_cadence_intervals_for_test(0, 0, &base, &rc);
    ASSERT_TRUE(base == 50, "dual cadence default base interval");
    ASSERT_TRUE(rc == 25, "dual cadence default rc interval");

    dual_cadence_intervals_for_test(3, 1, &base, &rc);
    ASSERT_TRUE(base == 8, "dual cadence clamps tiny base interval");
    ASSERT_TRUE(rc == 10, "dual cadence clamps tiny rc interval");
}

static void test_dual_reinvert_hard_trigger_demotion(void) {
    int last_total = 0;
    int last_iter = -1;
    int burst = 0;
    int demoted = 0;

    for (int k = 0; k < 5; k++) {
        dual_reinvert_hard_trigger_safety_step_for_test(k * 10,
                                                        k + 1,
                                                        &last_total,
                                                        &last_iter,
                                                        &burst,
                                                        &demoted);
    }
    ASSERT_TRUE(demoted == 0, "dual reinvert safety keeps control_all before burst cap");

    dual_reinvert_hard_trigger_safety_step_for_test(50,
                                                    6,
                                                    &last_total,
                                                    &last_iter,
                                                    &burst,
                                                    &demoted);
    ASSERT_TRUE(demoted == 1, "dual reinvert safety demotes control_all on hard-trigger burst");
    ASSERT_TRUE(burst >= 6, "dual reinvert safety tracks hard-trigger burst depth");
}

static void test_dual_reinvert_hard_trigger_gap_resets_burst(void) {
    int last_total = 0;
    int last_iter = -1;
    int burst = 0;
    int demoted = 0;

    dual_reinvert_hard_trigger_safety_step_for_test(0,
                                                    1,
                                                    &last_total,
                                                    &last_iter,
                                                    &burst,
                                                    &demoted);
    dual_reinvert_hard_trigger_safety_step_for_test(90,
                                                    2,
                                                    &last_total,
                                                    &last_iter,
                                                    &burst,
                                                    &demoted);
    ASSERT_TRUE(demoted == 0, "dual reinvert safety does not demote on sparse hard-trigger events");
    ASSERT_TRUE(burst == 1, "dual reinvert safety resets burst after long gap");
}

static void test_dual_reinvert_effective_mode_mapping(void) {
    ASSERT_TRUE(dual_reinvert_effective_mode_for_test(LP_REINVERT_MODE_CONTROL_ALL, 0) ==
                    LP_REINVERT_MODE_CONTROL_ALL,
                "dual reinvert effective mode keeps control_all when not demoted");
    ASSERT_TRUE(dual_reinvert_effective_mode_for_test(LP_REINVERT_MODE_CONTROL_ALL, 1) ==
                    LP_REINVERT_MODE_SHADOW,
                "dual reinvert effective mode demotes control_all to shadow");
    ASSERT_TRUE(dual_reinvert_effective_mode_for_test(LP_REINVERT_MODE_CONTROL_PHASE1, 1) ==
                    LP_REINVERT_MODE_CONTROL_PHASE1,
                "dual reinvert effective mode keeps non-control_all modes unchanged");
}

static void test_dual_phase1_rescue_progress_limit(void) {
    ASSERT_TRUE(dual_phase1_rescue_progress_limit_for_test(8) == 16,
                "dual phase1 rescue progress limit clamps small systems");
    ASSERT_TRUE(dual_phase1_rescue_progress_limit_for_test(120) == 16,
                "dual phase1 rescue progress limit keeps medium systems on short stall window");
    ASSERT_TRUE(dual_phase1_rescue_progress_limit_for_test(800) == 16,
                "dual phase1 rescue progress limit clamps large systems");
}

static void test_dual_phase1_rescue_progress_gate(void) {
    int best_rows = -1;
    int stall_count = 0;
    double best_max = 0.0;
    double best_sum = 0.0;

    ASSERT_TRUE(dual_phase1_rescue_progress_update_for_test(
                    5, 10.0, 30.0, 2,
                    &best_rows, &best_max, &best_sum, &stall_count) == 0,
                "dual phase1 rescue progress accepts first infeasibility sample");
    ASSERT_TRUE(best_rows == 5 && fabs(best_max - 10.0) < 1e-12 &&
                    fabs(best_sum - 30.0) < 1e-12 && stall_count == 0,
                "dual phase1 rescue progress records first best sample");

    ASSERT_TRUE(dual_phase1_rescue_progress_update_for_test(
                    5, 10.0, 30.0, 2,
                    &best_rows, &best_max, &best_sum, &stall_count) == 0,
                "dual phase1 rescue progress tolerates one stalled sample");
    ASSERT_TRUE(stall_count == 1,
                "dual phase1 rescue progress increments stall count on no progress");

    ASSERT_TRUE(dual_phase1_rescue_progress_update_for_test(
                    5, 9.5, 29.0, 2,
                    &best_rows, &best_max, &best_sum, &stall_count) == 0,
                "dual phase1 rescue progress resets on max infeasibility improvement");
    ASSERT_TRUE(stall_count == 0 && fabs(best_max - 9.5) < 1e-12,
                "dual phase1 rescue progress stores improved max infeasibility");

    ASSERT_TRUE(dual_phase1_rescue_progress_update_for_test(
                    4, 9.5, 28.0, 2,
                    &best_rows, &best_max, &best_sum, &stall_count) == 0,
                "dual phase1 rescue progress resets on fewer infeasible rows");
    ASSERT_TRUE(best_rows == 4 && stall_count == 0,
                "dual phase1 rescue progress stores improved infeasible-row count");

    ASSERT_TRUE(dual_phase1_rescue_progress_update_for_test(
                    4, 9.5, 28.0, 2,
                    &best_rows, &best_max, &best_sum, &stall_count) == 0,
                "dual phase1 rescue progress permits first repeated stalled sample");
    ASSERT_TRUE(dual_phase1_rescue_progress_update_for_test(
                    4, 9.5, 28.0, 2,
                    &best_rows, &best_max, &best_sum, &stall_count) == 1,
                "dual phase1 rescue progress aborts after repeated stalls");
}

static void test_primal_ratio_uses_current_entering_value_for_bound_flip(void) {
    LPModel *model = NULL;
    SimplexSolver *solver = NULL;
    SimplexTableau *tab = NULL;
    int idx[1] = {0};
    double val[1] = {1.0};
    int leaving = -99;
    double theta = -1.0;

    model = lp_model_create();
    ASSERT_TRUE(model != NULL, "primal ratio fixture model allocation");
    if (!model) return;

    model->obj_sense = RALPH_MINIMIZE;
    ASSERT_TRUE(lp_model_add_var(model, 0.0, 1.0, -1.0, 'C') == 0,
                "primal ratio fixture variable");
    ASSERT_TRUE(lp_model_add_constraint(model, 1, idx, val, RALPH_LESS_EQUAL, 100.0) == 0,
                "primal ratio fixture constraint");
    ASSERT_TRUE(lp_model_finalize(model) == 0, "primal ratio fixture finalize");

    solver = simplex_create(model);
    tab = tableau_create(model);
    ASSERT_TRUE(solver != NULL && tab != NULL, "primal ratio fixture tableau allocation");
    if (!solver || !tab) goto done;
    solver->tableau = tab;
    tab->owner = solver;

    ASSERT_TRUE(tableau_refactorize(tab) == 0, "primal ratio fixture refactor");
    ASSERT_TRUE(tableau_compute_solution(tab) == 0, "primal ratio fixture solution");

    tab->var_status[0] = RALPH_NONBASIC_LOWER;
    tab->x[0] = 0.1; /* Perturbed-bound state: status lower, value at old bound. */
    tab->lb_ext[0] = 0.0;
    tab->ub_ext[0] = 1.0;

    ASSERT_TRUE(ratio_test_harris(tab, 0, &leaving, &theta) == 0,
                "primal ratio harris succeeds");
    ASSERT_TRUE(leaving == -2, "primal ratio chooses entering bound flip");
    ASSERT_TRUE(fabs(theta - 0.9) < 1e-12,
                "primal ratio bound-flip theta uses current entering value");

done:
    if (solver) simplex_free(solver);
    else if (tab) tableau_free(tab);
    if (model) lp_model_free(model);
}

int main(void) {
    printf("=== Dual Ratio Flip Tests ===\n");
    test_flip_mode_applies_flip_only_step();
    test_harris_mode_keeps_regular_entering();
    test_harris_mode_accepts_zero_ratio_candidate();
    test_harris_mode_clamps_tiny_negative_ratio_candidate();
    test_flip_mode_respects_runtime_disable();
    test_adaptive_ratio_thresholds_scale_with_lu_health();
    test_theta_floor_keeps_degenerate_limiter();
    test_candidate_value_rejects_ineligible_sign_status();
    test_dual_violation_ignores_fixed_excluded_nonbasic();
    test_sparse_pressure_refactor_gate();
    test_dual_smcp_shift_toggle();
    test_dual_aorn_scan_direction();
    test_dual_aorn_kernel_selection();
    test_dual_cadence_clamp();
    test_dual_reinvert_hard_trigger_demotion();
    test_dual_reinvert_hard_trigger_gap_resets_burst();
    test_dual_reinvert_effective_mode_mapping();
    test_dual_phase1_rescue_progress_limit();
    test_dual_phase1_rescue_progress_gate();
    test_primal_ratio_uses_current_entering_value_for_bound_flip();
    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
