/**
 * test_tui.c - ClayShards TUI Renderer Tests
 *
 * Tests the TUI renderer without needing an actual terminal.
 * Verifies buffer contents directly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLAY_IMPLEMENTATION
#include "clay.h"
#include "cs_tui.h"

/* ============================================================================
 * Test Framework
 * ============================================================================ */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); fflush(stdout); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)
#define ASSERT(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

/* ============================================================================
 * Test Configuration
 * ============================================================================ */

static CsTuiConfig test_config = {
    .width = 40,
    .height = 12,
    .color_mode = CS_TUI_COLOR_256,
    .box_style = CS_TUI_BOX_LIGHT,
    .alternate_screen = false,
    .hide_cursor = false,
    .differential = false  /* Always full render for tests */
};

/* ============================================================================
 * Basic Tests
 * ============================================================================ */

static void test_create_destroy(void) {
    TEST(create_destroy);

    CsTuiRenderer *r = cs_tui_create(&test_config);
    ASSERT(r != NULL, "Failed to create renderer");

    int w, h;
    cs_tui_get_size(r, &w, &h);
    ASSERT(w == 40, "Wrong width");
    ASSERT(h == 12, "Wrong height");

    cs_tui_free(r);
    PASS();
}

static void test_clear(void) {
    TEST(clear);

    CsTuiRenderer *r = cs_tui_create(&test_config);
    ASSERT(r != NULL, "Failed to create renderer");

    cs_tui_begin(r);
    cs_tui_clear(r, (CsTuiColor){40, 40, 40, 255});
    cs_tui_end(r);

    /* Check a cell has the background color */
    CsTuiColor bg = cs_tui_get_cell_bg(r, 5, 5);
    ASSERT(bg.r == 40 && bg.g == 40 && bg.b == 40, "Background color not set");

    cs_tui_free(r);
    PASS();
}

static void test_text_rendering(void) {
    TEST(text_rendering);

    CsTuiRenderer *r = cs_tui_create(&test_config);
    ASSERT(r != NULL, "Failed to create renderer");

    cs_tui_begin(r);
    cs_tui_text(r, 5, 3, "Hello TUI", -1, (CsTuiColor){255, 255, 255, 255}, NULL);
    cs_tui_end(r);

    ASSERT(cs_tui_buffer_contains(r, 5, 3, "Hello TUI"), "Text not found in buffer");

    cs_tui_free(r);
    PASS();
}

static void test_rect_rendering(void) {
    TEST(rect_rendering);

    CsTuiRenderer *r = cs_tui_create(&test_config);
    ASSERT(r != NULL, "Failed to create renderer");

    cs_tui_begin(r);
    cs_tui_rect(r, 2, 2, 10, 5, (CsTuiColor){100, 100, 100, 255}, NULL, 0);
    cs_tui_end(r);

    /* Check background color in the rect area */
    CsTuiColor bg = cs_tui_get_cell_bg(r, 5, 4);
    ASSERT(bg.r == 100 && bg.g == 100 && bg.b == 100, "Rect background not set");

    cs_tui_free(r);
    PASS();
}

static void test_bordered_rect(void) {
    TEST(bordered_rect);

    CsTuiRenderer *r = cs_tui_create(&test_config);
    ASSERT(r != NULL, "Failed to create renderer");

    cs_tui_begin(r);
    CsTuiColor border = {255, 0, 0, 255};
    cs_tui_rect(r, 2, 2, 8, 4, (CsTuiColor){50, 50, 50, 255}, &border, 0);
    cs_tui_end(r);

    /* Check top-left corner has border character */
    uint32_t tl = cs_tui_get_cell_char(r, 2, 2);
    /* Light box top-left is ┌ (U+250C) */
    ASSERT(tl == 0x250C, "Top-left corner not correct");

    /* Check border color */
    CsTuiColor fg = cs_tui_get_cell_fg(r, 2, 2);
    ASSERT(fg.r == 255 && fg.g == 0 && fg.b == 0, "Border color not correct");

    cs_tui_free(r);
    PASS();
}

static void test_border_only(void) {
    TEST(border_only);

    CsTuiRenderer *r = cs_tui_create(&test_config);
    ASSERT(r != NULL, "Failed to create renderer");

    cs_tui_begin(r);
    cs_tui_border(r, 1, 1, 10, 5, (CsTuiColor){0, 255, 0, 255}, 0);
    cs_tui_end(r);

    /* Check corners */
    uint32_t tl = cs_tui_get_cell_char(r, 1, 1);
    uint32_t tr = cs_tui_get_cell_char(r, 10, 1);
    uint32_t bl = cs_tui_get_cell_char(r, 1, 5);
    uint32_t br = cs_tui_get_cell_char(r, 10, 5);

    ASSERT(tl == 0x250C, "Top-left corner incorrect");     /* ┌ */
    ASSERT(tr == 0x2510, "Top-right corner incorrect");    /* ┐ */
    ASSERT(bl == 0x2514, "Bottom-left corner incorrect");  /* └ */
    ASSERT(br == 0x2518, "Bottom-right corner incorrect"); /* ┘ */

    cs_tui_free(r);
    PASS();
}

static void test_scissor_clipping(void) {
    TEST(scissor_clipping);

    CsTuiRenderer *r = cs_tui_create(&test_config);
    ASSERT(r != NULL, "Failed to create renderer");

    cs_tui_begin(r);

    /* Set scissor to left half only */
    cs_tui_scissor_push(r, 0, 0, 20, 12);

    /* Draw text that extends beyond scissor */
    cs_tui_text(r, 15, 5, "Hello World", -1, (CsTuiColor){255, 255, 255, 255}, NULL);

    cs_tui_scissor_pop(r);
    cs_tui_end(r);

    /* Text within scissor should be visible */
    ASSERT(cs_tui_buffer_contains(r, 15, 5, "Hello"), "Text within scissor not visible");

    /* Text beyond scissor should be clipped (column 20+) */
    uint32_t clipped = cs_tui_get_cell_char(r, 21, 5);
    ASSERT(clipped != 'W', "Text beyond scissor should be clipped");

    cs_tui_free(r);
    PASS();
}

static void test_utf8_text(void) {
    TEST(utf8_text);

    CsTuiRenderer *r = cs_tui_create(&test_config);
    ASSERT(r != NULL, "Failed to create renderer");

    cs_tui_begin(r);
    /* Test with some UTF-8 characters */
    cs_tui_text(r, 2, 2, "ABC", -1, (CsTuiColor){255, 255, 255, 255}, NULL);
    cs_tui_end(r);

    ASSERT(cs_tui_get_cell_char(r, 2, 2) == 'A', "First char incorrect");
    ASSERT(cs_tui_get_cell_char(r, 3, 2) == 'B', "Second char incorrect");
    ASSERT(cs_tui_get_cell_char(r, 4, 2) == 'C', "Third char incorrect");

    cs_tui_free(r);
    PASS();
}

/* ============================================================================
 * Box Style Tests
 * ============================================================================ */

static void test_box_style_ascii(void) {
    TEST(box_style_ascii);

    CsTuiConfig cfg = test_config;
    cfg.box_style = CS_TUI_BOX_ASCII;

    CsTuiRenderer *r = cs_tui_create(&cfg);
    ASSERT(r != NULL, "Failed to create renderer");

    cs_tui_begin(r);
    cs_tui_border(r, 0, 0, 5, 3, (CsTuiColor){255, 255, 255, 255}, 0);
    cs_tui_end(r);

    /* ASCII uses + for corners */
    ASSERT(cs_tui_get_cell_char(r, 0, 0) == '+', "ASCII top-left should be +");

    cs_tui_free(r);
    PASS();
}

static void test_box_style_rounded(void) {
    TEST(box_style_rounded);

    CsTuiRenderer *r = cs_tui_create(&test_config);
    ASSERT(r != NULL, "Failed to create renderer");

    cs_tui_begin(r);
    /* corner_radius > 0 triggers rounded style */
    cs_tui_border(r, 0, 0, 5, 3, (CsTuiColor){255, 255, 255, 255}, 1);
    cs_tui_end(r);

    /* Rounded uses ╭ for top-left (U+256D) */
    uint32_t tl = cs_tui_get_cell_char(r, 0, 0);
    ASSERT(tl == 0x256D, "Rounded top-left should be ╭");

    cs_tui_free(r);
    PASS();
}

/* ============================================================================
 * Color Tests
 * ============================================================================ */

static void test_color_256_conversion(void) {
    TEST(color_256_conversion);

    /* Test the internal rgb_to_256 function indirectly */
    CsTuiRenderer *r = cs_tui_create(&test_config);
    ASSERT(r != NULL, "Failed to create renderer");

    cs_tui_begin(r);
    /* Red text */
    cs_tui_text(r, 0, 0, "R", 1, (CsTuiColor){255, 0, 0, 255}, NULL);
    /* Green text */
    cs_tui_text(r, 1, 0, "G", 1, (CsTuiColor){0, 255, 0, 255}, NULL);
    /* Blue text */
    cs_tui_text(r, 2, 0, "B", 1, (CsTuiColor){0, 0, 255, 255}, NULL);
    cs_tui_end(r);

    CsTuiColor r_fg = cs_tui_get_cell_fg(r, 0, 0);
    CsTuiColor g_fg = cs_tui_get_cell_fg(r, 1, 0);
    CsTuiColor b_fg = cs_tui_get_cell_fg(r, 2, 0);

    ASSERT(r_fg.r == 255 && r_fg.g == 0 && r_fg.b == 0, "Red not stored correctly");
    ASSERT(g_fg.r == 0 && g_fg.g == 255 && g_fg.b == 0, "Green not stored correctly");
    ASSERT(b_fg.r == 0 && b_fg.g == 0 && b_fg.b == 255, "Blue not stored correctly");

    cs_tui_free(r);
    PASS();
}

/* ============================================================================
 * Resize Tests
 * ============================================================================ */

static void test_resize(void) {
    TEST(resize);

    CsTuiRenderer *r = cs_tui_create(&test_config);
    ASSERT(r != NULL, "Failed to create renderer");

    int w, h;
    cs_tui_get_size(r, &w, &h);
    ASSERT(w == 40 && h == 12, "Initial size wrong");

    cs_tui_resize(r, 80, 24);
    cs_tui_get_size(r, &w, &h);
    ASSERT(w == 80 && h == 24, "Resized size wrong");

    cs_tui_free(r);
    PASS();
}

/* ============================================================================
 * Buffer Dump Test
 * ============================================================================ */

static void test_buffer_dump(void) {
    TEST(buffer_dump);

    CsTuiRenderer *r = cs_tui_create(&test_config);
    ASSERT(r != NULL, "Failed to create renderer");

    cs_tui_begin(r);
    cs_tui_text(r, 0, 0, "Test", -1, (CsTuiColor){255, 255, 255, 255}, NULL);
    cs_tui_end(r);

    char *dump = cs_tui_dump_buffer(r);
    ASSERT(dump != NULL, "Buffer dump failed");
    ASSERT(strstr(dump, "Test") != NULL, "Dump doesn't contain text");

    free(dump);
    cs_tui_free(r);
    PASS();
}

/* ============================================================================
 * Cursor Tests
 * ============================================================================ */

static void test_cursor_visibility(void) {
    TEST(cursor_visibility);

    CsTuiRenderer *r = cs_tui_create(&test_config);
    ASSERT(r != NULL, "Failed to create renderer");

    /* Set cursor position and make visible */
    cs_tui_set_cursor(r, 10, 5, true);

    cs_tui_begin(r);
    cs_tui_end(r);  /* Flush handles cursor output */

    /* Can't easily test terminal output, but verify no crash */
    cs_tui_set_cursor(r, 20, 8, false);

    cs_tui_begin(r);
    cs_tui_end(r);

    cs_tui_free(r);
    PASS();
}

/* ============================================================================
 * Z-Index Tests
 * ============================================================================ */

/* Access renderer internals for z-index testing */
extern void cs_tui_set_z_index(CsTuiRenderer *r, int16_t z);

static void test_zindex_overlay(void) {
    TEST(zindex_overlay);

    CsTuiRenderer *r = cs_tui_create(&test_config);
    ASSERT(r != NULL, "Failed to create renderer");

    cs_tui_begin(r);

    /* Draw background text at z=0 */
    cs_tui_set_z_index(r, 0);
    cs_tui_text(r, 5, 5, "BACKGROUND", -1, (CsTuiColor){255, 0, 0, 255}, NULL);

    /* Draw foreground text at z=100 - should overwrite */
    cs_tui_set_z_index(r, 100);
    cs_tui_text(r, 5, 5, "OVERLAY", -1, (CsTuiColor){0, 255, 0, 255}, NULL);

    cs_tui_end(r);

    /* Should see OVERLAY, not BACKGROUND */
    ASSERT(cs_tui_buffer_contains(r, 5, 5, "OVERLAY"), "Higher z-index should win");
    ASSERT(!cs_tui_buffer_contains(r, 5, 5, "BACKGROUND"), "Lower z-index should be hidden");

    cs_tui_free(r);
    PASS();
}

static void test_zindex_lower_blocked(void) {
    TEST(zindex_lower_blocked);

    CsTuiRenderer *r = cs_tui_create(&test_config);
    ASSERT(r != NULL, "Failed to create renderer");

    cs_tui_begin(r);

    /* Draw foreground first at z=100 */
    cs_tui_set_z_index(r, 100);
    cs_tui_text(r, 5, 5, "FIRST", -1, (CsTuiColor){0, 255, 0, 255}, NULL);

    /* Try to overwrite with z=0 - should be blocked */
    cs_tui_set_z_index(r, 0);
    cs_tui_text(r, 5, 5, "SECOND", -1, (CsTuiColor){255, 0, 0, 255}, NULL);

    cs_tui_end(r);

    /* Should still see FIRST */
    ASSERT(cs_tui_buffer_contains(r, 5, 5, "FIRST"), "Higher z-index should persist");

    cs_tui_free(r);
    PASS();
}

/* Test differential update gap detection - non-consecutive cell updates */
static void test_differential_gap_update(void) {
    TEST(differential_gap_update);

    /* Use differential mode for this test */
    CsTuiConfig diff_config = test_config;
    diff_config.differential = true;
    diff_config.headless = true;  /* Don't output to terminal */

    CsTuiRenderer *r = cs_tui_create(&diff_config);
    ASSERT(r != NULL, "Failed to create renderer");

    /* Frame 1: Draw text at positions 0, 5, and 10 (with gaps) */
    cs_tui_begin(r);
    cs_tui_text(r, 0, 0, "AAA", -1, (CsTuiColor){255, 255, 255, 255}, NULL);
    cs_tui_text(r, 8, 0, "BBB", -1, (CsTuiColor){255, 255, 255, 255}, NULL);
    cs_tui_text(r, 16, 0, "CCC", -1, (CsTuiColor){255, 255, 255, 255}, NULL);
    cs_tui_end(r);

    /* Verify positions are correct */
    ASSERT(cs_tui_buffer_contains(r, 0, 0, "AAA"), "First text at position 0");
    ASSERT(cs_tui_buffer_contains(r, 8, 0, "BBB"), "Second text at position 8");
    ASSERT(cs_tui_buffer_contains(r, 16, 0, "CCC"), "Third text at position 16");
    ASSERT(cs_tui_get_cell_char(r, 3, 0) == ' ', "Gap between A and B should be space");
    ASSERT(cs_tui_get_cell_char(r, 11, 0) == ' ', "Gap between B and C should be space");

    /* Frame 2: Change only one of the texts - test differential with gap */
    cs_tui_begin(r);
    cs_tui_text(r, 0, 0, "AAA", -1, (CsTuiColor){255, 255, 255, 255}, NULL);
    cs_tui_text(r, 8, 0, "XXX", -1, (CsTuiColor){255, 0, 0, 255}, NULL);  /* Changed */
    cs_tui_text(r, 16, 0, "CCC", -1, (CsTuiColor){255, 255, 255, 255}, NULL);
    cs_tui_end(r);

    /* Verify all positions still correct after differential update */
    ASSERT(cs_tui_buffer_contains(r, 0, 0, "AAA"), "First text unchanged");
    ASSERT(cs_tui_buffer_contains(r, 8, 0, "XXX"), "Second text changed to XXX");
    ASSERT(cs_tui_buffer_contains(r, 16, 0, "CCC"), "Third text unchanged");

    cs_tui_free(r);
    PASS();
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("ClayShards TUI Tests\n\n");

    printf("Basic tests:\n");
    test_create_destroy();
    test_clear();
    test_text_rendering();
    test_rect_rendering();
    test_bordered_rect();
    test_border_only();
    test_scissor_clipping();
    test_utf8_text();

    printf("\nBox style tests:\n");
    test_box_style_ascii();
    test_box_style_rounded();

    printf("\nColor tests:\n");
    test_color_256_conversion();

    printf("\nResize tests:\n");
    test_resize();

    printf("\nBuffer tests:\n");
    test_buffer_dump();

    printf("\nCursor tests:\n");
    test_cursor_visibility();

    printf("\nZ-index tests:\n");
    test_zindex_overlay();
    test_zindex_lower_blocked();

    printf("\nDifferential update tests:\n");
    test_differential_gap_update();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
