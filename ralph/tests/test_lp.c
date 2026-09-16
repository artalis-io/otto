/*
 * Ralph - LP Format Tests
 *
 * Tests for CPLEX LP file format reader and writer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ralph_test_mod_api.h"
#include "test_tmp.h"

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

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");

    int ret = ralph_test_read_lp(model, "tests/data/simple.lp");
    ASSERT(ret == 0, "LP file read successfully");

    ASSERT(ralph_test_get_num_vars(model) == 3, "3 variables");
    ASSERT(ralph_test_get_num_cons(model) == 2, "2 constraints");
    ASSERT(ralph_test_get_num_integers(model) == 0, "No integer variables");

    /* Solve and check */
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    /* Expected optimal: x1=20, x2=20, x3=0, obj=180 (MAXIMIZE) */
    double x[3];
    ralph_test_get_solution(model, x);
    double obj = ralph_test_get_objval(model);

    /* The optimal solution should give objective around 180 */
    ASSERT(obj < 200 && obj > 170, "Objective in expected range");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Read MIP with GENERAL and BINARY
 * ============================================================================ */

static void test_read_mip(void) {
    printf("\n=== Test: Read MIP with Integer Variables ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");

    int ret = ralph_test_read_lp(model, "tests/data/mip.lp");
    ASSERT(ret == 0, "LP file read successfully");

    ASSERT(ralph_test_get_num_vars(model) == 4, "4 variables (x1,x2,x3,y1)");
    ASSERT(ralph_test_get_num_cons(model) == 3, "3 constraints");
    ASSERT(ralph_test_get_num_integers(model) == 4, "4 integer/binary variables");
    ASSERT(ralph_test_is_mip(model) == 1, "Is MIP");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Read LP with all bound types
 * ============================================================================ */

static void test_read_bounds(void) {
    printf("\n=== Test: Read LP with Various Bounds ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");

    int ret = ralph_test_read_lp(model, "tests/data/bounds.lp");
    ASSERT(ret == 0, "LP file read successfully");

    ASSERT(ralph_test_get_num_vars(model) == 6, "6 variables");
    ASSERT(ralph_test_get_num_cons(model) == 1, "1 constraint");

    /* Solve to verify model is valid */
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status != RALPH_STATUS_ERROR,
           "Model solved without error");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Read LP with implicit coefficients
 * ============================================================================ */

static void test_read_implicit(void) {
    printf("\n=== Test: Read LP with Implicit Coefficients ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");

    int ret = ralph_test_read_lp(model, "tests/data/implicit.lp");
    ASSERT(ret == 0, "LP file read successfully");

    ASSERT(ralph_test_get_num_vars(model) == 4, "4 variables");
    ASSERT(ralph_test_get_num_cons(model) == 2, "2 constraints");

    /* Solve */
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Variable and constraint names
 * ============================================================================ */

static void test_names(void) {
    printf("\n=== Test: Variable and Constraint Names ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");

    int ret = ralph_test_read_lp(model, "tests/data/simple.lp");
    ASSERT(ret == 0, "LP file read successfully");

    /* Check variable names */
    const char *name = ralph_test_get_var_name(model, 0);
    ASSERT(name != NULL && strcmp(name, "x1") == 0, "Variable 0 named 'x1'");

    name = ralph_test_get_var_name(model, 1);
    ASSERT(name != NULL && strcmp(name, "x2") == 0, "Variable 1 named 'x2'");

    name = ralph_test_get_var_name(model, 2);
    ASSERT(name != NULL && strcmp(name, "x3") == 0, "Variable 2 named 'x3'");

    /* Check constraint names */
    name = ralph_test_get_con_name(model, 0);
    ASSERT(name != NULL && strcmp(name, "labor") == 0, "Constraint 0 named 'labor'");

    name = ralph_test_get_con_name(model, 1);
    ASSERT(name != NULL && strcmp(name, "machine") == 0, "Constraint 1 named 'machine'");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Write LP file
 * ============================================================================ */

static void test_write_lp(void) {
    printf("\n=== Test: Write LP File ===\n");

    /* Create a simple model programmatically */
    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_core_set_problem_name(model, "test_write");

    /* Add variables */
    ralph_test_add_var(model, 0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);
    ralph_test_set_var_name(model, 0, "x");

    ralph_test_add_var(model, 0, RALPH_INFINITY, 2.0, RALPH_CONTINUOUS);
    ralph_test_set_var_name(model, 1, "y");

    ralph_test_add_var(model, 0, 1, 3.0, RALPH_BINARY);
    ralph_test_set_var_name(model, 2, "z");

    /* Add constraint */
    int indices[] = {0, 1, 2};
    double values[] = {1.0, 1.0, 1.0};
    ralph_test_add_constraint(model, 3, indices, values, RALPH_GREATER_EQUAL, 5.0);
    ralph_test_set_con_name(model, 0, "sum");

    /* Write to file */
    char lp_path[320];
    ASSERT(ralph_tmp_path(lp_path, sizeof(lp_path), "test_write.lp") != NULL,
           "Resolved a temp path for the LP file");
    int ret = ralph_test_write_lp(model, lp_path);
    ASSERT(ret == 0, "LP file written successfully");

    ralph_test_free(model);

    /* Read it back */
    RalphModel *model2 = ralph_test_create();
    ret = ralph_test_read_lp(model2, lp_path);
    ASSERT(ret == 0, "LP file read back successfully");

    ASSERT(ralph_test_get_num_vars(model2) == 3, "3 variables after round-trip");
    ASSERT(ralph_test_get_num_cons(model2) == 1, "1 constraint after round-trip");
    ASSERT(ralph_test_get_num_integers(model2) == 1, "1 binary variable after round-trip");

    ralph_test_free(model2);
}

/* ============================================================================
 * Test: Round-trip (read -> solve -> write -> read -> solve)
 * ============================================================================ */

static void test_roundtrip(void) {
    printf("\n=== Test: Round-trip LP ===\n");

    /* Read original file */
    RalphModel *model1 = ralph_test_create();
    ASSERT(model1 != NULL, "Model 1 created");

    int ret = ralph_test_read_lp(model1, "tests/data/simple.lp");
    ASSERT(ret == 0, "Original LP file read");

    /* Solve */
    ralph_test_set_int_param(model1, "verbose", 0);
    ralph_test_optimize(model1);
    ASSERT(ralph_test_get_status(model1) == RALPH_STATUS_OPTIMAL, "Model 1 optimal");
    double obj1 = ralph_test_get_objval(model1);

    /* Write to temp file */
    char rt_path[320];
    ASSERT(ralph_tmp_path(rt_path, sizeof(rt_path), "roundtrip.lp") != NULL,
           "Resolved a temp path for the round-trip LP");
    ret = ralph_test_write_lp(model1, rt_path);
    ASSERT(ret == 0, "LP file written");

    /* Read back */
    RalphModel *model2 = ralph_test_create();
    ret = ralph_test_read_lp(model2, rt_path);
    ASSERT(ret == 0, "LP file read back");

    /* Solve */
    ralph_test_set_int_param(model2, "verbose", 0);
    ralph_test_optimize(model2);
    ASSERT(ralph_test_get_status(model2) == RALPH_STATUS_OPTIMAL, "Model 2 optimal");
    double obj2 = ralph_test_get_objval(model2);

    /* Compare objectives */
    ASSERT_EQ_DBL(obj1, obj2, "Objectives match after round-trip");

    ralph_test_free(model1);
    ralph_test_free(model2);
}

/* ============================================================================
 * Test: MPS write/read round-trip
 * ============================================================================ */

static void test_write_mps_roundtrip(void) {
    printf("\n=== Test: Write/Read MPS Round-trip ===\n");

    RalphModel *model1 = ralph_test_create();
    ASSERT(model1 != NULL, "Model 1 created");

    ralph_test_set_obj_sense(model1, RALPH_MINIMIZE);
    ralph_core_set_problem_name(model1, "mps roundtrip");
    ralph_test_set_obj_offset(model1, 4.0);

    /* x: continuous [0,2], y: integer [0,3], z: binary */
    ralph_test_add_var(model1, 0.0, 2.0, 1.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model1, 0.0, 3.0, 2.0, RALPH_INTEGER);
    ralph_test_add_var(model1, 0.0, 1.0, 3.0, RALPH_BINARY);
    ralph_test_set_var_name(model1, 0, "x flow");
    ralph_test_set_var_name(model1, 1, "y-int");
    ralph_test_set_var_name(model1, 2, "z.bin");

    int idx1[] = {0, 1, 2};
    double val1[] = {1.0, 1.0, 1.0};
    ralph_test_add_constraint(model1, 3, idx1, val1, RALPH_GREATER_EQUAL, 3.0);
    ralph_test_set_con_name(model1, 0, "demand row");

    int idx2[] = {0, 2};
    double val2[] = {1.0, 1.0};
    ralph_test_add_constraint(model1, 2, idx2, val2, RALPH_LESS_EQUAL, 7.0);
    ralph_test_set_con_name(model1, 1, "cap.row");

    ralph_test_set_int_param(model1, "verbose", 0);
    ASSERT(ralph_test_optimize(model1) == 0, "Model 1 solve call succeeds");
    ASSERT(ralph_test_get_status(model1) == RALPH_STATUS_OPTIMAL, "Model 1 optimal");
    ASSERT_EQ_DBL(ralph_test_get_objval(model1), 8.0, "Model 1 objective = 8.0");

    char mps_path[320];
    ASSERT(ralph_tmp_path(mps_path, sizeof(mps_path), "roundtrip.mps") != NULL,
           "Resolved a temp path for the MPS file");
    ASSERT(ralph_test_write_mps(model1, mps_path) == 0, "MPS file written");

    RalphModel *model2 = ralph_test_create();
    ASSERT(model2 != NULL, "Model 2 created");
    ASSERT(ralph_test_read_mps(model2, mps_path) == 0, "MPS file read back");

    ASSERT(ralph_test_get_num_vars(model2) == 3, "Round-trip keeps 3 variables");
    ASSERT(ralph_test_get_num_cons(model2) == 2, "Round-trip keeps 2 constraints");
    ASSERT(ralph_test_get_num_integers(model2) == 2, "Round-trip keeps integer/binary vars");

    ralph_test_set_int_param(model2, "verbose", 0);
    ASSERT(ralph_test_optimize(model2) == 0, "Model 2 solve call succeeds");
    ASSERT(ralph_test_get_status(model2) == RALPH_STATUS_OPTIMAL, "Model 2 optimal");
    ASSERT_EQ_DBL(ralph_test_get_objval(model2), 8.0, "Model 2 objective = 8.0");

    ralph_test_free(model1);
    ralph_test_free(model2);
}

/* ============================================================================
 * Test: MPS output is something other tools will actually read
 *
 * The round-trip test above passes Ralph's own reader, which is lenient about
 * where a record starts. GLPK is not, and for a while nothing here noticed
 * that every data record was being written in column 1 -- where MPS reserves
 * space for section indicators -- so glpsol rejected every file Ralph wrote
 * with "invalid indicator record" and the models needed hand-editing before
 * they could be used as a cross-check.
 *
 * This asserts the two format properties directly, so the guarantee does not
 * depend on having glpsol installed to notice.
 * ============================================================================ */

static int mps_is_section_line(const char *line) {
    static const char *sections[] = {
        "NAME", "ROWS", "COLUMNS", "RHS", "RANGES", "BOUNDS", "OBJSENSE",
        "ENDATA", NULL
    };
    for (int i = 0; sections[i]; i++) {
        size_t n = strlen(sections[i]);
        if (strncmp(line, sections[i], n) == 0 &&
            (line[n] == '\0' || line[n] == ' ' || line[n] == '\r' || line[n] == '\n')) {
            return 1;
        }
    }
    return 0;
}

static void test_write_mps_format(void) {
    printf("\n=== Test: MPS Fixed-Format Validity ===\n");

    /* --- a MIN model: must come out as plain MPS, no extensions --- */
    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0.0, 2.0, 1.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0.0, 3.0, 2.0, RALPH_INTEGER);
    ralph_test_add_var(model, 0.0, 1.0, 3.0, RALPH_BINARY);
    int idx[] = {0, 1, 2};
    double val[] = {1.0, 1.0, 1.0};
    ralph_test_add_constraint(model, 3, idx, val, RALPH_GREATER_EQUAL, 2.0);

    char path[320];
    ASSERT(ralph_tmp_path(path, sizeof(path), "format.mps") != NULL,
           "Resolved a temp path");
    ASSERT(ralph_test_write_mps(model, path) == 0, "MPS written");

    FILE *f = fopen(path, "r");
    ASSERT(f != NULL, "MPS reopened for inspection");

    char line[1024];
    int col1_offenders = 0;
    int saw_objsense = 0;
    int data_records = 0;
    while (f && fgets(line, sizeof(line), f)) {
        if (line[0] == '\n' || line[0] == '\r') continue;
        if (strncmp(line, "OBJSENSE", 8) == 0) saw_objsense = 1;
        if (mps_is_section_line(line)) continue;
        data_records++;
        /* A data record must not begin in column 1: a reader that sees one
         * there treats it as an indicator record. */
        if (line[0] != ' ' && line[0] != '\t') col1_offenders++;
    }
    if (f) fclose(f);

    ASSERT(data_records > 0, "MPS contains data records to check");
    ASSERT(col1_offenders == 0, "No data record starts in column 1");
    ASSERT(saw_objsense == 0, "A MIN model emits no OBJSENSE extension");
    ralph_test_free(model);
}

/* Split out from the format test above rather than folded into it: ASSERT
 * returns on the first failure, so a structural problem in the MIN file would
 * otherwise hide whether the objective sense still survives at all. */
static void test_write_mps_max_sense(void) {
    printf("\n=== Test: MPS MAX Objective Sense ===\n");

    RalphModel *mx = ralph_test_create();
    ASSERT(mx != NULL, "MAX model created");
    ralph_test_set_obj_sense(mx, RALPH_MAXIMIZE);
    ralph_test_add_var(mx, 0.0, 10.0, 1.0, RALPH_CONTINUOUS);
    int mi[] = {0};
    double mv[] = {1.0};
    ralph_test_add_constraint(mx, 1, mi, mv, RALPH_LESS_EQUAL, 5.0);

    char mpath[320];
    ASSERT(ralph_tmp_path(mpath, sizeof(mpath), "format_max.mps") != NULL,
           "Resolved a temp path for the MAX model");
    ASSERT(ralph_test_write_mps(mx, mpath) == 0, "MAX model written");

    RalphModel *back = ralph_test_create();
    ASSERT(ralph_test_read_mps(back, mpath) == 0, "MAX model read back");
    ralph_test_set_int_param(back, "verbose", 0);
    ASSERT(ralph_test_optimize(back) == 0, "MAX model solve call succeeds");
    ASSERT(ralph_test_get_status(back) == RALPH_STATUS_OPTIMAL, "MAX model optimal");
    /* 5.0 if the sense survived; 0.0 if it silently became a minimization. */
    ASSERT_EQ_DBL(ralph_test_get_objval(back), 5.0,
                  "MAX objective survives the round-trip");

    ralph_test_free(mx);
    ralph_test_free(back);
}


/* ============================================================================
 * Test: names that sanitise alike stay distinct
 *
 * Fixed-format MPS delimits fields by column, so a name may contain spaces and
 * punctuation, and real models do: forplan.mps has seven columns whose names
 * differ only in a trailing "#", ")", "+" and so on. Mapping each offending
 * character to '_' independently turned all seven into one name and merged
 * their coefficients -- a different model, not a malformed file, which is what
 * made it worth a test rather than a comment.
 * ============================================================================ */

static void test_write_mps_name_collisions(void) {
    printf("\n=== Test: MPS Name Collisions ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    /* Four names that all sanitise to "COL_X" character-by-character. */
    const char *raw[4] = { "COL X", "COL#X", "COL)X", "COL+X" };
    for (int i = 0; i < 4; i++) {
        ralph_test_add_var(model, 0.0, 10.0, 1.0 + i, RALPH_CONTINUOUS);
        ralph_test_set_var_name(model, i, raw[i]);
    }
    int idx[4] = {0, 1, 2, 3};
    double val[4] = {1.0, 1.0, 1.0, 1.0};
    ralph_test_add_constraint(model, 4, idx, val, RALPH_GREATER_EQUAL, 2.0);

    char path[320];
    ASSERT(ralph_tmp_path(path, sizeof(path), "collide.mps") != NULL,
           "Resolved a temp path");
    ASSERT(ralph_test_write_mps(model, path) == 0, "MPS written");

    /* Collect the column names actually emitted: field 2, columns 5-12. */
    FILE *f = fopen(path, "r");
    ASSERT(f != NULL, "MPS reopened");

    char line[1024];
    char seen[64][16];
    int nseen = 0, in_columns = 0, dupes = 0;
    while (f && fgets(line, sizeof(line), f)) {
        if (strncmp(line, "COLUMNS", 7) == 0) { in_columns = 1; continue; }
        if (strncmp(line, "RHS", 3) == 0) break;
        if (!in_columns || line[0] != ' ') continue;
        if (strstr(line, "MARKER")) continue;

        char name[16];
        int n = 0;
        for (int i = 4; i < 12 && line[i] && line[i] != '\n' && n < 15; i++) {
            name[n++] = line[i];
        }
        while (n > 0 && name[n - 1] == ' ') n--;
        name[n] = '\0';
        if (n == 0) continue;

        int found = 0;
        for (int i = 0; i < nseen; i++) {
            if (strcmp(seen[i], name) == 0) { found = 1; break; }
        }
        if (!found && nseen < 64) {
            snprintf(seen[nseen], sizeof(seen[0]), "%s", name);
            nseen++;
        }
    }
    if (f) fclose(f);

    /* Four variables must appear under four different names. Before the fix
     * all four collapsed to one, so this read 1. */
    ASSERT(nseen == 4, "Four colliding source names emit four distinct names");

    /* And each must still fit field 2, or the file stops being fixed-format. */
    for (int i = 0; i < nseen; i++) {
        if (strlen(seen[i]) > 8) dupes++;
    }
    ASSERT(dupes == 0, "Disambiguated names still fit the 8-column field");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: a ranged row survives the MPS round-trip as a range
 *
 * mps_reader.c expands a ranged row into two constraints, because the model
 * carries no range concept. Writing them back as two rows is correct but
 * lossy in a way that bites: nesm.mps came out as 751 rows instead of 663, and
 * one of the computed right-hand sides needed 18 characters to round-trip,
 * which no fixed-format field 4 can hold. The writer now folds the pair back
 * into a row plus a RANGES entry.
 * ============================================================================ */

static void test_write_mps_ranges(void) {
    printf("\n=== Test: MPS RANGES Reconstruction ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0.0, 10.0, 1.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0.0, 10.0, 1.0, RALPH_CONTINUOUS);

    /* The shape mps_reader.c produces for a ranged row: same coefficients,
     * G with the lower bound then L with the upper. */
    int idx[2] = {0, 1};
    double val[2] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 2.0);
    ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 5.0);

    char path[320];
    ASSERT(ralph_tmp_path(path, sizeof(path), "ranges.mps") != NULL,
           "Resolved a temp path");
    ASSERT(ralph_test_write_mps(model, path) == 0, "MPS written");

    FILE *f = fopen(path, "r");
    ASSERT(f != NULL, "MPS reopened");

    char line[1024];
    int saw_ranges = 0, row_entries = 0, in_rows = 0;
    double range_val = 0.0;
    while (f && fgets(line, sizeof(line), f)) {
        if (strncmp(line, "ROWS", 4) == 0)    { in_rows = 1; continue; }
        if (strncmp(line, "COLUMNS", 7) == 0) { in_rows = 0; continue; }
        if (in_rows && (line[0] == ' ')) row_entries++;
        if (strncmp(line, "RANGES", 6) == 0) { saw_ranges = 1; continue; }
        if (saw_ranges && line[0] == ' ' && range_val == 0.0) {
            char a[64], b[64];
            double v = 0.0;
            if (sscanf(line, "%63s %63s %lf", a, b, &v) == 3) range_val = v;
        }
    }
    if (f) fclose(f);

    /* One N row plus one constraint row, not one N row plus two. */
    ASSERT(row_entries == 2, "Ranged pair emits one constraint row, not two");
    ASSERT(saw_ranges == 1, "A RANGES section is written");
    ASSERT_EQ_DBL(range_val, 3.0, "Range value is hi - lo");

    /* And it has to mean the same thing coming back. */
    RalphModel *back = ralph_test_create();
    ASSERT(ralph_test_read_mps(back, path) == 0, "MPS read back");
    ASSERT(ralph_test_get_num_cons(back) == 2,
           "Reader re-expands the range into two constraints");

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_int_param(back, "verbose", 0);
    ralph_test_optimize(model);
    ralph_test_optimize(back);
    ASSERT(ralph_test_get_status(back) == RALPH_STATUS_OPTIMAL, "Round-trip optimal");
    ASSERT_EQ_DBL(ralph_test_get_objval(back), ralph_test_get_objval(model),
                  "Objective survives the range round-trip");

    ralph_test_free(model);
    ralph_test_free(back);
}

/* ============================================================================
 * Test: Name management API
 * ============================================================================ */

static void test_name_api(void) {
    printf("\n=== Test: Name Management API ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    /* Add variables */
    ralph_test_add_var(model, 0, 10, 1.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0, 10, 2.0, RALPH_INTEGER);

    /* Set names */
    int ret = ralph_test_set_var_name(model, 0, "alpha");
    ASSERT(ret == 0, "Set variable 0 name");

    ret = ralph_test_set_var_name(model, 1, "beta");
    ASSERT(ret == 0, "Set variable 1 name");

    /* Get names */
    const char *name = ralph_test_get_var_name(model, 0);
    ASSERT(name != NULL && strcmp(name, "alpha") == 0, "Get variable 0 name");

    name = ralph_test_get_var_name(model, 1);
    ASSERT(name != NULL && strcmp(name, "beta") == 0, "Get variable 1 name");

    /* Invalid indices */
    ASSERT(ralph_test_get_var_name(model, -1) == NULL, "Invalid index returns NULL");
    ASSERT(ralph_test_get_var_name(model, 100) == NULL, "Out of bounds returns NULL");

    /* Problem name */
    ret = ralph_core_set_problem_name(model, "test_problem");
    ASSERT(ret == 0, "Set problem name");

    name = ralph_core_get_problem_name(model);
    ASSERT(name != NULL && strcmp(name, "test_problem") == 0, "Get problem name");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Solution buffer writer
 * ============================================================================ */

static void test_solution_buf(void) {
    printf("\n=== Test: Solution Buffer Writer ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");

    /* Build a simple problem: max x+y s.t. x+y<=10, x,y>=0 */
    ralph_test_set_obj_sense(model, RALPH_MAXIMIZE);
    ralph_test_add_var(model, 0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);  /* x */
    ralph_test_add_var(model, 0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);  /* y */

    ralph_test_set_var_name(model, 0, "x");
    ralph_test_set_var_name(model, 1, "y");

    int idx[] = {0, 1};
    double val[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 10.0);

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    /* Write solution to buffer */
    char buf[4096];
    int len = ralph_test_write_solution_buf(model, buf, sizeof(buf));

    ASSERT(len > 0, "Buffer write returned positive length");
    ASSERT(len < (int)sizeof(buf), "Buffer not truncated");

    /* Check content */
    ASSERT(strstr(buf, "solution status: OPTIMAL") != NULL, "Contains OPTIMAL status");
    ASSERT(strstr(buf, "objective value:") != NULL, "Contains objective value");
    ASSERT(strstr(buf, "x ") != NULL, "Contains variable x");
    ASSERT(strstr(buf, "y ") != NULL, "Contains variable y");

    /* Test small buffer (truncation) */
    char small_buf[50];
    int small_len = ralph_test_write_solution_buf(model, small_buf, sizeof(small_buf));
    ASSERT(small_len >= (int)sizeof(small_buf) - 1, "Small buffer returns truncated length");

    /* Test NULL inputs */
    ASSERT(ralph_test_write_solution_buf(NULL, buf, sizeof(buf)) == -1, "NULL model returns -1");
    ASSERT(ralph_test_write_solution_buf(model, NULL, sizeof(buf)) == -1, "NULL buffer returns -1");
    ASSERT(ralph_test_write_solution_buf(model, buf, 0) == -1, "Zero size returns -1");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Invalid file handling
 * ============================================================================ */

static void test_invalid_file(void) {
    printf("\n=== Test: Invalid File Handling ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");

    /* Non-existent file */
    int ret = ralph_test_read_lp(model, "nonexistent.lp");
    ASSERT(ret != 0, "Non-existent file returns error");

    /* NULL filename */
    ret = ralph_test_read_lp(model, NULL);
    ASSERT(ret != 0, "NULL filename returns error");

    ralph_test_free(model);
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
    test_write_mps_format();
    test_write_mps_max_sense();
    test_write_mps_name_collisions();
    test_write_mps_ranges();
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
