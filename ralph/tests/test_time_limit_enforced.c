/*
 * test_time_limit_enforced.c
 *
 * A solve that has been given a time limit must stop.
 *
 * Every other Ralph test asks whether the answer is right. This one asks
 * whether there is an answer at all, because the two worst bugs found in this
 * solver were not wrong answers -- they were solves that never came back:
 *
 *   - bore3d adopted a recomputed point from a near-singular basis and ran
 *     20000 iterations where 39 would do.
 *   - bnl1 spun inside a single pivot, in repair_singular_basis -> lu_factorize,
 *     blowing a one-second limit by 45x. The per-iteration check was working
 *     perfectly; nothing below it looked at the clock.
 *
 * Both were found by accident -- one by a Windows port, one by a nightly gate
 * that had never run -- because no test was aiming at them.
 *
 * Why real problems rather than generated ones. The obvious version of this
 * test generates random LPs, and it is worthless: random instances are easy for
 * a simplex, and a first draft of this file solved 20000 of them with a slowest
 * time of 0.2ms against a 100ms limit. A test with a 500x margin cannot fail.
 * The NETLIB problems below are the ones the regression baseline already marks
 * as slow, so a 5ms budget cannot possibly be met honestly -- which means the
 * time limit is forced to do its job on every one of them, on real degenerate,
 * ill-conditioned input.
 *
 * The assertion is not about the objective. It is that the call comes back.
 *
 * Part of `make -C ralph test`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ralph_lp.h"
#include "sh_pal.h"

/*
 * The known-slow set from ralph/benchmarks/netlib_regression_baseline.json.
 * These are chosen precisely because none of them can finish in the budgets
 * below: every run here has to terminate by the limit rather than by solving.
 *
 * A fixed list rather than a directory scan on purpose -- scanning needs
 * dirent.h, which is why ralph-benchmark does not build under MSVC, and this
 * test runs on every platform.
 */
static const char *const SLOW_PROBLEMS[] = {
    "bnl2", "cycle", "d2q06c", "d6cube", "degen3", "fit2p", "greenbea",
    "greenbeb", "maros-r7", "maros", "perold", "pilot.ja", "pilot",
    "pilot.we", "pilot4", "pilot87", "pilotnov", "stocfor2", "wood1p", "woodw",
};
#define NUM_PROBLEMS ((int)(sizeof(SLOW_PROBLEMS) / sizeof(SLOW_PROBLEMS[0])))

/*
 * Limits small enough that these problems cannot be solved within them. Two
 * tiers rather than one, so a limit that only works at some magnitudes is still
 * caught, and only two because most of this test's wall time is spent reading
 * MPS files rather than solving -- pilot87 and maros-r7 are large.
 */
static const double LIMITS_SEC[] = { 0.005, 0.050 };
#define NUM_LIMITS ((int)(sizeof(LIMITS_SEC) / sizeof(LIMITS_SEC[0])))

/*
 * What a solve may take: ten times its limit, plus 250ms.
 *
 * The fixed part is presolve and setup, which is per-problem work that does not
 * shrink when the limit does -- with a 5ms limit the measured worst here is
 * maros-r7 at 81ms, and every other problem is under 40ms. 250ms leaves roughly
 * 3x headroom over that for a busy runner.
 *
 * Deliberately not tighter. A bound that fired on a 2x overrun would be flaky
 * and would end up muted, and the bugs this exists for do not overrun by a
 * little -- bnl1 overran by 45x and would have run for ever. Against these
 * limits, a solve that stopped checking the clock would take seconds.
 */
#define BUDGET_MS(limit_sec) ((limit_sec) * 1000.0 * 10.0 + 250.0)

static int tests_run = 0;
static int tests_passed = 0;

int main(void)
{
    int p, l;
    double worst_ratio = 0.0;
    const char *worst_name = "(none)";
    int loaded = 0;
    int hit_limit = 0;

    printf("Ralph Time-Limit Enforcement (a limited solve must stop)\n");
    printf("=======================================================\n");

    for (p = 0; p < NUM_PROBLEMS; p++) {
        char path[512];
        snprintf(path, sizeof(path), "benchmarks/netlib/%s.mps", SLOW_PROBLEMS[p]);

        for (l = 0; l < NUM_LIMITS; l++) {
            RalphLPModel *m = ralph_lp_create();
            double limit = LIMITS_SEC[l];
            double budget = BUDGET_MS(limit);
            uint64_t t0, t1;
            double ms, ratio;
            RalphLPStatus st;

            if (!m) { printf("  FAIL: out of memory\n"); return 1; }
            if (ralph_lp_read_mps(m, path) != 0) {
                /* Missing data is a skip, not a failure: report it once so an
                 * empty run cannot masquerade as a passing one. */
                ralph_lp_free(m);
                m = NULL;
                break;
            }
            loaded++;

            ralph_lp_set_dbl_param(m, "time_limit", limit);

            t0 = sh_monotonic_ns();
            ralph_lp_optimize(m);
            t1 = sh_monotonic_ns();

            ms = (double)(t1 - t0) / 1.0e6;
            st = ralph_lp_get_status(m);
            ralph_lp_free(m);

            if (st == RALPH_LP_STATUS_TIME_LIMIT) hit_limit++;

            ratio = ms / (limit * 1000.0);
            if (ratio > worst_ratio) {
                worst_ratio = ratio;
                worst_name = SLOW_PROBLEMS[p];
            }

            tests_run++;
            if (ms <= budget) {
                tests_passed++;
            } else {
                printf("  FAIL: %s with a %.0fms limit took %.0fms (%.0fx), status=%s\n",
                       SLOW_PROBLEMS[p], limit * 1000.0, ms, ratio,
                       ralph_lp_status_string(st));
            }
        }
    }

    printf("\n-------------------------------------------------------\n");
    if (loaded == 0) {
        printf("RESULT: no NETLIB problems found under benchmarks/netlib/.\n");
        printf("        This test asserts nothing without them. FAIL\n");
        return 1;
    }
    /*
     * If nothing ever reached its limit, these problems are no longer slow
     * enough to force it and this test has quietly stopped testing anything --
     * which is the exact failure mode it exists to catch, so it must not pass
     * silently. Revisit SLOW_PROBLEMS or lower LIMITS_SEC.
     */
    if (hit_limit == 0) {
        printf("RESULT: no solve reached its time limit, so the limit was never\n");
        printf("        exercised. The problem list needs revisiting. FAIL\n");
        return 1;
    }
    printf("Solves:      %d over %d problems (%d stopped on the limit)\n",
           tests_run, NUM_PROBLEMS, hit_limit);
    printf("Worst ratio: %.1fx the limit (%s)\n", worst_ratio, worst_name);
    printf("Passed %d/%d\n", tests_passed, tests_run);
    if (tests_run == tests_passed) {
        printf("RESULT: every limited solve terminated. PASS\n");
        return 0;
    }
    printf("RESULT: a solve ignored its time limit. FAIL\n");
    return 1;
}
