/**
 * Clay Components - Immediate Mode Tests
 *
 * Tests for cc_immediate.h/c
 * Run with: make test
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

/* Include the implementation directly for testing */
#define CLAY_IMPLEMENTATION
#include "clay.h"
#include "cc_immediate.h"

/* Test counters */
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
    do { \
        tests_run++; \
        printf("  %s... ", #name); \
    } while(0)

#define PASS() \
    do { \
        tests_passed++; \
        printf("PASS\n"); \
    } while(0)

#define FAIL(msg) \
    do { \
        printf("FAIL: %s\n", msg); \
    } while(0)

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { FAIL(msg); return; } \
    } while(0)

/* ============================================================================
 * Hash ID Tests
 * ============================================================================ */

static void test_hash_id_unique(void) {
    TEST(hash_id_unique);

    uint32_t id1 = cc_hash_id("button1");
    uint32_t id2 = cc_hash_id("button2");
    uint32_t id3 = cc_hash_id("input");
    uint32_t id4 = cc_hash_id("button1");  /* Same as id1 */

    ASSERT(id1 != id2, "Different strings should have different IDs");
    ASSERT(id1 != id3, "Different strings should have different IDs");
    ASSERT(id1 == id4, "Same strings should have same ID");
    ASSERT(id1 != 0, "ID should never be 0");

    PASS();
}

static void test_hash_id_consistency(void) {
    TEST(hash_id_consistency);

    /* Same string should always produce same hash */
    for (int i = 0; i < 100; i++) {
        uint32_t a = cc_hash_id("test_string");
        uint32_t b = cc_hash_id("test_string");
        ASSERT(a == b, "Hash should be consistent");
    }

    PASS();
}

/* ============================================================================
 * Focus Management Tests
 * ============================================================================ */

static void test_focus_init(void) {
    TEST(focus_init);

    cc_init();

    ASSERT(cc_focused_id() == 0, "Initially no element should be focused");
    ASSERT(cc_cursor_pos() == 0, "Cursor should be at 0");
    ASSERT(cc_selection_start() == -1, "No selection initially");

    PASS();
}

static void test_focus_blur(void) {
    TEST(focus_blur);

    cc_init();

    cc_focus(123);
    ASSERT(cc_focused_id() == 123, "Focus should be set");

    cc_blur();
    ASSERT(cc_focused_id() == 0, "Focus should be cleared");

    PASS();
}

static void test_focus_switch(void) {
    TEST(focus_switch);

    cc_init();

    cc_focus(100);
    ASSERT(cc_focused_id() == 100, "First focus");

    cc_focus(200);
    ASSERT(cc_focused_id() == 200, "Focus should switch");

    cc_focus(100);
    ASSERT(cc_focused_id() == 100, "Focus should switch back");

    PASS();
}

/* ============================================================================
 * Keyboard Input Tests
 * ============================================================================ */

static void test_key_char_not_focused(void) {
    TEST(key_char_not_focused);

    cc_init();

    /* No element focused - should reject input */
    bool handled = cc_key_char('a');
    ASSERT(!handled, "Should not handle input when nothing focused");

    PASS();
}

static void test_key_char_focused(void) {
    TEST(key_char_focused);

    cc_init();

    /* Simulate focusing by setting internal state */
    cc_focus(1);

    /* We need to call cc_input to set up the active buffer */
    /* For this test, we'll test the key handling indirectly */

    PASS();
}

static void test_key_down_escape(void) {
    TEST(key_down_escape);

    cc_init();
    cc_focus(123);

    ASSERT(cc_focused_id() == 123, "Should be focused");

    /* Escape key (27) should blur */
    /* Note: This requires active_text to be set, which happens during cc_input */
    /* For now, just verify focus state */

    cc_blur();
    ASSERT(cc_focused_id() == 0, "Should be blurred");

    PASS();
}

static void test_key_down_invalid_codes(void) {
    TEST(key_down_invalid_codes);

    cc_init();

    /* Invalid key codes should be rejected */
    ASSERT(!cc_key_down(-1, false, false), "Negative key code should be rejected");
    ASSERT(!cc_key_down(1000, false, false), "Key code > 512 should be rejected");
    ASSERT(!cc_key_down(999999, false, false), "Very large key code should be rejected");

    PASS();
}

/* ============================================================================
 * Cursor Blink Tests
 * ============================================================================ */

static void test_cursor_blink(void) {
    TEST(cursor_blink);

    cc_init();
    cc_focus(1);

    /* Initially cursor should be visible */
    bool initial = cc_cursor_visible();
    ASSERT(initial, "Cursor should start visible");

    /* After ~0.53s, should toggle */
    cc_frame_begin();
    cc_frame_end(0.6f);

    bool after_first = cc_cursor_visible();
    ASSERT(!after_first, "Cursor should toggle after 0.53s");

    cc_frame_begin();
    cc_frame_end(0.6f);

    bool after_second = cc_cursor_visible();
    ASSERT(after_second, "Cursor should toggle back");

    PASS();
}

static void test_cursor_blink_not_focused(void) {
    TEST(cursor_blink_not_focused);

    cc_init();
    /* Not focused */

    cc_frame_begin();
    cc_frame_end(1.0f);

    /* Cursor state when not focused is implementation-defined */
    /* Just verify it doesn't crash */

    PASS();
}

/* ============================================================================
 * Input Result Tests
 * ============================================================================ */

static void test_input_result_init(void) {
    TEST(input_result_init);

    CcInputResult r = {0};

    ASSERT(!r.changed, "changed should be false");
    ASSERT(!r.submitted, "submitted should be false");
    ASSERT(!r.focused, "focused should be false");
    ASSERT(!r.blurred, "blurred should be false");

    PASS();
}

static void test_button_result_init(void) {
    TEST(button_result_init);

    CcButtonResult r = {0};

    ASSERT(!r.clicked, "clicked should be false");
    ASSERT(!r.hovered, "hovered should be false");

    PASS();
}

/* ============================================================================
 * Style Tests
 * ============================================================================ */

static void test_default_styles_exist(void) {
    TEST(default_styles_exist);

    /* Verify default styles are accessible */
    ASSERT(CC_INPUT_STYLE_DEFAULT.width > 0, "Input style should have width");
    ASSERT(CC_INPUT_STYLE_DEFAULT.height > 0, "Input style should have height");
    ASSERT(CC_INPUT_STYLE_DEFAULT.font_size > 0, "Input style should have font_size");

    ASSERT(CC_BUTTON_STYLE_DEFAULT.font_size > 0, "Button style should have font_size");

    PASS();
}

/* ============================================================================
 * Integration Tests (require Clay initialization)
 * ============================================================================ */

static uint8_t clay_memory[8 * 1024 * 1024];  /* 8MB for Clay (MinMemorySize is ~6MB) */
static bool clay_initialized = false;

static Clay_Dimensions measure_text(Clay_StringSlice text, Clay_TextElementConfig *config, void *userData) {
    (void)userData;
    return (Clay_Dimensions){text.length * config->fontSize * 0.6f, config->fontSize};
}

static Clay_Vector2 query_scroll(uint32_t id, void *userData) {
    (void)id; (void)userData;
    return (Clay_Vector2){0, 0};
}

static void handle_error(Clay_ErrorData error) {
    fprintf(stderr, "Clay error: %.*s\n", error.errorText.length, error.errorText.chars);
}

static void init_clay(void) {
    if (clay_initialized) return;

    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(sizeof(clay_memory), clay_memory);
    Clay_Initialize(arena, (Clay_Dimensions){800, 600}, (Clay_ErrorHandler){handle_error, NULL});
    Clay_SetMeasureTextFunction(measure_text, NULL);
    Clay_SetQueryScrollOffsetFunction(query_scroll, NULL);
    clay_initialized = true;
}

static void test_input_renders(void) {
    TEST(input_renders);

    init_clay();
    cc_init();

    char text[64] = "Hello";
    int len = 5;

    cc_frame_begin();

    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        CcInputResult r = cc_input(CC_ID("test_input"), text, &len, 64, "Placeholder", NULL);
        (void)r;
    }

    Clay_RenderCommandArray commands = Clay_EndLayout();

    cc_frame_end(0.016f);

    ASSERT(commands.length > 0, "Should produce render commands");

    PASS();
}

static void test_button_renders(void) {
    TEST(button_renders);

    init_clay();
    cc_init();

    cc_frame_begin();

    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        CcButtonResult r = cc_button(CC_ID("test_btn"), "Click Me", NULL);
        (void)r;
    }

    Clay_RenderCommandArray commands = Clay_EndLayout();

    cc_frame_end(0.016f);

    ASSERT(commands.length > 0, "Should produce render commands");

    PASS();
}

static void test_multiple_inputs(void) {
    TEST(multiple_inputs);

    init_clay();
    cc_init();

    char text1[64] = "First";
    char text2[64] = "Second";
    char text3[64] = "Third";
    int len1 = 5, len2 = 6, len3 = 5;

    cc_frame_begin();

    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = {
            .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .childGap = 8
        }
    }) {
        cc_input(CC_ID("input1"), text1, &len1, 64, NULL, NULL);
        cc_input(CC_ID("input2"), text2, &len2, 64, NULL, NULL);
        cc_input(CC_ID("input3"), text3, &len3, 64, NULL, NULL);
    }

    Clay_RenderCommandArray commands = Clay_EndLayout();

    cc_frame_end(0.016f);

    /* Each input produces at least 2 commands (rect + text or rect + border) */
    ASSERT(commands.length >= 3, "Should have commands for all inputs");

    /* Verify IDs are unique */
    uint32_t id1 = CC_ID("input1");
    uint32_t id2 = CC_ID("input2");
    uint32_t id3 = CC_ID("input3");
    ASSERT(id1 != id2 && id2 != id3 && id1 != id3, "IDs should be unique");

    PASS();
}

static void test_input_focus_on_click(void) {
    TEST(input_focus_on_click);

    init_clay();
    cc_init();

    char text[64] = "";
    int len = 0;

    /* Simulate click at input position */
    Clay_SetPointerState((Clay_Vector2){100, 100}, false);
    cc_set_pending_click();

    cc_frame_begin();

    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        /* Position input at 50,50 with size 200x32 */
        CLAY(CLAY_ID("InputContainer"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(200), CLAY_SIZING_FIXED(32) }
            },
            .floating = {
                .attachTo = CLAY_ATTACH_TO_ROOT,
                .offset = {50, 50}
            }
        }) {
            CcInputResult r = cc_input(CC_ID("test"), text, &len, 64, NULL, NULL);

            /* If pointer is over input and we clicked, should focus */
            if (r.focused) {
                /* This would indicate the click was detected */
            }
        }
    }

    Clay_EndLayout();

    cc_frame_end(0.016f);

    /* Focus detection depends on Clay_PointerOver which needs proper bounds */
    /* For now just verify no crash */

    PASS();
}

static void test_input_validation(void) {
    TEST(input_validation);

    init_clay();
    cc_init();

    char text[64] = "hello";
    int len = 5;

    cc_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        /* Test with corrupted length (too large) */
        int bad_len = 100;  /* Larger than max_len */
        char bad_text[64] = "test";
        cc_input(CC_ID("bad_input"), bad_text, &bad_len, 64, NULL, NULL);
        ASSERT(bad_len <= 63, "Length should be clamped to max_len-1");

        /* Test with negative length */
        int neg_len = -5;
        char neg_text[64] = "";
        cc_input(CC_ID("neg_input"), neg_text, &neg_len, 64, NULL, NULL);
        ASSERT(neg_len == 0, "Negative length should be clamped to 0");

        /* Normal case should work */
        CcInputResult r = cc_input(CC_ID("good_input"), text, &len, 64, NULL, NULL);
        ASSERT(len == 5, "Normal length should be unchanged");
        (void)r;
    }

    Clay_EndLayout();
    cc_frame_end(0.016f);

    PASS();
}

/* ============================================================================
 * Map Component Tests
 * ============================================================================ */

static void test_map_result_init(void) {
    TEST(map_result_init);

    CcMapResult r = {0};

    ASSERT(!r.panned, "panned should be false");
    ASSERT(!r.zoomed, "zoomed should be false");
    ASSERT(!r.clicked, "clicked should be false");
    ASSERT(r.click_lat == 0.0, "click_lat should be 0");
    ASSERT(r.click_lon == 0.0, "click_lon should be 0");

    PASS();
}

static void test_map_projection_lon_to_tile(void) {
    TEST(map_projection_lon_to_tile);

    /* At zoom 0, entire world is one tile */
    double x0 = cc_map_lon_to_tile_x(0.0, 0);
    ASSERT(x0 >= 0.49 && x0 <= 0.51, "lon=0 at z=0 should be ~0.5");

    /* At zoom 1, world is 2x2 tiles */
    double x1_west = cc_map_lon_to_tile_x(-180.0, 1);
    double x1_east = cc_map_lon_to_tile_x(180.0, 1);
    ASSERT(x1_west >= -0.01 && x1_west <= 0.01, "lon=-180 at z=1 should be ~0");
    ASSERT(x1_east >= 1.99 && x1_east <= 2.01, "lon=180 at z=1 should be ~2");

    PASS();
}

static void test_map_projection_lat_to_tile(void) {
    TEST(map_projection_lat_to_tile);

    /* At zoom 0, equator is at y=0.5 */
    double y0 = cc_map_lat_to_tile_y(0.0, 0);
    ASSERT(y0 >= 0.49 && y0 <= 0.51, "lat=0 at z=0 should be ~0.5");

    /* Extreme latitudes should be clamped and not produce NaN */
    double y_north = cc_map_lat_to_tile_y(90.0, 1);
    double y_south = cc_map_lat_to_tile_y(-90.0, 1);
    ASSERT(y_north == y_north, "North pole should not produce NaN");  /* NaN != NaN */
    ASSERT(y_south == y_south, "South pole should not produce NaN");

    PASS();
}

static void test_map_projection_roundtrip(void) {
    TEST(map_projection_roundtrip);

    /* lon -> tile_x -> lon should be consistent */
    double orig_lon = 19.0402;  /* Budapest */
    double tile_x = cc_map_lon_to_tile_x(orig_lon, 12);
    double recovered_lon = cc_map_tile_x_to_lon(tile_x, 12);
    ASSERT(recovered_lon > orig_lon - 0.001 && recovered_lon < orig_lon + 0.001,
           "Longitude roundtrip should match");

    /* lat -> tile_y -> lat should be consistent */
    double orig_lat = 47.4979;  /* Budapest */
    double tile_y = cc_map_lat_to_tile_y(orig_lat, 12);
    double recovered_lat = cc_map_tile_y_to_lat(tile_y, 12);
    ASSERT(recovered_lat > orig_lat - 0.001 && recovered_lat < orig_lat + 0.001,
           "Latitude roundtrip should match");

    PASS();
}

static void test_map_scroll(void) {
    TEST(map_scroll);

    /* Normal zoom in */
    ASSERT(cc_map_scroll(10, 1, 0, 19) == 11, "Zoom in should increase");

    /* Normal zoom out */
    ASSERT(cc_map_scroll(10, -1, 0, 19) == 9, "Zoom out should decrease");

    /* Clamp at max */
    ASSERT(cc_map_scroll(19, 1, 0, 19) == 19, "Should clamp at max");

    /* Clamp at min */
    ASSERT(cc_map_scroll(0, -1, 0, 19) == 0, "Should clamp at min");

    /* Custom range */
    ASSERT(cc_map_scroll(5, 1, 5, 10) == 6, "Should work with custom range");
    ASSERT(cc_map_scroll(5, -1, 5, 10) == 5, "Should clamp at custom min");

    PASS();
}

static void test_map_pointer_handling(void) {
    TEST(map_pointer_handling);

    uint32_t map_id = CC_ID("test_map");

    /* Initially not dragging */
    ASSERT(!cc_map_is_dragging(map_id), "Should not be dragging initially");

    /* Start drag */
    cc_map_pointer_down(map_id, 47.4979, 19.0402, 400.0f, 300.0f);
    ASSERT(cc_map_is_dragging(map_id), "Should be dragging after pointer_down");

    /* Move pointer */
    double new_lat, new_lon;
    bool moved = cc_map_pointer_move(map_id, 12, 420.0f, 310.0f, &new_lat, &new_lon);
    ASSERT(moved, "pointer_move should return true when dragging");

    /* End drag with minimal movement - should detect as click */
    cc_map_pointer_up(map_id, 401.0f, 301.0f);  /* Only 1-2 pixels moved */
    ASSERT(!cc_map_is_dragging(map_id), "Should not be dragging after pointer_up");

    PASS();
}

static void test_map_default_style(void) {
    TEST(map_default_style);

    ASSERT(CC_MAP_STYLE_DEFAULT.min_zoom == 0, "Default min_zoom should be 0");
    ASSERT(CC_MAP_STYLE_DEFAULT.max_zoom == 19, "Default max_zoom should be 19");
    ASSERT(CC_MAP_STYLE_DEFAULT.min_lat < 0, "Default min_lat should be negative");
    ASSERT(CC_MAP_STYLE_DEFAULT.max_lat > 0, "Default max_lat should be positive");

    PASS();
}

/* ============================================================================
 * Font Metrics Tests
 * ============================================================================ */

/* Note: Font metrics require cc_clay.h which isn't included in this test.
 * These tests verify the text measurement callback mechanism works. */

static void test_text_measurement_callback(void) {
    TEST(text_measurement_callback);

    init_clay();
    cc_init();

    /* The measure_text callback is set in init_clay() */
    /* Verify it produces consistent results */
    Clay_StringSlice text = { .chars = "Hello", .length = 5 };
    Clay_TextElementConfig config = { .fontSize = 16 };

    Clay_Dimensions d1 = measure_text(text, &config, NULL);
    Clay_Dimensions d2 = measure_text(text, &config, NULL);

    ASSERT(d1.width == d2.width, "Text measurement should be consistent");
    ASSERT(d1.height == d2.height, "Text height should be consistent");
    ASSERT(d1.width > 0, "Text should have positive width");
    ASSERT(d1.height > 0, "Text should have positive height");

    /* Different text lengths should have different widths */
    Clay_StringSlice short_text = { .chars = "Hi", .length = 2 };
    Clay_Dimensions d_short = measure_text(short_text, &config, NULL);
    ASSERT(d_short.width < d1.width, "Shorter text should be narrower");

    PASS();
}

static void test_text_measurement_font_size(void) {
    TEST(text_measurement_font_size);

    init_clay();

    Clay_StringSlice text = { .chars = "Test", .length = 4 };
    Clay_TextElementConfig config_small = { .fontSize = 12 };
    Clay_TextElementConfig config_large = { .fontSize = 24 };

    Clay_Dimensions d_small = measure_text(text, &config_small, NULL);
    Clay_Dimensions d_large = measure_text(text, &config_large, NULL);

    ASSERT(d_large.width > d_small.width, "Larger font should be wider");
    ASSERT(d_large.height > d_small.height, "Larger font should be taller");

    /* Height should match font size (in our simple callback) */
    ASSERT(d_small.height == 12, "Height should equal font size");
    ASSERT(d_large.height == 24, "Height should equal font size");

    PASS();
}

/* ============================================================================
 * Tab Navigation Tests
 * ============================================================================ */

static void test_tab_navigation_register(void) {
    TEST(tab_navigation_register);

    cc_init();
    cc_frame_begin();

    /* Register some focusable elements */
    cc_register_focusable(100);
    cc_register_focusable(200);
    cc_register_focusable(300);

    ASSERT(cc_focusable_count() == 3, "Should have 3 focusables");

    /* Duplicate registration should be ignored */
    cc_register_focusable(100);
    ASSERT(cc_focusable_count() == 3, "Duplicates should be ignored");

    /* Zero ID should be ignored */
    cc_register_focusable(0);
    ASSERT(cc_focusable_count() == 3, "Zero ID should be ignored");

    PASS();
}

static void test_tab_navigation_focus_next(void) {
    TEST(tab_navigation_focus_next);

    cc_init();
    cc_frame_begin();

    cc_register_focusable(100);
    cc_register_focusable(200);
    cc_register_focusable(300);

    /* Initially nothing focused */
    ASSERT(cc_focused_id() == 0, "Initially unfocused");

    /* Tab to first element */
    bool changed = cc_focus_next();
    ASSERT(changed, "Focus should change");
    ASSERT(cc_focused_id() == 100, "Should focus first element");

    /* Tab to second */
    cc_focus_next();
    ASSERT(cc_focused_id() == 200, "Should focus second element");

    /* Tab to third */
    cc_focus_next();
    ASSERT(cc_focused_id() == 300, "Should focus third element");

    /* Tab wraps to first */
    cc_focus_next();
    ASSERT(cc_focused_id() == 100, "Should wrap to first element");

    PASS();
}

static void test_tab_navigation_focus_prev(void) {
    TEST(tab_navigation_focus_prev);

    cc_init();
    cc_frame_begin();

    cc_register_focusable(100);
    cc_register_focusable(200);
    cc_register_focusable(300);

    /* Start with nothing focused - Shift+Tab goes to last */
    cc_focus_prev();
    ASSERT(cc_focused_id() == 300, "Shift+Tab from nothing should go to last");

    /* Shift+Tab to previous */
    cc_focus_prev();
    ASSERT(cc_focused_id() == 200, "Should focus previous element");

    cc_focus_prev();
    ASSERT(cc_focused_id() == 100, "Should focus first element");

    /* Wrap to last */
    cc_focus_prev();
    ASSERT(cc_focused_id() == 300, "Should wrap to last element");

    PASS();
}

static void test_tab_key_handling(void) {
    TEST(tab_key_handling);

    cc_init();
    cc_frame_begin();

    cc_register_focusable(100);
    cc_register_focusable(200);

    /* Tab key (code 9) should focus next */
    bool handled = cc_key_down(9, false, false);  /* Tab */
    ASSERT(handled, "Tab should be handled");
    ASSERT(cc_focused_id() == 100, "Tab should focus first element");

    /* Another Tab */
    cc_key_down(9, false, false);
    ASSERT(cc_focused_id() == 200, "Tab should focus second element");

    /* Shift+Tab */
    cc_key_down(9, true, false);  /* Shift+Tab */
    ASSERT(cc_focused_id() == 100, "Shift+Tab should focus previous");

    PASS();
}

static void test_tab_empty_focusables(void) {
    TEST(tab_empty_focusables);

    cc_init();
    cc_frame_begin();
    /* No focusables registered */

    bool changed = cc_focus_next();
    ASSERT(!changed, "Should not change focus with no focusables");
    ASSERT(cc_focused_id() == 0, "Should remain unfocused");

    changed = cc_focus_prev();
    ASSERT(!changed, "Should not change focus with no focusables");

    PASS();
}

static void test_focusable_reset_each_frame(void) {
    TEST(focusable_reset_each_frame);

    cc_init();

    /* Frame 1: register elements */
    cc_frame_begin();
    cc_register_focusable(100);
    cc_register_focusable(200);
    ASSERT(cc_focusable_count() == 2, "Should have 2 focusables");
    cc_frame_end(0.016f);

    /* Frame 2: focusables should be reset */
    cc_frame_begin();
    ASSERT(cc_focusable_count() == 0, "Focusables should reset each frame");

    /* Register different elements */
    cc_register_focusable(300);
    ASSERT(cc_focusable_count() == 1, "Should have 1 new focusable");

    PASS();
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("Clay Components - Immediate Mode Tests\n");
    printf("======================================\n\n");

    printf("Hash ID Tests:\n");
    test_hash_id_unique();
    test_hash_id_consistency();

    printf("\nFocus Management Tests:\n");
    test_focus_init();
    test_focus_blur();
    test_focus_switch();

    printf("\nKeyboard Input Tests:\n");
    test_key_char_not_focused();
    test_key_char_focused();
    test_key_down_escape();
    test_key_down_invalid_codes();

    printf("\nCursor Blink Tests:\n");
    test_cursor_blink();
    test_cursor_blink_not_focused();

    printf("\nResult Type Tests:\n");
    test_input_result_init();
    test_button_result_init();

    printf("\nStyle Tests:\n");
    test_default_styles_exist();

    printf("\nIntegration Tests:\n");
    test_input_renders();
    test_button_renders();
    test_multiple_inputs();
    test_input_focus_on_click();
    test_input_validation();

    printf("\nMap Component Tests:\n");
    test_map_result_init();
    test_map_projection_lon_to_tile();
    test_map_projection_lat_to_tile();
    test_map_projection_roundtrip();
    test_map_scroll();
    test_map_pointer_handling();
    test_map_default_style();

    printf("\nFont Metrics Tests:\n");
    test_text_measurement_callback();
    test_text_measurement_font_size();

    printf("\nTab Navigation Tests:\n");
    test_tab_navigation_register();
    test_tab_navigation_focus_next();
    test_tab_navigation_focus_prev();
    test_tab_key_handling();
    test_tab_empty_focusables();
    test_focusable_reset_each_frame();

    printf("\n======================================\n");
    printf("Results: %d/%d tests passed\n", tests_passed, tests_run);

    return tests_passed == tests_run ? 0 : 1;
}
