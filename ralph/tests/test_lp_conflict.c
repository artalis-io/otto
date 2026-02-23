/*
 * Tests for LP conflict/IIS refinement public APIs.
 *
 * This module is intentionally separate from telemetry/logging tests.
 */

#include <stdio.h>
#include "ralph_test_mod_api.h"

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

#define ASSERT_INT_EQ(a, b, msg) do { \
    tests_run++; \
    if ((a) == (b)) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%d != %d)\n", msg, (int)(a), (int)(b)); \
    } \
} while (0)

static RalphModel* build_row_core_infeasible(int force_two_phase) {
    RalphModel *model = ralph_test_create();
    if (!model) return NULL;

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "method", 0);
    ralph_test_set_int_param(model, "presolve", 0);
    ralph_test_set_int_param(model, "force_two_phase", force_two_phase ? 1 : 0);

    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 0.0, RALPH_CONTINUOUS); /* x */
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 0.0, RALPH_CONTINUOUS); /* y */

    /* Infeasible core: x >= 2 and x <= 1 */
    {
        int idx[] = {0}; double val[] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 2.0); /* row 0 */
    }
    {
        int idx[] = {0}; double val[] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_LESS_EQUAL, 1.0);    /* row 1 */
    }
    /* Redundant rows */
    {
        int idx[] = {1}; double val[] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 0.0); /* row 2 */
    }
    {
        int idx[] = {0, 1}; double val[] = {1.0, 1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 0.0); /* row 3 */
    }

    return model;
}

static RalphModel* build_rows_plus_bound_conflict(void) {
    RalphModel *model = ralph_test_create();
    if (!model) return NULL;

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "method", 0);
    ralph_test_set_int_param(model, "presolve", 0);
    ralph_test_set_int_param(model, "force_two_phase", 1);

    /* Keep a bound conflict in-model. */
    ralph_test_add_var(model, 2.0, 1.0, 0.0, RALPH_CONTINUOUS); /* 2 <= x <= 1 */

    /* Also add row conflict so current LP status is INFEASIBLE robustly. */
    {
        int idx[] = {0}; double val[] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 10.0);
    }
    {
        int idx[] = {0}; double val[] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_LESS_EQUAL, 5.0);
    }

    return model;
}

static void test_lp_conflict_guards(void) {
    RalphModel *lp = build_row_core_infeasible(0);
    RalphModel *mip = NULL;
    RalphConflictOptions opts;
    RalphConflictMember members[8];
    RalphConflictReport report;
    int count = 0;

    ASSERT_TRUE(lp != NULL, "guard: LP model created");
    if (!lp) return;

    opts.include_bounds = 1;
    opts.use_farkas_seed = 0;
    ASSERT_INT_EQ(ralph_compute_lp_conflict(lp, &opts, members, 8, &count, &report), -1,
                  "guard: conflict API unavailable before solve");

    ASSERT_INT_EQ(ralph_test_optimize_lp(lp), 0, "guard: LP solve succeeds");
    ASSERT_INT_EQ((int)ralph_test_get_status(lp), (int)RALPH_STATUS_INFEASIBLE,
                  "guard: LP infeasible");

    ASSERT_INT_EQ(ralph_compute_lp_conflict(NULL, &opts, members, 8, &count, &report), -1,
                  "guard: NULL model rejected");
    ASSERT_INT_EQ(ralph_compute_lp_conflict(lp, &opts, members, 8, NULL, &report), -1,
                  "guard: NULL count rejected");
    ASSERT_INT_EQ(ralph_compute_lp_conflict(lp, &opts, NULL, 8, &count, &report), -1,
                  "guard: NULL members rejected when capacity>0");
    ASSERT_INT_EQ(ralph_compute_lp_conflict(lp, &opts, members, -1, &count, &report), -1,
                  "guard: negative capacity rejected");

    mip = ralph_test_create();
    ASSERT_TRUE(mip != NULL, "guard: MIP model created");
    if (mip) {
        ralph_test_set_obj_sense(mip, RALPH_MAXIMIZE);
        ralph_test_add_var(mip, 0.0, 1.0, 1.0, RALPH_BINARY);
        ASSERT_INT_EQ(ralph_test_optimize_mip(mip), 0, "guard: MIP optimize succeeds");
        ASSERT_INT_EQ(ralph_compute_lp_conflict(mip, &opts, members, 8, &count, &report), -1,
                      "guard: conflict API is LP-only");
    }

    ralph_test_free(lp);
    ralph_test_free(mip);
}

static void test_lp_conflict_row_only_and_iis_compat(void) {
    RalphModel *model = build_row_core_infeasible(0);
    RalphConflictOptions opts;
    RalphConflictMember members[8];
    RalphConflictReport report;
    int count = 0;
    int flags[4] = {0, 0, 0, 0};
    int iis_size = 0;

    ASSERT_TRUE(model != NULL, "row-only: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0, "row-only: optimize succeeds");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_INFEASIBLE,
                  "row-only: model infeasible");

    opts.include_bounds = 0;
    opts.use_farkas_seed = 0;
    ASSERT_INT_EQ(ralph_compute_lp_conflict(model, &opts, members, 8, &count, &report), 0,
                  "row-only: conflict extraction succeeds");
    ASSERT_INT_EQ(count, 2, "row-only: expected conflict size");
    ASSERT_INT_EQ((int)members[0].type, (int)RALPH_CONFLICT_MEMBER_ROW, "row-only: member 0 type");
    ASSERT_INT_EQ((int)members[0].index, 0, "row-only: member 0 row");
    ASSERT_INT_EQ((int)members[1].type, (int)RALPH_CONFLICT_MEMBER_ROW, "row-only: member 1 type");
    ASSERT_INT_EQ((int)members[1].index, 1, "row-only: member 1 row");
    ASSERT_INT_EQ(report.final_size, 2, "row-only: report final size");

    ASSERT_INT_EQ(ralph_compute_lp_iis(model, flags, &iis_size), 0,
                  "row-only: IIS compatibility succeeds");
    ASSERT_INT_EQ(iis_size, 2, "row-only: IIS size");
    ASSERT_INT_EQ(flags[0], 1, "row-only: IIS includes row 0");
    ASSERT_INT_EQ(flags[1], 1, "row-only: IIS includes row 1");
    ASSERT_INT_EQ(flags[2], 0, "row-only: IIS excludes row 2");
    ASSERT_INT_EQ(flags[3], 0, "row-only: IIS excludes row 3");

    count = 0;
    ASSERT_INT_EQ(ralph_compute_lp_conflict(model, &opts, NULL, 0, &count, NULL), -1,
                  "row-only: size query reports insufficient capacity");
    ASSERT_INT_EQ(count, 2, "row-only: size query returns required count");

    ralph_test_free(model);
}

static void test_lp_conflict_include_bounds_stability(void) {
    RalphModel *model = build_row_core_infeasible(0);
    RalphConflictOptions opts;
    RalphConflictMember members[8];
    int count = 0;

    ASSERT_TRUE(model != NULL, "include-bounds: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0, "include-bounds: optimize succeeds");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_INFEASIBLE,
                  "include-bounds: model infeasible");

    opts.include_bounds = 1;
    opts.use_farkas_seed = 0;
    ASSERT_INT_EQ(ralph_compute_lp_conflict(model, &opts, members, 8, &count, NULL), 0,
                  "include-bounds: conflict extraction succeeds");
    ASSERT_TRUE(count >= 2, "include-bounds: conflict size >= row core");
    ASSERT_INT_EQ((int)members[0].type, (int)RALPH_CONFLICT_MEMBER_ROW,
                  "include-bounds: member 0 row");
    ASSERT_INT_EQ((int)members[0].index, 0, "include-bounds: member 0 row idx");
    ASSERT_INT_EQ((int)members[1].type, (int)RALPH_CONFLICT_MEMBER_ROW,
                  "include-bounds: member 1 row");
    ASSERT_INT_EQ((int)members[1].index, 1, "include-bounds: member 1 row idx");

    ralph_test_free(model);
}

static void test_lp_conflict_bound_members(void) {
    RalphModel *model = build_rows_plus_bound_conflict();
    RalphConflictOptions opts;
    RalphConflictMember members[8];
    int count = 0;

    ASSERT_TRUE(model != NULL, "bound-members: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0, "bound-members: optimize succeeds");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_INFEASIBLE,
                  "bound-members: model infeasible");

    opts.include_bounds = 0;
    opts.use_farkas_seed = 0;
    ASSERT_INT_EQ(ralph_compute_lp_conflict(model, &opts, members, 8, &count, NULL), -1,
                  "bound-members: row-only extraction unavailable when bounds alone are infeasible");

    opts.include_bounds = 1;
    opts.use_farkas_seed = 0;
    ASSERT_INT_EQ(ralph_compute_lp_conflict(model, &opts, members, 8, &count, NULL), 0,
                  "bound-members: bound-aware extraction succeeds");
    ASSERT_INT_EQ(count, 2, "bound-members: expected conflict size");
    ASSERT_INT_EQ((int)members[0].type, (int)RALPH_CONFLICT_MEMBER_VAR_LB,
                  "bound-members: member 0 is lower bound");
    ASSERT_INT_EQ((int)members[0].index, 0, "bound-members: member 0 var index");
    ASSERT_INT_EQ((int)members[1].type, (int)RALPH_CONFLICT_MEMBER_VAR_UB,
                  "bound-members: member 1 is upper bound");
    ASSERT_INT_EQ((int)members[1].index, 0, "bound-members: member 1 var index");

    ralph_test_free(model);
}

static void test_lp_conflict_farkas_seed_stability(void) {
    RalphModel *model = build_row_core_infeasible(1);
    RalphConflictOptions opts_no_seed;
    RalphConflictOptions opts_seed;
    RalphConflictMember a[8];
    RalphConflictMember b[8];
    RalphConflictReport report = {0};
    int count_a = 0;
    int count_b = 0;

    ASSERT_TRUE(model != NULL, "seed: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0, "seed: optimize succeeds");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_INFEASIBLE,
                  "seed: model infeasible");

    opts_no_seed.include_bounds = 0;
    opts_no_seed.use_farkas_seed = 0;
    opts_seed.include_bounds = 0;
    opts_seed.use_farkas_seed = 1;

    ASSERT_INT_EQ(ralph_compute_lp_conflict(model, &opts_no_seed, a, 8, &count_a, NULL), 0,
                  "seed: no-seed extraction succeeds");
    ASSERT_INT_EQ(ralph_compute_lp_conflict(model, &opts_seed, b, 8, &count_b, &report), 0,
                  "seed: seeded extraction succeeds");
    ASSERT_INT_EQ(count_a, count_b, "seed: sizes match no-seed path");

    for (int i = 0; i < count_a; i++) {
        ASSERT_INT_EQ((int)a[i].type, (int)b[i].type, "seed: member type stable");
        ASSERT_INT_EQ(a[i].index, b[i].index, "seed: member index stable");
    }
    ASSERT_TRUE(report.used_farkas_seed == 0 || report.used_farkas_seed == 1,
                "seed: report flag is boolean");
    ASSERT_TRUE(report.seeded_rows >= 0, "seed: seeded rows non-negative");

    ralph_test_free(model);
}

int main(void) {
    printf("=== LP Conflict/IIS Tests ===\n");

    test_lp_conflict_guards();
    test_lp_conflict_row_only_and_iis_compat();
    test_lp_conflict_include_bounds_stability();
    test_lp_conflict_bound_members();
    test_lp_conflict_farkas_seed_stability();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
