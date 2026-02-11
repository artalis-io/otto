#include <stdio.h>
#include "lp.h"

/* Internal test hook from simplex.c */
int simplex_choose_basis_action_for_test(double pivot,
                                         int force_refactor,
                                         int lu_update_status,
                                         int lu_reason,
                                         int repeat_pattern,
                                         double growth_factor);

enum {
    EXPECT_UPDATE = 0,
    EXPECT_REFACTOR = 1,
    EXPECT_REPAIR = 2,
    EXPECT_ABORT = 3
};

typedef struct {
    const char *name;
    double pivot;
    int force_refactor;
    int lu_update_status;
    int lu_reason;
    int repeat_pattern;
    double growth_factor;
    int expected_action;
} PolicyCase;

static int run_case(const PolicyCase *tc) {
    int got = simplex_choose_basis_action_for_test(tc->pivot,
                                                   tc->force_refactor,
                                                   tc->lu_update_status,
                                                   tc->lu_reason,
                                                   tc->repeat_pattern,
                                                   tc->growth_factor);
    if (got != tc->expected_action) {
        fprintf(stderr, "FAIL: %s (expected=%d got=%d)\n",
                tc->name, tc->expected_action, got);
        return 0;
    }
    printf("PASS: %s\n", tc->name);
    return 1;
}

int main(void) {
    const PolicyCase cases[] = {
        {
            .name = "default path uses LU update",
            .pivot = 1e-2,
            .force_refactor = 0,
            .lu_update_status = 0,
            .lu_reason = LU_FAIL_NONE,
            .repeat_pattern = 0,
            .growth_factor = 1.0,
            .expected_action = EXPECT_UPDATE
        },
        {
            .name = "tiny pivot aborts immediately",
            .pivot = 1e-12,
            .force_refactor = 0,
            .lu_update_status = 0,
            .lu_reason = LU_FAIL_NONE,
            .repeat_pattern = 0,
            .growth_factor = 1.0,
            .expected_action = EXPECT_ABORT
        },
        {
            .name = "forced-refactor flag bypasses LU update",
            .pivot = 1e-2,
            .force_refactor = 1,
            .lu_update_status = 0,
            .lu_reason = LU_FAIL_NONE,
            .repeat_pattern = 0,
            .growth_factor = 1.0,
            .expected_action = EXPECT_REFACTOR
        },
        {
            .name = "repeat-pattern trigger forces refactor",
            .pivot = 1e-2,
            .force_refactor = 0,
            .lu_update_status = 0,
            .lu_reason = LU_FAIL_NONE,
            .repeat_pattern = RALPH_PHASE1_REPEAT_REFACTOR_TRIGGER,
            .growth_factor = 1.0,
            .expected_action = EXPECT_REFACTOR
        },
        {
            .name = "growth-trigger forces refactor",
            .pivot = 1e-2,
            .force_refactor = 0,
            .lu_update_status = 0,
            .lu_reason = LU_FAIL_NONE,
            .repeat_pattern = 0,
            .growth_factor = RALPH_LU_GROWTH_REFACTOR_THRESHOLD * 1.01,
            .expected_action = EXPECT_REFACTOR
        },
        {
            .name = "failed LU update escalates to refactor",
            .pivot = 1e-2,
            .force_refactor = 0,
            .lu_update_status = -1,
            .lu_reason = LU_FAIL_MAX_UPDATES,
            .repeat_pattern = 0,
            .growth_factor = 1.0,
            .expected_action = EXPECT_REFACTOR
        },
        {
            .name = "failed refactor escalates to repair",
            .pivot = 1e-2,
            .force_refactor = 0,
            .lu_update_status = -2,
            .lu_reason = LU_FAIL_FACTOR_SINGULAR,
            .repeat_pattern = 0,
            .growth_factor = 1.0,
            .expected_action = EXPECT_REPAIR
        },
        {
            .name = "failed repair aborts",
            .pivot = 1e-2,
            .force_refactor = 0,
            .lu_update_status = -3,
            .lu_reason = LU_FAIL_FACTOR_SINGULAR,
            .repeat_pattern = 0,
            .growth_factor = 1.0,
            .expected_action = EXPECT_ABORT
        }
    };

    int pass = 0;
    int total = (int)(sizeof(cases) / sizeof(cases[0]));
    for (int i = 0; i < total; i++) {
        pass += run_case(&cases[i]);
    }

    printf("\nPolicy cases passed: %d/%d\n", pass, total);
    return (pass == total) ? 0 : 1;
}
