/*
 * test_diff.c - Tests for nx_diff change detection
 */

#include "nx_diff.h"
#include "sh_arena.h"
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
 * Helpers
 * ============================================================================ */

/* Helper: parse diff JSON and extract summary counts */
static void parse_summary(const char *json, size_t len,
                          int *added, int *removed, int *modified, int *unchanged)
{
    SHArena *arena = sh_arena_create(64 * 1024);
    ASSERT(arena != NULL);
    ShJsonValue *root = NULL;
    ASSERT(sh_json_parse(json, len, arena, &root) == SH_JSON_OK);

    ShJsonValue *summary = sh_json_get(root, "summary");
    ASSERT(summary != NULL);

    *added = (int)sh_json_as_double(sh_json_get(summary, "added"), -1);
    *removed = (int)sh_json_as_double(sh_json_get(summary, "removed"), -1);
    *modified = (int)sh_json_as_double(sh_json_get(summary, "modified"), -1);
    *unchanged = (int)sh_json_as_double(sh_json_get(summary, "unchanged"), -1);

    sh_arena_free(arena);
}

/* Helper: parse diff JSON and get array length for a key */
static int get_array_len(const char *json, size_t len, const char *key)
{
    SHArena *arena = sh_arena_create(64 * 1024);
    ASSERT(arena != NULL);
    ShJsonValue *root = NULL;
    ASSERT(sh_json_parse(json, len, arena, &root) == SH_JSON_OK);

    ShJsonValue *arr = sh_json_get(root, key);
    ASSERT(arr != NULL);
    int count = (int)sh_json_array_len(arr);

    sh_arena_free(arena);
    return count;
}

/* ============================================================================
 * Tests
 * ============================================================================ */

TEST(null_input)
{
    char *out = NULL;
    size_t out_len = 0;
    SHArena *arena = sh_arena_create(64 * 1024);
    ASSERT(arena != NULL);

    ASSERT_EQ(nx_diff(NULL, 0, "{}", 2, arena, &out, &out_len), NX_DIFF_ERR_NULL);
    ASSERT_EQ(nx_diff("{}", 2, NULL, 0, arena, &out, &out_len), NX_DIFF_ERR_NULL);
    ASSERT_EQ(nx_diff("{}", 2, "{}", 2, NULL, &out, &out_len), NX_DIFF_ERR_ARENA);
    ASSERT_EQ(nx_diff("{}", 2, "{}", 2, arena, NULL, &out_len), NX_DIFF_ERR_NULL);
    ASSERT_EQ(nx_diff("{}", 2, "{}", 2, arena, &out, NULL), NX_DIFF_ERR_NULL);

    sh_arena_free(arena);
}

TEST(identical_records)
{
    const char *doc =
        "{\"records\":["
        "{\"id\":\"a\",\"name\":\"Alice\",\"age\":30},"
        "{\"id\":\"b\",\"name\":\"Bob\",\"age\":25}"
        "]}";
    size_t doc_len = strlen(doc);

    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL;
    size_t out_len = 0;

    ASSERT_EQ(nx_diff(doc, doc_len, doc, doc_len, arena, &out, &out_len), NX_DIFF_OK);
    ASSERT(out != NULL);
    ASSERT(out_len > 0);

    int added, removed, modified, unchanged;
    parse_summary(out, out_len, &added, &removed, &modified, &unchanged);
    ASSERT_EQ(added, 0);
    ASSERT_EQ(removed, 0);
    ASSERT_EQ(modified, 0);
    ASSERT_EQ(unchanged, 2);

    ASSERT_EQ(get_array_len(out, out_len, "added"), 0);
    ASSERT_EQ(get_array_len(out, out_len, "removed"), 0);
    ASSERT_EQ(get_array_len(out, out_len, "modified"), 0);

    free(out);
    sh_arena_free(arena);
}

TEST(record_added)
{
    const char *old_doc =
        "{\"records\":["
        "{\"id\":\"a\",\"name\":\"Alice\"}"
        "]}";
    const char *new_doc =
        "{\"records\":["
        "{\"id\":\"a\",\"name\":\"Alice\"},"
        "{\"id\":\"b\",\"name\":\"Bob\"}"
        "]}";

    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL;
    size_t out_len = 0;

    ASSERT_EQ(nx_diff(old_doc, strlen(old_doc), new_doc, strlen(new_doc),
                      arena, &out, &out_len), NX_DIFF_OK);

    int added, removed, modified, unchanged;
    parse_summary(out, out_len, &added, &removed, &modified, &unchanged);
    ASSERT_EQ(added, 1);
    ASSERT_EQ(removed, 0);
    ASSERT_EQ(modified, 0);
    ASSERT_EQ(unchanged, 1);

    ASSERT_EQ(get_array_len(out, out_len, "added"), 1);

    free(out);
    sh_arena_free(arena);
}

TEST(record_removed)
{
    const char *old_doc =
        "{\"records\":["
        "{\"id\":\"a\",\"name\":\"Alice\"},"
        "{\"id\":\"b\",\"name\":\"Bob\"}"
        "]}";
    const char *new_doc =
        "{\"records\":["
        "{\"id\":\"a\",\"name\":\"Alice\"}"
        "]}";

    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL;
    size_t out_len = 0;

    ASSERT_EQ(nx_diff(old_doc, strlen(old_doc), new_doc, strlen(new_doc),
                      arena, &out, &out_len), NX_DIFF_OK);

    int added, removed, modified, unchanged;
    parse_summary(out, out_len, &added, &removed, &modified, &unchanged);
    ASSERT_EQ(added, 0);
    ASSERT_EQ(removed, 1);
    ASSERT_EQ(modified, 0);
    ASSERT_EQ(unchanged, 1);

    ASSERT_EQ(get_array_len(out, out_len, "removed"), 1);

    free(out);
    sh_arena_free(arena);
}

TEST(record_modified)
{
    const char *old_doc =
        "{\"records\":["
        "{\"id\":\"a\",\"name\":\"Alice\",\"age\":30}"
        "]}";
    const char *new_doc =
        "{\"records\":["
        "{\"id\":\"a\",\"name\":\"Alice\",\"age\":31}"
        "]}";

    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL;
    size_t out_len = 0;

    ASSERT_EQ(nx_diff(old_doc, strlen(old_doc), new_doc, strlen(new_doc),
                      arena, &out, &out_len), NX_DIFF_OK);

    int added, removed, modified, unchanged;
    parse_summary(out, out_len, &added, &removed, &modified, &unchanged);
    ASSERT_EQ(added, 0);
    ASSERT_EQ(removed, 0);
    ASSERT_EQ(modified, 1);
    ASSERT_EQ(unchanged, 0);

    ASSERT_EQ(get_array_len(out, out_len, "modified"), 1);

    /* Verify modified entry has old/new */
    SHArena *a2 = sh_arena_create(64 * 1024);
    ShJsonValue *root = NULL;
    ASSERT(sh_json_parse(out, out_len, a2, &root) == SH_JSON_OK);
    ShJsonValue *mod_arr = sh_json_get(root, "modified");
    ShJsonValue *mod0 = sh_json_array_get(mod_arr, 0);
    ASSERT(mod0 != NULL);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(mod0, "id"), ""), "a");
    ASSERT(sh_json_get(mod0, "old") != NULL);
    ASSERT(sh_json_get(mod0, "new") != NULL);
    sh_arena_free(a2);

    free(out);
    sh_arena_free(arena);
}

TEST(mixed_changes)
{
    const char *old_doc =
        "{\"records\":["
        "{\"id\":\"a\",\"name\":\"Alice\",\"age\":30},"
        "{\"id\":\"b\",\"name\":\"Bob\",\"age\":25},"
        "{\"id\":\"c\",\"name\":\"Charlie\",\"age\":35}"
        "]}";
    /* a: unchanged, b: modified (age 25→26), c: removed, d: added */
    const char *new_doc =
        "{\"records\":["
        "{\"id\":\"a\",\"name\":\"Alice\",\"age\":30},"
        "{\"id\":\"b\",\"name\":\"Bob\",\"age\":26},"
        "{\"id\":\"d\",\"name\":\"Diana\",\"age\":28}"
        "]}";

    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL;
    size_t out_len = 0;

    ASSERT_EQ(nx_diff(old_doc, strlen(old_doc), new_doc, strlen(new_doc),
                      arena, &out, &out_len), NX_DIFF_OK);

    int added, removed, modified, unchanged;
    parse_summary(out, out_len, &added, &removed, &modified, &unchanged);
    ASSERT_EQ(added, 1);
    ASSERT_EQ(removed, 1);
    ASSERT_EQ(modified, 1);
    ASSERT_EQ(unchanged, 1);

    ASSERT_EQ(get_array_len(out, out_len, "added"), 1);
    ASSERT_EQ(get_array_len(out, out_len, "removed"), 1);
    ASSERT_EQ(get_array_len(out, out_len, "modified"), 1);

    free(out);
    sh_arena_free(arena);
}

TEST(empty_records)
{
    const char *doc = "{\"records\":[]}";
    size_t doc_len = strlen(doc);

    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL;
    size_t out_len = 0;

    ASSERT_EQ(nx_diff(doc, doc_len, doc, doc_len, arena, &out, &out_len), NX_DIFF_OK);

    int added, removed, modified, unchanged;
    parse_summary(out, out_len, &added, &removed, &modified, &unchanged);
    ASSERT_EQ(added, 0);
    ASSERT_EQ(removed, 0);
    ASSERT_EQ(modified, 0);
    ASSERT_EQ(unchanged, 0);

    free(out);
    sh_arena_free(arena);
}

TEST(records_without_id_skipped)
{
    const char *old_doc =
        "{\"records\":["
        "{\"id\":\"a\",\"name\":\"Alice\"},"
        "{\"name\":\"NoId\"}"
        "]}";
    const char *new_doc =
        "{\"records\":["
        "{\"id\":\"a\",\"name\":\"Alice\"},"
        "{\"name\":\"AlsoNoId\"}"
        "]}";

    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL;
    size_t out_len = 0;

    ASSERT_EQ(nx_diff(old_doc, strlen(old_doc), new_doc, strlen(new_doc),
                      arena, &out, &out_len), NX_DIFF_OK);

    int added, removed, modified, unchanged;
    parse_summary(out, out_len, &added, &removed, &modified, &unchanged);
    ASSERT_EQ(unchanged, 1);
    /* Records without id are skipped entirely */
    ASSERT_EQ(added, 0);
    ASSERT_EQ(removed, 0);
    ASSERT_EQ(modified, 0);

    free(out);
    sh_arena_free(arena);
}

TEST(status_strings)
{
    ASSERT_STREQ(nx_diff_status_str(NX_DIFF_OK), "OK");
    ASSERT_STREQ(nx_diff_status_str(NX_DIFF_ERR_NULL), "NULL input");
    ASSERT_STREQ(nx_diff_status_str(NX_DIFF_ERR_PARSE), "JSON parse error");
    ASSERT_STREQ(nx_diff_status_str(NX_DIFF_ERR_ARENA), "Arena allocation failure");
    ASSERT_STREQ(nx_diff_status_str((NxDiffStatus)99), "Unknown error");
}

TEST(parse_error)
{
    const char *bad = "not json";
    const char *good = "{\"records\":[]}";
    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL;
    size_t out_len = 0;

    ASSERT_EQ(nx_diff(bad, strlen(bad), good, strlen(good),
                      arena, &out, &out_len), NX_DIFF_ERR_PARSE);
    ASSERT_EQ(nx_diff(good, strlen(good), bad, strlen(bad),
                      arena, &out, &out_len), NX_DIFF_ERR_PARSE);

    sh_arena_free(arena);
}

TEST(missing_records_key)
{
    const char *no_records = "{\"data\":[]}";
    const char *good = "{\"records\":[]}";
    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL;
    size_t out_len = 0;

    ASSERT_EQ(nx_diff(no_records, strlen(no_records), good, strlen(good),
                      arena, &out, &out_len), NX_DIFF_ERR_PARSE);

    sh_arena_free(arena);
}

TEST(nx_diff_marker)
{
    const char *doc = "{\"records\":[{\"id\":\"a\",\"v\":1}]}";
    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL;
    size_t out_len = 0;

    ASSERT_EQ(nx_diff(doc, strlen(doc), doc, strlen(doc),
                      arena, &out, &out_len), NX_DIFF_OK);

    /* Verify nx_diff marker in output */
    SHArena *a2 = sh_arena_create(64 * 1024);
    ShJsonValue *root = NULL;
    ASSERT(sh_json_parse(out, out_len, a2, &root) == SH_JSON_OK);
    int marker = (int)sh_json_as_double(sh_json_get(root, "nx_diff"), 0);
    ASSERT_EQ(marker, 1);
    sh_arena_free(a2);

    free(out);
    sh_arena_free(arena);
}

TEST(all_added)
{
    const char *old_doc = "{\"records\":[]}";
    const char *new_doc =
        "{\"records\":["
        "{\"id\":\"a\",\"v\":1},"
        "{\"id\":\"b\",\"v\":2},"
        "{\"id\":\"c\",\"v\":3}"
        "]}";

    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL;
    size_t out_len = 0;

    ASSERT_EQ(nx_diff(old_doc, strlen(old_doc), new_doc, strlen(new_doc),
                      arena, &out, &out_len), NX_DIFF_OK);

    int added, removed, modified, unchanged;
    parse_summary(out, out_len, &added, &removed, &modified, &unchanged);
    ASSERT_EQ(added, 3);
    ASSERT_EQ(removed, 0);
    ASSERT_EQ(modified, 0);
    ASSERT_EQ(unchanged, 0);

    free(out);
    sh_arena_free(arena);
}

TEST(all_removed)
{
    const char *old_doc =
        "{\"records\":["
        "{\"id\":\"a\",\"v\":1},"
        "{\"id\":\"b\",\"v\":2}"
        "]}";
    const char *new_doc = "{\"records\":[]}";

    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL;
    size_t out_len = 0;

    ASSERT_EQ(nx_diff(old_doc, strlen(old_doc), new_doc, strlen(new_doc),
                      arena, &out, &out_len), NX_DIFF_OK);

    int added, removed, modified, unchanged;
    parse_summary(out, out_len, &added, &removed, &modified, &unchanged);
    ASSERT_EQ(added, 0);
    ASSERT_EQ(removed, 2);
    ASSERT_EQ(modified, 0);
    ASSERT_EQ(unchanged, 0);

    free(out);
    sh_arena_free(arena);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
    printf("nx_diff tests:\n");

    RUN_TEST(null_input);
    RUN_TEST(identical_records);
    RUN_TEST(record_added);
    RUN_TEST(record_removed);
    RUN_TEST(record_modified);
    RUN_TEST(mixed_changes);
    RUN_TEST(empty_records);
    RUN_TEST(records_without_id_skipped);
    RUN_TEST(status_strings);
    RUN_TEST(parse_error);
    RUN_TEST(missing_records_key);
    RUN_TEST(nx_diff_marker);
    RUN_TEST(all_added);
    RUN_TEST(all_removed);

    printf("\n  %d/%d tests passed\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
