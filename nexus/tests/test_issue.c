/*
 * test_issue.c - Tests for NxIssueList dynamic issue tracker
 */

#include "nx_issue.h"
#include "sh_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Test Framework
 * ============================================================================ */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  %-55s ", #name); \
    fflush(stdout); \
    test_##name(); \
    tests_run++; \
    tests_passed++; \
    printf("[PASS]\n"); \
} while (0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("[FAIL]\n    Assertion failed: %s\n    at %s:%d\n", #cond, __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_STREQ(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (strcmp(_a, _b) != 0) { \
        printf("[FAIL]\n    Expected: \"%s\"\n    Got:      \"%s\"\n    at %s:%d\n", \
               _b, _a, __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

/* ============================================================================
 * Tests
 * ============================================================================ */

TEST(init_free)
{
    NxIssueList list;
    nx_issue_list_init(&list);
    ASSERT(list.items == NULL);
    ASSERT_EQ(list.count, 0);
    ASSERT_EQ(list.capacity, 0);
    nx_issue_list_free(&list);
    ASSERT(list.items == NULL);
    ASSERT_EQ(list.count, 0);
}

TEST(add_single)
{
    NxIssueList list;
    nx_issue_list_init(&list);

    int rc = nx_issue_add(&list, NX_STAGE_B, NX_ISSUE_ERROR,
                          5, "lat", "required_field_empty",
                          "required field empty");
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(list.count, 1);
    ASSERT(list.capacity >= 1);

    ASSERT_EQ(list.items[0].stage, NX_STAGE_B);
    ASSERT_EQ(list.items[0].severity, NX_ISSUE_ERROR);
    ASSERT_EQ(list.items[0].row, 5);
    ASSERT_STREQ(list.items[0].field, "lat");
    ASSERT_STREQ(list.items[0].code, "required_field_empty");
    ASSERT_STREQ(list.items[0].message, "required field empty");

    nx_issue_list_free(&list);
}

TEST(add_formatted)
{
    NxIssueList list;
    nx_issue_list_init(&list);

    int rc = nx_issue_addf(&list, NX_STAGE_X, NX_ISSUE_WARNING,
                           8, "lat", "geo_bounds",
                           "lat %.1f outside bounds [%.1f, %.1f]",
                           44.5, 45.7, 48.6);
    ASSERT_EQ(rc, 0);
    ASSERT_STREQ(list.items[0].message,
                 "lat 44.5 outside bounds [45.7, 48.6]");

    nx_issue_list_free(&list);
}

TEST(null_fields)
{
    NxIssueList list;
    nx_issue_list_init(&list);

    int rc = nx_issue_add(&list, NX_STAGE_A, NX_ISSUE_INFO,
                          -1, NULL, "rows_limit",
                          "Hit MAX_ROWS cap");
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(list.items[0].row, -1);
    ASSERT_STREQ(list.items[0].field, "");
    ASSERT_STREQ(list.items[0].code, "rows_limit");

    nx_issue_list_free(&list);
}

TEST(null_list_safe)
{
    /* Should not crash */
    ASSERT_EQ(nx_issue_add(NULL, NX_STAGE_A, NX_ISSUE_INFO, 0, "", "", ""), -1);
    ASSERT_EQ(nx_issue_addf(NULL, NX_STAGE_A, NX_ISSUE_INFO, 0, "", "", "x"), -1);
    ASSERT_EQ(nx_issue_count(NULL, -1, -1), 0);
    nx_issue_write_json(NULL, -1, NULL);
}

TEST(dynamic_growth)
{
    NxIssueList list;
    nx_issue_list_init(&list);

    /* Add 2048 issues — well beyond old MAX_REJECTIONS=1024 */
    for (int i = 0; i < 2048; i++) {
        int rc = nx_issue_addf(&list, NX_STAGE_B, NX_ISSUE_ERROR,
                               i, "field", "rejection",
                               "Row %d rejected", i);
        ASSERT_EQ(rc, 0);
    }

    ASSERT_EQ(list.count, 2048);
    ASSERT(list.capacity >= 2048);

    /* Verify first and last */
    ASSERT_EQ(list.items[0].row, 0);
    ASSERT_EQ(list.items[2047].row, 2047);
    ASSERT_STREQ(list.items[2047].message, "Row 2047 rejected");

    nx_issue_list_free(&list);
}

TEST(count_by_severity)
{
    NxIssueList list;
    nx_issue_list_init(&list);

    nx_issue_add(&list, NX_STAGE_B, NX_ISSUE_ERROR, 1, "", "e1", "err1");
    nx_issue_add(&list, NX_STAGE_B, NX_ISSUE_ERROR, 2, "", "e2", "err2");
    nx_issue_add(&list, NX_STAGE_B, NX_ISSUE_WARNING, 3, "", "w1", "warn1");
    nx_issue_add(&list, NX_STAGE_X, NX_ISSUE_ERROR, 4, "", "e3", "err3");
    nx_issue_add(&list, NX_STAGE_A, NX_ISSUE_INFO, -1, "", "i1", "info1");

    /* Count all */
    ASSERT_EQ(nx_issue_count(&list, -1, -1), 5);

    /* Count by severity */
    ASSERT_EQ(nx_issue_count(&list, -1, NX_ISSUE_ERROR), 3);
    ASSERT_EQ(nx_issue_count(&list, -1, NX_ISSUE_WARNING), 1);
    ASSERT_EQ(nx_issue_count(&list, -1, NX_ISSUE_INFO), 1);

    /* Count by stage */
    ASSERT_EQ(nx_issue_count(&list, NX_STAGE_B, -1), 3);
    ASSERT_EQ(nx_issue_count(&list, NX_STAGE_X, -1), 1);
    ASSERT_EQ(nx_issue_count(&list, NX_STAGE_A, -1), 1);
    ASSERT_EQ(nx_issue_count(&list, NX_STAGE_D, -1), 0);

    /* Count by stage+severity */
    ASSERT_EQ(nx_issue_count(&list, NX_STAGE_B, NX_ISSUE_ERROR), 2);
    ASSERT_EQ(nx_issue_count(&list, NX_STAGE_B, NX_ISSUE_WARNING), 1);

    nx_issue_list_free(&list);
}

TEST(stage_strings)
{
    ASSERT_STREQ(nx_stage_str(NX_STAGE_A), "A");
    ASSERT_STREQ(nx_stage_str(NX_STAGE_M), "M");
    ASSERT_STREQ(nx_stage_str(NX_STAGE_B), "B");
    ASSERT_STREQ(nx_stage_str(NX_STAGE_X), "X");
    ASSERT_STREQ(nx_stage_str(NX_STAGE_D), "D");

    ASSERT_EQ(nx_stage_tag(NX_STAGE_A), 'A');
    ASSERT_EQ(nx_stage_tag(NX_STAGE_B), 'B');
}

TEST(severity_strings)
{
    ASSERT_STREQ(nx_issue_severity_str(NX_ISSUE_INFO), "info");
    ASSERT_STREQ(nx_issue_severity_str(NX_ISSUE_WARNING), "warning");
    ASSERT_STREQ(nx_issue_severity_str(NX_ISSUE_ERROR), "error");
}

TEST(write_json)
{
    NxIssueList list;
    nx_issue_list_init(&list);

    nx_issue_add(&list, NX_STAGE_B, NX_ISSUE_ERROR, 5, "lat",
                 "required_field_empty", "required field empty");
    nx_issue_add(&list, NX_STAGE_X, NX_ISSUE_WARNING, 8, "lon",
                 "geo_bounds", "lon out of bounds");

    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    /* Write all issues */
    nx_issue_write_json(&list, -1, &w);

    char *json = sh_json_buf_take(&jb);
    ASSERT(json != NULL);

    /* Should contain both issues */
    ASSERT(strstr(json, "\"stage\":\"B\"") != NULL);
    ASSERT(strstr(json, "\"stage\":\"X\"") != NULL);
    ASSERT(strstr(json, "\"required_field_empty\"") != NULL);
    ASSERT(strstr(json, "\"geo_bounds\"") != NULL);
    ASSERT(strstr(json, "\"row\":5") != NULL);
    ASSERT(strstr(json, "\"row\":8") != NULL);

    free(json);
    nx_issue_list_free(&list);
}

TEST(write_json_filtered)
{
    NxIssueList list;
    nx_issue_list_init(&list);

    nx_issue_add(&list, NX_STAGE_B, NX_ISSUE_ERROR, 1, "", "e1", "stage B");
    nx_issue_add(&list, NX_STAGE_X, NX_ISSUE_WARNING, 2, "", "w1", "stage X");
    nx_issue_add(&list, NX_STAGE_B, NX_ISSUE_WARNING, 3, "", "w2", "stage B again");

    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    /* Write only stage B */
    nx_issue_write_json(&list, NX_STAGE_B, &w);

    char *json = sh_json_buf_take(&jb);
    ASSERT(json != NULL);

    /* Should contain stage B issues only */
    ASSERT(strstr(json, "\"stage\":\"B\"") != NULL);
    ASSERT(strstr(json, "\"stage\":\"X\"") == NULL);
    ASSERT(strstr(json, "\"row\":1") != NULL);
    ASSERT(strstr(json, "\"row\":3") != NULL);
    ASSERT(strstr(json, "\"row\":2") == NULL);

    free(json);
    nx_issue_list_free(&list);
}

TEST(message_truncation)
{
    NxIssueList list;
    nx_issue_list_init(&list);

    /* Build a message longer than 256 bytes */
    char long_msg[512];
    memset(long_msg, 'A', sizeof(long_msg) - 1);
    long_msg[sizeof(long_msg) - 1] = '\0';

    int rc = nx_issue_add(&list, NX_STAGE_A, NX_ISSUE_WARNING,
                          1, "", "test", long_msg);
    ASSERT_EQ(rc, 0);
    ASSERT(strlen(list.items[0].message) < 256);

    nx_issue_list_free(&list);
}

TEST(double_free_safe)
{
    NxIssueList list;
    nx_issue_list_init(&list);
    nx_issue_add(&list, NX_STAGE_A, NX_ISSUE_INFO, 0, "", "x", "y");
    nx_issue_list_free(&list);
    /* Double free should be safe */
    nx_issue_list_free(&list);
    ASSERT(list.items == NULL);
    ASSERT_EQ(list.count, 0);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
    printf("nx_issue tests:\n");

    RUN_TEST(init_free);
    RUN_TEST(add_single);
    RUN_TEST(add_formatted);
    RUN_TEST(null_fields);
    RUN_TEST(null_list_safe);
    RUN_TEST(dynamic_growth);
    RUN_TEST(count_by_severity);
    RUN_TEST(stage_strings);
    RUN_TEST(severity_strings);
    RUN_TEST(write_json);
    RUN_TEST(write_json_filtered);
    RUN_TEST(message_truncation);
    RUN_TEST(double_free_safe);

    printf("\n  %d/%d tests passed\n\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
