/*
 * Tests for structured LP/MIP API error mapping.
 */

#include <stdio.h>
#include <string.h>

#include "ralph_lp.h"
#include "ralph_mip.h"

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

static RalphLPModel* build_small_lp(void) {
    RalphLPModel *model = ralph_lp_create();
    int idx[1] = {0};
    double val[1] = {1.0};
    if (!model) return NULL;
    if (ralph_lp_set_obj_sense(model, RALPH_LP_OBJ_MINIMIZE) != 0) return model;
    if (ralph_lp_add_var(model, 0.0, RALPH_LP_INFINITY, 1.0, RALPH_LP_VAR_CONTINUOUS) < 0) return model;
    (void)ralph_lp_add_constraint(model, 1, idx, val, RALPH_LP_SENSE_GREATER_EQUAL, 1.0);
    return model;
}

static void test_unknown_parameter_name_maps_error(void) {
    RalphLPModel *model = build_small_lp();
    RalphAPIError err;

    ASSERT_TRUE(model != NULL, "unknown-param: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_lp_set_int_param(model, "does_not_exist_param", 1), -1,
                  "unknown-param: set fails");
    ASSERT_INT_EQ(ralph_lp_get_last_error(model, &err), 0,
                  "unknown-param: last error available");
    ASSERT_INT_EQ((int)err.domain, (int)RALPH_ERROR_DOMAIN_PARAMETER,
                  "unknown-param: domain=PARAMETER");
    ASSERT_INT_EQ((int)err.code, (int)RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                  "unknown-param: code=UNKNOWN_PARAMETER");
    ASSERT_INT_EQ((int)err.api_id, (int)RALPH_ERROR_API_PARAMETER,
                  "unknown-param: api=PARAMETER");

    ASSERT_INT_EQ(ralph_lp_clear_error(model), 0,
                  "unknown-param: clear succeeds");
    ASSERT_INT_EQ(ralph_lp_get_last_error(model, &err), -1,
                  "unknown-param: clear removes snapshot");

    ralph_lp_free(model);
}

static void test_scope_mismatch_maps_error(void) {
    RalphMIPModel *model = ralph_mip_create();
    RalphAPIError err;

    ASSERT_TRUE(model != NULL, "scope-mismatch: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_mip_set_int_param(model, "lp_algorithm",
                                          (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX),
                  -1,
                  "scope-mismatch: MIP setter rejects LP-only param");
    ASSERT_INT_EQ(ralph_mip_get_last_error(model, &err), 0,
                  "scope-mismatch: last error available");
    ASSERT_INT_EQ((int)err.domain, (int)RALPH_ERROR_DOMAIN_PARAMETER,
                  "scope-mismatch: domain=PARAMETER");
    ASSERT_INT_EQ((int)err.code, (int)RALPH_ERROR_CODE_PARAMETER_SCOPE_MISMATCH,
                  "scope-mismatch: code=SCOPE_MISMATCH");
    ASSERT_INT_EQ((int)err.api_id, (int)RALPH_ERROR_API_PARAMETER,
                  "scope-mismatch: api=PARAMETER");

    ralph_mip_free(model);
}

static void test_solution_unavailable_maps_error(void) {
    RalphLPModel *model = build_small_lp();
    RalphAPIError err;
    double x[1] = {0.0};

    ASSERT_TRUE(model != NULL, "solution-unavailable: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_lp_get_solution(model, x), -1,
                  "solution-unavailable: solution query fails before solve");
    ASSERT_INT_EQ(ralph_lp_get_last_error(model, &err), 0,
                  "solution-unavailable: last error available");
    ASSERT_INT_EQ((int)err.domain, (int)RALPH_ERROR_DOMAIN_STATE,
                  "solution-unavailable: domain=STATE");
    ASSERT_INT_EQ((int)err.code, (int)RALPH_ERROR_CODE_NOT_AVAILABLE,
                  "solution-unavailable: code=NOT_AVAILABLE");
    ASSERT_INT_EQ((int)err.api_id, (int)RALPH_ERROR_API_SOLUTION_QUERY,
                  "solution-unavailable: api=SOLUTION_QUERY");

    ralph_lp_free(model);
}

static void test_model_edit_out_of_range_maps_error(void) {
    RalphLPModel *model = build_small_lp();
    RalphAPIError err;

    ASSERT_TRUE(model != NULL, "model-edit: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_lp_set_obj_coef(model, 5, 2.0), -1,
                  "model-edit: set obj coef fails for out-of-range var");
    ASSERT_INT_EQ(ralph_lp_get_last_error(model, &err), 0,
                  "model-edit: last error available");
    ASSERT_INT_EQ((int)err.domain, (int)RALPH_ERROR_DOMAIN_RANGE,
                  "model-edit: domain=RANGE");
    ASSERT_INT_EQ((int)err.code, (int)RALPH_ERROR_CODE_OUT_OF_RANGE,
                  "model-edit: code=OUT_OF_RANGE");
    ASSERT_INT_EQ((int)err.api_id, (int)RALPH_ERROR_API_MODEL_EDIT,
                  "model-edit: api=MODEL_EDIT");

    ralph_lp_free(model);
}

static void test_tls_io_open_failure_maps_error(void) {
    RalphAPIError err;
    RalphLPBasis *basis;

    ASSERT_INT_EQ(ralph_lp_clear_error(NULL), 0,
                  "tls-io: clear TLS error succeeds");
    basis = ralph_lp_read_basis_file("/tmp/ralph_missing_basis_file.bas");
    ASSERT_TRUE(basis == NULL, "tls-io: read basis fails for missing file");
    ASSERT_INT_EQ(ralph_lp_get_last_error(NULL, &err), 0,
                  "tls-io: TLS last error available");
    ASSERT_INT_EQ((int)err.domain, (int)RALPH_ERROR_DOMAIN_IO,
                  "tls-io: domain=IO");
    ASSERT_INT_EQ((int)err.code, (int)RALPH_ERROR_CODE_IO_OPEN_FAILED,
                  "tls-io: code=IO_OPEN_FAILED");
    ASSERT_INT_EQ((int)err.api_id, (int)RALPH_ERROR_API_IO,
                  "tls-io: api=IO");
}

static void test_external_invalid_provider_maps_tls_error(void) {
    RalphAPIError err;
    RalphLPExternalCapabilities caps;

    ASSERT_INT_EQ(ralph_lp_clear_error(NULL), 0,
                  "external-invalid-provider: clear TLS error succeeds");
    ASSERT_INT_EQ(ralph_lp_external_provider_capabilities((RalphLPExternalProvider)99, &caps), -1,
                  "external-invalid-provider: invalid provider rejected");
    ASSERT_INT_EQ(ralph_lp_get_last_error(NULL, &err), 0,
                  "external-invalid-provider: TLS last error available");
    ASSERT_INT_EQ((int)err.domain, (int)RALPH_ERROR_DOMAIN_RANGE,
                  "external-invalid-provider: domain=RANGE");
    ASSERT_INT_EQ((int)err.code, (int)RALPH_ERROR_CODE_OUT_OF_RANGE,
                  "external-invalid-provider: code=OUT_OF_RANGE");
    ASSERT_INT_EQ((int)err.api_id, (int)RALPH_ERROR_API_EXTERNAL,
                  "external-invalid-provider: api=EXTERNAL");
}

static void test_lp_only_solve_on_mip_maps_error(void) {
    RalphLPModel *model = ralph_lp_create();
    RalphAPIError err;

    ASSERT_TRUE(model != NULL, "lp-only-on-mip: model created");
    if (!model) return;
    ASSERT_INT_EQ(ralph_lp_add_var(model, 0.0, 1.0, 1.0, RALPH_LP_VAR_BINARY), 0,
                  "lp-only-on-mip: add binary variable");

    ASSERT_INT_EQ(ralph_lp_optimize(model), -1,
                  "lp-only-on-mip: LP optimize rejects MIP model");
    ASSERT_INT_EQ(ralph_lp_get_last_error(model, &err), 0,
                  "lp-only-on-mip: last error available");
    ASSERT_INT_EQ((int)err.domain, (int)RALPH_ERROR_DOMAIN_STATE,
                  "lp-only-on-mip: domain=STATE");
    ASSERT_INT_EQ((int)err.code, (int)RALPH_ERROR_CODE_NOT_AVAILABLE,
                  "lp-only-on-mip: code=NOT_AVAILABLE");
    ASSERT_INT_EQ((int)err.api_id, (int)RALPH_ERROR_API_SOLVE,
                  "lp-only-on-mip: api=SOLVE");

    ralph_lp_free(model);
}

int main(void) {
    printf("Running LP API error mapping tests...\n");

    test_unknown_parameter_name_maps_error();
    test_scope_mismatch_maps_error();
    test_solution_unavailable_maps_error();
    test_model_edit_out_of_range_maps_error();
    test_tls_io_open_failure_maps_error();
    test_external_invalid_provider_maps_tls_error();
    test_lp_only_solve_on_mip_maps_error();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
