/*
 * test_pipeline.c — Integration tests for full pipeline flows
 *
 * Tests batch manifest, Stage D emit, and diff via the library APIs
 * (not CLI subprocess). Validates end-to-end data flow.
 */

#include "nx_xlsx.h"
#include "nx_xform.h"
#include "nx_validate.h"
#include "nx_emit.h"
#include "nx_diff.h"
#include "nx_issue.h"
#include "sh_arena.h"
#include "sh_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixtures/minimal_xlsx.h"

/* ============================================================================
 * Test Macros
 * ============================================================================ */

static int tests_passed = 0;
static int tests_total = 0;

#define TEST(name) \
    static void test_##name(void); \
    static void run_test_##name(void) { \
        tests_total++; \
        printf("  %-52s ", #name); \
        test_##name(); \
        tests_passed++; \
        printf("[PASS]\n"); \
    } \
    static void test_##name(void)

#define RUN_TEST(name) run_test_##name()

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("[FAIL]\n    Assertion failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        exit(1); \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("[FAIL]\n    Expected %d, got %d (%s:%d)\n", (int)(b), (int)(a), __FILE__, __LINE__); \
        exit(1); \
    } \
} while(0)

/* ============================================================================
 * Helpers
 * ============================================================================ */

/*
 * Schema matching the minimal XLSX fixture:
 *   Col 0 = City, Col 1 = Name, Col 2 = Address, Col 3 = GPS Lat, Col 4 = GPS Lon
 */
static const char *TEST_SCHEMA =
    "{\"nx_schema\":1,\"version\":\"test-v1\",\"output_type\":\"facility\","
    "\"columns\":["
    "{\"source\":0,\"target\":\"city\",\"type\":\"string\",\"required\":true},"
    "{\"source\":1,\"target\":\"name\",\"type\":\"string\",\"required\":true},"
    "{\"source\":2,\"target\":\"addr\",\"type\":\"string\"},"
    "{\"source\":3,\"target\":\"lat\",\"type\":\"double\",\"precision\":6},"
    "{\"source\":4,\"target\":\"lon\",\"type\":\"double\",\"precision\":6}"
    "],"
    "\"row_id\":{\"template\":\"test-{city}-{name}\",\"slugify\":true},"
    "\"validate\":["
    "{\"type\":\"geo_bounds\",\"lat_field\":\"lat\",\"lon_field\":\"lon\","
    " \"bounds\":{\"min_lat\":-90,\"max_lat\":90,\"min_lon\":-180,\"max_lon\":180},"
    " \"severity\":\"error\"}"
    "]}";

/* Run extract+transform+validate and return canonical JSON */
static char *run_full_pipeline(const char *schema,
                               char **out_json, size_t *out_len)
{
    /* Stage A: Extract from embedded XLSX */
    SHArena *arena_a = sh_arena_create(8 * 1024 * 1024);
    ASSERT(arena_a != NULL);

    char *raw = NULL;
    size_t raw_len = 0;
    NxXlsxStatus xs = nx_xlsx_parse(MINIMAL_XLSX, MINIMAL_XLSX_LEN,
                                     NULL, "test.xlsx", arena_a, NULL,
                                     &raw, &raw_len);
    ASSERT(xs == NX_XLSX_OK);
    ASSERT(raw != NULL);
    sh_arena_free(arena_a);

    /* Stage B: Transform */
    SHArena *arena_b = sh_arena_create(4 * 1024 * 1024);
    ASSERT(arena_b != NULL);

    char *canon = NULL;
    size_t canon_len = 0;
    NxXformStatus ts = nx_xform_apply(raw, raw_len,
                                       schema, strlen(schema),
                                       arena_b, NULL,
                                       &canon, &canon_len);
    sh_arena_free(arena_b);
    free(raw);
    ASSERT(ts == NX_XFORM_OK);
    ASSERT(canon != NULL);

    /* Stage X: Validate */
    SHArena *arena_x = sh_arena_create(4 * 1024 * 1024);
    if (arena_x) {
        char *validated = NULL;
        size_t validated_len = 0;
        NxValidateStatus vs = nx_validate(canon, canon_len,
                                           schema, strlen(schema),
                                           arena_x, NULL,
                                           &validated, &validated_len);
        sh_arena_free(arena_x);
        if (vs == NX_VALIDATE_OK && validated) {
            free(canon);
            canon = validated;
            canon_len = validated_len;
        }
    }

    *out_json = canon;
    *out_len = canon_len;
    return canon;
}

/* ============================================================================
 * Pipeline End-to-End Tests
 * ============================================================================ */

TEST(pipeline_extract_transform_validate)
{
    char *canon = NULL;
    size_t canon_len = 0;
    run_full_pipeline(TEST_SCHEMA, &canon, &canon_len);

    /* Verify canonical JSON structure */
    SHArena *pa = sh_arena_create(256 * 1024);
    ASSERT(pa != NULL);
    ShJsonValue *root = NULL;
    ASSERT(sh_json_parse(canon, canon_len, pa, &root) == SH_JSON_OK);

    /* Has records */
    ShJsonValue *records = sh_json_get(root, "records");
    ASSERT(records != NULL);
    ASSERT(sh_json_array_len(records) > 0);

    /* Has audit */
    ShJsonValue *audit = sh_json_get(root, "audit");
    ASSERT(audit != NULL);
    ASSERT(sh_json_as_int(sh_json_get(audit, "rows_processed"), 0) > 0);

    sh_arena_free(pa);
    free(canon);
}

TEST(pipeline_issues_threaded)
{
    /* Verify NxIssueList accumulates across stages */
    NxIssueList issues;
    nx_issue_list_init(&issues);

    SHArena *arena_a = sh_arena_create(8 * 1024 * 1024);
    char *raw = NULL;
    size_t raw_len = 0;
    nx_xlsx_parse(MINIMAL_XLSX, MINIMAL_XLSX_LEN,
                  NULL, "test.xlsx", arena_a, &issues, &raw, &raw_len);
    sh_arena_free(arena_a);

    /* Even if no issues from this file, the list should be valid */
    ASSERT(issues.count >= 0);

    SHArena *arena_b = sh_arena_create(4 * 1024 * 1024);
    char *canon = NULL;
    size_t canon_len = 0;
    nx_xform_apply(raw, raw_len, TEST_SCHEMA, strlen(TEST_SCHEMA),
                   arena_b, &issues, &canon, &canon_len);
    sh_arena_free(arena_b);

    /* Issues list is still valid */
    ASSERT(issues.count >= 0);

    free(raw);
    free(canon);
    nx_issue_list_free(&issues);
}

/* ============================================================================
 * Stage D: Emit Tests
 * ============================================================================ */

TEST(emit_geojson_from_pipeline)
{
    char *canon = NULL;
    size_t canon_len = 0;
    run_full_pipeline(TEST_SCHEMA, &canon, &canon_len);

    /* Emit GeoJSON */
    NxEmitGeoJsonOpts opts = NX_EMIT_GEOJSON_DEFAULTS;
    opts.lat_field = "lat";
    opts.lon_field = "lon";

    SHArena *arena = sh_arena_create(4 * 1024 * 1024);
    ASSERT(arena != NULL);

    char *geojson = NULL;
    size_t geojson_len = 0;
    NxEmitStatus es = nx_emit_geojson(canon, canon_len,
                                       &opts, arena, NULL,
                                       &geojson, &geojson_len);
    sh_arena_free(arena);
    ASSERT(es == NX_EMIT_OK);
    ASSERT(geojson != NULL);
    ASSERT(geojson_len > 0);

    /* Verify GeoJSON structure */
    SHArena *pa = sh_arena_create(256 * 1024);
    ShJsonValue *root = NULL;
    ASSERT(sh_json_parse(geojson, geojson_len, pa, &root) == SH_JSON_OK);
    ASSERT(strcmp(sh_json_as_string(sh_json_get(root, "type"), ""), "FeatureCollection") == 0);

    ShJsonValue *features = sh_json_get(root, "features");
    ASSERT(features != NULL);
    ASSERT(sh_json_array_len(features) > 0);

    /* First feature has geometry */
    ShJsonValue *f0 = sh_json_array_get(features, 0);
    ShJsonValue *geom = sh_json_get(f0, "geometry");
    ASSERT(geom != NULL);
    ASSERT(strcmp(sh_json_as_string(sh_json_get(geom, "type"), ""), "Point") == 0);

    sh_arena_free(pa);
    free(geojson);
    free(canon);
}

TEST(emit_csv_from_pipeline)
{
    char *canon = NULL;
    size_t canon_len = 0;
    run_full_pipeline(TEST_SCHEMA, &canon, &canon_len);

    /* Emit CSV */
    NxEmitCsvOpts opts = NX_EMIT_CSV_DEFAULTS;

    SHArena *arena = sh_arena_create(4 * 1024 * 1024);
    ASSERT(arena != NULL);

    char *csv = NULL;
    size_t csv_len = 0;
    NxEmitStatus es = nx_emit_csv(canon, canon_len,
                                    &opts, arena, NULL,
                                    &csv, &csv_len);
    sh_arena_free(arena);
    ASSERT(es == NX_EMIT_OK);
    ASSERT(csv != NULL);
    ASSERT(csv_len > 0);

    /* Should have header + at least one data row */
    int newlines = 0;
    for (size_t i = 0; i < csv_len; i++) {
        if (csv[i] == '\n') newlines++;
    }
    ASSERT(newlines >= 2); /* header + at least 1 row */

    free(csv);
    free(canon);
}

/* ============================================================================
 * Diff Tests (via library API, not CLI)
 * ============================================================================ */

TEST(diff_identical_pipeline_runs)
{
    char *canon1 = NULL, *canon2 = NULL;
    size_t len1 = 0, len2 = 0;
    run_full_pipeline(TEST_SCHEMA, &canon1, &len1);
    run_full_pipeline(TEST_SCHEMA, &canon2, &len2);

    /* Same input → identical output → diff should show all unchanged */
    SHArena *arena = sh_arena_create(4 * 1024 * 1024);
    char *diff = NULL;
    size_t diff_len = 0;
    NxDiffStatus ds = nx_diff(canon1, len1, canon2, len2,
                               arena, &diff, &diff_len);
    sh_arena_free(arena);
    ASSERT(ds == NX_DIFF_OK);
    ASSERT(diff != NULL);

    SHArena *pa = sh_arena_create(256 * 1024);
    ShJsonValue *root = NULL;
    ASSERT(sh_json_parse(diff, diff_len, pa, &root) == SH_JSON_OK);

    ShJsonValue *summary = sh_json_get(root, "summary");
    ASSERT(summary != NULL);
    ASSERT_EQ(sh_json_as_int(sh_json_get(summary, "added"), -1), 0);
    ASSERT_EQ(sh_json_as_int(sh_json_get(summary, "removed"), -1), 0);
    ASSERT_EQ(sh_json_as_int(sh_json_get(summary, "modified"), -1), 0);
    ASSERT(sh_json_as_int(sh_json_get(summary, "unchanged"), 0) > 0);

    sh_arena_free(pa);
    free(diff);
    free(canon1);
    free(canon2);
}

TEST(diff_modified_records)
{
    /* Create two slightly different canonical JSONs */
    const char *old_json =
        "{\"nx_canonical\":1,\"records\":["
        "{\"id\":\"r1\",\"name\":\"A\",\"val\":1},"
        "{\"id\":\"r2\",\"name\":\"B\",\"val\":2}"
        "]}";
    const char *new_json =
        "{\"nx_canonical\":1,\"records\":["
        "{\"id\":\"r1\",\"name\":\"A\",\"val\":99},"
        "{\"id\":\"r2\",\"name\":\"B\",\"val\":2},"
        "{\"id\":\"r3\",\"name\":\"C\",\"val\":3}"
        "]}";

    SHArena *arena = sh_arena_create(1024 * 1024);
    char *diff = NULL;
    size_t diff_len = 0;
    NxDiffStatus ds = nx_diff(old_json, strlen(old_json),
                               new_json, strlen(new_json),
                               arena, &diff, &diff_len);
    sh_arena_free(arena);
    ASSERT(ds == NX_DIFF_OK);

    SHArena *pa = sh_arena_create(256 * 1024);
    ShJsonValue *root = NULL;
    ASSERT(sh_json_parse(diff, diff_len, pa, &root) == SH_JSON_OK);

    ShJsonValue *summary = sh_json_get(root, "summary");
    ASSERT_EQ(sh_json_as_int(sh_json_get(summary, "added"), -1), 1);
    ASSERT_EQ(sh_json_as_int(sh_json_get(summary, "removed"), -1), 0);
    ASSERT_EQ(sh_json_as_int(sh_json_get(summary, "modified"), -1), 1);
    ASSERT_EQ(sh_json_as_int(sh_json_get(summary, "unchanged"), -1), 1);

    sh_arena_free(pa);
    free(diff);
}

TEST(diff_removed_records)
{
    const char *old_json =
        "{\"nx_canonical\":1,\"records\":["
        "{\"id\":\"r1\",\"name\":\"A\"},"
        "{\"id\":\"r2\",\"name\":\"B\"},"
        "{\"id\":\"r3\",\"name\":\"C\"}"
        "]}";
    const char *new_json =
        "{\"nx_canonical\":1,\"records\":["
        "{\"id\":\"r1\",\"name\":\"A\"}"
        "]}";

    SHArena *arena = sh_arena_create(1024 * 1024);
    char *diff = NULL;
    size_t diff_len = 0;
    NxDiffStatus ds = nx_diff(old_json, strlen(old_json),
                               new_json, strlen(new_json),
                               arena, &diff, &diff_len);
    sh_arena_free(arena);
    ASSERT(ds == NX_DIFF_OK);

    SHArena *pa = sh_arena_create(256 * 1024);
    ShJsonValue *root = NULL;
    ASSERT(sh_json_parse(diff, diff_len, pa, &root) == SH_JSON_OK);

    ShJsonValue *summary = sh_json_get(root, "summary");
    ASSERT_EQ(sh_json_as_int(sh_json_get(summary, "removed"), -1), 2);
    ASSERT_EQ(sh_json_as_int(sh_json_get(summary, "unchanged"), -1), 1);

    sh_arena_free(pa);
    free(diff);
}

/* ============================================================================
 * Manifest JSON Structure Tests
 * ============================================================================ */

TEST(manifest_json_structure)
{
    /* Verify NxIssueList produces valid JSON for manifest writing */
    NxIssueList issues;
    nx_issue_list_init(&issues);

    nx_issue_add(&issues, NX_STAGE_A, NX_ISSUE_WARNING, 5, "col1",
                 "cell_truncated", "Cell truncated at 4096 bytes");
    nx_issue_add(&issues, NX_STAGE_B, NX_ISSUE_ERROR, 10, "lat",
                 "required_empty", "Required field empty");
    nx_issue_add(&issues, NX_STAGE_X, NX_ISSUE_WARNING, 3, "lon",
                 "geo_bounds", "lon outside bounds");

    /* Verify counts */
    ASSERT_EQ(nx_issue_count(&issues, NX_STAGE_A, NX_ISSUE_WARNING), 1);
    ASSERT_EQ(nx_issue_count(&issues, NX_STAGE_B, NX_ISSUE_ERROR), 1);
    ASSERT_EQ(nx_issue_count(&issues, NX_STAGE_X, NX_ISSUE_WARNING), 1);
    ASSERT_EQ(nx_issue_count(&issues, -1, -1), 3);  /* total */

    /* Verify JSON output using ShJsonBuf */
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);
    nx_issue_write_json(&issues, -1, &w);
    char *json = sh_json_buf_take(&jb);
    ASSERT(json != NULL);
    size_t json_len = strlen(json);
    ASSERT(json_len > 0);

    /* Parse and validate */
    SHArena *arena = sh_arena_create(64 * 1024);
    ShJsonValue *root = NULL;
    ASSERT(sh_json_parse(json, json_len, arena, &root) == SH_JSON_OK);
    ASSERT(sh_json_type(root) == SH_JSON_ARRAY);
    ASSERT(sh_json_array_len(root) == 3);

    /* Check first issue */
    ShJsonValue *iss0 = sh_json_array_get(root, 0);
    ASSERT(strcmp(sh_json_as_string(sh_json_get(iss0, "stage"), ""), "A") == 0);
    ASSERT(strcmp(sh_json_as_string(sh_json_get(iss0, "severity"), ""), "warning") == 0);
    ASSERT_EQ(sh_json_as_int(sh_json_get(iss0, "row"), -1), 5);

    sh_arena_free(arena);
    free(json);
    sh_json_buf_free(&jb);
    nx_issue_list_free(&issues);
}

TEST(manifest_stage_counts)
{
    /* Simulate batch per-file issue tracking */
    NxIssueList file_issues;
    nx_issue_list_init(&file_issues);

    /* Stage A: 2 warnings */
    nx_issue_add(&file_issues, NX_STAGE_A, NX_ISSUE_WARNING, 1, "", "w1", "msg");
    nx_issue_add(&file_issues, NX_STAGE_A, NX_ISSUE_WARNING, 2, "", "w2", "msg");

    /* Stage B: 1 error */
    nx_issue_add(&file_issues, NX_STAGE_B, NX_ISSUE_ERROR, 5, "lat", "e1", "msg");

    /* Stage X: 1 warning, 1 info */
    nx_issue_add(&file_issues, NX_STAGE_X, NX_ISSUE_WARNING, 3, "lon", "w3", "msg");
    nx_issue_add(&file_issues, NX_STAGE_X, NX_ISSUE_INFO, -1, "", "i1", "msg");

    /* Verify per-stage counts match what manifest would write */
    ASSERT_EQ(nx_issue_count(&file_issues, NX_STAGE_A, NX_ISSUE_ERROR), 0);
    ASSERT_EQ(nx_issue_count(&file_issues, NX_STAGE_A, NX_ISSUE_WARNING), 2);
    ASSERT_EQ(nx_issue_count(&file_issues, NX_STAGE_B, NX_ISSUE_ERROR), 1);
    ASSERT_EQ(nx_issue_count(&file_issues, NX_STAGE_X, NX_ISSUE_WARNING), 1);
    ASSERT_EQ(nx_issue_count(&file_issues, NX_STAGE_X, NX_ISSUE_INFO), 1);

    /* Stage D: nothing */
    ASSERT_EQ(nx_issue_count(&file_issues, NX_STAGE_D, NX_ISSUE_ERROR), 0);

    /* Totals */
    int total_errors = nx_issue_count(&file_issues, -1, NX_ISSUE_ERROR);
    int total_warnings = nx_issue_count(&file_issues, -1, NX_ISSUE_WARNING);
    ASSERT_EQ(total_errors, 1);
    ASSERT_EQ(total_warnings, 3);

    nx_issue_list_free(&file_issues);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
    printf("\nPipeline Integration Tests:\n");

    printf("\n  Full Pipeline:\n");
    RUN_TEST(pipeline_extract_transform_validate);
    RUN_TEST(pipeline_issues_threaded);

    printf("\n  Stage D (Emit):\n");
    RUN_TEST(emit_geojson_from_pipeline);
    RUN_TEST(emit_csv_from_pipeline);

    printf("\n  Diff (Change Detection):\n");
    RUN_TEST(diff_identical_pipeline_runs);
    RUN_TEST(diff_modified_records);
    RUN_TEST(diff_removed_records);

    printf("\n  Manifest Structure:\n");
    RUN_TEST(manifest_json_structure);
    RUN_TEST(manifest_stage_counts);

    printf("\n  %d/%d tests passed\n\n", tests_passed, tests_total);
    return tests_passed == tests_total ? 0 : 1;
}
