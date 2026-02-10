/*
 * test_sh_json.c - Unit tests for JSON parser
 */

#include "sh_json.h"
#include "sh_arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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
        printf("[FAIL]\n    Assertion failed: %s\n", #cond); \
        exit(1); \
    } \
} while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NEAR(a, b, eps) ASSERT(fabs((a) - (b)) < (eps))
#define ASSERT_STREQ(a, b) ASSERT(strcmp((a), (b)) == 0)

/* Helper to parse JSON and assert success */
static ShJsonValue *parse_ok(const char *json, SHArena *arena) {
    ShJsonValue *val = NULL;
    ShJsonStatus status = sh_json_parse(json, strlen(json), arena, &val);
    if (status != SH_JSON_OK) {
        printf("[FAIL]\n    Parse failed: %s (input: %s)\n",
               sh_json_status_str(status), json);
        exit(1);
    }
    return val;
}

/* Helper to parse JSON and assert failure with specific error */
static void parse_err(const char *json, ShJsonStatus expected, SHArena *arena) {
    ShJsonValue *val = NULL;
    ShJsonStatus status = sh_json_parse(json, strlen(json), arena, &val);
    if (status != expected) {
        printf("[FAIL]\n    Expected %s, got %s (input: %s)\n",
               sh_json_status_str(expected), sh_json_status_str(status), json);
        exit(1);
    }
}

/* ============================================================================
 * 1. Basic Parsing Tests (15 tests)
 * ============================================================================ */

TEST(parse_null)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("null", arena);
    ASSERT(sh_json_is_null(v));
    ASSERT_EQ(sh_json_type(v), SH_JSON_NULL);
    sh_arena_free(arena);
}

TEST(parse_true)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("true", arena);
    ASSERT_EQ(sh_json_type(v), SH_JSON_BOOL);
    ASSERT_EQ(sh_json_as_bool(v, false), true);
    sh_arena_free(arena);
}

TEST(parse_false)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("false", arena);
    ASSERT_EQ(sh_json_type(v), SH_JSON_BOOL);
    ASSERT_EQ(sh_json_as_bool(v, true), false);
    sh_arena_free(arena);
}

TEST(parse_integer)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("42", arena);
    ASSERT_EQ(sh_json_type(v), SH_JSON_NUMBER);
    ASSERT_NEAR(sh_json_as_double(v, 0), 42.0, 0.001);
    ASSERT_EQ(sh_json_as_int(v, 0), 42);
    sh_arena_free(arena);
}

TEST(parse_negative_integer)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("-17", arena);
    ASSERT_EQ(sh_json_type(v), SH_JSON_NUMBER);
    ASSERT_NEAR(sh_json_as_double(v, 0), -17.0, 0.001);
    sh_arena_free(arena);
}

TEST(parse_float)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("3.14159", arena);
    ASSERT_EQ(sh_json_type(v), SH_JSON_NUMBER);
    ASSERT_NEAR(sh_json_as_double(v, 0), 3.14159, 0.00001);
    sh_arena_free(arena);
}

TEST(parse_exponent)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("1e10", arena);
    ASSERT_EQ(sh_json_type(v), SH_JSON_NUMBER);
    ASSERT_NEAR(sh_json_as_double(v, 0), 1e10, 1e5);
    sh_arena_free(arena);
}

TEST(parse_negative_exponent)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("2.5E-3", arena);
    ASSERT_EQ(sh_json_type(v), SH_JSON_NUMBER);
    ASSERT_NEAR(sh_json_as_double(v, 0), 0.0025, 0.00001);
    sh_arena_free(arena);
}

TEST(parse_empty_string)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("\"\"", arena);
    ASSERT_EQ(sh_json_type(v), SH_JSON_STRING);
    ASSERT_STREQ(sh_json_as_string(v, "x"), "");
    ASSERT_EQ(sh_json_string_len(v), 0);
    sh_arena_free(arena);
}

TEST(parse_simple_string)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("\"hello\"", arena);
    ASSERT_EQ(sh_json_type(v), SH_JSON_STRING);
    ASSERT_STREQ(sh_json_as_string(v, ""), "hello");
    ASSERT_EQ(sh_json_string_len(v), 5);
    sh_arena_free(arena);
}

TEST(parse_string_with_spaces)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("\"hello world\"", arena);
    ASSERT_EQ(sh_json_type(v), SH_JSON_STRING);
    ASSERT_STREQ(sh_json_as_string(v, ""), "hello world");
    sh_arena_free(arena);
}

TEST(parse_empty_array)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("[]", arena);
    ASSERT_EQ(sh_json_type(v), SH_JSON_ARRAY);
    ASSERT_EQ(sh_json_array_len(v), 0);
    sh_arena_free(arena);
}

TEST(parse_empty_object)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("{}", arena);
    ASSERT_EQ(sh_json_type(v), SH_JSON_OBJECT);
    ASSERT_EQ(sh_json_object_len(v), 0);
    sh_arena_free(arena);
}

TEST(parse_array_of_numbers)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("[1, 2, 3]", arena);
    ASSERT_EQ(sh_json_type(v), SH_JSON_ARRAY);
    ASSERT_EQ(sh_json_array_len(v), 3);
    ASSERT_NEAR(sh_json_as_double(sh_json_array_get(v, 0), 0), 1.0, 0.001);
    ASSERT_NEAR(sh_json_as_double(sh_json_array_get(v, 1), 0), 2.0, 0.001);
    ASSERT_NEAR(sh_json_as_double(sh_json_array_get(v, 2), 0), 3.0, 0.001);
    sh_arena_free(arena);
}

TEST(parse_nested_object)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("{\"user\": {\"name\": \"Alice\", \"age\": 30}}", arena);
    ASSERT_EQ(sh_json_type(v), SH_JSON_OBJECT);
    ShJsonValue *user = sh_json_get(v, "user");
    ASSERT(user != NULL);
    ASSERT_EQ(sh_json_type(user), SH_JSON_OBJECT);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(user, "name"), ""), "Alice");
    ASSERT_EQ(sh_json_as_int(sh_json_get(user, "age"), 0), 30);
    sh_arena_free(arena);
}

/* ============================================================================
 * 2. String Escape Tests (12 tests)
 * ============================================================================ */

TEST(escape_quote)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("\"hello \\\"world\\\"\"", arena);
    ASSERT_STREQ(sh_json_as_string(v, ""), "hello \"world\"");
    sh_arena_free(arena);
}

TEST(escape_backslash)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("\"path\\\\to\\\\file\"", arena);
    ASSERT_STREQ(sh_json_as_string(v, ""), "path\\to\\file");
    sh_arena_free(arena);
}

TEST(escape_slash)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("\"http:\\/\\/example.com\"", arena);
    ASSERT_STREQ(sh_json_as_string(v, ""), "http://example.com");
    sh_arena_free(arena);
}

TEST(escape_backspace)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("\"a\\bb\"", arena);
    ASSERT_STREQ(sh_json_as_string(v, ""), "a\bb");
    sh_arena_free(arena);
}

TEST(escape_formfeed)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("\"a\\fb\"", arena);
    ASSERT_STREQ(sh_json_as_string(v, ""), "a\fb");
    sh_arena_free(arena);
}

TEST(escape_newline)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("\"line1\\nline2\"", arena);
    ASSERT_STREQ(sh_json_as_string(v, ""), "line1\nline2");
    sh_arena_free(arena);
}

TEST(escape_carriage_return)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("\"a\\rb\"", arena);
    ASSERT_STREQ(sh_json_as_string(v, ""), "a\rb");
    sh_arena_free(arena);
}

TEST(escape_tab)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("\"col1\\tcol2\"", arena);
    ASSERT_STREQ(sh_json_as_string(v, ""), "col1\tcol2");
    sh_arena_free(arena);
}

TEST(escape_unicode_ascii)
{
    SHArena *arena = sh_arena_create(1024);
    /* \u0041 = 'A' */
    ShJsonValue *v = parse_ok("\"\\u0041\"", arena);
    ASSERT_STREQ(sh_json_as_string(v, ""), "A");
    sh_arena_free(arena);
}

TEST(escape_unicode_euro)
{
    SHArena *arena = sh_arena_create(1024);
    /* \u20AC = '€' (UTF-8: E2 82 AC) */
    ShJsonValue *v = parse_ok("\"\\u20AC\"", arena);
    ASSERT_STREQ(sh_json_as_string(v, ""), "\xE2\x82\xAC");
    ASSERT_EQ(sh_json_string_len(v), 3);  /* 3 bytes in UTF-8 */
    sh_arena_free(arena);
}

TEST(escape_unicode_emoji)
{
    SHArena *arena = sh_arena_create(1024);
    /* \uD83D\uDE00 = 😀 (U+1F600, UTF-8: F0 9F 98 80) */
    ShJsonValue *v = parse_ok("\"\\uD83D\\uDE00\"", arena);
    ASSERT_STREQ(sh_json_as_string(v, ""), "\xF0\x9F\x98\x80");
    ASSERT_EQ(sh_json_string_len(v), 4);  /* 4 bytes in UTF-8 */
    sh_arena_free(arena);
}

TEST(escape_mixed)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("\"tab:\\there\\nnewline\"", arena);
    ASSERT_STREQ(sh_json_as_string(v, ""), "tab:\there\nnewline");
    sh_arena_free(arena);
}

/* ============================================================================
 * 3. Number Edge Cases (10 tests)
 * ============================================================================ */

TEST(number_zero)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("0", arena);
    ASSERT_NEAR(sh_json_as_double(v, -1), 0.0, 0.001);
    sh_arena_free(arena);
}

TEST(number_negative_zero)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("-0", arena);
    ASSERT_NEAR(sh_json_as_double(v, -1), 0.0, 0.001);
    sh_arena_free(arena);
}

TEST(number_zero_point_five)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("0.5", arena);
    ASSERT_NEAR(sh_json_as_double(v, 0), 0.5, 0.001);
    sh_arena_free(arena);
}

TEST(number_large_integer)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("9007199254740991", arena);  /* MAX_SAFE_INTEGER */
    ASSERT_NEAR(sh_json_as_double(v, 0), 9007199254740991.0, 1.0);
    sh_arena_free(arena);
}

TEST(number_small_float)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("0.000001", arena);
    ASSERT_NEAR(sh_json_as_double(v, 0), 0.000001, 0.0000001);
    sh_arena_free(arena);
}

TEST(number_max_double)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("1.7976931348623157e308", arena);
    ASSERT(sh_json_as_double(v, 0) > 1e307);
    sh_arena_free(arena);
}

TEST(number_scientific_upper)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("1E10", arena);
    ASSERT_NEAR(sh_json_as_double(v, 0), 1e10, 1e5);
    sh_arena_free(arena);
}

TEST(number_scientific_lower)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("1e10", arena);
    ASSERT_NEAR(sh_json_as_double(v, 0), 1e10, 1e5);
    sh_arena_free(arena);
}

TEST(number_scientific_positive_exp)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("1e+10", arena);
    ASSERT_NEAR(sh_json_as_double(v, 0), 1e10, 1e5);
    sh_arena_free(arena);
}

TEST(number_scientific_negative_exp)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("1e-10", arena);
    ASSERT_NEAR(sh_json_as_double(v, 0), 1e-10, 1e-15);
    sh_arena_free(arena);
}

/* ============================================================================
 * 4. Error Cases (18 tests)
 * ============================================================================ */

TEST(err_null_input)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *val = NULL;
    ASSERT_EQ(sh_json_parse(NULL, 0, arena, &val), SH_JSON_ERR_NULL);
    sh_arena_free(arena);
}

TEST(err_empty_input)
{
    SHArena *arena = sh_arena_create(1024);
    parse_err("", SH_JSON_ERR_EMPTY, arena);
    sh_arena_free(arena);
}

TEST(err_just_whitespace)
{
    SHArena *arena = sh_arena_create(1024);
    parse_err("   \t\n  ", SH_JSON_ERR_EMPTY, arena);
    sh_arena_free(arena);
}

TEST(err_trailing_comma_array)
{
    SHArena *arena = sh_arena_create(1024);
    parse_err("[1, 2, ]", SH_JSON_ERR_SYNTAX, arena);
    sh_arena_free(arena);
}

TEST(err_trailing_comma_object)
{
    SHArena *arena = sh_arena_create(1024);
    parse_err("{\"a\": 1, }", SH_JSON_ERR_SYNTAX, arena);
    sh_arena_free(arena);
}

TEST(err_missing_colon)
{
    SHArena *arena = sh_arena_create(1024);
    parse_err("{\"a\" 1}", SH_JSON_ERR_SYNTAX, arena);
    sh_arena_free(arena);
}

TEST(err_missing_value)
{
    SHArena *arena = sh_arena_create(1024);
    parse_err("{\"a\":}", SH_JSON_ERR_SYNTAX, arena);
    sh_arena_free(arena);
}

TEST(err_unquoted_key)
{
    SHArena *arena = sh_arena_create(1024);
    parse_err("{a: 1}", SH_JSON_ERR_SYNTAX, arena);
    sh_arena_free(arena);
}

TEST(err_unterminated_string)
{
    SHArena *arena = sh_arena_create(1024);
    parse_err("\"hello", SH_JSON_ERR_UNTERMINATED_STRING, arena);
    sh_arena_free(arena);
}

TEST(err_newline_in_string)
{
    SHArena *arena = sh_arena_create(1024);
    parse_err("\"hello\nworld\"", SH_JSON_ERR_SYNTAX, arena);
    sh_arena_free(arena);
}

TEST(err_invalid_escape)
{
    SHArena *arena = sh_arena_create(1024);
    parse_err("\"hello\\x00\"", SH_JSON_ERR_INVALID_ESCAPE, arena);
    sh_arena_free(arena);
}

TEST(err_invalid_unicode_escape)
{
    SHArena *arena = sh_arena_create(1024);
    parse_err("\"\\uGHIJ\"", SH_JSON_ERR_INVALID_ESCAPE, arena);
    sh_arena_free(arena);
}

TEST(err_truncated_unicode)
{
    SHArena *arena = sh_arena_create(1024);
    parse_err("\"\\u00\"", SH_JSON_ERR_INVALID_ESCAPE, arena);
    sh_arena_free(arena);
}

TEST(err_leading_plus)
{
    SHArena *arena = sh_arena_create(1024);
    parse_err("+5", SH_JSON_ERR_SYNTAX, arena);
    sh_arena_free(arena);
}

TEST(err_leading_dot)
{
    SHArena *arena = sh_arena_create(1024);
    parse_err(".5", SH_JSON_ERR_SYNTAX, arena);
    sh_arena_free(arena);
}

TEST(err_trailing_dot)
{
    SHArena *arena = sh_arena_create(1024);
    parse_err("5.", SH_JSON_ERR_INVALID_NUMBER, arena);
    sh_arena_free(arena);
}

TEST(err_leading_zero)
{
    SHArena *arena = sh_arena_create(1024);
    parse_err("01", SH_JSON_ERR_INVALID_NUMBER, arena);
    sh_arena_free(arena);
}

TEST(err_depth_exceeded)
{
    SHArena *arena = sh_arena_create(8192);
    /* Build deeply nested JSON: [[[[...]]]] (65 levels) */
    char json[256];
    memset(json, '[', 65);
    memset(json + 65, ']', 65);
    json[130] = '\0';
    parse_err(json, SH_JSON_ERR_DEPTH_EXCEEDED, arena);
    sh_arena_free(arena);
}

/* ============================================================================
 * 5. Value Access Tests (20 tests)
 * ============================================================================ */

TEST(type_null)
{
    ASSERT_EQ(sh_json_type(NULL), SH_JSON_NULL);
}

TEST(type_bool)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("true", arena);
    ASSERT_EQ(sh_json_type(v), SH_JSON_BOOL);
    sh_arena_free(arena);
}

TEST(type_number)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("42", arena);
    ASSERT_EQ(sh_json_type(v), SH_JSON_NUMBER);
    sh_arena_free(arena);
}

TEST(type_string)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("\"hello\"", arena);
    ASSERT_EQ(sh_json_type(v), SH_JSON_STRING);
    sh_arena_free(arena);
}

TEST(type_array)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("[1, 2]", arena);
    ASSERT_EQ(sh_json_type(v), SH_JSON_ARRAY);
    sh_arena_free(arena);
}

TEST(type_object)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("{\"a\": 1}", arena);
    ASSERT_EQ(sh_json_type(v), SH_JSON_OBJECT);
    sh_arena_free(arena);
}

TEST(as_bool_null_value)
{
    ASSERT_EQ(sh_json_as_bool(NULL, true), true);
    ASSERT_EQ(sh_json_as_bool(NULL, false), false);
}

TEST(as_double_null_value)
{
    ASSERT_NEAR(sh_json_as_double(NULL, 42.5), 42.5, 0.001);
}

TEST(as_string_null_value)
{
    ASSERT_STREQ(sh_json_as_string(NULL, "default"), "default");
}

TEST(array_get_null_value)
{
    ASSERT(sh_json_array_get(NULL, 0) == NULL);
}

TEST(object_get_null_value)
{
    ASSERT(sh_json_get(NULL, "key") == NULL);
}

TEST(as_bool_from_number)
{
    /* Type mismatch returns default */
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("42", arena);
    ASSERT_EQ(sh_json_as_bool(v, true), true);  /* Returns default */
    sh_arena_free(arena);
}

TEST(as_double_from_string)
{
    /* Type mismatch returns default */
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("\"42\"", arena);
    ASSERT_NEAR(sh_json_as_double(v, 99.0), 99.0, 0.001);  /* Returns default */
    sh_arena_free(arena);
}

TEST(as_string_from_number)
{
    /* Type mismatch returns default */
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("42", arena);
    ASSERT_STREQ(sh_json_as_string(v, "default"), "default");
    sh_arena_free(arena);
}

TEST(array_len)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("[1, 2, 3, 4, 5]", arena);
    ASSERT_EQ(sh_json_array_len(v), 5);
    sh_arena_free(arena);
}

TEST(array_get_valid)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("[10, 20, 30]", arena);
    ASSERT_EQ(sh_json_as_int(sh_json_array_get(v, 1), 0), 20);
    sh_arena_free(arena);
}

TEST(array_get_out_of_bounds)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("[1, 2, 3]", arena);
    ASSERT(sh_json_array_get(v, 3) == NULL);
    ASSERT(sh_json_array_get(v, 100) == NULL);
    sh_arena_free(arena);
}

TEST(object_len)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("{\"a\": 1, \"b\": 2, \"c\": 3}", arena);
    ASSERT_EQ(sh_json_object_len(v), 3);
    sh_arena_free(arena);
}

TEST(object_get_exists)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("{\"name\": \"Alice\", \"age\": 30}", arena);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(v, "name"), ""), "Alice");
    ASSERT_EQ(sh_json_as_int(sh_json_get(v, "age"), 0), 30);
    sh_arena_free(arena);
}

TEST(object_get_missing)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("{\"a\": 1}", arena);
    ASSERT(sh_json_get(v, "b") == NULL);
    ASSERT(sh_json_get(v, "nonexistent") == NULL);
    sh_arena_free(arena);
}

/* ============================================================================
 * 6. Path Access Tests (10 tests)
 * ============================================================================ */

TEST(path_simple_key)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("{\"name\": \"Alice\"}", arena);
    ASSERT_STREQ(sh_json_as_string(sh_json_get_path(v, "name"), ""), "Alice");
    sh_arena_free(arena);
}

TEST(path_nested_keys)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("{\"user\": {\"name\": \"Bob\"}}", arena);
    ASSERT_STREQ(sh_json_as_string(sh_json_get_path(v, "user.name"), ""), "Bob");
    sh_arena_free(arena);
}

TEST(path_array_index)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("{\"items\": [10, 20, 30]}", arena);
    ASSERT_EQ(sh_json_as_int(sh_json_get_path(v, "items[0]"), 0), 10);
    ASSERT_EQ(sh_json_as_int(sh_json_get_path(v, "items[2]"), 0), 30);
    sh_arena_free(arena);
}

TEST(path_nested_array)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("{\"data\": {\"items\": [{\"name\": \"x\"}, {\"name\": \"y\"}, {\"name\": \"z\"}]}}", arena);
    ASSERT_STREQ(sh_json_as_string(sh_json_get_path(v, "data.items[2].name"), ""), "z");
    sh_arena_free(arena);
}

TEST(path_missing_key)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("{\"a\": 1}", arena);
    ASSERT(sh_json_get_path(v, "nonexistent") == NULL);
    ASSERT(sh_json_get_path(v, "a.b") == NULL);
    sh_arena_free(arena);
}

TEST(path_invalid_index)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("{\"items\": [1, 2, 3]}", arena);
    ASSERT(sh_json_get_path(v, "items[999]") == NULL);
    sh_arena_free(arena);
}

TEST(path_non_array_index)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("{\"name\": \"test\"}", arena);
    ASSERT(sh_json_get_path(v, "name[0]") == NULL);
    sh_arena_free(arena);
}

TEST(path_non_object_key)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("{\"items\": [1, 2, 3]}", arena);
    ASSERT(sh_json_get_path(v, "items.foo") == NULL);
    sh_arena_free(arena);
}

TEST(path_empty)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("{\"a\": 1}", arena);
    /* Empty path should return root */
    ShJsonValue *result = sh_json_get_path(v, "");
    ASSERT(result == v);
    sh_arena_free(arena);
}

TEST(path_null_value)
{
    ASSERT(sh_json_get_path(NULL, "a") == NULL);
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("{\"a\": 1}", arena);
    ASSERT(sh_json_get_path(v, NULL) == NULL);
    sh_arena_free(arena);
}

/* ============================================================================
 * 7. FuelWise Integration Tests (10 tests)
 * ============================================================================ */

TEST(parse_station)
{
    SHArena *arena = sh_arena_create(2048);
    ShJsonValue *v = parse_ok(
        "{\"id\": 1, \"lat\": 47.4979, \"lon\": 19.0402, \"price\": 1.45}",
        arena);
    ASSERT_EQ(sh_json_as_int(sh_json_get(v, "id"), 0), 1);
    ASSERT_NEAR(sh_json_as_double(sh_json_get(v, "lat"), 0), 47.4979, 0.0001);
    ASSERT_NEAR(sh_json_as_double(sh_json_get(v, "lon"), 0), 19.0402, 0.0001);
    ASSERT_NEAR(sh_json_as_double(sh_json_get(v, "price"), 0), 1.45, 0.01);
    sh_arena_free(arena);
}

TEST(parse_stations_array)
{
    SHArena *arena = sh_arena_create(4096);
    ShJsonValue *v = parse_ok(
        "[{\"id\": 1, \"price\": 1.45}, {\"id\": 2, \"price\": 1.52}, {\"id\": 3, \"price\": 1.38}]",
        arena);
    ASSERT_EQ(sh_json_array_len(v), 3);
    ASSERT_NEAR(sh_json_as_double(sh_json_get(sh_json_array_get(v, 0), "price"), 0), 1.45, 0.01);
    ASSERT_NEAR(sh_json_as_double(sh_json_get(sh_json_array_get(v, 2), "price"), 0), 1.38, 0.01);
    sh_arena_free(arena);
}

TEST(parse_polyline)
{
    SHArena *arena = sh_arena_create(2048);
    ShJsonValue *v = parse_ok(
        "[[47.5, 19.0], [47.6, 19.1], [47.7, 19.2]]",
        arena);
    ASSERT_EQ(sh_json_array_len(v), 3);
    ShJsonValue *p0 = sh_json_array_get(v, 0);
    ASSERT_NEAR(sh_json_as_double(sh_json_array_get(p0, 0), 0), 47.5, 0.01);
    ASSERT_NEAR(sh_json_as_double(sh_json_array_get(p0, 1), 0), 19.0, 0.01);
    sh_arena_free(arena);
}

TEST(parse_solve_request)
{
    SHArena *arena = sh_arena_create(4096);
    ShJsonValue *v = parse_ok(
        "{"
        "\"total_distance\": 500.0,"
        "\"tank_capacity\": 200.0,"
        "\"current_fuel\": 50.0,"
        "\"consumption\": 0.35,"
        "\"minimum_fuel\": 20.0,"
        "\"stations\": [{\"id\": 1, \"distance\": 100, \"price\": 1.45}]"
        "}",
        arena);
    ASSERT_NEAR(sh_json_as_double(sh_json_get(v, "total_distance"), 0), 500.0, 0.1);
    ASSERT_NEAR(sh_json_as_double(sh_json_get(v, "tank_capacity"), 0), 200.0, 0.1);
    ShJsonValue *stations = sh_json_get(v, "stations");
    ASSERT_EQ(sh_json_array_len(stations), 1);
    sh_arena_free(arena);
}

TEST(parse_optimize_request)
{
    SHArena *arena = sh_arena_create(8192);
    ShJsonValue *v = parse_ok(
        "{"
        "\"polyline\": [[47.5, 19.0], [47.6, 19.1]],"
        "\"stations\": [{\"lat\": 47.55, \"lon\": 19.05, \"price\": 1.45}],"
        "\"tank_capacity\": 200,"
        "\"fuel_level\": 50"
        "}",
        arena);
    ShJsonValue *polyline = sh_json_get(v, "polyline");
    ASSERT_EQ(sh_json_array_len(polyline), 2);
    ShJsonValue *stations = sh_json_get(v, "stations");
    ASSERT_EQ(sh_json_array_len(stations), 1);
    sh_arena_free(arena);
}

TEST(parse_segment)
{
    SHArena *arena = sh_arena_create(2048);
    ShJsonValue *v = parse_ok(
        "{\"start_distance\": 0, \"cargo_weight\": 15000, \"consumption\": 0.35}",
        arena);
    ASSERT_NEAR(sh_json_as_double(sh_json_get(v, "start_distance"), -1), 0.0, 0.01);
    ASSERT_NEAR(sh_json_as_double(sh_json_get(v, "cargo_weight"), 0), 15000.0, 0.1);
    ASSERT_NEAR(sh_json_as_double(sh_json_get(v, "consumption"), 0), 0.35, 0.001);
    sh_arena_free(arena);
}

TEST(parse_vehicle_config)
{
    SHArena *arena = sh_arena_create(2048);
    ShJsonValue *v = parse_ok(
        "{"
        "\"tank_capacity\": 400,"
        "\"fuel_type\": \"diesel\","
        "\"axle_count\": 5,"
        "\"weight_limit\": 40000"
        "}",
        arena);
    ASSERT_EQ(sh_json_as_int(sh_json_get(v, "tank_capacity"), 0), 400);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(v, "fuel_type"), ""), "diesel");
    ASSERT_EQ(sh_json_as_int(sh_json_get(v, "axle_count"), 0), 5);
    sh_arena_free(arena);
}

TEST(parse_constraints)
{
    SHArena *arena = sh_arena_create(2048);
    ShJsonValue *v = parse_ok(
        "{"
        "\"min_fuel\": 20,"
        "\"min_purchase\": 10,"
        "\"stop_cost\": 5.0,"
        "\"max_detour\": 50"
        "}",
        arena);
    ASSERT_EQ(sh_json_as_int(sh_json_get(v, "min_fuel"), 0), 20);
    ASSERT_EQ(sh_json_as_int(sh_json_get(v, "min_purchase"), 0), 10);
    ASSERT_NEAR(sh_json_as_double(sh_json_get(v, "stop_cost"), 0), 5.0, 0.01);
    sh_arena_free(arena);
}

TEST(parse_empty_stations)
{
    SHArena *arena = sh_arena_create(2048);
    ShJsonValue *v = parse_ok("{\"stations\": []}", arena);
    ShJsonValue *stations = sh_json_get(v, "stations");
    ASSERT_EQ(sh_json_array_len(stations), 0);
    sh_arena_free(arena);
}

TEST(parse_unicode_station_name)
{
    SHArena *arena = sh_arena_create(2048);
    /* Station with Hungarian name */
    ShJsonValue *v = parse_ok(
        "{\"name\": \"Shell Budapest K\\u00f6r\\u00fat\", \"price\": 1.45}",
        arena);
    /* \u00f6 = ö, \u00fa = ú → "Körút" */
    const char *name = sh_json_as_string(sh_json_get(v, "name"), "");
    ASSERT(strstr(name, "Shell") != NULL);
    sh_arena_free(arena);
}

/* ============================================================================
 * 8. Additional Edge Cases (5 tests)
 * ============================================================================ */

TEST(whitespace_handling)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("  \t\n{ \r\n  \"a\" \t:\t 1\n } \n ", arena);
    ASSERT_EQ(sh_json_as_int(sh_json_get(v, "a"), 0), 1);
    sh_arena_free(arena);
}

TEST(nested_arrays)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("[[1, 2], [3, 4], [5, 6]]", arena);
    ASSERT_EQ(sh_json_array_len(v), 3);
    ShJsonValue *inner = sh_json_array_get(v, 1);
    ASSERT_EQ(sh_json_array_len(inner), 2);
    ASSERT_EQ(sh_json_as_int(sh_json_array_get(inner, 0), 0), 3);
    sh_arena_free(arena);
}

TEST(mixed_array)
{
    SHArena *arena = sh_arena_create(1024);
    ShJsonValue *v = parse_ok("[null, true, 42, \"hello\", [], {}]", arena);
    ASSERT_EQ(sh_json_array_len(v), 6);
    ASSERT(sh_json_is_null(sh_json_array_get(v, 0)));
    ASSERT_EQ(sh_json_as_bool(sh_json_array_get(v, 1), false), true);
    ASSERT_EQ(sh_json_as_int(sh_json_array_get(v, 2), 0), 42);
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(v, 3), ""), "hello");
    ASSERT_EQ(sh_json_type(sh_json_array_get(v, 4)), SH_JSON_ARRAY);
    ASSERT_EQ(sh_json_type(sh_json_array_get(v, 5)), SH_JSON_OBJECT);
    sh_arena_free(arena);
}

TEST(trailing_content)
{
    SHArena *arena = sh_arena_create(1024);
    /* Valid JSON followed by garbage should fail */
    parse_err("42 extra", SH_JSON_ERR_SYNTAX, arena);
    sh_arena_free(arena);
}

TEST(as_int_overflow)
{
    SHArena *arena = sh_arena_create(1024);
    /* Large number should clamp to INT_MAX */
    ShJsonValue *v = parse_ok("9999999999999", arena);
    ASSERT_EQ(sh_json_as_int(v, 0), 2147483647);
    /* Negative large number should clamp to INT_MIN */
    ShJsonValue *v2 = parse_ok("-9999999999999", arena);
    ASSERT_EQ(sh_json_as_int(v2, 0), -2147483648);
    sh_arena_free(arena);
}

/* ============================================================================
 * JSON Writer Tests
 * ============================================================================ */

/* Buffer-based writer for testing */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} WriterBuf;

static int test_write_fn(void *ctx, const char *data, size_t len) {
    WriterBuf *wb = (WriterBuf *)ctx;
    if (wb->len + len >= wb->cap) return -1;  /* Buffer overflow */
    memcpy(wb->buf + wb->len, data, len);
    wb->len += len;
    wb->buf[wb->len] = '\0';
    return 0;
}

static void writer_buf_init(WriterBuf *wb, char *buf, size_t cap) {
    wb->buf = buf;
    wb->len = 0;
    wb->cap = cap;
    buf[0] = '\0';
}

TEST(write_null)
{
    char buf[64];
    WriterBuf wb;
    writer_buf_init(&wb, buf, sizeof(buf));

    ShJsonWriter w;
    sh_json_writer_init(&w, test_write_fn, &wb);
    ASSERT_EQ(sh_json_write_null(&w), 0);
    ASSERT_STREQ(buf, "null");
}

TEST(write_bool_true)
{
    char buf[64];
    WriterBuf wb;
    writer_buf_init(&wb, buf, sizeof(buf));

    ShJsonWriter w;
    sh_json_writer_init(&w, test_write_fn, &wb);
    ASSERT_EQ(sh_json_write_bool(&w, true), 0);
    ASSERT_STREQ(buf, "true");
}

TEST(write_bool_false)
{
    char buf[64];
    WriterBuf wb;
    writer_buf_init(&wb, buf, sizeof(buf));

    ShJsonWriter w;
    sh_json_writer_init(&w, test_write_fn, &wb);
    ASSERT_EQ(sh_json_write_bool(&w, false), 0);
    ASSERT_STREQ(buf, "false");
}

TEST(write_int_positive)
{
    char buf[64];
    WriterBuf wb;
    writer_buf_init(&wb, buf, sizeof(buf));

    ShJsonWriter w;
    sh_json_writer_init(&w, test_write_fn, &wb);
    ASSERT_EQ(sh_json_write_int(&w, 42), 0);
    ASSERT_STREQ(buf, "42");
}

TEST(write_int_negative)
{
    char buf[64];
    WriterBuf wb;
    writer_buf_init(&wb, buf, sizeof(buf));

    ShJsonWriter w;
    sh_json_writer_init(&w, test_write_fn, &wb);
    ASSERT_EQ(sh_json_write_int(&w, -123), 0);
    ASSERT_STREQ(buf, "-123");
}

TEST(write_int_zero)
{
    char buf[64];
    WriterBuf wb;
    writer_buf_init(&wb, buf, sizeof(buf));

    ShJsonWriter w;
    sh_json_writer_init(&w, test_write_fn, &wb);
    ASSERT_EQ(sh_json_write_int(&w, 0), 0);
    ASSERT_STREQ(buf, "0");
}

TEST(write_double)
{
    char buf[64];
    WriterBuf wb;
    writer_buf_init(&wb, buf, sizeof(buf));

    ShJsonWriter w;
    sh_json_writer_init(&w, test_write_fn, &wb);
    ASSERT_EQ(sh_json_write_double(&w, 3.14159), 0);
    /* %g with precision 6 gives 3.14159 */
    ASSERT(strstr(buf, "3.14159") != NULL);
}

TEST(write_double_precision)
{
    char buf[64];
    WriterBuf wb;
    writer_buf_init(&wb, buf, sizeof(buf));

    ShJsonWriter w;
    sh_json_writer_init(&w, test_write_fn, &wb);
    ASSERT_EQ(sh_json_write_double_fmt(&w, 1.23456789, 3), 0);
    ASSERT_STREQ(buf, "1.23");
}

TEST(write_double_nan)
{
    char buf[64];
    WriterBuf wb;
    writer_buf_init(&wb, buf, sizeof(buf));

    ShJsonWriter w;
    sh_json_writer_init(&w, test_write_fn, &wb);
    double nan_val = 0.0 / 0.0;
    ASSERT_EQ(sh_json_write_double(&w, nan_val), 0);
    ASSERT_STREQ(buf, "null");  /* NaN becomes null */
}

TEST(write_double_inf)
{
    char buf[64];
    WriterBuf wb;
    writer_buf_init(&wb, buf, sizeof(buf));

    ShJsonWriter w;
    sh_json_writer_init(&w, test_write_fn, &wb);
    double inf_val = 1.0 / 0.0;
    ASSERT_EQ(sh_json_write_double(&w, inf_val), 0);
    ASSERT_STREQ(buf, "null");  /* Infinity becomes null */
}

TEST(write_string_simple)
{
    char buf[64];
    WriterBuf wb;
    writer_buf_init(&wb, buf, sizeof(buf));

    ShJsonWriter w;
    sh_json_writer_init(&w, test_write_fn, &wb);
    ASSERT_EQ(sh_json_write_string(&w, "hello"), 0);
    ASSERT_STREQ(buf, "\"hello\"");
}

TEST(write_string_escapes)
{
    char buf[128];
    WriterBuf wb;
    writer_buf_init(&wb, buf, sizeof(buf));

    ShJsonWriter w;
    sh_json_writer_init(&w, test_write_fn, &wb);
    ASSERT_EQ(sh_json_write_string(&w, "line1\nline2\ttab\"quote\\slash"), 0);
    ASSERT_STREQ(buf, "\"line1\\nline2\\ttab\\\"quote\\\\slash\"");
}

TEST(write_string_null)
{
    char buf[64];
    WriterBuf wb;
    writer_buf_init(&wb, buf, sizeof(buf));

    ShJsonWriter w;
    sh_json_writer_init(&w, test_write_fn, &wb);
    ASSERT_EQ(sh_json_write_string(&w, NULL), 0);
    ASSERT_STREQ(buf, "null");  /* NULL string becomes null */
}

TEST(write_empty_object)
{
    char buf[64];
    WriterBuf wb;
    writer_buf_init(&wb, buf, sizeof(buf));

    ShJsonWriter w;
    sh_json_writer_init(&w, test_write_fn, &wb);
    ASSERT_EQ(sh_json_write_object_start(&w), 0);
    ASSERT_EQ(sh_json_write_object_end(&w), 0);
    ASSERT_STREQ(buf, "{}");
}

TEST(write_object_single)
{
    char buf[64];
    WriterBuf wb;
    writer_buf_init(&wb, buf, sizeof(buf));

    ShJsonWriter w;
    sh_json_writer_init(&w, test_write_fn, &wb);
    ASSERT_EQ(sh_json_write_object_start(&w), 0);
    ASSERT_EQ(sh_json_write_key(&w, "name"), 0);
    ASSERT_EQ(sh_json_write_string(&w, "test"), 0);
    ASSERT_EQ(sh_json_write_object_end(&w), 0);
    ASSERT_STREQ(buf, "{\"name\":\"test\"}");
}

TEST(write_object_multiple)
{
    char buf[128];
    WriterBuf wb;
    writer_buf_init(&wb, buf, sizeof(buf));

    ShJsonWriter w;
    sh_json_writer_init(&w, test_write_fn, &wb);
    ASSERT_EQ(sh_json_write_object_start(&w), 0);
    ASSERT_EQ(sh_json_write_key(&w, "a"), 0);
    ASSERT_EQ(sh_json_write_int(&w, 1), 0);
    ASSERT_EQ(sh_json_write_key(&w, "b"), 0);
    ASSERT_EQ(sh_json_write_int(&w, 2), 0);
    ASSERT_EQ(sh_json_write_object_end(&w), 0);
    ASSERT_STREQ(buf, "{\"a\":1,\"b\":2}");
}

TEST(write_empty_array)
{
    char buf[64];
    WriterBuf wb;
    writer_buf_init(&wb, buf, sizeof(buf));

    ShJsonWriter w;
    sh_json_writer_init(&w, test_write_fn, &wb);
    ASSERT_EQ(sh_json_write_array_start(&w), 0);
    ASSERT_EQ(sh_json_write_array_end(&w), 0);
    ASSERT_STREQ(buf, "[]");
}

TEST(write_array_numbers)
{
    char buf[64];
    WriterBuf wb;
    writer_buf_init(&wb, buf, sizeof(buf));

    ShJsonWriter w;
    sh_json_writer_init(&w, test_write_fn, &wb);
    ASSERT_EQ(sh_json_write_array_start(&w), 0);
    ASSERT_EQ(sh_json_write_int(&w, 1), 0);
    ASSERT_EQ(sh_json_write_int(&w, 2), 0);
    ASSERT_EQ(sh_json_write_int(&w, 3), 0);
    ASSERT_EQ(sh_json_write_array_end(&w), 0);
    ASSERT_STREQ(buf, "[1,2,3]");
}

TEST(write_nested_object)
{
    char buf[128];
    WriterBuf wb;
    writer_buf_init(&wb, buf, sizeof(buf));

    ShJsonWriter w;
    sh_json_writer_init(&w, test_write_fn, &wb);
    ASSERT_EQ(sh_json_write_object_start(&w), 0);
    ASSERT_EQ(sh_json_write_key(&w, "outer"), 0);
    ASSERT_EQ(sh_json_write_object_start(&w), 0);
    ASSERT_EQ(sh_json_write_key(&w, "inner"), 0);
    ASSERT_EQ(sh_json_write_int(&w, 42), 0);
    ASSERT_EQ(sh_json_write_object_end(&w), 0);
    ASSERT_EQ(sh_json_write_object_end(&w), 0);
    ASSERT_STREQ(buf, "{\"outer\":{\"inner\":42}}");
}

TEST(write_nested_array)
{
    char buf[128];
    WriterBuf wb;
    writer_buf_init(&wb, buf, sizeof(buf));

    ShJsonWriter w;
    sh_json_writer_init(&w, test_write_fn, &wb);
    ASSERT_EQ(sh_json_write_array_start(&w), 0);
    ASSERT_EQ(sh_json_write_array_start(&w), 0);
    ASSERT_EQ(sh_json_write_int(&w, 1), 0);
    ASSERT_EQ(sh_json_write_int(&w, 2), 0);
    ASSERT_EQ(sh_json_write_array_end(&w), 0);
    ASSERT_EQ(sh_json_write_array_start(&w), 0);
    ASSERT_EQ(sh_json_write_int(&w, 3), 0);
    ASSERT_EQ(sh_json_write_int(&w, 4), 0);
    ASSERT_EQ(sh_json_write_array_end(&w), 0);
    ASSERT_EQ(sh_json_write_array_end(&w), 0);
    ASSERT_STREQ(buf, "[[1,2],[3,4]]");
}

TEST(write_mixed)
{
    char buf[256];
    WriterBuf wb;
    writer_buf_init(&wb, buf, sizeof(buf));

    ShJsonWriter w;
    sh_json_writer_init(&w, test_write_fn, &wb);
    ASSERT_EQ(sh_json_write_object_start(&w), 0);
    ASSERT_EQ(sh_json_write_key(&w, "status"), 0);
    ASSERT_EQ(sh_json_write_string(&w, "ok"), 0);
    ASSERT_EQ(sh_json_write_key(&w, "data"), 0);
    ASSERT_EQ(sh_json_write_array_start(&w), 0);
    ASSERT_EQ(sh_json_write_int(&w, 1), 0);
    ASSERT_EQ(sh_json_write_null(&w), 0);
    ASSERT_EQ(sh_json_write_bool(&w, true), 0);
    ASSERT_EQ(sh_json_write_array_end(&w), 0);
    ASSERT_EQ(sh_json_write_object_end(&w), 0);
    ASSERT_STREQ(buf, "{\"status\":\"ok\",\"data\":[1,null,true]}");
}

TEST(write_kv_helpers)
{
    char buf[256];
    WriterBuf wb;
    writer_buf_init(&wb, buf, sizeof(buf));

    ShJsonWriter w;
    sh_json_writer_init(&w, test_write_fn, &wb);
    ASSERT_EQ(sh_json_write_object_start(&w), 0);
    ASSERT_EQ(sh_json_write_kv_string(&w, "name", "test"), 0);
    ASSERT_EQ(sh_json_write_kv_int(&w, "count", 42), 0);
    ASSERT_EQ(sh_json_write_kv_double(&w, "value", 3.14), 0);
    ASSERT_EQ(sh_json_write_kv_bool(&w, "active", true), 0);
    ASSERT_EQ(sh_json_write_kv_null(&w, "empty"), 0);
    ASSERT_EQ(sh_json_write_object_end(&w), 0);
    ASSERT(strstr(buf, "\"name\":\"test\"") != NULL);
    ASSERT(strstr(buf, "\"count\":42") != NULL);
    ASSERT(strstr(buf, "\"active\":true") != NULL);
    ASSERT(strstr(buf, "\"empty\":null") != NULL);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
    printf("\n=== JSON Parser Tests ===\n\n");

    printf("Basic Parsing:\n");
    RUN_TEST(parse_null);
    RUN_TEST(parse_true);
    RUN_TEST(parse_false);
    RUN_TEST(parse_integer);
    RUN_TEST(parse_negative_integer);
    RUN_TEST(parse_float);
    RUN_TEST(parse_exponent);
    RUN_TEST(parse_negative_exponent);
    RUN_TEST(parse_empty_string);
    RUN_TEST(parse_simple_string);
    RUN_TEST(parse_string_with_spaces);
    RUN_TEST(parse_empty_array);
    RUN_TEST(parse_empty_object);
    RUN_TEST(parse_array_of_numbers);
    RUN_TEST(parse_nested_object);

    printf("\nString Escapes:\n");
    RUN_TEST(escape_quote);
    RUN_TEST(escape_backslash);
    RUN_TEST(escape_slash);
    RUN_TEST(escape_backspace);
    RUN_TEST(escape_formfeed);
    RUN_TEST(escape_newline);
    RUN_TEST(escape_carriage_return);
    RUN_TEST(escape_tab);
    RUN_TEST(escape_unicode_ascii);
    RUN_TEST(escape_unicode_euro);
    RUN_TEST(escape_unicode_emoji);
    RUN_TEST(escape_mixed);

    printf("\nNumber Edge Cases:\n");
    RUN_TEST(number_zero);
    RUN_TEST(number_negative_zero);
    RUN_TEST(number_zero_point_five);
    RUN_TEST(number_large_integer);
    RUN_TEST(number_small_float);
    RUN_TEST(number_max_double);
    RUN_TEST(number_scientific_upper);
    RUN_TEST(number_scientific_lower);
    RUN_TEST(number_scientific_positive_exp);
    RUN_TEST(number_scientific_negative_exp);

    printf("\nError Cases:\n");
    RUN_TEST(err_null_input);
    RUN_TEST(err_empty_input);
    RUN_TEST(err_just_whitespace);
    RUN_TEST(err_trailing_comma_array);
    RUN_TEST(err_trailing_comma_object);
    RUN_TEST(err_missing_colon);
    RUN_TEST(err_missing_value);
    RUN_TEST(err_unquoted_key);
    RUN_TEST(err_unterminated_string);
    RUN_TEST(err_newline_in_string);
    RUN_TEST(err_invalid_escape);
    RUN_TEST(err_invalid_unicode_escape);
    RUN_TEST(err_truncated_unicode);
    RUN_TEST(err_leading_plus);
    RUN_TEST(err_leading_dot);
    RUN_TEST(err_trailing_dot);
    RUN_TEST(err_leading_zero);
    RUN_TEST(err_depth_exceeded);

    printf("\nValue Access:\n");
    RUN_TEST(type_null);
    RUN_TEST(type_bool);
    RUN_TEST(type_number);
    RUN_TEST(type_string);
    RUN_TEST(type_array);
    RUN_TEST(type_object);
    RUN_TEST(as_bool_null_value);
    RUN_TEST(as_double_null_value);
    RUN_TEST(as_string_null_value);
    RUN_TEST(array_get_null_value);
    RUN_TEST(object_get_null_value);
    RUN_TEST(as_bool_from_number);
    RUN_TEST(as_double_from_string);
    RUN_TEST(as_string_from_number);
    RUN_TEST(array_len);
    RUN_TEST(array_get_valid);
    RUN_TEST(array_get_out_of_bounds);
    RUN_TEST(object_len);
    RUN_TEST(object_get_exists);
    RUN_TEST(object_get_missing);

    printf("\nPath Access:\n");
    RUN_TEST(path_simple_key);
    RUN_TEST(path_nested_keys);
    RUN_TEST(path_array_index);
    RUN_TEST(path_nested_array);
    RUN_TEST(path_missing_key);
    RUN_TEST(path_invalid_index);
    RUN_TEST(path_non_array_index);
    RUN_TEST(path_non_object_key);
    RUN_TEST(path_empty);
    RUN_TEST(path_null_value);

    printf("\nFuelWise Integration:\n");
    RUN_TEST(parse_station);
    RUN_TEST(parse_stations_array);
    RUN_TEST(parse_polyline);
    RUN_TEST(parse_solve_request);
    RUN_TEST(parse_optimize_request);
    RUN_TEST(parse_segment);
    RUN_TEST(parse_vehicle_config);
    RUN_TEST(parse_constraints);
    RUN_TEST(parse_empty_stations);
    RUN_TEST(parse_unicode_station_name);

    printf("\nAdditional Edge Cases:\n");
    RUN_TEST(whitespace_handling);
    RUN_TEST(nested_arrays);
    RUN_TEST(mixed_array);
    RUN_TEST(trailing_content);
    RUN_TEST(as_int_overflow);

    printf("\nJSON Writer:\n");
    RUN_TEST(write_null);
    RUN_TEST(write_bool_true);
    RUN_TEST(write_bool_false);
    RUN_TEST(write_int_positive);
    RUN_TEST(write_int_negative);
    RUN_TEST(write_int_zero);
    RUN_TEST(write_double);
    RUN_TEST(write_double_precision);
    RUN_TEST(write_double_nan);
    RUN_TEST(write_double_inf);
    RUN_TEST(write_string_simple);
    RUN_TEST(write_string_escapes);
    RUN_TEST(write_string_null);
    RUN_TEST(write_empty_object);
    RUN_TEST(write_object_single);
    RUN_TEST(write_object_multiple);
    RUN_TEST(write_empty_array);
    RUN_TEST(write_array_numbers);
    RUN_TEST(write_nested_object);
    RUN_TEST(write_nested_array);
    RUN_TEST(write_mixed);
    RUN_TEST(write_kv_helpers);

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);

    return tests_run == tests_passed ? 0 : 1;
}
