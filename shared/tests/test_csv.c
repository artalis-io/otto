/*
 * test_csv.c - Unit tests for CSV pull parser
 */

#include "sh_csv.h"
#include "sh_arena.h"
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
        printf("[FAIL]\n    Assertion failed: %s\n", #cond); \
        exit(1); \
    } \
} while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_STREQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("[FAIL]\n    Expected: \"%s\"\n    Got:      \"%s\"\n", (b), (a)); \
        exit(1); \
    } \
} while (0)

#define ASSERT_FIELD(r, tok, expected) do { \
    ASSERT_EQ(sh_csv_next(&(r), &(tok)), SH_CSV_TOKEN_FIELD); \
    ASSERT((tok).data != NULL); \
    ASSERT_STREQ((tok).data, expected); \
} while (0)

#define ASSERT_ROW_END(r, tok) \
    ASSERT_EQ(sh_csv_next(&(r), &(tok)), SH_CSV_TOKEN_ROW_END)

#define ASSERT_EOF(r, tok) \
    ASSERT_EQ(sh_csv_next(&(r), &(tok)), SH_CSV_TOKEN_EOF)

/* ============================================================================
 * Tests
 * ============================================================================ */

TEST(csv_default_opts)
{
    ShCsvOpts opts;
    sh_csv_opts_default(&opts);
    ASSERT_EQ(opts.delimiter, 0);
    ASSERT_EQ(opts.quote, '"');
    ASSERT_EQ(opts.has_header, 0);
    ASSERT_EQ(opts.skip_empty_rows, 1);
    ASSERT_EQ(opts.trim_fields, 0);
    ASSERT_EQ(opts.max_field_len, 32768);
    ASSERT_EQ(opts.max_columns, 1024);
}

TEST(csv_null_inputs)
{
    ShCsvReader r;
    SHArena *arena = sh_arena_create(4096);

    /* NULL reader */
    ASSERT_EQ(sh_csv_init(NULL, "a", 1, NULL, arena), SH_CSV_ERR_NULL);

    /* NULL arena */
    ASSERT_EQ(sh_csv_init(&r, "a", 1, NULL, NULL), SH_CSV_ERR_NULL);

    /* NULL data is OK (treated as empty) */
    ASSERT_EQ(sh_csv_init(&r, NULL, 0, NULL, arena), SH_CSV_OK);
    ShCsvToken tok;
    ASSERT_EOF(r, tok);

    /* NULL reader for accessors */
    ASSERT_EQ(sh_csv_row_number(NULL), -1);
    ASSERT_EQ(sh_csv_column_number(NULL), -1);

    sh_arena_free(arena);
}

TEST(csv_simple_2x2)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    const char *csv = "a,b\nc,d\n";

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), NULL, arena), SH_CSV_OK);

    ASSERT_FIELD(r, tok, "a");
    ASSERT_FIELD(r, tok, "b");
    ASSERT_ROW_END(r, tok);
    ASSERT_FIELD(r, tok, "c");
    ASSERT_FIELD(r, tok, "d");
    ASSERT_ROW_END(r, tok);
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_quoted_with_commas)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    const char *csv = "name,address\n\"Smith\",\"123 Main, Apt 4\"\n";

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), NULL, arena), SH_CSV_OK);

    ASSERT_FIELD(r, tok, "name");
    ASSERT_FIELD(r, tok, "address");
    ASSERT_ROW_END(r, tok);
    ASSERT_FIELD(r, tok, "Smith");
    ASSERT_FIELD(r, tok, "123 Main, Apt 4");
    ASSERT_ROW_END(r, tok);
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_quoted_with_newlines)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    const char *csv = "a,\"line1\nline2\"\nb,c\n";

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), NULL, arena), SH_CSV_OK);

    ASSERT_FIELD(r, tok, "a");
    ASSERT_FIELD(r, tok, "line1\nline2");
    ASSERT_ROW_END(r, tok);
    ASSERT_FIELD(r, tok, "b");
    ASSERT_FIELD(r, tok, "c");
    ASSERT_ROW_END(r, tok);
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_escaped_quotes)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    const char *csv = "\"she said \"\"hello\"\"\",b\n";

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), NULL, arena), SH_CSV_OK);

    ASSERT_FIELD(r, tok, "she said \"hello\"");
    ASSERT_FIELD(r, tok, "b");
    ASSERT_ROW_END(r, tok);
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_tab_auto_detect)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    const char *csv = "a\tb\tc\n1\t2\t3\n";

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), NULL, arena), SH_CSV_OK);
    ASSERT_EQ(r.delimiter, '\t');

    ASSERT_FIELD(r, tok, "a");
    ASSERT_FIELD(r, tok, "b");
    ASSERT_FIELD(r, tok, "c");
    ASSERT_ROW_END(r, tok);
    ASSERT_FIELD(r, tok, "1");
    ASSERT_FIELD(r, tok, "2");
    ASSERT_FIELD(r, tok, "3");
    ASSERT_ROW_END(r, tok);
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_semicolon_auto_detect)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    const char *csv = "a;b;c\n1;2;3\n";

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), NULL, arena), SH_CSV_OK);
    ASSERT_EQ(r.delimiter, ';');

    ASSERT_FIELD(r, tok, "a");
    ASSERT_FIELD(r, tok, "b");
    ASSERT_FIELD(r, tok, "c");
    ASSERT_ROW_END(r, tok);

    sh_arena_free(arena);
}

TEST(csv_utf8_bom)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    /* EF BB BF followed by "a,b\n" */
    const char csv[] = "\xEF\xBB\xBF" "a,b\nc,d\n";
    size_t csv_len = sizeof(csv) - 1;

    ASSERT_EQ(sh_csv_init(&r, csv, csv_len, NULL, arena), SH_CSV_OK);

    ASSERT_FIELD(r, tok, "a");
    ASSERT_FIELD(r, tok, "b");
    ASSERT_ROW_END(r, tok);
    ASSERT_FIELD(r, tok, "c");
    ASSERT_FIELD(r, tok, "d");
    ASSERT_ROW_END(r, tok);
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_empty_fields)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    const char *csv = ",a,,b,\n";

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), NULL, arena), SH_CSV_OK);

    ASSERT_FIELD(r, tok, "");
    ASSERT_FIELD(r, tok, "a");
    ASSERT_FIELD(r, tok, "");
    ASSERT_FIELD(r, tok, "b");
    ASSERT_FIELD(r, tok, "");
    ASSERT_ROW_END(r, tok);
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_trim_whitespace)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    ShCsvOpts opts;
    sh_csv_opts_default(&opts);
    opts.trim_fields = 1;
    opts.delimiter = ',';

    const char *csv = "  hello  , world ,  foo  \n";

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), &opts, arena), SH_CSV_OK);

    ASSERT_FIELD(r, tok, "hello");
    ASSERT_FIELD(r, tok, "world");
    ASSERT_FIELD(r, tok, "foo");
    ASSERT_ROW_END(r, tok);
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_skip_empty_rows)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    const char *csv = "a,b\n\n\nc,d\n";

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), NULL, arena), SH_CSV_OK);

    ASSERT_FIELD(r, tok, "a");
    ASSERT_FIELD(r, tok, "b");
    ASSERT_ROW_END(r, tok);
    /* Empty rows should be skipped */
    ASSERT_FIELD(r, tok, "c");
    ASSERT_FIELD(r, tok, "d");
    ASSERT_ROW_END(r, tok);
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_crlf_line_endings)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    const char *csv = "a,b\r\nc,d\r\n";

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), NULL, arena), SH_CSV_OK);

    ASSERT_FIELD(r, tok, "a");
    ASSERT_FIELD(r, tok, "b");
    ASSERT_ROW_END(r, tok);
    ASSERT_FIELD(r, tok, "c");
    ASSERT_FIELD(r, tok, "d");
    ASSERT_ROW_END(r, tok);
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_single_column)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    const char *csv = "alpha\nbeta\ngamma\n";

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), NULL, arena), SH_CSV_OK);

    ASSERT_FIELD(r, tok, "alpha");
    ASSERT_ROW_END(r, tok);
    ASSERT_FIELD(r, tok, "beta");
    ASSERT_ROW_END(r, tok);
    ASSERT_FIELD(r, tok, "gamma");
    ASSERT_ROW_END(r, tok);
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_large_field)
{
    SHArena *arena = sh_arena_create(65536);
    ShCsvReader r;
    ShCsvToken tok;
    ShCsvOpts opts;
    sh_csv_opts_default(&opts);
    opts.max_field_len = 100;
    opts.delimiter = ',';

    /* Build a field of exactly 100 chars */
    char csv[256];
    memset(csv, 'x', 100);
    csv[100] = '\n';
    csv[101] = '\0';

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), &opts, arena), SH_CSV_OK);

    /* Field of 100 chars should succeed (equal to max) */
    ASSERT_EQ(sh_csv_next(&r, &tok), SH_CSV_TOKEN_FIELD);
    ASSERT_EQ(tok.len, 100);

    sh_arena_free(arena);
}

TEST(csv_field_exceeds_max)
{
    SHArena *arena = sh_arena_create(65536);
    ShCsvReader r;
    ShCsvToken tok;
    ShCsvOpts opts;
    sh_csv_opts_default(&opts);
    opts.max_field_len = 10;
    opts.delimiter = ',';

    const char *csv = "short,this_field_is_way_too_long\n";

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), &opts, arena), SH_CSV_OK);

    /* First field OK */
    ASSERT_FIELD(r, tok, "short");

    /* Second field exceeds max - should get EOF (error) */
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_max_columns_limit)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    ShCsvOpts opts;
    sh_csv_opts_default(&opts);
    opts.max_columns = 3;
    opts.delimiter = ',';

    const char *csv = "a,b,c,d,e\n";

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), &opts, arena), SH_CSV_OK);

    /* Should get 3 fields, then EOF due to column limit */
    ASSERT_FIELD(r, tok, "a");
    ASSERT_FIELD(r, tok, "b");
    ASSERT_FIELD(r, tok, "c");
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_explicit_delimiter)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    ShCsvOpts opts;
    sh_csv_opts_default(&opts);
    opts.delimiter = '|';

    const char *csv = "a|b|c\n1|2|3\n";

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), &opts, arena), SH_CSV_OK);
    ASSERT_EQ(r.delimiter, '|');

    ASSERT_FIELD(r, tok, "a");
    ASSERT_FIELD(r, tok, "b");
    ASSERT_FIELD(r, tok, "c");
    ASSERT_ROW_END(r, tok);
    ASSERT_FIELD(r, tok, "1");
    ASSERT_FIELD(r, tok, "2");
    ASSERT_FIELD(r, tok, "3");
    ASSERT_ROW_END(r, tok);
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_mixed_quoted_unquoted)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    const char *csv = "plain,\"quoted\",another\n";

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), NULL, arena), SH_CSV_OK);

    ASSERT_FIELD(r, tok, "plain");
    ASSERT_FIELD(r, tok, "quoted");
    ASSERT_FIELD(r, tok, "another");
    ASSERT_ROW_END(r, tok);
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_trailing_newline)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;

    /* With trailing newline */
    const char *csv1 = "a,b\n";
    ASSERT_EQ(sh_csv_init(&r, csv1, strlen(csv1), NULL, arena), SH_CSV_OK);

    ASSERT_FIELD(r, tok, "a");
    ASSERT_FIELD(r, tok, "b");
    ASSERT_ROW_END(r, tok);
    ASSERT_EOF(r, tok);

    /* Without trailing newline */
    sh_arena_reset(arena);
    const char *csv2 = "a,b";
    ASSERT_EQ(sh_csv_init(&r, csv2, strlen(csv2), NULL, arena), SH_CSV_OK);

    ASSERT_FIELD(r, tok, "a");
    ASSERT_FIELD(r, tok, "b");
    ASSERT_ROW_END(r, tok);
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_empty_input)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;

    ASSERT_EQ(sh_csv_init(&r, "", 0, NULL, arena), SH_CSV_OK);
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_row_column_tracking)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    const char *csv = "a,b,c\nd,e,f\n";

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), NULL, arena), SH_CSV_OK);

    /* Row 0 */
    ASSERT_EQ(sh_csv_row_number(&r), 0);

    ASSERT_FIELD(r, tok, "a");
    ASSERT_EQ(sh_csv_row_number(&r), 0);
    ASSERT_EQ(sh_csv_column_number(&r), 1); /* col incremented after parse */

    ASSERT_FIELD(r, tok, "b");
    ASSERT_EQ(sh_csv_column_number(&r), 2);

    ASSERT_FIELD(r, tok, "c");
    ASSERT_EQ(sh_csv_column_number(&r), 3);

    ASSERT_ROW_END(r, tok);
    ASSERT_EQ(sh_csv_row_number(&r), 1);
    ASSERT_EQ(sh_csv_column_number(&r), 0);

    /* Row 1 */
    ASSERT_FIELD(r, tok, "d");
    ASSERT_EQ(sh_csv_row_number(&r), 1);

    ASSERT_FIELD(r, tok, "e");
    ASSERT_FIELD(r, tok, "f");
    ASSERT_ROW_END(r, tok);
    ASSERT_EQ(sh_csv_row_number(&r), 2);

    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_quoted_empty)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    const char *csv = "\"\",a,\"\"\n";

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), NULL, arena), SH_CSV_OK);

    ASSERT_FIELD(r, tok, "");
    ASSERT_FIELD(r, tok, "a");
    ASSERT_FIELD(r, tok, "");
    ASSERT_ROW_END(r, tok);
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_pipe_auto_detect)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    const char *csv = "a|b|c\n1|2|3\n";

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), NULL, arena), SH_CSV_OK);
    ASSERT_EQ(r.delimiter, '|');

    ASSERT_FIELD(r, tok, "a");
    ASSERT_FIELD(r, tok, "b");
    ASSERT_FIELD(r, tok, "c");
    ASSERT_ROW_END(r, tok);

    sh_arena_free(arena);
}

TEST(csv_no_skip_empty_rows)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    ShCsvOpts opts;
    sh_csv_opts_default(&opts);
    opts.skip_empty_rows = 0;
    opts.delimiter = ',';

    const char *csv = "a\n\nb\n";

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), &opts, arena), SH_CSV_OK);

    ASSERT_FIELD(r, tok, "a");
    ASSERT_ROW_END(r, tok);

    /* Empty row produces a single empty field */
    ASSERT_FIELD(r, tok, "");
    ASSERT_ROW_END(r, tok);

    ASSERT_FIELD(r, tok, "b");
    ASSERT_ROW_END(r, tok);
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_only_bom)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };

    ASSERT_EQ(sh_csv_init(&r, (const char *)bom, sizeof(bom), NULL, arena), SH_CSV_OK);
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_quoted_field_with_crlf)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    const char *csv = "\"line1\r\nline2\",b\r\n";

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), NULL, arena), SH_CSV_OK);

    ASSERT_FIELD(r, tok, "line1\r\nline2");
    ASSERT_FIELD(r, tok, "b");
    ASSERT_ROW_END(r, tok);
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_single_field_no_newline)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    const char *csv = "hello";

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), NULL, arena), SH_CSV_OK);

    ASSERT_FIELD(r, tok, "hello");
    ASSERT_ROW_END(r, tok);
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_trim_does_not_affect_quoted)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    ShCsvOpts opts;
    sh_csv_opts_default(&opts);
    opts.trim_fields = 1;
    opts.delimiter = ',';

    const char *csv = "  trimmed  ,\"  not trimmed  \"\n";

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), &opts, arena), SH_CSV_OK);

    ASSERT_FIELD(r, tok, "trimmed");
    /* Quoted fields should preserve whitespace even with trim on */
    ASSERT_FIELD(r, tok, "  not trimmed  ");
    ASSERT_ROW_END(r, tok);
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

TEST(csv_multiple_escaped_quotes)
{
    SHArena *arena = sh_arena_create(4096);
    ShCsvReader r;
    ShCsvToken tok;
    const char *csv = "\"a\"\"b\"\"c\"\n";

    ASSERT_EQ(sh_csv_init(&r, csv, strlen(csv), NULL, arena), SH_CSV_OK);

    ASSERT_FIELD(r, tok, "a\"b\"c");
    ASSERT_ROW_END(r, tok);
    ASSERT_EOF(r, tok);

    sh_arena_free(arena);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
    printf("\nCSV Parser Tests:\n");

    RUN_TEST(csv_default_opts);
    RUN_TEST(csv_null_inputs);
    RUN_TEST(csv_simple_2x2);
    RUN_TEST(csv_quoted_with_commas);
    RUN_TEST(csv_quoted_with_newlines);
    RUN_TEST(csv_escaped_quotes);
    RUN_TEST(csv_tab_auto_detect);
    RUN_TEST(csv_semicolon_auto_detect);
    RUN_TEST(csv_utf8_bom);
    RUN_TEST(csv_empty_fields);
    RUN_TEST(csv_trim_whitespace);
    RUN_TEST(csv_skip_empty_rows);
    RUN_TEST(csv_crlf_line_endings);
    RUN_TEST(csv_single_column);
    RUN_TEST(csv_large_field);
    RUN_TEST(csv_field_exceeds_max);
    RUN_TEST(csv_max_columns_limit);
    RUN_TEST(csv_explicit_delimiter);
    RUN_TEST(csv_mixed_quoted_unquoted);
    RUN_TEST(csv_trailing_newline);
    RUN_TEST(csv_empty_input);
    RUN_TEST(csv_row_column_tracking);
    RUN_TEST(csv_quoted_empty);
    RUN_TEST(csv_pipe_auto_detect);
    RUN_TEST(csv_no_skip_empty_rows);
    RUN_TEST(csv_only_bom);
    RUN_TEST(csv_quoted_field_with_crlf);
    RUN_TEST(csv_single_field_no_newline);
    RUN_TEST(csv_trim_does_not_affect_quoted);
    RUN_TEST(csv_multiple_escaped_quotes);

    printf("\nCSV Parser: %d passed, %d total\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
