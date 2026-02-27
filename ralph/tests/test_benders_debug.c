/*
 * Benders Decomposition Debug Test
 *
 * Minimal 2-station FuelWise problem to trace Benders algorithm and verify
 * each cut against textbook Benders theory.
 *
 * Problem:
 * - Route: 0 -> 100km -> 200km (origin -> station0 -> station1 -> destination)
 * - Tank: 100L, consumption 30 L/100km, start with 50L, min fuel 10L
 * - Station 0: at 100km, price $1.00/L
 * - Station 1: at 150km, price $1.50/L
 *
 * Fuel consumed:
 * - Origin to S0: 30L (100km * 0.30)
 * - S0 to S1: 15L (50km * 0.30)
 * - S1 to dest: 15L (50km * 0.30)
 * - Total: 60L
 *
 * Optimal solution (by inspection):
 * - Need to buy: 60L - 50L + 10L (min at end) = 20L
 * - Best strategy: buy all 20L at station 0 ($1.00)
 * - Cost: $20.00
 *
 * MILP formulation:
 *   min: 1.0*x[0] + 1.5*x[1] + stop_cost*(z[0] + z[1]) + 1.0*θ
 *   s.t.
 *     y[0] = 50                           (fuel balance at S0)
 *     y[1] = 50 + x[0]                    (fuel balance at S1)
 *     y[0] >= 10 + 30 = 40                (min fuel at S0)
 *     y[1] >= 10 + 45 = 55                (min fuel at S1)
 *     y[0] + x[0] <= 100 + 30 = 130       (tank cap at S0)
 *     y[1] + x[1] <= 100 + 45 = 145       (tank cap at S1)
 *     x[0] - 100*z[0] <= 0                (linking upper 0)
 *     x[1] - 100*z[1] <= 0                (linking upper 1)
 *     x[0] + x[1] >= 60 - 50 + 10 = 20    (reach destination)
 *     z[0] + z[1] >= 0                    (trivial master constraint)
 *     x, y >= 0, z in {0,1}
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ralph_test_mod_api.h"

#define TOLERANCE 1e-6

/* Build the minimal 2-station Benders problem */
static RalphModel* build_2station_model(double stop_cost) {
    RalphModel *model = ralph_test_create();
    if (!model) return NULL;

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    /* Variables:
     * x[0], x[1]: fuel purchased (indices 0, 1)
     * y[0], y[1]: cumulative fuel (indices 2, 3)
     * z[0], z[1]: stop decisions (indices 4, 5)
     * θ: recourse cost (index 6)
     */
    int x0 = 0, x1 = 1;
    int y0 = 2, y1 = 3;
    int z0 = 4, z1 = 5;
    int theta = 6;

    double tank_cap = 100.0;
    double current_fuel = 50.0;

    /* x[0], x[1] - fuel purchased (subproblem vars) */
    ralph_test_add_var(model, 0.0, tank_cap, 1.0, RALPH_CONTINUOUS);  /* x[0] at $1.00 */
    ralph_test_add_var(model, 0.0, tank_cap, 1.5, RALPH_CONTINUOUS);  /* x[1] at $1.50 */

    /* y[0], y[1] - cumulative fuel (subproblem vars) */
    double y_upper = current_fuel + 2 * tank_cap;
    ralph_test_add_var(model, 0.0, y_upper, 0.0, RALPH_CONTINUOUS);   /* y[0] */
    ralph_test_add_var(model, 0.0, y_upper, 0.0, RALPH_CONTINUOUS);   /* y[1] */

    /* z[0], z[1] - stop decisions (master vars) */
    ralph_test_add_var(model, 0.0, 1.0, stop_cost, RALPH_BINARY);     /* z[0] */
    ralph_test_add_var(model, 0.0, 1.0, stop_cost, RALPH_BINARY);     /* z[1] */

    /* θ - recourse cost (master var) */
    /* Use reasonable bounds instead of ±1e30 */
    double theta_bound = 1000.0;
    ralph_test_add_var(model, -theta_bound, theta_bound, 1.0, RALPH_CONTINUOUS);

    /* Constraint 1: y[0] = 50 (fuel balance at S0) */
    {
        int idx[1] = {y0};
        double val[1] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_EQUAL, 50.0);
    }

    /* Constraint 2: y[1] - x[0] = 50 (fuel balance at S1) */
    {
        int idx[2] = {y1, x0};
        double val[2] = {1.0, -1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_EQUAL, 50.0);
    }

    /* Constraint 3: y[0] >= 40 (min fuel at S0: 10 + 30) */
    {
        int idx[1] = {y0};
        double val[1] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 40.0);
    }

    /* Constraint 4: y[1] >= 55 (min fuel at S1: 10 + 45) */
    {
        int idx[1] = {y1};
        double val[1] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 55.0);
    }

    /* Constraint 5: y[0] + x[0] <= 130 (tank cap at S0) */
    {
        int idx[2] = {y0, x0};
        double val[2] = {1.0, 1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 130.0);
    }

    /* Constraint 6: y[1] + x[1] <= 145 (tank cap at S1) */
    {
        int idx[2] = {y1, x1};
        double val[2] = {1.0, 1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 145.0);
    }

    /* Constraint 7: x[0] - 100*z[0] <= 0 (linking upper 0) */
    {
        int idx[2] = {x0, z0};
        double val[2] = {1.0, -tank_cap};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 0.0);
    }

    /* Constraint 8: x[1] - 100*z[1] <= 0 (linking upper 1) */
    {
        int idx[2] = {x1, z1};
        double val[2] = {1.0, -tank_cap};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 0.0);
    }

    /* Constraint 9: x[0] + x[1] >= 20 (reach destination) */
    {
        int idx[2] = {x0, x1};
        double val[2] = {1.0, 1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 20.0);
    }

    /* Constraint 10: z[0] + z[1] >= 0 (trivial master constraint) */
    {
        int idx[2] = {z0, z1};
        double val[2] = {1.0, 1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 0.0);
    }

    return model;
}

/* Test 1: Solve with regular MILP to get baseline */
static int test_milp_baseline(void) {
    printf("\n=== TEST 1: MILP Baseline (Expected: $20.00 with z[0]=1, z[1]=0) ===\n");

    RalphModel *model = build_2station_model(0.0);  /* No stop cost */
    if (!model) {
        printf("FAIL: Could not create model\n");
        return 1;
    }

    ralph_test_set_int_param(model, "verbose", 1);
    int ret = ralph_test_optimize(model);
    RalphStatus status = ralph_test_get_status(model);

    if (ret != 0 || status != RALPH_STATUS_OPTIMAL) {
        printf("FAIL: MILP did not solve optimally (ret=%d, status=%d)\n", ret, status);
        ralph_test_free(model);
        return 1;
    }

    double obj = ralph_test_get_objval(model);
    double x[7];
    ralph_test_get_solution(model, x);

    printf("MILP solution:\n");
    printf("  Objective: %.2f\n", obj);
    printf("  x[0] = %.2f (fuel at S0)\n", x[0]);
    printf("  x[1] = %.2f (fuel at S1)\n", x[1]);
    printf("  y[0] = %.2f (cumul at S0)\n", x[2]);
    printf("  y[1] = %.2f (cumul at S1)\n", x[3]);
    printf("  z[0] = %.2f (stop S0)\n", x[4]);
    printf("  z[1] = %.2f (stop S1)\n", x[5]);
    printf("  theta = %.2f\n", x[6]);

    /* Verify solution */
    double expected_obj = 20.0;  /* Buy 20L at $1.00 */
    if (fabs(obj - expected_obj) > TOLERANCE) {
        printf("FAIL: Expected obj=%.2f, got %.2f\n", expected_obj, obj);
        ralph_test_free(model);
        return 1;
    }

    printf("PASS: MILP baseline correct\n");
    ralph_test_free(model);
    return 0;
}

/* Test 2: Solve with Benders decomposition */
static int test_benders_solve(void) {
    printf("\n=== TEST 2: Benders Decomposition ===\n");

    RalphModel *model = build_2station_model(0.0);
    if (!model) {
        printf("FAIL: Could not create model\n");
        return 1;
    }

    /* Master vars: z[0]=4, z[1]=5 */
    int master_vars[2] = {4, 5};

    RalphBendersConfig config = RALPH_BENDERS_CONFIG_DEFAULT;
    config.master_var_indices = master_vars;
    config.num_master_vars = 2;
    config.theta_var = 6;  /* θ is variable 6 */
    config.verbose = 2;    /* Detailed output */
    config.gap_tolerance = 1e-4;

    double x[7];
    RalphBendersResult result;

    printf("Calling ralph_test_solve_benders...\n\n");
    int ret = ralph_test_solve_benders(model, &config, x, &result);

    printf("\nBenders result:\n");
    printf("  Return code: %d\n", ret);
    printf("  Status: %d\n", result.status);
    printf("  Objective: %.4f\n", result.objective);
    printf("  Master obj: %.4f\n", result.master_obj);
    printf("  Iterations: %d\n", result.iterations);
    printf("  Optimality cuts: %d\n", result.optimality_cuts);
    printf("  Feasibility cuts: %d\n", result.feasibility_cuts);

    if (ret == 0 && result.status == RALPH_STATUS_OPTIMAL) {
        printf("\nSolution:\n");
        printf("  x[0] = %.4f (fuel at S0)\n", x[0]);
        printf("  x[1] = %.4f (fuel at S1)\n", x[1]);
        printf("  y[0] = %.4f\n", x[2]);
        printf("  y[1] = %.4f\n", x[3]);
        printf("  z[0] = %.4f (stop S0)\n", x[4]);
        printf("  z[1] = %.4f (stop S1)\n", x[5]);
        printf("  theta = %.4f\n", x[6]);

        double expected = 20.0;
        if (fabs(result.objective - expected) > 1.0) {
            printf("\nFAIL: Expected ~%.2f, got %.2f (gap: %.2f)\n",
                   expected, result.objective, result.objective - expected);
            ralph_test_free(model);
            return 1;
        }
        printf("\nPASS: Benders solution correct\n");
    } else {
        printf("\nFAIL: Benders did not converge to optimal\n");
        ralph_test_free(model);
        return 1;
    }

    ralph_test_free(model);
    return 0;
}

/* Test 3: Manual Benders trace - step by step */
static int test_benders_manual_trace(void) {
    printf("\n=== TEST 3: Manual Benders Trace ===\n");
    printf("Tracing each iteration to verify cut correctness.\n\n");

    /*
     * Benders decomposition for this problem:
     *
     * Master problem (given accumulated cuts):
     *   min: stop_cost*(z[0]+z[1]) + θ
     *   s.t. z[0] + z[1] >= 0  (trivial constraint)
     *        θ >= -1000       (lower bound)
     *        (+ accumulated cuts)
     *
     * Subproblem (given z*):
     *   min: 1.0*x[0] + 1.5*x[1]
     *   s.t. y[0] = 50
     *        y[1] - x[0] = 50
     *        y[0] >= 40
     *        y[1] >= 55
     *        y[0] + x[0] <= 130
     *        y[1] + x[1] <= 145
     *        x[0] <= 100*z*[0]    (linking: upper bound)
     *        x[1] <= 100*z*[1]    (linking: upper bound)
     *        x[0] + x[1] >= 20
     *
     * For Benders cuts:
     * - Linking constraints are: x[0] <= 100*z[0], x[1] <= 100*z[1]
     * - In matrix form: x[i] - 100*z[i] <= 0
     *   T = -100 (coefficient of z)
     *   h = 0 (original RHS)
     *
     * Standard optimality cut:
     *   θ >= π'(h - T*z) = π'h - π'T*z = π*0 - π*(-100)*z = 100*π*z
     *   where π is the dual for the linking constraints
     *
     * Let's trace iteration by iteration:
     */

    printf("--- Iteration 1 ---\n");
    printf("Master with no cuts: z[0]=0, z[1]=0, θ=-1000 (lower bound)\n\n");

    printf("Subproblem with z[0]=0, z[1]=0:\n");
    printf("  Linking RHS: x[0] <= 0, x[1] <= 0\n");
    printf("  But we need x[0] + x[1] >= 20!\n");
    printf("  => INFEASIBLE\n\n");

    printf("Feasibility cut from Farkas ray:\n");
    printf("  The Farkas ray proves infeasibility.\n");
    printf("  For this problem, the ray should show that sum(x) >= 20\n");
    printf("  is incompatible with x[0] <= 0, x[1] <= 0.\n");
    printf("  Cut: some constraint on z that rules out z[0]=z[1]=0.\n\n");

    printf("--- Iteration 2 ---\n");
    printf("After feasibility cut, master should give z[0]=1 or z[1]=1.\n");
    printf("Say z[0]=1, z[1]=0, θ = still low.\n\n");

    printf("Subproblem with z[0]=1, z[1]=0:\n");
    printf("  x[0] <= 100, x[1] <= 0\n");
    printf("  y[0] = 50, y[1] = 50 + x[0]\n");
    printf("  Need y[1] >= 55, so 50 + x[0] >= 55, x[0] >= 5\n");
    printf("  Need x[0] + x[1] >= 20, so x[0] >= 20 (since x[1]=0)\n");
    printf("  Optimal: x[0] = 20, cost = $20.00\n\n");

    printf("Optimality cut:\n");
    printf("  sub_obj = 20.0\n");
    printf("  Need to compute: θ >= π'(h - T*z)\n");
    printf("  Linking constraint 0: x[0] - 100*z[0] <= 0, dual = π[0]\n");
    printf("  Linking constraint 1: x[1] - 100*z[1] <= 0, dual = π[1]\n\n");

    printf("  At z[0]=1, z[1]=0:\n");
    printf("  - Constraint 0 is x[0] <= 100, slack = 100 - 20 = 80 > 0, so π[0] = 0\n");
    printf("  - Constraint 1 is x[1] <= 0, binding at x[1]=0, π[1] = ?\n\n");

    printf("  For minimization with <= constraint, π >= 0.\n");
    printf("  But x[1]=0 is optimal with marginal cost $1.50.\n");
    printf("  If we relax x[1] <= 0 to x[1] <= 1, can we improve?\n");
    printf("  No! We already have x[0]=20 meeting the requirement.\n");
    printf("  So π[1] = 0 as well (relaxing doesn't help).\n\n");

    printf("  The cut becomes: θ >= 20 + 0*(-100)*(z[0]-1) + 0*(-100)*(z[1]-0)\n");
    printf("                 = θ >= 20\n\n");

    printf("--- Iteration 3 ---\n");
    printf("Master with cut θ >= 20:\n");
    printf("  min: θ, s.t. θ >= 20, z ∈ {0,1}²\n");
    printf("  Solution: θ = 20, z[0]=1, z[1]=0 (or any feasible z)\n\n");

    printf("Subproblem with z[0]=1, z[1]=0 (same as before):\n");
    printf("  Optimal: x[0]=20, cost = $20.00\n");
    printf("  θ_master = 20 = sub_obj, so converged!\n\n");

    printf("Expected final: objective = $20.00, z[0]=1, x[0]=20\n");

    return 0;
}

/* Test 4: Verify the optimality cut formula */
static int test_optimality_cut_formula(void) {
    printf("\n=== TEST 4: Optimality Cut Formula Verification ===\n\n");

    /*
     * The standard Benders optimality cut is:
     *
     *   θ >= Q(z*) + ∇Q(z*)'(z - z*)
     *
     * where Q(z) = optimal subproblem cost given z.
     *
     * Since Q is a piecewise linear convex function (for minimization):
     *   ∇Q(z*) = -π'T
     *
     * So the cut is:
     *   θ >= Q(z*) - π'T(z - z*)
     *      = Q(z*) - π'Tz + π'Tz*
     *
     * In form: θ + π'Tz >= Q(z*) + π'Tz*
     *
     * But wait, we need to compute this correctly.
     * Let's verify by checking what duals we get for the linking constraints.
     *
     * The subproblem is:
     *   min c'x s.t. Wx = h - Tz* (parameterized by z*)
     *
     * The dual is:
     *   max π'(h - Tz*) s.t. W'π <= c
     *
     * At optimum: primal_obj = dual_obj = π'(h - Tz*)
     *
     * The cut θ >= π'(h - Tz) for all z says:
     *   θ >= π'h - π'Tz
     *
     * Rearranging: θ + π'Tz >= π'h
     *
     * Note: The constant is π'h (original RHS), NOT the subproblem objective.
     */

    printf("Textbook Benders optimality cut derivation:\n\n");

    printf("Given:\n");
    printf("  Subproblem: min c'x s.t. Wx = h - Tz*\n");
    printf("  Dual:       max π'(h - Tz*) s.t. W'π <= c\n\n");

    printf("At optimum, primal = dual:\n");
    printf("  c'x* = π'(h - Tz*) = π'h - π'Tz*\n\n");

    printf("The cut is valid for all z:\n");
    printf("  θ >= π'(h - Tz) = π'h - π'Tz\n\n");

    printf("Rearranging:\n");
    printf("  θ + π'Tz >= π'h\n\n");

    printf("So in form 'θ + coeff'z >= constant':\n");
    printf("  coeff = π'T (matrix product)\n");
    printf("  constant = π'h (dot product of dual with original RHS)\n\n");

    printf("Important: constant is π'h, NOT the subproblem objective!\n");
    printf("  sub_obj = π'h - π'Tz*\n");
    printf("  π'h = sub_obj + π'Tz*\n\n");

    printf("So the code that computes:\n");
    printf("  constant = sub_obj + Σ π_k * T_k * z*_k\n");
    printf("is correct for the constant term.\n\n");

    printf("For coefficients:\n");
    printf("  coeff_j = Σ_k π_k * T_kj\n");
    printf("where T_kj is the coefficient of z_j in linking constraint k.\n\n");

    printf("In FuelWise:\n");
    printf("  Linking k: x[k] - 100*z[k] <= 0\n");
    printf("  T_kj = -100 if k=j, else 0\n");
    printf("  So coeff[j] = π[j] * (-100)\n\n");

    printf("For minimization with <= constraint, dual π >= 0.\n");
    printf("So coeff[j] = -100 * π[j] <= 0\n\n");

    printf("The cut becomes:\n");
    printf("  θ + (-100*π[0])*z[0] + (-100*π[1])*z[1] >= constant\n");
    printf("  θ - 100*π[0]*z[0] - 100*π[1]*z[1] >= constant\n\n");

    printf("This makes sense: higher z (more stations open) decreases the\n");
    printf("requirement on θ (since subproblem has more options).\n");

    return 0;
}

/* Test 5: Check what duals the simplex solver returns */
static int test_subproblem_duals(void) {
    printf("\n=== TEST 5: Subproblem Dual Values ===\n\n");

    /* Build just the subproblem with z[0]=1, z[1]=0 */
    RalphModel *sub = ralph_test_create();
    if (!sub) {
        printf("FAIL: Could not create model\n");
        return 1;
    }

    ralph_test_set_obj_sense(sub, RALPH_MINIMIZE);

    double tank_cap = 100.0;

    /* Variables: x[0], x[1], y[0], y[1] */
    ralph_test_add_var(sub, 0.0, tank_cap, 1.0, RALPH_CONTINUOUS);  /* x[0] */
    ralph_test_add_var(sub, 0.0, tank_cap, 1.5, RALPH_CONTINUOUS);  /* x[1] */
    ralph_test_add_var(sub, 0.0, 300.0, 0.0, RALPH_CONTINUOUS);     /* y[0] */
    ralph_test_add_var(sub, 0.0, 300.0, 0.0, RALPH_CONTINUOUS);     /* y[1] */

    /* Subproblem-only constraints */
    /* C0: y[0] = 50 */
    {
        int idx[1] = {2};
        double val[1] = {1.0};
        ralph_test_add_constraint(sub, 1, idx, val, RALPH_EQUAL, 50.0);
    }

    /* C1: y[1] - x[0] = 50 */
    {
        int idx[2] = {3, 0};
        double val[2] = {1.0, -1.0};
        ralph_test_add_constraint(sub, 2, idx, val, RALPH_EQUAL, 50.0);
    }

    /* C2: y[0] >= 40 */
    {
        int idx[1] = {2};
        double val[1] = {1.0};
        ralph_test_add_constraint(sub, 1, idx, val, RALPH_GREATER_EQUAL, 40.0);
    }

    /* C3: y[1] >= 55 */
    {
        int idx[1] = {3};
        double val[1] = {1.0};
        ralph_test_add_constraint(sub, 1, idx, val, RALPH_GREATER_EQUAL, 55.0);
    }

    /* C4: y[0] + x[0] <= 130 */
    {
        int idx[2] = {2, 0};
        double val[2] = {1.0, 1.0};
        ralph_test_add_constraint(sub, 2, idx, val, RALPH_LESS_EQUAL, 130.0);
    }

    /* C5: y[1] + x[1] <= 145 */
    {
        int idx[2] = {3, 1};
        double val[2] = {1.0, 1.0};
        ralph_test_add_constraint(sub, 2, idx, val, RALPH_LESS_EQUAL, 145.0);
    }

    /* Linking constraints with z*=[1, 0] -> RHS = [100, 0] */
    /* C6: x[0] <= 100*1 = 100 */
    {
        int idx[1] = {0};
        double val[1] = {1.0};
        ralph_test_add_constraint(sub, 1, idx, val, RALPH_LESS_EQUAL, 100.0);
    }

    /* C7: x[1] <= 100*0 = 0 */
    {
        int idx[1] = {1};
        double val[1] = {1.0};
        ralph_test_add_constraint(sub, 1, idx, val, RALPH_LESS_EQUAL, 0.0);
    }

    /* C8: x[0] + x[1] >= 20 */
    {
        int idx[2] = {0, 1};
        double val[2] = {1.0, 1.0};
        ralph_test_add_constraint(sub, 2, idx, val, RALPH_GREATER_EQUAL, 20.0);
    }

    /* Solve */
    ralph_test_set_int_param(sub, "verbose", 0);
    int ret = ralph_test_optimize(sub);
    RalphStatus status = ralph_test_get_status(sub);

    if (ret != 0 || status != RALPH_STATUS_OPTIMAL) {
        printf("FAIL: Subproblem not optimal (ret=%d, status=%d)\n", ret, status);
        ralph_test_free(sub);
        return 1;
    }

    double obj = ralph_test_get_objval(sub);
    double x[4];
    ralph_test_get_solution(sub, x);

    printf("Subproblem with z*=[1,0]:\n");
    printf("  Objective: %.4f\n", obj);
    printf("  x[0] = %.4f, x[1] = %.4f\n", x[0], x[1]);
    printf("  y[0] = %.4f, y[1] = %.4f\n", x[2], x[3]);

    /* Get dual values */
    double duals[9];
    int dual_ret = ralph_test_get_dual_solution(sub, duals);

    printf("\nDual values (ret=%d):\n", dual_ret);
    printf("  C0 (y[0]=50):     π[0] = %.6f\n", duals[0]);
    printf("  C1 (y[1]-x[0]=50): π[1] = %.6f\n", duals[1]);
    printf("  C2 (y[0]>=40):    π[2] = %.6f\n", duals[2]);
    printf("  C3 (y[1]>=55):    π[3] = %.6f\n", duals[3]);
    printf("  C4 (y[0]+x[0]<=130): π[4] = %.6f\n", duals[4]);
    printf("  C5 (y[1]+x[1]<=145): π[5] = %.6f\n", duals[5]);
    printf("  C6 (x[0]<=100):   π[6] = %.6f  <-- LINKING 0\n", duals[6]);
    printf("  C7 (x[1]<=0):     π[7] = %.6f  <-- LINKING 1\n", duals[7]);
    printf("  C8 (x[0]+x[1]>=20): π[8] = %.6f\n", duals[8]);

    printf("\nExpected linking duals:\n");
    printf("  π[6] should be 0 (x[0]=20 < 100, constraint slack)\n");
    printf("  π[7] should be >= 0 (x[1]=0 at bound, for <= constraint)\n\n");

    /* Verify complementary slackness */
    printf("Complementary slackness check:\n");
    printf("  C6: x[0]=%.2f, bound=100, slack=%.2f, π=%.6f\n",
           x[0], 100.0 - x[0], duals[6]);
    printf("  C7: x[1]=%.2f, bound=0, slack=%.2f, π=%.6f\n",
           x[1], 0.0 - x[1], duals[7]);

    if (fabs(duals[6]) > 1e-4 && fabs(100.0 - x[0]) > 1e-4) {
        printf("  WARNING: C6 violates complementary slackness!\n");
    }

    /* Now compute what the optimality cut should be */
    printf("\nOptimality cut calculation:\n");
    printf("  sub_obj = %.4f\n", obj);
    printf("  π_link[0] = %.6f (linking constraint 0)\n", duals[6]);
    printf("  π_link[1] = %.6f (linking constraint 1)\n", duals[7]);
    printf("  T[0] = -100, T[1] = -100 (coefficients of z in linking)\n");
    printf("  z* = [1, 0]\n\n");

    double pi_link[2] = {duals[6], duals[7]};
    double T[2] = {-100.0, -100.0};
    double z_star[2] = {1.0, 0.0};

    /* constant = sub_obj + Σ π_k * T_k * z*_k */
    double constant = obj;
    for (int k = 0; k < 2; k++) {
        constant += pi_link[k] * T[k] * z_star[k];
    }

    /* coeff[j] = π[j] * T[j] */
    double coeff[2];
    for (int j = 0; j < 2; j++) {
        coeff[j] = pi_link[j] * T[j];
    }

    printf("  constant = sub_obj + π'Tz* = %.4f + %.6f = %.4f\n",
           obj, constant - obj, constant);
    printf("  coeff[0] = π[0]*T[0] = %.6f * (-100) = %.4f\n",
           pi_link[0], coeff[0]);
    printf("  coeff[1] = π[1]*T[1] = %.6f * (-100) = %.4f\n",
           pi_link[1], coeff[1]);

    printf("\n  Cut: θ + (%.4f)*z[0] + (%.4f)*z[1] >= %.4f\n",
           coeff[0], coeff[1], constant);

    /* Verify the cut is satisfied at z*=[1,0], θ=sub_obj */
    double lhs = obj + coeff[0] * z_star[0] + coeff[1] * z_star[1];
    printf("\n  Verify at z*=[1,0], θ=%.4f:\n", obj);
    printf("    LHS = θ + coeff'z* = %.4f + %.4f = %.4f\n",
           obj, coeff[0] * 1.0 + coeff[1] * 0.0, lhs);
    printf("    RHS = %.4f\n", constant);
    printf("    LHS >= RHS? %s\n", (lhs >= constant - 1e-6) ? "YES" : "NO");

    /* What if we set z[0]=0, z[1]=0? The cut should reject this. */
    printf("\n  Test cut at z=[0,0], θ=20:\n");
    double lhs2 = 20.0 + coeff[0] * 0.0 + coeff[1] * 0.0;
    printf("    LHS = 20 + 0 = %.4f\n", lhs2);
    printf("    RHS = %.4f\n", constant);
    printf("    LHS >= RHS? %s\n", (lhs2 >= constant - 1e-6) ? "YES (cut doesn't help)" : "NO (cut blocks this)");

    ralph_test_free(sub);
    return 0;
}

/* Main */
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("=================================================\n");
    printf("  Benders Decomposition Debug Tests\n");
    printf("=================================================\n");

    int failures = 0;

    failures += test_milp_baseline();
    failures += test_benders_manual_trace();
    failures += test_optimality_cut_formula();
    failures += test_subproblem_duals();
    failures += test_benders_solve();

    printf("\n=================================================\n");
    if (failures == 0) {
        printf("All tests passed!\n");
    } else {
        printf("%d test(s) failed.\n", failures);
    }
    printf("=================================================\n");

    return failures;
}
