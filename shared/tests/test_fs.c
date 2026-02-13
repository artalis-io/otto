/*
 * test_fs.c - Tests for sh_fs filesystem utilities
 */

#include "sh_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

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
        printf("[FAIL]\n    Assertion failed: %s\n    at %s:%d\n", \
               #cond, __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))

/* Helper: check if path is a directory */
static int is_dir(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

/* Helper: recursive rmdir (for cleanup) */
static void rmdir_r(const char *path)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", path);
    (void)system(cmd); /* OK for test cleanup only */
}

/* Base temp directory for tests */
#define TEST_DIR "/tmp/otto_test_fs"

/* ============================================================================
 * Tests
 * ============================================================================ */

TEST(mkdirs_null_returns_error)
{
    ASSERT_EQ(sh_mkdirs(NULL), -1);
}

TEST(mkdirs_empty_returns_error)
{
    ASSERT_EQ(sh_mkdirs(""), -1);
}

TEST(mkdirs_single_level)
{
    rmdir_r(TEST_DIR);
    char path[256];
    snprintf(path, sizeof(path), "%s/single", TEST_DIR);
    ASSERT_EQ(sh_mkdirs(path), 0);
    ASSERT(is_dir(path));
    rmdir_r(TEST_DIR);
}

TEST(mkdirs_nested)
{
    rmdir_r(TEST_DIR);
    char path[256];
    snprintf(path, sizeof(path), "%s/a/b/c/d", TEST_DIR);
    ASSERT_EQ(sh_mkdirs(path), 0);
    ASSERT(is_dir(path));

    /* Verify intermediate directories exist too */
    char inter[256];
    snprintf(inter, sizeof(inter), "%s/a", TEST_DIR);
    ASSERT(is_dir(inter));
    snprintf(inter, sizeof(inter), "%s/a/b", TEST_DIR);
    ASSERT(is_dir(inter));
    snprintf(inter, sizeof(inter), "%s/a/b/c", TEST_DIR);
    ASSERT(is_dir(inter));

    rmdir_r(TEST_DIR);
}

TEST(mkdirs_existing_dir)
{
    rmdir_r(TEST_DIR);
    char path[256];
    snprintf(path, sizeof(path), "%s/exists", TEST_DIR);
    ASSERT_EQ(sh_mkdirs(path), 0);

    /* Second call on same path should succeed (EEXIST) */
    ASSERT_EQ(sh_mkdirs(path), 0);
    ASSERT(is_dir(path));

    rmdir_r(TEST_DIR);
}

TEST(mkdirs_trailing_slash)
{
    rmdir_r(TEST_DIR);
    char path[256];
    snprintf(path, sizeof(path), "%s/trailing/", TEST_DIR);
    ASSERT_EQ(sh_mkdirs(path), 0);

    /* Verify dir without trailing slash */
    char check[256];
    snprintf(check, sizeof(check), "%s/trailing", TEST_DIR);
    ASSERT(is_dir(check));

    rmdir_r(TEST_DIR);
}

TEST(mkdirs_partially_existing)
{
    rmdir_r(TEST_DIR);

    /* Create first level */
    char first[256];
    snprintf(first, sizeof(first), "%s/partial", TEST_DIR);
    ASSERT_EQ(sh_mkdirs(first), 0);

    /* Now create deeper level - should still work */
    char deep[256];
    snprintf(deep, sizeof(deep), "%s/partial/sub1/sub2", TEST_DIR);
    ASSERT_EQ(sh_mkdirs(deep), 0);
    ASSERT(is_dir(deep));

    rmdir_r(TEST_DIR);
}

TEST(mkdirs_absolute_path)
{
    rmdir_r(TEST_DIR);
    char path[256];
    snprintf(path, sizeof(path), "%s/abs/path/test", TEST_DIR);
    ASSERT_EQ(sh_mkdirs(path), 0);
    ASSERT(is_dir(path));
    rmdir_r(TEST_DIR);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
    printf("\nFilesystem Utility Tests:\n\n");

    /* Clean up any leftover test dirs */
    rmdir_r(TEST_DIR);

    RUN_TEST(mkdirs_null_returns_error);
    RUN_TEST(mkdirs_empty_returns_error);
    RUN_TEST(mkdirs_single_level);
    RUN_TEST(mkdirs_nested);
    RUN_TEST(mkdirs_existing_dir);
    RUN_TEST(mkdirs_trailing_slash);
    RUN_TEST(mkdirs_partially_existing);
    RUN_TEST(mkdirs_absolute_path);

    /* Final cleanup */
    rmdir_r(TEST_DIR);

    printf("\nFilesystem: %d passed, %d total\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
