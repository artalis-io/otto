/*
 * Ralph API Tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ralph_api.h"

/* Simple test framework */
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  Testing %s...", #name); \
    test_##name(); \
    printf(" PASSED\n"); \
    tests_passed++; \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf(" FAILED at line %d: %s\n", __LINE__, #cond); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf(" FAILED at line %d: %s != %s\n", __LINE__, #a, #b); \
        tests_failed++; \
        return; \
    } \
} while(0)

/* Test: Create and free context */
TEST(context_lifecycle) {
    RalphAPIContext *ctx = ralph_api_create();
    ASSERT(ctx != NULL);
    ASSERT(ralph_api_ready(ctx) == 1);
    ralph_api_free(ctx);
}

/* Test: Health endpoint */
TEST(health_endpoint) {
    RalphAPIContext *ctx = ralph_api_create();
    ASSERT(ctx != NULL);

    RalphAPIRequest req = {
        .method = "GET",
        .path = "/api/v1/health",
        .query = NULL,
        .body = NULL,
        .body_len = 0
    };

    RalphAPIResponse resp;
    int result = ralph_api_handle(ctx, &req, &resp);
    ASSERT_EQ(result, 0);
    ASSERT_EQ(resp.status_code, 200);
    ASSERT(strstr((char*)resp.body, "\"status\":\"ok\"") != NULL);

    ralph_api_response_free(&resp);
    ralph_api_free(ctx);
}

/* Test: Formats endpoint */
TEST(formats_endpoint) {
    RalphAPIContext *ctx = ralph_api_create();
    ASSERT(ctx != NULL);

    RalphAPIRequest req = {
        .method = "GET",
        .path = "/api/v1/formats",
        .query = NULL,
        .body = NULL,
        .body_len = 0
    };

    RalphAPIResponse resp;
    int result = ralph_api_handle(ctx, &req, &resp);
    ASSERT_EQ(result, 0);
    ASSERT_EQ(resp.status_code, 200);
    ASSERT(strstr((char*)resp.body, "\"lp\"") != NULL);
    ASSERT(strstr((char*)resp.body, "\"mps\"") != NULL);

    ralph_api_response_free(&resp);
    ralph_api_free(ctx);
}

/* Test: Solve simple LP */
TEST(solve_simple_lp) {
    RalphAPIContext *ctx = ralph_api_create();
    ASSERT(ctx != NULL);

    const char *json_body =
        "{"
        "\"format\":\"lp\","
        "\"problem\":\"max: 5 x + 3 y\\n"
        "subject to\\n"
        "wood: 2 x + 4 y <= 40\\n"
        "labor: 3 x + 2 y <= 24\\n"
        "bounds\\n"
        "x >= 0\\n"
        "y >= 0\\n"
        "end\","
        "\"timeout_ms\":5000"
        "}";

    RalphAPIRequest req = {
        .method = "POST",
        .path = "/api/v1/solve",
        .query = NULL,
        .body = json_body,
        .body_len = strlen(json_body)
    };

    RalphAPIResponse resp;
    int result = ralph_api_handle(ctx, &req, &resp);
    ASSERT_EQ(result, 0);
    ASSERT_EQ(resp.status_code, 200);
    ASSERT(strstr((char*)resp.body, "\"status\":\"optimal\"") != NULL);
    ASSERT(strstr((char*)resp.body, "\"objective\":") != NULL);
    ASSERT(strstr((char*)resp.body, "\"x\":") != NULL);
    ASSERT(strstr((char*)resp.body, "\"y\":") != NULL);

    /* Optimal solution: x=8, y=0, obj=5*8+3*0=40 */
    /* (labor constraint: 3*8+2*0=24<=24, wood: 2*8+4*0=16<=40) */
    char *obj_str = strstr((char*)resp.body, "\"objective\":");
    ASSERT(obj_str != NULL);
    double obj = 0;
    sscanf(obj_str, "\"objective\":%lf", &obj);
    ASSERT(obj > 39.9 && obj < 40.1);

    ralph_api_response_free(&resp);
    ralph_api_free(ctx);
}

/* Test: 404 for unknown endpoint */
TEST(not_found) {
    RalphAPIContext *ctx = ralph_api_create();
    ASSERT(ctx != NULL);

    RalphAPIRequest req = {
        .method = "GET",
        .path = "/api/v1/unknown",
        .query = NULL,
        .body = NULL,
        .body_len = 0
    };

    RalphAPIResponse resp;
    int result = ralph_api_handle(ctx, &req, &resp);
    ASSERT_EQ(result, 0);
    ASSERT_EQ(resp.status_code, 404);

    ralph_api_response_free(&resp);
    ralph_api_free(ctx);
}

/* Test: 400 for missing body */
TEST(missing_body) {
    RalphAPIContext *ctx = ralph_api_create();
    ASSERT(ctx != NULL);

    RalphAPIRequest req = {
        .method = "POST",
        .path = "/api/v1/solve",
        .query = NULL,
        .body = NULL,
        .body_len = 0
    };

    RalphAPIResponse resp;
    int result = ralph_api_handle(ctx, &req, &resp);
    ASSERT_EQ(result, 0);
    ASSERT_EQ(resp.status_code, 400);

    ralph_api_response_free(&resp);
    ralph_api_free(ctx);
}

/* Test: 400 for parse error */
TEST(parse_error) {
    RalphAPIContext *ctx = ralph_api_create();
    ASSERT(ctx != NULL);

    const char *json_body =
        "{"
        "\"format\":\"lp\","
        "\"problem\":\"this is not valid LP syntax\""
        "}";

    RalphAPIRequest req = {
        .method = "POST",
        .path = "/api/v1/solve",
        .query = NULL,
        .body = json_body,
        .body_len = strlen(json_body)
    };

    RalphAPIResponse resp;
    int result = ralph_api_handle(ctx, &req, &resp);
    ASSERT_EQ(result, 0);
    ASSERT_EQ(resp.status_code, 400);
    ASSERT(strstr((char*)resp.body, "error") != NULL);

    ralph_api_response_free(&resp);
    ralph_api_free(ctx);
}

/* Test: WASM helper functions */
TEST(wasm_helpers) {
    RalphAPIContext *ctx = ralph_api_create();
    ASSERT(ctx != NULL);

    RalphAPIRequest req = {
        .method = "GET",
        .path = "/api/v1/health",
        .query = NULL,
        .body = NULL,
        .body_len = 0
    };

    RalphAPIResponse resp;
    ralph_api_handle(ctx, &req, &resp);

    ASSERT_EQ(ralph_api_response_status(&resp), 200);
    ASSERT(strcmp(ralph_api_response_content_type(&resp), "application/json") == 0);
    ASSERT(ralph_api_response_body(&resp) != NULL);
    ASSERT(ralph_api_response_body_len(&resp) > 0);

    ralph_api_response_free(&resp);
    ralph_api_free(ctx);
}

/* Test: Version string */
TEST(version) {
    const char *version = ralph_api_version();
    ASSERT(version != NULL);
    ASSERT(strlen(version) > 0);
}

/* Test: Raw LP body with format=lp query param → SOL output */
TEST(raw_lp_sol_output) {
    RalphAPIContext *ctx = ralph_api_create();
    ASSERT(ctx != NULL);

    /* Raw LP problem (not wrapped in JSON) */
    const char *lp_body =
        "max: 5 x + 3 y\n"
        "subject to\n"
        "wood: 2 x + 4 y <= 40\n"
        "labor: 3 x + 2 y <= 24\n"
        "bounds\n"
        "x >= 0\n"
        "y >= 0\n"
        "end";

    RalphAPIRequest req = {
        .method = "POST",
        .path = "/api/v1/solve",
        .query = "format=lp",  /* Key: format in query string */
        .body = lp_body,
        .body_len = strlen(lp_body)
    };

    RalphAPIResponse resp;
    int result = ralph_api_handle(ctx, &req, &resp);
    ASSERT_EQ(result, 0);
    ASSERT_EQ(resp.status_code, 200);

    /* SOL format output (uses UPPERCASE from ralph_status_string) */
    ASSERT(strstr((char*)resp.body, "solution status: OPTIMAL") != NULL);
    ASSERT(strstr((char*)resp.body, "objective value:") != NULL);
    ASSERT(strstr((char*)resp.body, "x ") != NULL);
    ASSERT(strstr((char*)resp.body, "y ") != NULL);

    /* Content type should be text/plain for SOL output */
    ASSERT(strcmp(resp.content_type, "text/plain") == 0);

    ralph_api_response_free(&resp);
    ralph_api_free(ctx);
}

/* Test: JSON format param still works (backward compat) */
TEST(json_format_backward_compat) {
    RalphAPIContext *ctx = ralph_api_create();
    ASSERT(ctx != NULL);

    /* Same JSON as solve_simple_lp but with format=json query param */
    const char *json_body =
        "{"
        "\"format\":\"lp\","
        "\"problem\":\"max: 5 x + 3 y\\n"
        "subject to\\n"
        "wood: 2 x + 4 y <= 40\\n"
        "labor: 3 x + 2 y <= 24\\n"
        "bounds\\n"
        "x >= 0\\n"
        "y >= 0\\n"
        "end\""
        "}";

    /* Explicitly specify format=json in query */
    RalphAPIRequest req = {
        .method = "POST",
        .path = "/api/v1/solve",
        .query = "format=json",
        .body = json_body,
        .body_len = strlen(json_body)
    };

    RalphAPIResponse resp;
    int result = ralph_api_handle(ctx, &req, &resp);
    ASSERT_EQ(result, 0);
    ASSERT_EQ(resp.status_code, 200);

    /* JSON format output */
    ASSERT(strstr((char*)resp.body, "\"status\":\"optimal\"") != NULL);
    ASSERT(strcmp(resp.content_type, "application/json") == 0);

    ralph_api_response_free(&resp);
    ralph_api_free(ctx);
}

/* Test: Invalid format query param */
TEST(invalid_format_param) {
    RalphAPIContext *ctx = ralph_api_create();
    ASSERT(ctx != NULL);

    const char *body = "max: x\nsubject to\nc1: x <= 10\nend";

    RalphAPIRequest req = {
        .method = "POST",
        .path = "/api/v1/solve",
        .query = "format=invalid",
        .body = body,
        .body_len = strlen(body)
    };

    RalphAPIResponse resp;
    int result = ralph_api_handle(ctx, &req, &resp);
    ASSERT_EQ(result, 0);
    ASSERT_EQ(resp.status_code, 400);
    ASSERT(strstr((char*)resp.body, "Invalid format") != NULL);

    ralph_api_response_free(&resp);
    ralph_api_free(ctx);
}

/* Test: Timeout via query param */
TEST(timeout_query_param) {
    RalphAPIContext *ctx = ralph_api_create();
    ASSERT(ctx != NULL);

    /* Note: LP parser requires explicit coefficients (1 x not just x) */
    const char *lp_body =
        "max: 1 x\n"
        "subject to\n"
        "c1: 1 x <= 10\n"
        "end";

    RalphAPIRequest req = {
        .method = "POST",
        .path = "/api/v1/solve",
        .query = "format=lp&timeout_ms=1000",
        .body = lp_body,
        .body_len = strlen(lp_body)
    };

    RalphAPIResponse resp;
    int result = ralph_api_handle(ctx, &req, &resp);
    ASSERT_EQ(result, 0);
    ASSERT_EQ(resp.status_code, 200);
    ASSERT(strstr((char*)resp.body, "solution status: OPTIMAL") != NULL);

    ralph_api_response_free(&resp);
    ralph_api_free(ctx);
}

int main(void) {
    printf("Ralph API Tests\n");
    printf("===============\n\n");

    RUN_TEST(context_lifecycle);
    RUN_TEST(health_endpoint);
    RUN_TEST(formats_endpoint);
    RUN_TEST(solve_simple_lp);
    RUN_TEST(not_found);
    RUN_TEST(missing_body);
    RUN_TEST(parse_error);
    RUN_TEST(wasm_helpers);
    RUN_TEST(version);
    RUN_TEST(raw_lp_sol_output);
    RUN_TEST(json_format_backward_compat);
    RUN_TEST(invalid_format_param);
    RUN_TEST(timeout_query_param);

    printf("\n===============\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
