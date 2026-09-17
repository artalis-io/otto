/*
 * test_verify.c - Tests for nx_verify (Stage V faithfulness checks)
 *
 * Pure C, no external deps: crafts raw + canonical + schema JSON and asserts
 * on the NxVerifyReport. Replaces the python reconcile self-test in CI.
 */
#include "nx_verify.h"
#include "nx_issue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0, tests_passed = 0;
#define RUN_TEST(name) do { \
    printf("  %-55s ", #name); fflush(stdout); \
    test_##name(); tests_run++; tests_passed++; printf("[PASS]\n"); \
} while (0)
#define ASSERT(cond) do { \
    if (!(cond)) { printf("[FAIL]\n    %s\n    at %s:%d\n", #cond, __FILE__, __LINE__); exit(1); } \
} while (0)

/* Common raw: 3 real columns (id, w, name), 2 rows. */
static const char *RAW =
    "{\"nx_raw\":1,\"source\":{\"filename\":\"x.csv\",\"sha256\":\"ABC\",\"format\":\"csv\"},"
    "\"tables\":[{\"name\":\"S\",\"index\":0,\"headers\":[\"id\",\"w\",\"name\"],\"header_row\":0,"
    "\"rows\":[{\"row\":1,\"cells\":[\"1\",\"10.5\",\"Acme\"]},"
    "{\"row\":2,\"cells\":[\"2\",\"20\",\"Beta\"]}]}]}";

static const char *SCHEMA =
    "{\"nx_schema\":2,\"version\":\"t\",\"columns\":["
    "{\"source\":0,\"target\":\"id\",\"type\":\"string\",\"transforms\":[\"trim\"]},"
    "{\"source\":1,\"target\":\"w\",\"type\":\"double\"},"
    "{\"source\":2,\"target\":\"name\",\"type\":\"string\",\"transforms\":[\"trim\"]}]}";

static NxVerifyReport run(const char *raw, const char *canon, const char *schema)
{
    NxIssueList issues; nx_issue_list_init(&issues);
    NxVerifyOptions opt; nx_verify_options_default(&opt);
    NxVerifyReport rep;
    NxVerifyStatus st = nx_verify(raw, strlen(raw), canon, strlen(canon),
                                  schema, strlen(schema), &opt, &issues, &rep);
    ASSERT(st == NX_VERIFY_OK);
    nx_issue_list_free(&issues);
    return rep;
}

static void test_faithful_clean(void)
{
    const char *canon =
        "{\"nx_canonical\":1,\"source_sha256\":\"ABC\",\"records\":["
        "{\"id\":\"1\",\"w\":10.5,\"name\":\"Acme\"},"
        "{\"id\":\"2\",\"w\":20,\"name\":\"Beta\"}],\"record_count\":2,"
        "\"audit\":{\"rejections\":[]}}";
    NxVerifyReport r = run(RAW, canon, SCHEMA);
    ASSERT(nx_verify_clean(&r));
    ASSERT(r.rows_checked == 2);
    ASSERT(r.fields_lossy == 0 && r.fields_mismatch == 0);
    ASSERT(r.fields_verified == 6);   /* 3 fields x 2 rows */
}

static void test_lossy_numeric_truncation(void)
{
    /* w emitted as 10 but raw is 10.5 -> lossy (the int-truncation bug class) */
    const char *canon =
        "{\"nx_canonical\":1,\"source_sha256\":\"ABC\",\"records\":["
        "{\"id\":\"1\",\"w\":10,\"name\":\"Acme\"},"
        "{\"id\":\"2\",\"w\":20,\"name\":\"Beta\"}],\"record_count\":2,"
        "\"audit\":{\"rejections\":[]}}";
    NxVerifyReport r = run(RAW, canon, SCHEMA);
    ASSERT(!nx_verify_clean(&r));
    ASSERT(r.fields_lossy == 1);
}

static void test_precision_clamp_detected(void)
{
    /* raw 8769.627, emitted 8769.63 (6 sig figs) -> lossy at 1e-9 tol */
    const char *raw =
        "{\"nx_raw\":1,\"source\":{\"sha256\":\"ABC\"},"
        "\"tables\":[{\"index\":0,\"headers\":[\"w\"],"
        "\"rows\":[{\"row\":1,\"cells\":[\"8769.627\"]}]}]}";
    const char *schema =
        "{\"nx_schema\":2,\"columns\":[{\"source\":0,\"target\":\"w\",\"type\":\"double\"}]}";
    const char *canon =
        "{\"nx_canonical\":1,\"source_sha256\":\"ABC\","
        "\"records\":[{\"w\":8769.63}],\"record_count\":1,\"audit\":{\"rejections\":[]}}";
    NxVerifyReport r = run(raw, canon, schema);
    ASSERT(r.fields_lossy == 1 && !nx_verify_clean(&r));
}

static void test_string_mismatch(void)
{
    const char *canon =
        "{\"nx_canonical\":1,\"source_sha256\":\"ABC\",\"records\":["
        "{\"id\":\"1\",\"w\":10.5,\"name\":\"WRONG\"},"
        "{\"id\":\"2\",\"w\":20,\"name\":\"Beta\"}],\"record_count\":2,"
        "\"audit\":{\"rejections\":[]}}";
    NxVerifyReport r = run(RAW, canon, SCHEMA);
    ASSERT(r.fields_mismatch == 1 && !nx_verify_clean(&r));
}

static void test_provenance_mismatch(void)
{
    const char *canon =
        "{\"nx_canonical\":1,\"source_sha256\":\"XYZ\",\"records\":["
        "{\"id\":\"1\",\"w\":10.5,\"name\":\"Acme\"},"
        "{\"id\":\"2\",\"w\":20,\"name\":\"Beta\"}],\"record_count\":2,"
        "\"audit\":{\"rejections\":[]}}";
    NxVerifyReport r = run(RAW, canon, SCHEMA);
    ASSERT(r.provenance_ok == 0 && !nx_verify_clean(&r));
}

static void test_derived_field_unverified_but_clean(void)
{
    /* extra column sourced from a virtual index (5 >= 3 real cols) */
    const char *schema =
        "{\"nx_schema\":2,\"columns\":["
        "{\"source\":0,\"target\":\"id\",\"type\":\"string\",\"transforms\":[\"trim\"]},"
        "{\"source\":5,\"target\":\"flag\",\"type\":\"bool\"}]}";
    const char *canon =
        "{\"nx_canonical\":1,\"source_sha256\":\"ABC\",\"records\":["
        "{\"id\":\"1\",\"flag\":true},{\"id\":\"2\",\"flag\":false}],"
        "\"record_count\":2,\"audit\":{\"rejections\":[]}}";
    NxVerifyReport r = run(RAW, canon, schema);
    ASSERT(r.fields_unverified == 1);   /* flag: derived, not checked */
    ASSERT(nx_verify_clean(&r));         /* but nothing failed */
}

static void test_rejected_rows_skipped(void)
{
    /* row 2 was rejected at Stage B; only row 1 is a record -> conservation clean */
    const char *canon =
        "{\"nx_canonical\":1,\"source_sha256\":\"ABC\",\"records\":["
        "{\"id\":\"1\",\"w\":10.5,\"name\":\"Acme\"}],\"record_count\":1,"
        "\"audit\":{\"rejections\":[{\"row\":2,\"field\":\"w\",\"reason\":\"x\"}]}}";
    NxVerifyReport r = run(RAW, canon, SCHEMA);
    ASSERT(r.rows_checked == 1 && r.rows_unmatched == 0 && nx_verify_clean(&r));
}

static void test_status_strings(void)
{
    ASSERT(strcmp(nx_verify_status_str(NX_VERIFY_OK), "ok") == 0);
    ASSERT(nx_verify_status_str(NX_VERIFY_ERR_PARSE) != NULL);
}

int main(void)
{
    printf("nx_verify tests:\n");
    RUN_TEST(faithful_clean);
    RUN_TEST(lossy_numeric_truncation);
    RUN_TEST(precision_clamp_detected);
    RUN_TEST(string_mismatch);
    RUN_TEST(provenance_mismatch);
    RUN_TEST(derived_field_unverified_but_clean);
    RUN_TEST(rejected_rows_skipped);
    RUN_TEST(status_strings);
    printf("\n  %d/%d tests passed\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
