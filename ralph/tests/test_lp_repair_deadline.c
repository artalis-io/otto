/*
 * repair_singular_basis() must honour the solve's wall-clock budget.
 *
 * The bug this pins down: the time limit was checked once per simplex
 * iteration, but a single pivot can descend into repair_singular_basis(), which
 * rebuilds the basis and factorizes it up to MAX_REPAIRS times with no clock
 * check anywhere inside. On a few hundred rows that runs to tens of seconds. A
 * solve given a one-second limit was caught by gdb still inside iteration 2542,
 * in repair_singular_basis -> lu_factorize, 45 seconds in.
 *
 * The test does not need a singular basis, which is the point. On a healthy
 * basis the first factorization inside repair succeeds and the function returns
 * 0 straight away. So an exhausted budget is the only thing that can turn that
 * same call into a -1, and the two cases below differ in nothing else.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ralph_test_mod_api.h"
#include "lp.h"

extern SimplexTableau *tableau_create(LPModel *model);
extern void tableau_free(SimplexTableau *tab);
extern int repair_singular_basis(SimplexTableau *tab);
extern double lp_telemetry_now_ms(void);
int lp_model_finalize(LPModel *model);

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
        printf("  PASS: %s\n", msg); \
    } else { \
        printf("  FAIL: %s\n", msg); \
    } \
} while (0)

/* min x + 2y  s.t.  x + y = 3, x >= 1, y >= 0 */
static LPModel *build_model(void)
{
    LPModel *model = lp_model_create();
    int idx[] = {0, 1};
    double val[] = {1.0, 1.0};

    if (!model) return NULL;
    model->obj_sense = 1;
    lp_model_add_var(model, 1.0, 1e30, 1.0, 'C');
    lp_model_add_var(model, 0.0, 1e30, 2.0, 'C');
    lp_model_add_constraint(model, 2, idx, val, 'E', 3.0);
    lp_model_finalize(model);
    return model;
}

/*
 * `elapsed_ms` is how long the solve is pretended to have been running, and
 * `limit_sec` its budget. Returns what repair_singular_basis() answered.
 */
static int repair_with_budget(double limit_sec, double elapsed_ms, int *ok)
{
    LPModel *model = build_model();
    SimplexTableau *tab = NULL;
    SimplexSolver solver;
    int rc;

    *ok = 0;
    if (!model) return 0;
    tab = tableau_create(model);
    if (!tab) { lp_model_free(model); return 0; }

    /* Only the fields the deadline check reads; the tableau does not own it. */
    memset(&solver, 0, sizeof(solver));
    solver.time_limit = limit_sec;
    solver.progress_start_ms = lp_telemetry_now_ms() - elapsed_ms;
    tab->owner = &solver;

    rc = repair_singular_basis(tab);

    tab->owner = NULL;
    tableau_free(tab);
    lp_model_free(model);
    *ok = 1;
    return rc;
}

int main(void)
{
    int ok = 0;
    int rc;

    printf("=== LP Repair Deadline Tests ===\n");

    /* Budget intact: the basis is fine, so the repair's first factorization
     * succeeds and it reports success. This is the control -- without it, a
     * guard that simply always failed would pass the test below. */
    rc = repair_with_budget(60.0, 1.0, &ok);
    CHECK(ok, "control: tableau built");
    CHECK(rc == 0, "with budget remaining, repair succeeds on a healthy basis");

    /* Same call, same basis, budget spent. */
    rc = repair_with_budget(1.0, 10000.0, &ok);
    CHECK(ok, "expired: tableau built");
    CHECK(rc == -1, "with the budget spent, repair gives up instead of working");

    /* No limit set at all must not be read as "no time left". */
    rc = repair_with_budget(0.0, 10000.0, &ok);
    CHECK(rc == 0, "an unset time limit does not abort the repair");

    /* RALPH_INFINITY means unlimited, same as unset. */
    rc = repair_with_budget(RALPH_INFINITY, 10000.0, &ok);
    CHECK(rc == 0, "an infinite time limit does not abort the repair");

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_run == tests_passed) ? 0 : 1;
}
