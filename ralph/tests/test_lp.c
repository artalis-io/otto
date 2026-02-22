/*
 * Ralph - LP Format Tests
 *
 * Tests for CPLEX LP file format reader and writer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ralph.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL: %s\n", msg); \
        tests_failed++; \
        return; \
    } else { \
        printf("  PASS: %s\n", msg); \
        tests_passed++; \
    } \
} while(0)

#define ASSERT_EQ_DBL(a, b, msg) do { \
    if (fabs((a) - (b)) > 1e-6) { \
        printf("  FAIL: %s (got %g, expected %g)\n", msg, (a), (b)); \
        tests_failed++; \
        return; \
    } else { \
        printf("  PASS: %s\n", msg); \
        tests_passed++; \
    } \
} while(0)

/* ============================================================================
 * Test: Read simple LP
 * ============================================================================ */

static void test_read_simple(void) {
    printf("\n=== Test: Read Simple LP ===\n");

    RalphModel *model = ralph_create();
    ASSERT(model != NULL, "Model created");

    int ret = ralph_read_lp(model, "tests/data/simple.lp");
    ASSERT(ret == 0, "LP file read successfully");

    ASSERT(ralph_get_num_vars(model) == 3, "3 variables");
    ASSERT(ralph_get_num_cons(model) == 2, "2 constraints");
    ASSERT(ralph_get_num_integers(model) == 0, "No integer variables");

    /* Solve and check */
    ralph_set_int_param(model, "verbose", 0);
    ralph_optimize(model);

    ASSERT(ralph_get_status(model) == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    /* Expected optimal: x1=20, x2=20, x3=0, obj=180 (MAXIMIZE) */
    double x[3];
    ralph_get_solution(model, x);
    double obj = ralph_get_objval(model);

    /* The optimal solution should give objective around 180 */
    ASSERT(obj < 200 && obj > 170, "Objective in expected range");

    ralph_free(model);
}

/* ============================================================================
 * Test: Read MIP with GENERAL and BINARY
 * ============================================================================ */

static void test_read_mip(void) {
    printf("\n=== Test: Read MIP with Integer Variables ===\n");

    RalphModel *model = ralph_create();
    ASSERT(model != NULL, "Model created");

    int ret = ralph_read_lp(model, "tests/data/mip.lp");
    ASSERT(ret == 0, "LP file read successfully");

    ASSERT(ralph_get_num_vars(model) == 4, "4 variables (x1,x2,x3,y1)");
    ASSERT(ralph_get_num_cons(model) == 3, "3 constraints");
    ASSERT(ralph_get_num_integers(model) == 4, "4 integer/binary variables");
    ASSERT(ralph_is_mip(model) == 1, "Is MIP");

    ralph_free(model);
}

/* ============================================================================
 * Test: Read LP with all bound types
 * ============================================================================ */

static void test_read_bounds(void) {
    printf("\n=== Test: Read LP with Various Bounds ===\n");

    RalphModel *model = ralph_create();
    ASSERT(model != NULL, "Model created");

    int ret = ralph_read_lp(model, "tests/data/bounds.lp");
    ASSERT(ret == 0, "LP file read successfully");

    ASSERT(ralph_get_num_vars(model) == 6, "6 variables");
    ASSERT(ralph_get_num_cons(model) == 1, "1 constraint");

    /* Solve to verify model is valid */
    ralph_set_int_param(model, "verbose", 0);
    ralph_optimize(model);

    RalphStatus status = ralph_get_status(model);
    ASSERT(status != RALPH_STATUS_ERROR,
           "Model solved without error");

    ralph_free(model);
}

/* ============================================================================
 * Test: Read LP with implicit coefficients
 * ============================================================================ */

static void test_read_implicit(void) {
    printf("\n=== Test: Read LP with Implicit Coefficients ===\n");

    RalphModel *model = ralph_create();
    ASSERT(model != NULL, "Model created");

    int ret = ralph_read_lp(model, "tests/data/implicit.lp");
    ASSERT(ret == 0, "LP file read successfully");

    ASSERT(ralph_get_num_vars(model) == 4, "4 variables");
    ASSERT(ralph_get_num_cons(model) == 2, "2 constraints");

    /* Solve */
    ralph_set_int_param(model, "verbose", 0);
    ralph_optimize(model);

    ASSERT(ralph_get_status(model) == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    ralph_free(model);
}

/* ============================================================================
 * Test: Variable and constraint names
 * ============================================================================ */

static void test_names(void) {
    printf("\n=== Test: Variable and Constraint Names ===\n");

    RalphModel *model = ralph_create();
    ASSERT(model != NULL, "Model created");

    int ret = ralph_read_lp(model, "tests/data/simple.lp");
    ASSERT(ret == 0, "LP file read successfully");

    /* Check variable names */
    const char *name = ralph_get_var_name(model, 0);
    ASSERT(name != NULL && strcmp(name, "x1") == 0, "Variable 0 named 'x1'");

    name = ralph_get_var_name(model, 1);
    ASSERT(name != NULL && strcmp(name, "x2") == 0, "Variable 1 named 'x2'");

    name = ralph_get_var_name(model, 2);
    ASSERT(name != NULL && strcmp(name, "x3") == 0, "Variable 2 named 'x3'");

    /* Check constraint names */
    name = ralph_get_con_name(model, 0);
    ASSERT(name != NULL && strcmp(name, "labor") == 0, "Constraint 0 named 'labor'");

    name = ralph_get_con_name(model, 1);
    ASSERT(name != NULL && strcmp(name, "machine") == 0, "Constraint 1 named 'machine'");

    ralph_free(model);
}

/* ============================================================================
 * Test: Write LP file
 * ============================================================================ */

static void test_write_lp(void) {
    printf("\n=== Test: Write LP File ===\n");

    /* Create a simple model programmatically */
    RalphModel *model = ralph_create();
    ASSERT(model != NULL, "Model created");

    ralph_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_set_problem_name(model, "test_write");

    /* Add variables */
    ralph_add_var(model, 0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);
    ralph_set_var_name(model, 0, "x");

    ralph_add_var(model, 0, RALPH_INFINITY, 2.0, RALPH_CONTINUOUS);
    ralph_set_var_name(model, 1, "y");

    ralph_add_var(model, 0, 1, 3.0, RALPH_BINARY);
    ralph_set_var_name(model, 2, "z");

    /* Add constraint */
    int indices[] = {0, 1, 2};
    double values[] = {1.0, 1.0, 1.0};
    ralph_add_constraint(model, 3, indices, values, RALPH_GREATER_EQUAL, 5.0);
    ralph_set_con_name(model, 0, "sum");

    /* Write to file */
    int ret = ralph_write_lp(model, "/tmp/test_write.lp");
    ASSERT(ret == 0, "LP file written successfully");

    ralph_free(model);

    /* Read it back */
    RalphModel *model2 = ralph_create();
    ret = ralph_read_lp(model2, "/tmp/test_write.lp");
    ASSERT(ret == 0, "LP file read back successfully");

    ASSERT(ralph_get_num_vars(model2) == 3, "3 variables after round-trip");
    ASSERT(ralph_get_num_cons(model2) == 1, "1 constraint after round-trip");
    ASSERT(ralph_get_num_integers(model2) == 1, "1 binary variable after round-trip");

    ralph_free(model2);
}

/* ============================================================================
 * Test: Round-trip (read -> solve -> write -> read -> solve)
 * ============================================================================ */

static void test_roundtrip(void) {
    printf("\n=== Test: Round-trip LP ===\n");

    /* Read original file */
    RalphModel *model1 = ralph_create();
    ASSERT(model1 != NULL, "Model 1 created");

    int ret = ralph_read_lp(model1, "tests/data/simple.lp");
    ASSERT(ret == 0, "Original LP file read");

    /* Solve */
    ralph_set_int_param(model1, "verbose", 0);
    ralph_optimize(model1);
    ASSERT(ralph_get_status(model1) == RALPH_STATUS_OPTIMAL, "Model 1 optimal");
    double obj1 = ralph_get_objval(model1);

    /* Write to temp file */
    ret = ralph_write_lp(model1, "/tmp/roundtrip.lp");
    ASSERT(ret == 0, "LP file written");

    /* Read back */
    RalphModel *model2 = ralph_create();
    ret = ralph_read_lp(model2, "/tmp/roundtrip.lp");
    ASSERT(ret == 0, "LP file read back");

    /* Solve */
    ralph_set_int_param(model2, "verbose", 0);
    ralph_optimize(model2);
    ASSERT(ralph_get_status(model2) == RALPH_STATUS_OPTIMAL, "Model 2 optimal");
    double obj2 = ralph_get_objval(model2);

    /* Compare objectives */
    ASSERT_EQ_DBL(obj1, obj2, "Objectives match after round-trip");

    ralph_free(model1);
    ralph_free(model2);
}

/* ============================================================================
 * Test: MPS write/read round-trip
 * ============================================================================ */

static void test_write_mps_roundtrip(void) {
    printf("\n=== Test: Write/Read MPS Round-trip ===\n");

    RalphModel *model1 = ralph_create();
    ASSERT(model1 != NULL, "Model 1 created");

    ralph_set_obj_sense(model1, RALPH_MINIMIZE);
    ralph_set_problem_name(model1, "mps roundtrip");
    ralph_set_obj_offset(model1, 4.0);

    /* x: continuous [0,2], y: integer [0,3], z: binary */
    ralph_add_var(model1, 0.0, 2.0, 1.0, RALPH_CONTINUOUS);
    ralph_add_var(model1, 0.0, 3.0, 2.0, RALPH_INTEGER);
    ralph_add_var(model1, 0.0, 1.0, 3.0, RALPH_BINARY);
    ralph_set_var_name(model1, 0, "x flow");
    ralph_set_var_name(model1, 1, "y-int");
    ralph_set_var_name(model1, 2, "z.bin");

    int idx1[] = {0, 1, 2};
    double val1[] = {1.0, 1.0, 1.0};
    ralph_add_constraint(model1, 3, idx1, val1, RALPH_GREATER_EQUAL, 3.0);
    ralph_set_con_name(model1, 0, "demand row");

    int idx2[] = {0, 2};
    double val2[] = {1.0, 1.0};
    ralph_add_constraint(model1, 2, idx2, val2, RALPH_LESS_EQUAL, 7.0);
    ralph_set_con_name(model1, 1, "cap.row");

    ralph_set_int_param(model1, "verbose", 0);
    ASSERT(ralph_optimize(model1) == 0, "Model 1 solve call succeeds");
    ASSERT(ralph_get_status(model1) == RALPH_STATUS_OPTIMAL, "Model 1 optimal");
    ASSERT_EQ_DBL(ralph_get_objval(model1), 8.0, "Model 1 objective = 8.0");

    ASSERT(ralph_write_mps(model1, "/tmp/roundtrip.mps") == 0, "MPS file written");

    RalphModel *model2 = ralph_create();
    ASSERT(model2 != NULL, "Model 2 created");
    ASSERT(ralph_read_mps(model2, "/tmp/roundtrip.mps") == 0, "MPS file read back");

    ASSERT(ralph_get_num_vars(model2) == 3, "Round-trip keeps 3 variables");
    ASSERT(ralph_get_num_cons(model2) == 2, "Round-trip keeps 2 constraints");
    ASSERT(ralph_get_num_integers(model2) == 2, "Round-trip keeps integer/binary vars");

    ralph_set_int_param(model2, "verbose", 0);
    ASSERT(ralph_optimize(model2) == 0, "Model 2 solve call succeeds");
    ASSERT(ralph_get_status(model2) == RALPH_STATUS_OPTIMAL, "Model 2 optimal");
    ASSERT_EQ_DBL(ralph_get_objval(model2), 8.0, "Model 2 objective = 8.0");

    ralph_free(model1);
    ralph_free(model2);
}

/* ============================================================================
 * Test: Name management API
 * ============================================================================ */

static void test_name_api(void) {
    printf("\n=== Test: Name Management API ===\n");

    RalphModel *model = ralph_create();
    ASSERT(model != NULL, "Model created");

    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    /* Add variables */
    ralph_add_var(model, 0, 10, 1.0, RALPH_CONTINUOUS);
    ralph_add_var(model, 0, 10, 2.0, RALPH_INTEGER);

    /* Set names */
    int ret = ralph_set_var_name(model, 0, "alpha");
    ASSERT(ret == 0, "Set variable 0 name");

    ret = ralph_set_var_name(model, 1, "beta");
    ASSERT(ret == 0, "Set variable 1 name");

    /* Get names */
    const char *name = ralph_get_var_name(model, 0);
    ASSERT(name != NULL && strcmp(name, "alpha") == 0, "Get variable 0 name");

    name = ralph_get_var_name(model, 1);
    ASSERT(name != NULL && strcmp(name, "beta") == 0, "Get variable 1 name");

    /* Invalid indices */
    ASSERT(ralph_get_var_name(model, -1) == NULL, "Invalid index returns NULL");
    ASSERT(ralph_get_var_name(model, 100) == NULL, "Out of bounds returns NULL");

    /* Problem name */
    ret = ralph_set_problem_name(model, "test_problem");
    ASSERT(ret == 0, "Set problem name");

    name = ralph_get_problem_name(model);
    ASSERT(name != NULL && strcmp(name, "test_problem") == 0, "Get problem name");

    ralph_free(model);
}

/* ============================================================================
 * Test: Solution buffer writer
 * ============================================================================ */

static void test_solution_buf(void) {
    printf("\n=== Test: Solution Buffer Writer ===\n");

    RalphModel *model = ralph_create();
    ASSERT(model != NULL, "Model created");

    /* Build a simple problem: max x+y s.t. x+y<=10, x,y>=0 */
    ralph_set_obj_sense(model, RALPH_MAXIMIZE);
    ralph_add_var(model, 0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);  /* x */
    ralph_add_var(model, 0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);  /* y */

    ralph_set_var_name(model, 0, "x");
    ralph_set_var_name(model, 1, "y");

    int idx[] = {0, 1};
    double val[] = {1.0, 1.0};
    ralph_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 10.0);

    ralph_set_int_param(model, "verbose", 0);
    ralph_optimize(model);

    ASSERT(ralph_get_status(model) == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    /* Write solution to buffer */
    char buf[4096];
    int len = ralph_write_solution_buf(model, buf, sizeof(buf));

    ASSERT(len > 0, "Buffer write returned positive length");
    ASSERT(len < (int)sizeof(buf), "Buffer not truncated");

    /* Check content */
    ASSERT(strstr(buf, "solution status: OPTIMAL") != NULL, "Contains OPTIMAL status");
    ASSERT(strstr(buf, "objective value:") != NULL, "Contains objective value");
    ASSERT(strstr(buf, "x ") != NULL, "Contains variable x");
    ASSERT(strstr(buf, "y ") != NULL, "Contains variable y");

    /* Test small buffer (truncation) */
    char small_buf[50];
    int small_len = ralph_write_solution_buf(model, small_buf, sizeof(small_buf));
    ASSERT(small_len >= (int)sizeof(small_buf) - 1, "Small buffer returns truncated length");

    /* Test NULL inputs */
    ASSERT(ralph_write_solution_buf(NULL, buf, sizeof(buf)) == -1, "NULL model returns -1");
    ASSERT(ralph_write_solution_buf(model, NULL, sizeof(buf)) == -1, "NULL buffer returns -1");
    ASSERT(ralph_write_solution_buf(model, buf, 0) == -1, "Zero size returns -1");

    ralph_free(model);
}

/* ============================================================================
 * Test: Invalid file handling
 * ============================================================================ */

static void test_invalid_file(void) {
    printf("\n=== Test: Invalid File Handling ===\n");

    RalphModel *model = ralph_create();
    ASSERT(model != NULL, "Model created");

    /* Non-existent file */
    int ret = ralph_read_lp(model, "nonexistent.lp");
    ASSERT(ret != 0, "Non-existent file returns error");

    /* NULL filename */
    ret = ralph_read_lp(model, NULL);
    ASSERT(ret != 0, "NULL filename returns error");

    ralph_free(model);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║           Ralph LP Format Test Suite                 ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");

    test_read_simple();
    test_read_mip();
    test_read_bounds();
    test_read_implicit();
    test_names();
    test_write_lp();
    test_roundtrip();
    test_write_mps_roundtrip();
    test_name_api();
    test_solution_buf();
    test_invalid_file();

    printf("\n══════════════════════════════════════════════════════════\n");
    printf("Test Summary: %d/%d passed (%.1f%%)\n",
           tests_passed, tests_passed + tests_failed,
           100.0 * tests_passed / (tests_passed + tests_failed));

    if (tests_failed == 0) {
        printf("\n✓ All tests passed!\n");
        return 0;
    } else {
        printf("\n✗ %d tests failed\n", tests_failed);
        return 1;
    }
}
