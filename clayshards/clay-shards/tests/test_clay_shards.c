/**
 * Clay Components - Immediate Mode Tests
 *
 * Tests for cs_immediate.h/c
 * Run with: make test
 */

#include <stdio.h>
#include <stdlib.h>  /* For malloc/free in custom allocator test */
#include <string.h>
#include <assert.h>

/* Include the implementation directly for testing */
#define CLAY_IMPLEMENTATION
#include "clay.h"
#include "cs_immediate.h"
#include "cs_render.h"
#include "cs_map.h"
#include "cs_map_provider.h"
#include "../src/cs_internal.h"  /* For CsState, cs_get_state, cs_widget_state */

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

    uint32_t id1 = cs_hash_id("button1");
    uint32_t id2 = cs_hash_id("button2");
    uint32_t id3 = cs_hash_id("input");
    uint32_t id4 = cs_hash_id("button1");  /* Same as id1 */

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
        uint32_t a = cs_hash_id("test_string");
        uint32_t b = cs_hash_id("test_string");
        ASSERT(a == b, "Hash should be consistent");
    }

    PASS();
}

static void test_hash_id_null_safe(void) {
    TEST(hash_id_null_safe);

    /* NULL input should not crash and should return valid ID */
    uint32_t id = cs_hash_id(NULL);
    ASSERT(id != 0, "NULL should return non-zero ID");

    /* Empty string should work */
    uint32_t empty_id = cs_hash_id("");
    ASSERT(empty_id != 0, "Empty string should return non-zero ID");

    PASS();
}

/* ============================================================================
 * Focus Management Tests
 * ============================================================================ */

static void test_focus_init(void) {
    TEST(focus_init);

    cs_init();

    ASSERT(cs_focused_id() == 0, "Initially no element should be focused");
    ASSERT(cs_cursor_pos() == 0, "Cursor should be at 0");
    ASSERT(cs_selection_start() == -1, "No selection initially");

    PASS();
}

static void test_focus_blur(void) {
    TEST(focus_blur);

    cs_init();

    cs_focus(123);
    ASSERT(cs_focused_id() == 123, "Focus should be set");

    cs_blur();
    ASSERT(cs_focused_id() == 0, "Focus should be cleared");

    PASS();
}

static void test_focus_switch(void) {
    TEST(focus_switch);

    cs_init();

    cs_focus(100);
    ASSERT(cs_focused_id() == 100, "First focus");

    cs_focus(200);
    ASSERT(cs_focused_id() == 200, "Focus should switch");

    cs_focus(100);
    ASSERT(cs_focused_id() == 100, "Focus should switch back");

    PASS();
}

/* ============================================================================
 * Keyboard Input Tests
 * ============================================================================ */

static void test_key_char_not_focused(void) {
    TEST(key_char_not_focused);

    cs_init();

    /* No element focused - should reject input */
    bool handled = cs_key_char('a');
    ASSERT(!handled, "Should not handle input when nothing focused");

    PASS();
}

static void test_key_char_focused(void) {
    TEST(key_char_focused);

    cs_init();

    /* Simulate focusing by setting internal state */
    cs_focus(1);

    /* We need to call cs_input to set up the active buffer */
    /* For this test, we'll test the key handling indirectly */

    PASS();
}

static void test_key_down_escape(void) {
    TEST(key_down_escape);

    cs_init();
    cs_focus(123);

    ASSERT(cs_focused_id() == 123, "Should be focused");

    /* Escape key (27) should blur */
    /* Note: This requires active_text to be set, which happens during cs_input */
    /* For now, just verify focus state */

    cs_blur();
    ASSERT(cs_focused_id() == 0, "Should be blurred");

    PASS();
}

static void test_key_down_invalid_codes(void) {
    TEST(key_down_invalid_codes);

    cs_init();

    /* Invalid key codes should be rejected */
    ASSERT(!cs_key_down(-1, false, false), "Negative key code should be rejected");
    ASSERT(!cs_key_down(1000, false, false), "Key code > 512 should be rejected");
    ASSERT(!cs_key_down(999999, false, false), "Very large key code should be rejected");

    PASS();
}

/* ============================================================================
 * Cursor Blink Tests
 * ============================================================================ */

static void test_cursor_blink(void) {
    TEST(cursor_blink);

    cs_init();
    cs_focus(1);

    /* Initially cursor should be visible */
    bool initial = cs_cursor_visible();
    ASSERT(initial, "Cursor should start visible");

    /* After ~0.53s, should toggle */
    cs_frame_begin();
    cs_frame_end(0.6f);

    bool after_first = cs_cursor_visible();
    ASSERT(!after_first, "Cursor should toggle after 0.53s");

    cs_frame_begin();
    cs_frame_end(0.6f);

    bool after_second = cs_cursor_visible();
    ASSERT(after_second, "Cursor should toggle back");

    PASS();
}

static void test_cursor_blink_not_focused(void) {
    TEST(cursor_blink_not_focused);

    cs_init();
    /* Not focused */

    cs_frame_begin();
    cs_frame_end(1.0f);

    /* Cursor state when not focused is implementation-defined */
    /* Just verify it doesn't crash */

    PASS();
}

/* ============================================================================
 * Input Result Tests
 * ============================================================================ */

static void test_input_result_init(void) {
    TEST(input_result_init);

    CsInputResult r = {0};

    ASSERT(!r.changed, "changed should be false");
    ASSERT(!r.submitted, "submitted should be false");
    ASSERT(!r.focused, "focused should be false");
    ASSERT(!r.blurred, "blurred should be false");

    PASS();
}

static void test_button_result_init(void) {
    TEST(button_result_init);

    CsButtonResult r = {0};

    ASSERT(!r.clicked, "clicked should be false");
    ASSERT(!r.hovered, "hovered should be false");

    PASS();
}

static void test_checkbox_result_init(void) {
    TEST(checkbox_result_init);

    CsCheckboxResult r = {0};

    ASSERT(!r.changed, "changed should be false");
    ASSERT(!r.checked, "checked should be false");
    ASSERT(!r.hovered, "hovered should be false");

    PASS();
}

static void test_toggle_result_init(void) {
    TEST(toggle_result_init);

    CsToggleResult r = {0};

    ASSERT(!r.changed, "changed should be false");
    ASSERT(!r.on, "on should be false");
    ASSERT(!r.hovered, "hovered should be false");

    PASS();
}

static void test_slider_result_init(void) {
    TEST(slider_result_init);

    CsSliderResult r = {0};

    ASSERT(!r.changed, "changed should be false");
    ASSERT(!r.dragging, "dragging should be false");
    ASSERT(!r.hovered, "hovered should be false");
    ASSERT(r.value == 0.0f, "value should be 0");

    PASS();
}

static void test_dropdown_result_init(void) {
    TEST(dropdown_result_init);

    CsDropdownResult r = {0};

    ASSERT(!r.changed, "changed should be false");
    ASSERT(!r.opened, "opened should be false");
    ASSERT(!r.closed, "closed should be false");
    ASSERT(!r.hovered, "hovered should be false");
    ASSERT(r.selected == 0, "selected should be 0");

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

    ASSERT(CS_CHECKBOX_STYLE_DEFAULT.size > 0, "Checkbox style should have size");
    ASSERT(CS_CHECKBOX_STYLE_DEFAULT.font_size > 0, "Checkbox style should have font_size");

    ASSERT(CS_TOGGLE_STYLE_DEFAULT.width > 0, "Toggle style should have width");
    ASSERT(CS_TOGGLE_STYLE_DEFAULT.height > 0, "Toggle style should have height");

    ASSERT(CS_SLIDER_STYLE_DEFAULT.width > 0, "Slider style should have width");
    ASSERT(CS_SLIDER_STYLE_DEFAULT.height > 0, "Slider style should have height");
    ASSERT(CS_SLIDER_STYLE_DEFAULT.thumb_size > 0, "Slider style should have thumb_size");

    ASSERT(CS_DROPDOWN_STYLE_DEFAULT.width > 0, "Dropdown style should have width");
    ASSERT(CS_DROPDOWN_STYLE_DEFAULT.height > 0, "Dropdown style should have height");
    ASSERT(CS_DROPDOWN_STYLE_DEFAULT.font_size > 0, "Dropdown style should have font_size");

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
    cs_init();

    char text[64] = "Hello";
    int len = 5;

    cs_frame_begin();

    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        CsInputResult r = cs_input(CS_ID("test_input"), text, &len, 64, "Placeholder", NULL);
        (void)r;
    }

    Clay_RenderCommandArray commands = Clay_EndLayout();

    cs_frame_end(0.016f);

    ASSERT(commands.length > 0, "Should produce render commands");

    PASS();
}

static void test_button_renders(void) {
    TEST(button_renders);

    init_clay();
    cs_init();

    cs_frame_begin();

    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        CsButtonResult r = cs_button(CS_ID("test_btn"), "Click Me", NULL);
        (void)r;
    }

    Clay_RenderCommandArray commands = Clay_EndLayout();

    cs_frame_end(0.016f);

    ASSERT(commands.length > 0, "Should produce render commands");

    PASS();
}

static void test_checkbox_renders(void) {
    TEST(checkbox_renders);

    init_clay();
    cs_init();

    bool checked = false;

    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        CsCheckboxResult r = cs_checkbox(CS_ID("test_check"), &checked, "Test Label", NULL);
        (void)r;
    }

    Clay_RenderCommandArray commands = Clay_EndLayout();
    cs_frame_end(0.016f);

    ASSERT(commands.length > 0, "Should produce render commands");

    PASS();
}

static void test_checkbox_toggle(void) {
    TEST(checkbox_toggle);

    init_clay();
    cs_init();

    bool checked = false;
    CsState *g = cs_get_state();

    /* Frame 1: render checkbox unchecked */
    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        CsCheckboxResult r = cs_checkbox(CS_ID("toggle_check"), &checked, "Toggle", NULL);
        ASSERT(!r.changed, "Should not change on first render");
        ASSERT(!r.checked, "Should be unchecked initially");
    }

    Clay_EndLayout();
    cs_frame_end(0.016f);

    /* Frame 2: simulate click - set pending_click and hover */
    cs_frame_begin();
    g->pending_click = true;

    Clay_BeginLayout();
    /* Simulate hover by setting pointer over element */
    Clay_SetPointerState((Clay_Vector2){100, 100}, false);

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        CsCheckboxResult r = cs_checkbox(CS_ID("toggle_check"), &checked, "Toggle", NULL);
        /* Note: hover detection uses previous frame data, so we may not see changed=true */
        /* But we can verify the state mutation */
        (void)r;
    }

    Clay_EndLayout();
    cs_frame_end(0.016f);

    PASS();
}

static void test_checkbox_null_checked(void) {
    TEST(checkbox_null_checked);

    init_clay();
    cs_init();

    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        /* NULL checked pointer should return empty result without crashing */
        CsCheckboxResult r = cs_checkbox(CS_ID("null_check"), NULL, "Label", NULL);
        ASSERT(!r.changed, "Should not change with NULL checked");
        ASSERT(!r.checked, "Should be false with NULL checked");
    }

    Clay_EndLayout();
    cs_frame_end(0.016f);

    PASS();
}

static void test_checkbox_no_label(void) {
    TEST(checkbox_no_label);

    init_clay();
    cs_init();

    bool checked = true;

    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        /* No label should work fine */
        CsCheckboxResult r = cs_checkbox(CS_ID("nolabel_check"), &checked, NULL, NULL);
        ASSERT(r.checked, "Should reflect checked state");
    }

    Clay_RenderCommandArray commands = Clay_EndLayout();
    cs_frame_end(0.016f);

    ASSERT(commands.length > 0, "Should still produce render commands");

    PASS();
}

static void test_checkbox_keyboard_toggle(void) {
    TEST(checkbox_keyboard_toggle);

    init_clay();
    cs_init();

    bool checked = false;
    CsState *g = cs_get_state();

    /* Frame 1: Focus the checkbox */
    cs_frame_begin();
    cs_focus(CS_ID("kb_check"));
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        cs_checkbox(CS_ID("kb_check"), &checked, "KB Toggle", NULL);
    }

    Clay_EndLayout();
    cs_frame_end(0.016f);

    ASSERT(!checked, "Should be unchecked initially");

    /* Frame 2: Simulate Enter key press while focused */
    cs_frame_begin();
    g->pending_enter = true;
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        CsCheckboxResult r = cs_checkbox(CS_ID("kb_check"), &checked, "KB Toggle", NULL);
        ASSERT(r.changed, "Should change on Enter key");
        ASSERT(r.checked, "Should be checked after toggle");
    }

    Clay_EndLayout();
    cs_frame_end(0.016f);

    ASSERT(checked, "State should be toggled");

    PASS();
}

static void test_toggle_renders(void) {
    TEST(toggle_renders);

    init_clay();
    cs_init();

    bool on = false;

    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        CsToggleResult r = cs_toggle(CS_ID("test_toggle"), &on, "Dark Mode", NULL);
        (void)r;
    }

    Clay_RenderCommandArray commands = Clay_EndLayout();
    cs_frame_end(0.016f);

    ASSERT(commands.length > 0, "Should produce render commands");

    PASS();
}

static void test_toggle_switch(void) {
    TEST(toggle_switch);

    init_clay();
    cs_init();

    bool on = false;
    CsState *g = cs_get_state();

    /* Frame 1: Focus the toggle */
    cs_frame_begin();
    cs_focus(CS_ID("switch_toggle"));
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        cs_toggle(CS_ID("switch_toggle"), &on, "Enable", NULL);
    }

    Clay_EndLayout();
    cs_frame_end(0.016f);

    ASSERT(!on, "Should be off initially");

    /* Frame 2: Simulate Enter key press while focused */
    cs_frame_begin();
    g->pending_enter = true;
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        CsToggleResult r = cs_toggle(CS_ID("switch_toggle"), &on, "Enable", NULL);
        ASSERT(r.changed, "Should change on Enter key");
        ASSERT(r.on, "Should be on after toggle");
    }

    Clay_EndLayout();
    cs_frame_end(0.016f);

    ASSERT(on, "State should be toggled on");

    PASS();
}

static void test_toggle_null_on(void) {
    TEST(toggle_null_on);

    init_clay();
    cs_init();

    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        /* NULL on pointer should return empty result without crashing */
        CsToggleResult r = cs_toggle(CS_ID("null_toggle"), NULL, "Label", NULL);
        ASSERT(!r.changed, "Should not change with NULL on");
        ASSERT(!r.on, "Should be false with NULL on");
    }

    Clay_EndLayout();
    cs_frame_end(0.016f);

    PASS();
}

static void test_toggle_no_label(void) {
    TEST(toggle_no_label);

    init_clay();
    cs_init();

    bool on = true;

    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        /* No label should work fine */
        CsToggleResult r = cs_toggle(CS_ID("nolabel_toggle"), &on, NULL, NULL);
        ASSERT(r.on, "Should reflect on state");
    }

    Clay_RenderCommandArray commands = Clay_EndLayout();
    cs_frame_end(0.016f);

    ASSERT(commands.length > 0, "Should still produce render commands");

    PASS();
}

static void test_toggle_label_left(void) {
    TEST(toggle_label_left);

    init_clay();
    cs_init();

    bool on = false;

    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        CsToggleStyle style = CS_TOGGLE_STYLE_DEFAULT;
        style.label_left = true;
        CsToggleResult r = cs_toggle(CS_ID("left_label"), &on, "Left Label", &style);
        (void)r;
    }

    Clay_RenderCommandArray commands = Clay_EndLayout();
    cs_frame_end(0.016f);

    ASSERT(commands.length > 0, "Should produce render commands with left label");

    PASS();
}

static void test_slider_renders(void) {
    TEST(slider_renders);

    init_clay();
    cs_init();

    float value = 50.0f;

    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        CsSliderResult r = cs_slider(CS_ID("test_slider"), &value, 0.0f, 100.0f, "Volume", NULL);
        (void)r;
    }

    Clay_RenderCommandArray commands = Clay_EndLayout();
    cs_frame_end(0.016f);

    ASSERT(commands.length > 0, "Should produce render commands");

    PASS();
}

static void test_slider_null_value(void) {
    TEST(slider_null_value);

    init_clay();
    cs_init();

    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        /* NULL value pointer should return empty result without crashing */
        CsSliderResult r = cs_slider(CS_ID("null_slider"), NULL, 0.0f, 100.0f, "Label", NULL);
        ASSERT(!r.changed, "Should not change with NULL value");
        ASSERT(!r.dragging, "Should not be dragging with NULL value");
    }

    Clay_EndLayout();
    cs_frame_end(0.016f);

    PASS();
}

static void test_slider_invalid_range(void) {
    TEST(slider_invalid_range);

    init_clay();
    cs_init();

    float value = 50.0f;

    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        /* Invalid range (max <= min) should return early */
        CsSliderResult r = cs_slider(CS_ID("bad_range"), &value, 100.0f, 0.0f, "Bad", NULL);
        ASSERT(!r.changed, "Should not change with invalid range");
        ASSERT(r.value == 50.0f, "Should return original value");
    }

    Clay_EndLayout();
    cs_frame_end(0.016f);

    PASS();
}

static void test_slider_no_label(void) {
    TEST(slider_no_label);

    init_clay();
    cs_init();

    float value = 25.0f;

    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        CsSliderResult r = cs_slider(CS_ID("nolabel_slider"), &value, 0.0f, 100.0f, NULL, NULL);
        ASSERT(r.value == 25.0f, "Should reflect value");
    }

    Clay_RenderCommandArray commands = Clay_EndLayout();
    cs_frame_end(0.016f);

    ASSERT(commands.length > 0, "Should still produce render commands");

    PASS();
}

static void test_slider_show_value(void) {
    TEST(slider_show_value);

    init_clay();
    cs_init();

    float value = 75.0f;

    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        CsSliderStyle style = CS_SLIDER_STYLE_DEFAULT;
        style.show_value = true;
        CsSliderResult r = cs_slider(CS_ID("show_val"), &value, 0.0f, 100.0f, "With Value", &style);
        (void)r;
    }

    Clay_RenderCommandArray commands = Clay_EndLayout();
    cs_frame_end(0.016f);

    ASSERT(commands.length > 0, "Should produce render commands with value display");

    PASS();
}

/* ============================================================================
 * Input-Before-Render Regression Tests
 *
 * These tests verify that input handling happens BEFORE rendering, so that
 * state changes are reflected immediately in the same frame (no 1-frame lag).
 * ============================================================================ */

static void test_slider_keyboard_input_immediate(void) {
    TEST(slider_keyboard_input_immediate);

    init_clay();
    cs_init();

    float value = 0.5f;
    const CsSliderStyle style = {
        .width = 100.0f,
        .height = 8.0f,
        .thumb_size = 10.0f,
        .step = 0.1f,  /* 10% steps */
        .show_value = true
    };

    /* Focus the slider and set pending left arrow key */
    uint32_t slider_id = CS_ID("immediate_slider");
    cs_focus(slider_id);

    CsState *g = cs_get_state();
    g->pending_arrow_left = true;  /* Request value decrease */

    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        CsSliderResult r = cs_slider(slider_id, &value, 0.0f, 1.0f, "Test", &style);

        /* CRITICAL: Value should be updated IMMEDIATELY, not next frame */
        ASSERT(r.changed, "Slider should report change on same frame as input");
        ASSERT(value < 0.5f, "Value should be decreased on same frame");
        ASSERT(r.value < 0.5f, "Result value should match decreased value");
    }

    Clay_EndLayout();
    cs_frame_end(0.016f);

    PASS();
}

static void test_dropdown_enter_closes_immediate(void) {
    TEST(dropdown_enter_closes_immediate);

    init_clay();
    cs_init();
    cs_dropdown_close_all();

    int selected = 1;
    const char *options[] = {"Low", "Medium", "High"};
    uint32_t dd_id = CS_ID("immediate_dropdown");

    /* First, open the dropdown */
    cs_focus(dd_id);
    CsState *g = cs_get_state();
    g->pending_arrow_down = true;  /* Opens dropdown if closed */

    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        cs_dropdown(dd_id, &selected, options, 3, NULL);
    }

    Clay_EndLayout();
    cs_frame_end(0.016f);

    ASSERT(cs_dropdown_is_open(dd_id), "Dropdown should be open after arrow down");

    /* Now close it with Enter - should close on SAME frame */
    g->pending_enter = true;

    cs_frame_begin();
    Clay_BeginLayout();

    CsDropdownResult r = {0};
    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        r = cs_dropdown(dd_id, &selected, options, 3, NULL);
    }

    /* CRITICAL: Dropdown should be closed IMMEDIATELY, not next frame */
    ASSERT(r.closed, "Dropdown should report closed on same frame as Enter");
    ASSERT(!cs_dropdown_is_open(dd_id), "Dropdown should be closed after Enter");

    Clay_EndLayout();
    cs_frame_end(0.016f);

    PASS();
}

static void test_dropdown_renders(void) {
    TEST(dropdown_renders);

    init_clay();
    cs_init();
    cs_dropdown_close_all();

    int selected = 0;
    const char *options[] = {"Option 1", "Option 2", "Option 3"};

    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        CsDropdownResult r = cs_dropdown(CS_ID("test_dropdown"), &selected, options, 3, NULL);
        (void)r;
    }

    Clay_RenderCommandArray commands = Clay_EndLayout();
    cs_frame_end(0.016f);

    ASSERT(commands.length > 0, "Should produce render commands");

    PASS();
}

static void test_dropdown_null_params(void) {
    TEST(dropdown_null_params);

    init_clay();
    cs_init();

    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        /* NULL selected pointer should return empty result without crashing */
        const char *options[] = {"A", "B"};
        CsDropdownResult r1 = cs_dropdown(CS_ID("null_sel"), NULL, options, 2, NULL);
        ASSERT(!r1.changed, "Should not change with NULL selected");

        /* NULL options should return empty result */
        int sel = 0;
        CsDropdownResult r2 = cs_dropdown(CS_ID("null_opts"), &sel, NULL, 2, NULL);
        ASSERT(!r2.changed, "Should not change with NULL options");

        /* Zero count should return empty result */
        CsDropdownResult r3 = cs_dropdown(CS_ID("zero_count"), &sel, options, 0, NULL);
        ASSERT(!r3.changed, "Should not change with zero count");
    }

    Clay_EndLayout();
    cs_frame_end(0.016f);

    PASS();
}

static void test_dropdown_selection_clamp(void) {
    TEST(dropdown_selection_clamp);

    init_clay();
    cs_init();
    cs_dropdown_close_all();

    const char *options[] = {"A", "B", "C"};

    /* Test negative selection gets clamped to 0 */
    int sel_neg = -5;
    cs_frame_begin();
    Clay_BeginLayout();
    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        CsDropdownResult r = cs_dropdown(CS_ID("neg_sel"), &sel_neg, options, 3, NULL);
        ASSERT(r.selected == 0, "Negative selection should clamp to 0");
    }
    Clay_EndLayout();
    cs_frame_end(0.016f);

    ASSERT(sel_neg == 0, "Negative selection should be clamped to 0");

    /* Test out-of-bounds selection gets clamped */
    int sel_high = 10;
    cs_frame_begin();
    Clay_BeginLayout();
    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        CsDropdownResult r = cs_dropdown(CS_ID("high_sel"), &sel_high, options, 3, NULL);
        ASSERT(r.selected == 2, "High selection should clamp to count-1");
    }
    Clay_EndLayout();
    cs_frame_end(0.016f);

    ASSERT(sel_high == 2, "High selection should be clamped to count-1");

    PASS();
}

static void test_dropdown_open_close(void) {
    TEST(dropdown_open_close);

    init_clay();
    cs_init();
    cs_dropdown_close_all();

    uint32_t dd_id = CS_ID("open_close_dd");
    ASSERT(!cs_dropdown_is_open(dd_id), "Should not be open initially");

    /* Close all should work even when nothing is open */
    cs_dropdown_close_all();
    ASSERT(!cs_dropdown_is_open(dd_id), "Should still not be open");

    /* Close specific should work even when not open */
    cs_dropdown_close(dd_id);
    ASSERT(!cs_dropdown_is_open(dd_id), "Should still not be open after close");

    PASS();
}

static void test_dropdown_default_style(void) {
    TEST(dropdown_default_style);

    ASSERT(CS_DROPDOWN_STYLE_DEFAULT.width > 0, "Should have width");
    ASSERT(CS_DROPDOWN_STYLE_DEFAULT.height > 0, "Should have height");
    ASSERT(CS_DROPDOWN_STYLE_DEFAULT.font_size > 0, "Should have font_size");

    PASS();
}

static void test_multiple_inputs(void) {
    TEST(multiple_inputs);

    init_clay();
    cs_init();

    char text1[64] = "First";
    char text2[64] = "Second";
    char text3[64] = "Third";
    int len1 = 5, len2 = 6, len3 = 5;

    cs_frame_begin();

    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = {
            .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .childGap = 8
        }
    }) {
        cs_input(CS_ID("input1"), text1, &len1, 64, NULL, NULL);
        cs_input(CS_ID("input2"), text2, &len2, 64, NULL, NULL);
        cs_input(CS_ID("input3"), text3, &len3, 64, NULL, NULL);
    }

    Clay_RenderCommandArray commands = Clay_EndLayout();

    cs_frame_end(0.016f);

    /* Each input produces at least 2 commands (rect + text or rect + border) */
    ASSERT(commands.length >= 3, "Should have commands for all inputs");

    /* Verify IDs are unique */
    uint32_t id1 = CS_ID("input1");
    uint32_t id2 = CS_ID("input2");
    uint32_t id3 = CS_ID("input3");
    ASSERT(id1 != id2 && id2 != id3 && id1 != id3, "IDs should be unique");

    PASS();
}

static void test_input_focus_on_click(void) {
    TEST(input_focus_on_click);

    init_clay();
    cs_init();

    char text[64] = "";
    int len = 0;

    /* Simulate click at input position */
    Clay_SetPointerState((Clay_Vector2){100, 100}, false);
    cs_set_pending_click();

    cs_frame_begin();

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
            CsInputResult r = cs_input(CS_ID("test"), text, &len, 64, NULL, NULL);

            /* If pointer is over input and we clicked, should focus */
            if (r.focused) {
                /* This would indicate the click was detected */
            }
        }
    }

    Clay_EndLayout();

    cs_frame_end(0.016f);

    /* Focus detection depends on Clay_PointerOver which needs proper bounds */
    /* For now just verify no crash */

    PASS();
}

static void test_input_validation(void) {
    TEST(input_validation);

    init_clay();
    cs_init();

    char text[64] = "hello";
    int len = 5;

    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        /* Test with corrupted length (too large) */
        int bad_len = 100;  /* Larger than max_len */
        char bad_text[64] = "test";
        cs_input(CS_ID("bad_input"), bad_text, &bad_len, 64, NULL, NULL);
        ASSERT(bad_len <= 63, "Length should be clamped to max_len-1");

        /* Test with negative length */
        int neg_len = -5;
        char neg_text[64] = "";
        cs_input(CS_ID("neg_input"), neg_text, &neg_len, 64, NULL, NULL);
        ASSERT(neg_len == 0, "Negative length should be clamped to 0");

        /* Normal case should work */
        CsInputResult r = cs_input(CS_ID("good_input"), text, &len, 64, NULL, NULL);
        ASSERT(len == 5, "Normal length should be unchanged");
        (void)r;
    }

    Clay_EndLayout();
    cs_frame_end(0.016f);

    PASS();
}

/* ============================================================================
 * Map Component Tests
 * ============================================================================ */

static void test_map_result_init(void) {
    TEST(map_result_init);

    CsMapResult r = {0};

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
    double x0 = cs_map_lon_to_tile_x(0.0, 0);
    ASSERT(x0 >= 0.49 && x0 <= 0.51, "lon=0 at z=0 should be ~0.5");

    /* At zoom 1, world is 2x2 tiles */
    double x1_west = cs_map_lon_to_tile_x(-180.0, 1);
    double x1_east = cs_map_lon_to_tile_x(180.0, 1);
    ASSERT(x1_west >= -0.01 && x1_west <= 0.01, "lon=-180 at z=1 should be ~0");
    ASSERT(x1_east >= 1.99 && x1_east <= 2.01, "lon=180 at z=1 should be ~2");

    PASS();
}

static void test_map_projection_lat_to_tile(void) {
    TEST(map_projection_lat_to_tile);

    /* At zoom 0, equator is at y=0.5 */
    double y0 = cs_map_lat_to_tile_y(0.0, 0);
    ASSERT(y0 >= 0.49 && y0 <= 0.51, "lat=0 at z=0 should be ~0.5");

    /* Extreme latitudes should be clamped and not produce NaN */
    double y_north = cs_map_lat_to_tile_y(90.0, 1);
    double y_south = cs_map_lat_to_tile_y(-90.0, 1);
    ASSERT(y_north == y_north, "North pole should not produce NaN");  /* NaN != NaN */
    ASSERT(y_south == y_south, "South pole should not produce NaN");

    PASS();
}

static void test_map_projection_roundtrip(void) {
    TEST(map_projection_roundtrip);

    /* lon -> tile_x -> lon should be consistent */
    double orig_lon = 19.0402;  /* Budapest */
    double tile_x = cs_map_lon_to_tile_x(orig_lon, 12);
    double recovered_lon = cs_map_tile_x_to_lon(tile_x, 12);
    ASSERT(recovered_lon > orig_lon - 0.001 && recovered_lon < orig_lon + 0.001,
           "Longitude roundtrip should match");

    /* lat -> tile_y -> lat should be consistent */
    double orig_lat = 47.4979;  /* Budapest */
    double tile_y = cs_map_lat_to_tile_y(orig_lat, 12);
    double recovered_lat = cs_map_tile_y_to_lat(tile_y, 12);
    ASSERT(recovered_lat > orig_lat - 0.001 && recovered_lat < orig_lat + 0.001,
           "Latitude roundtrip should match");

    PASS();
}

static void test_map_scroll(void) {
    TEST(map_scroll);

    /* Normal zoom in */
    ASSERT(cs_map_scroll(10, 1, 0, 19) == 11, "Zoom in should increase");

    /* Normal zoom out */
    ASSERT(cs_map_scroll(10, -1, 0, 19) == 9, "Zoom out should decrease");

    /* Clamp at max */
    ASSERT(cs_map_scroll(19, 1, 0, 19) == 19, "Should clamp at max");

    /* Clamp at min */
    ASSERT(cs_map_scroll(0, -1, 0, 19) == 0, "Should clamp at min");

    /* Custom range */
    ASSERT(cs_map_scroll(5, 1, 5, 10) == 6, "Should work with custom range");
    ASSERT(cs_map_scroll(5, -1, 5, 10) == 5, "Should clamp at custom min");

    PASS();
}

static void test_map_pointer_handling(void) {
    TEST(map_pointer_handling);

    uint32_t map_id = CS_ID("test_map");

    /* Initially not dragging */
    ASSERT(!cs_map_is_dragging(map_id), "Should not be dragging initially");

    /* Start drag - new signature with map dimensions and zoom */
    cs_map_pointer_down(map_id, 47.4979, 19.0402, 400.0f, 300.0f, 800.0f, 600.0f, 12);
    ASSERT(cs_map_is_dragging(map_id), "Should be dragging after pointer_down");

    /* Move pointer */
    double new_lat, new_lon;
    bool moved = cs_map_pointer_move(map_id, 12, 420.0f, 310.0f, 800.0f, 600.0f, &new_lat, &new_lon);
    ASSERT(moved, "pointer_move should return true when dragging");

    /* End drag with minimal movement - should detect as click */
    cs_map_pointer_up(map_id, 401.0f, 301.0f);  /* Only 1-2 pixels moved */
    ASSERT(!cs_map_is_dragging(map_id), "Should not be dragging after pointer_up");

    PASS();
}

static void test_map_default_style(void) {
    TEST(map_default_style);

    ASSERT(CS_MAP_STYLE_DEFAULT.min_zoom == 0, "Default min_zoom should be 0");
    ASSERT(CS_MAP_STYLE_DEFAULT.max_zoom == 19, "Default max_zoom should be 19");
    ASSERT(CS_MAP_STYLE_DEFAULT.min_lat < 0, "Default min_lat should be negative");
    ASSERT(CS_MAP_STYLE_DEFAULT.max_lat > 0, "Default max_lat should be positive");

    PASS();
}

/* ============================================================================
 * Multi-Map Independence Tests
 * ============================================================================ */

static void test_multiple_maps_independent_state(void) {
    TEST(multiple_maps_independent_state);

    uint32_t map1 = CS_ID("map1");
    uint32_t map2 = CS_ID("map2");

    /* Verify IDs are different */
    ASSERT(map1 != map2, "Map IDs should be different");

    /* Start drag on map1 */
    cs_map_pointer_down(map1, 47.0, 19.0, 100.0f, 100.0f, 800.0f, 600.0f, 12);
    ASSERT(cs_map_is_dragging(map1), "map1 should be dragging");
    ASSERT(!cs_map_is_dragging(map2), "map2 should NOT be dragging");

    /* Start drag on map2 */
    cs_map_pointer_down(map2, 48.0, 20.0, 200.0f, 200.0f, 800.0f, 600.0f, 12);
    ASSERT(cs_map_is_dragging(map1), "map1 should still be dragging");
    ASSERT(cs_map_is_dragging(map2), "map2 should now be dragging");

    /* Stop map1 drag */
    cs_map_pointer_up(map1, 120.0f, 120.0f);
    ASSERT(!cs_map_is_dragging(map1), "map1 should stop dragging");
    ASSERT(cs_map_is_dragging(map2), "map2 should still be dragging");

    /* Stop map2 drag */
    cs_map_pointer_up(map2, 220.0f, 220.0f);
    ASSERT(!cs_map_is_dragging(map2), "map2 should stop dragging");

    PASS();
}

static void test_per_map_overlays(void) {
    TEST(per_map_overlays);

    init_clay();
    cs_init();

    uint32_t map1 = CS_ID("overlay_map1");
    uint32_t map2 = CS_ID("overlay_map2");

    double lat1 = 47.0, lon1 = 19.0;
    double lat2 = 48.0, lon2 = 20.0;
    int zoom1 = 12, zoom2 = 10;

    /* Frame with overlays on map1 */
    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        /* Map1 with 2 markers */
        cs_map_begin(map1, &lat1, &lon1, &zoom1, 400, 300, NULL);
        cs_marker(CS_ID("m1a"), 47.0, 19.0, NULL);
        cs_marker(CS_ID("m1b"), 47.1, 19.1, NULL);
        cs_map_end();

        /* Map2 with 1 marker */
        cs_map_begin(map2, &lat2, &lon2, &zoom2, 400, 300, NULL);
        cs_marker(CS_ID("m2a"), 48.0, 20.0, NULL);
        cs_map_end();
    }

    Clay_EndLayout();
    cs_frame_end(0.016f);

    /* Verify overlay counts */
    ASSERT(cs_map_overlay_count(map1) == 2, "map1 should have 2 overlays");
    ASSERT(cs_map_overlay_count(map2) == 1, "map2 should have 1 overlay");

    /* Verify overlay IDs are correct for each map */
    ASSERT(cs_map_overlay_id(map1, 0) == CS_ID("m1a"), "map1 overlay 0 should be m1a");
    ASSERT(cs_map_overlay_id(map1, 1) == CS_ID("m1b"), "map1 overlay 1 should be m1b");
    ASSERT(cs_map_overlay_id(map2, 0) == CS_ID("m2a"), "map2 overlay 0 should be m2a");

    PASS();
}

static void test_marker_hit_test(void) {
    TEST(marker_hit_test);

    init_clay();
    cs_init();

    uint32_t map_id = CS_ID("hit_test_map");
    double lat = 47.0, lon = 19.0;
    int zoom = 12;

    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        cs_map_begin(map_id, &lat, &lon, &zoom, 800, 600, NULL);
        /* Marker at center with radius 10 */
        cs_marker(CS_ID("center_marker"), 47.0, 19.0, &(CsMarkerStyle){
            .color = {1, 0, 0, 1}, .radius = 10.0f, .border_color = {1, 1, 1, 1}, .border_width = 2
        });
        cs_map_end();
    }

    Clay_EndLayout();
    cs_frame_end(0.016f);

    /* Hit test at center (400, 300) - should hit the marker */
    uint32_t hit_center = cs_map_hit_test(map_id, 400.0f, 300.0f, 47.0, 19.0, 12, 800.0f, 600.0f);
    ASSERT(hit_center == CS_ID("center_marker"), "Should hit marker at center");

    /* Hit test slightly off center but within radius */
    uint32_t hit_near = cs_map_hit_test(map_id, 405.0f, 305.0f, 47.0, 19.0, 12, 800.0f, 600.0f);
    ASSERT(hit_near == CS_ID("center_marker"), "Should hit marker near center");

    /* Hit test far from marker - should miss */
    uint32_t hit_far = cs_map_hit_test(map_id, 100.0f, 100.0f, 47.0, 19.0, 12, 800.0f, 600.0f);
    ASSERT(hit_far == 0, "Should miss marker when far away");

    PASS();
}

static void test_polyline_hit_test(void) {
    TEST(polyline_hit_test);

    init_clay();
    cs_init();

    uint32_t map_id = CS_ID("polyline_hit_map");
    double lat = 47.0, lon = 19.0;
    int zoom = 12;

    /* Create a horizontal polyline through the center */
    CsGeoPoint points[] = {
        {47.0, 18.9},  /* Left of center */
        {47.0, 19.1}   /* Right of center */
    };

    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        cs_map_begin(map_id, &lat, &lon, &zoom, 800, 600, NULL);
        cs_polyline(CS_ID("test_line"), points, 2, &(CsPolylineStyle){
            .color = {0, 0, 1, 1}, .width = 6.0f
        });
        cs_map_end();
    }

    Clay_EndLayout();
    cs_frame_end(0.016f);

    /* Hit test on the line (center, y=300) - should hit */
    uint32_t hit_on = cs_map_hit_test(map_id, 400.0f, 300.0f, 47.0, 19.0, 12, 800.0f, 600.0f);
    ASSERT(hit_on == CS_ID("test_line"), "Should hit polyline at center");

    /* Hit test slightly above (still within width/2 + tolerance) */
    uint32_t hit_near = cs_map_hit_test(map_id, 400.0f, 295.0f, 47.0, 19.0, 12, 800.0f, 600.0f);
    ASSERT(hit_near == CS_ID("test_line"), "Should hit polyline slightly off");

    /* Hit test far from line - should miss */
    uint32_t hit_far = cs_map_hit_test(map_id, 400.0f, 100.0f, 47.0, 19.0, 12, 800.0f, 600.0f);
    ASSERT(hit_far == 0, "Should miss polyline when far");

    PASS();
}

static void test_draggable_marker(void) {
    TEST(draggable_marker);

    init_clay();
    cs_init();

    uint32_t map_id = CS_ID("drag_marker_map");
    double lat = 47.0, lon = 19.0;
    int zoom = 12;

    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        cs_map_begin(map_id, &lat, &lon, &zoom, 800, 600, NULL);
        /* Draggable marker at center */
        cs_marker(CS_ID("drag_marker"), 47.0, 19.0, &(CsMarkerStyle){
            .color = {1, 0, 0, 1}, .radius = 10.0f, .border_color = {1, 1, 1, 1},
            .border_width = 2, .draggable = true
        });
        cs_map_end();
    }

    Clay_EndLayout();
    cs_frame_end(0.016f);

    /* Verify marker is draggable */
    ASSERT(cs_map_overlay_marker_draggable(map_id, 0) == true, "Marker should be draggable");

    /* Click on marker - should start marker drag, not map drag */
    cs_map_pointer_down(map_id, 47.0, 19.0, 400.0f, 300.0f, 800.0f, 600.0f, 12);
    ASSERT(!cs_map_is_dragging(map_id), "Should NOT be dragging map");
    ASSERT(cs_map_is_dragging_marker(map_id), "Should be dragging marker");

    /* Get dragging overlay ID */
    uint32_t dragging = cs_map_get_dragging_overlay(map_id);
    ASSERT(dragging == CS_ID("drag_marker"), "Should be dragging the marker");

    /* End drag */
    cs_map_pointer_up(map_id, 420.0f, 320.0f);
    ASSERT(!cs_map_is_dragging_marker(map_id), "Should not be dragging marker after up");

    PASS();
}

static void test_non_draggable_marker_falls_through(void) {
    TEST(non_draggable_marker_falls_through);

    init_clay();
    cs_init();

    uint32_t map_id = CS_ID("nondrag_map");
    double lat = 47.0, lon = 19.0;
    int zoom = 12;

    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        cs_map_begin(map_id, &lat, &lon, &zoom, 800, 600, NULL);
        /* Non-draggable marker at center (default draggable=false) */
        cs_marker(CS_ID("static_marker"), 47.0, 19.0, NULL);
        cs_map_end();
    }

    Clay_EndLayout();
    cs_frame_end(0.016f);

    /* Verify marker is NOT draggable */
    ASSERT(cs_map_overlay_marker_draggable(map_id, 0) == false, "Marker should NOT be draggable");

    /* Click on marker - should fall through to map drag */
    cs_map_pointer_down(map_id, 47.0, 19.0, 400.0f, 300.0f, 800.0f, 600.0f, 12);
    ASSERT(cs_map_is_dragging(map_id), "Should be dragging map");
    ASSERT(!cs_map_is_dragging_marker(map_id), "Should NOT be dragging marker");

    /* End drag */
    cs_map_pointer_up(map_id, 420.0f, 320.0f);

    PASS();
}

static void test_hovered_overlay_state(void) {
    TEST(hovered_overlay_state);

    uint32_t map_id = CS_ID("hover_map");

    /* Initially no hovered overlay */
    ASSERT(cs_map_get_hovered_overlay(map_id) == 0, "Initially no hovered overlay");

    /* Set hovered overlay */
    cs_map_set_hovered_overlay(map_id, CS_ID("some_overlay"));
    ASSERT(cs_map_get_hovered_overlay(map_id) == CS_ID("some_overlay"), "Hovered should be set");

    /* Clear hovered overlay */
    cs_map_set_hovered_overlay(map_id, 0);
    ASSERT(cs_map_get_hovered_overlay(map_id) == 0, "Hovered should be cleared");

    PASS();
}

/* ============================================================================
 * Font Metrics Tests
 * ============================================================================ */

/* Note: Font metrics require cs_clay.h which isn't included in this test.
 * These tests verify the text measurement callback mechanism works. */

static void test_text_measurement_callback(void) {
    TEST(text_measurement_callback);

    init_clay();
    cs_init();

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

    cs_init();
    cs_frame_begin();

    /* Register some focusable elements */
    cs_register_focusable(100);
    cs_register_focusable(200);
    cs_register_focusable(300);

    ASSERT(cs_focusable_count() == 3, "Should have 3 focusables");

    /* Duplicate registration should be ignored */
    cs_register_focusable(100);
    ASSERT(cs_focusable_count() == 3, "Duplicates should be ignored");

    /* Zero ID should be ignored */
    cs_register_focusable(0);
    ASSERT(cs_focusable_count() == 3, "Zero ID should be ignored");

    PASS();
}

static void test_tab_navigation_focus_next(void) {
    TEST(tab_navigation_focus_next);

    cs_init();
    cs_frame_begin();

    cs_register_focusable(100);
    cs_register_focusable(200);
    cs_register_focusable(300);

    /* Initially nothing focused */
    ASSERT(cs_focused_id() == 0, "Initially unfocused");

    /* Tab to first element */
    bool changed = cs_focus_next();
    ASSERT(changed, "Focus should change");
    ASSERT(cs_focused_id() == 100, "Should focus first element");

    /* Tab to second */
    cs_focus_next();
    ASSERT(cs_focused_id() == 200, "Should focus second element");

    /* Tab to third */
    cs_focus_next();
    ASSERT(cs_focused_id() == 300, "Should focus third element");

    /* Tab wraps to first */
    cs_focus_next();
    ASSERT(cs_focused_id() == 100, "Should wrap to first element");

    PASS();
}

static void test_tab_navigation_focus_prev(void) {
    TEST(tab_navigation_focus_prev);

    cs_init();
    cs_frame_begin();

    cs_register_focusable(100);
    cs_register_focusable(200);
    cs_register_focusable(300);

    /* Start with nothing focused - Shift+Tab goes to last */
    cs_focus_prev();
    ASSERT(cs_focused_id() == 300, "Shift+Tab from nothing should go to last");

    /* Shift+Tab to previous */
    cs_focus_prev();
    ASSERT(cs_focused_id() == 200, "Should focus previous element");

    cs_focus_prev();
    ASSERT(cs_focused_id() == 100, "Should focus first element");

    /* Wrap to last */
    cs_focus_prev();
    ASSERT(cs_focused_id() == 300, "Should wrap to last element");

    PASS();
}

static void test_tab_key_handling(void) {
    TEST(tab_key_handling);

    cs_init();
    cs_frame_begin();

    cs_register_focusable(100);
    cs_register_focusable(200);

    /* Tab key (code 9) should focus next */
    bool handled = cs_key_down(9, false, false);  /* Tab */
    ASSERT(handled, "Tab should be handled");
    ASSERT(cs_focused_id() == 100, "Tab should focus first element");

    /* Another Tab */
    cs_key_down(9, false, false);
    ASSERT(cs_focused_id() == 200, "Tab should focus second element");

    /* Shift+Tab */
    cs_key_down(9, true, false);  /* Shift+Tab */
    ASSERT(cs_focused_id() == 100, "Shift+Tab should focus previous");

    PASS();
}

static void test_tab_empty_focusables(void) {
    TEST(tab_empty_focusables);

    cs_init();
    cs_frame_begin();
    /* No focusables registered */

    bool changed = cs_focus_next();
    ASSERT(!changed, "Should not change focus with no focusables");
    ASSERT(cs_focused_id() == 0, "Should remain unfocused");

    changed = cs_focus_prev();
    ASSERT(!changed, "Should not change focus with no focusables");

    PASS();
}

static void test_focusable_reset_each_frame(void) {
    TEST(focusable_reset_each_frame);

    cs_init();

    /* Frame 1: register elements */
    cs_frame_begin();
    cs_register_focusable(100);
    cs_register_focusable(200);
    ASSERT(cs_focusable_count() == 2, "Should have 2 focusables");
    cs_frame_end(0.016f);

    /* Frame 2: focusables should be reset */
    cs_frame_begin();
    ASSERT(cs_focusable_count() == 0, "Focusables should reset each frame");

    /* Register different elements */
    cs_register_focusable(300);
    ASSERT(cs_focusable_count() == 1, "Should have 1 new focusable");

    PASS();
}

static void test_tab_preserves_cursor_across_focus(void) {
    TEST(tab_preserves_cursor_across_focus);

    cs_init();

    uint32_t input_id = CS_ID("search_input");
    uint32_t btn1_id = CS_ID("button1");
    uint32_t btn2_id = CS_ID("button2");

    char text[64] = "";
    int len = 0;
    CsState *g = cs_get_state();

    /* Frame 1: Register focusables, focus input, type text */
    cs_frame_begin();
    cs_register_focusable(input_id);
    cs_register_focusable(btn1_id);
    cs_register_focusable(btn2_id);

    /* Focus the input */
    cs_focus(input_id);

    /* Set up active buffer (simulating what cs_input does) */
    g->active_text = text;
    g->active_len = &len;
    g->active_max_len = 64;

    /* Type "hello" */
    cs_key_char('h');
    cs_key_char('e');
    cs_key_char('l');
    cs_key_char('l');
    cs_key_char('o');

    ASSERT(len == 5, "Should have typed 5 chars");
    int cursor_after_typing = cs_cursor_pos();
    ASSERT(cursor_after_typing == 5, "Cursor should be at 5");

    cs_frame_end(0.016f);

    /* Frame 2: Tab to button1 */
    cs_frame_begin();
    cs_register_focusable(input_id);
    cs_register_focusable(btn1_id);
    cs_register_focusable(btn2_id);

    /* Clear active buffer (input not focused this frame) */
    g->active_text = NULL;
    g->active_len = NULL;

    cs_key_down(9, false, false);  /* Tab */
    ASSERT(cs_focused_id() == btn1_id, "Should focus button1");

    cs_frame_end(0.016f);

    /* Frame 3: Shift+Tab back to input */
    cs_frame_begin();
    cs_register_focusable(input_id);
    cs_register_focusable(btn1_id);
    cs_register_focusable(btn2_id);

    cs_key_down(9, true, false);  /* Shift+Tab */
    ASSERT(cs_focused_id() == input_id, "Should focus input again");

    /* Set active buffer again (simulating cs_input focus) */
    g->active_text = text;
    g->active_len = &len;
    g->active_max_len = 64;

    /* Check cursor position - should be preserved! */
    int cursor_after_tab = cs_cursor_pos();
    ASSERT(cursor_after_tab == 5, "Cursor should still be at 5 after Tab round-trip");

    cs_frame_end(0.016f);

    PASS();
}

/* ============================================================================
 * Widget State Store Tests
 * ============================================================================ */

static void test_widget_state_persistence(void) {
    TEST(widget_state_persistence);

    cs_init();

    /* Get state for widget, modify it */
    uint32_t id = CS_ID("test_widget");
    cs_focus(id);

    /* Simulate typing - need active_text buffer */
    char text[64] = "hello";
    int len = 5;
    CsState *g = cs_get_state();
    g->active_text = text;
    g->active_len = &len;
    g->active_max_len = 64;

    /* Type a character */
    cs_key_char('!');
    ASSERT(len == 6, "Should have typed a character");

    int cursor_after = cs_cursor_pos();
    ASSERT(cursor_after == 1, "Cursor should be at 1");

    /* Focus something else, then come back */
    cs_focus(CS_ID("other_widget"));
    cs_focus(id);

    /* Cursor position should be preserved in widget state */
    /* This is the ClayShards behavior - widget state persists */
    int cursor_refocus = cs_cursor_pos();
    ASSERT(cursor_refocus == 1, "Cursor persists on refocus");

    PASS();
}

static void test_widget_state_isolation(void) {
    TEST(widget_state_isolation);

    cs_init();

    uint32_t id1 = CS_ID("widget1");
    uint32_t id2 = CS_ID("widget2");

    /* Different IDs should get different state */
    ASSERT(id1 != id2, "IDs should be different");

    /* Focus widget1, set some state */
    cs_focus(id1);
    ASSERT(cs_focused_id() == id1, "Widget1 should be focused");

    /* Focus widget2 */
    cs_focus(id2);
    ASSERT(cs_focused_id() == id2, "Widget2 should be focused");

    PASS();
}

/* ============================================================================
 * Error Tracking Tests
 * ============================================================================ */

static void test_error_tracking_init(void) {
    TEST(error_tracking_init);

    cs_init();
    cs_clear_errors();

    ASSERT(cs_get_last_error() == CS_ERR_NONE, "No error initially");
    ASSERT(cs_get_error_count() == 0, "Error count should be 0");

    PASS();
}

static void test_error_tracking_record(void) {
    TEST(error_tracking_record);

    cs_init();
    cs_clear_errors();

    /* Record an error */
    cs_record_error(CS_ERR_ALLOC_FAILED);
    ASSERT(cs_get_last_error() == CS_ERR_ALLOC_FAILED, "Error should be recorded");
    ASSERT(cs_get_error_count() == 1, "Error count should be 1");

    /* Record another error */
    cs_record_error(CS_ERR_CAPACITY_EXCEEDED);
    ASSERT(cs_get_last_error() == CS_ERR_CAPACITY_EXCEEDED, "Last error updated");
    ASSERT(cs_get_error_count() == 2, "Error count should be 2");

    /* Clear errors */
    cs_clear_errors();
    ASSERT(cs_get_last_error() == CS_ERR_NONE, "Error should be cleared");
    ASSERT(cs_get_error_count() == 0, "Error count should be 0");

    PASS();
}

static void test_widget_state_stress(void) {
    TEST(widget_state_stress);

    cs_init();

    /* Fill most of the widget state hash table (capacity = 256) */
    for (int i = 1; i <= 200; i++) {
        uint32_t id = 1000 + i;  /* Different IDs */
        CsWidgetState *w = cs_widget_state(id);
        ASSERT(w != NULL, "Should get widget state");
        ASSERT(w->id == id, "Widget ID should match");
    }

    /* All should still be retrievable */
    for (int i = 1; i <= 200; i++) {
        uint32_t id = 1000 + i;
        CsWidgetState *w = cs_widget_state(id);
        ASSERT(w != NULL, "Should still retrieve widget state");
        ASSERT(w->id == id, "Widget ID should still match");
    }

    PASS();
}

static void test_map_state_table_full(void) {
    TEST(map_state_table_full);

    init_clay();
    cs_init();
    cs_map_cleanup();
    cs_clear_errors();

    double lat = 47.0, lon = 19.0;
    int zoom = 10;

    /* Fill the map state table (capacity = 16) by creating maps */
    /* Note: cs_map_begin initializes map state internally */
    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        for (int i = 1; i <= CS_MAP_STATE_CAPACITY; i++) {
            uint32_t id = 5000 + i;
            cs_map(id, &lat, &lon, &zoom, 100, 100, NULL);
        }
    }

    Clay_EndLayout();
    cs_frame_end(0.016f);

    /* Now the table should be full. Try to create one more. */
    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root2"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        uint32_t extra_id = 5000 + CS_MAP_STATE_CAPACITY + 1;
        CsMapResult r = cs_map(extra_id, &lat, &lon, &zoom, 100, 100, NULL);
        /* When table is full, cs_map returns empty result */
        (void)r;
        ASSERT(cs_get_last_error() == CS_ERR_CAPACITY_EXCEEDED, "Should record capacity exceeded");
    }

    Clay_EndLayout();
    cs_frame_end(0.016f);

    /* Cleanup */
    cs_map_cleanup();
    cs_clear_errors();

    PASS();
}

/* ============================================================================
 * Custom Allocator Tests
 * ============================================================================ */

/* Tracking allocator for testing */
static int g_alloc_count = 0;
static int g_free_count = 0;
static size_t g_total_allocated = 0;

static void* tracking_alloc(size_t size, void *user_data) {
    (void)user_data;
    g_alloc_count++;
    g_total_allocated += size;
    return malloc(size);
}

static void* tracking_realloc(void *ptr, size_t size, void *user_data) {
    (void)user_data;
    /* Count reallocs as allocations for testing purposes */
    g_alloc_count++;
    g_total_allocated += size;  /* Note: doesn't subtract old size, but good enough for testing */
    return realloc(ptr, size);
}

static void tracking_free(void *ptr, void *user_data) {
    (void)user_data;
    if (ptr) g_free_count++;
    free(ptr);
}

static void test_custom_allocator_set(void) {
    TEST(custom_allocator_set);

    /* Reset tracking */
    g_alloc_count = 0;
    g_free_count = 0;
    g_total_allocated = 0;

    /* Set custom allocator */
    CsAllocator tracking = {
        .alloc = tracking_alloc,
        .realloc = tracking_realloc,
        .free = tracking_free,
        .user_data = NULL
    };
    cs_set_allocator(&tracking);

    const CsAllocator *current = cs_get_allocator();
    ASSERT(current->alloc == tracking_alloc, "Allocator should be set");
    ASSERT(current->free == tracking_free, "Free should be set");

    /* Reset to default */
    cs_set_allocator(NULL);

    PASS();
}

static void test_custom_allocator_used(void) {
    TEST(custom_allocator_used);

    /* Reset tracking */
    g_alloc_count = 0;
    g_free_count = 0;
    g_total_allocated = 0;

    /* Set custom allocator */
    CsAllocator tracking = {
        .alloc = tracking_alloc,
        .realloc = tracking_realloc,
        .free = tracking_free,
        .user_data = NULL
    };
    cs_set_allocator(&tracking);

    init_clay();
    cs_init();
    cs_map_cleanup();

    /* Create a map with a polyline - this triggers allocation */
    uint32_t map_id = CS_ID("alloc_test_map");
    double lat = 47.0, lon = 19.0;
    int zoom = 10;

    CsGeoPoint points[] = {
        {47.0, 19.0}, {47.1, 19.1}, {47.2, 19.2}, {47.3, 19.3}
    };

    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600) } }
    }) {
        cs_map_begin(map_id, &lat, &lon, &zoom, 800, 600, NULL);
        cs_polyline(CS_ID("test_route"), points, 4, NULL);
        cs_map_end();
    }

    Clay_EndLayout();
    cs_frame_end(0.016f);

    /* Should have used our allocator for polyline buffer */
    ASSERT(g_alloc_count > 0, "Custom allocator should be called");

    /* Cleanup should use our free */
    int alloc_before_cleanup = g_alloc_count;
    cs_map_destroy(map_id);
    ASSERT(g_free_count > 0, "Custom free should be called on cleanup");

    /* Reset to default allocator */
    cs_set_allocator(NULL);
    cs_map_cleanup();

    (void)alloc_before_cleanup;  /* Suppress unused warning */

    PASS();
}

static void test_default_allocator(void) {
    TEST(default_allocator);

    /* Ensure default allocator works */
    cs_set_allocator(NULL);

    const CsAllocator *a = cs_get_allocator();
    ASSERT(a != NULL, "Should have default allocator");
    ASSERT(a->alloc != NULL, "Default alloc should exist");
    ASSERT(a->realloc != NULL, "Default realloc should exist");
    ASSERT(a->free != NULL, "Default free should exist");

    /* Test that default allocator works */
    void *ptr = cs_alloc(100);
    ASSERT(ptr != NULL, "Default alloc should work");
    cs_free(ptr);

    PASS();
}

/* ============================================================================
 * Thread-Local Storage Tests
 * ============================================================================ */

static void test_tls_state_isolation(void) {
    TEST(tls_state_isolation);

    /* In single-threaded test, just verify state is properly isolated per cs_init() */
    cs_init();
    cs_focus(123);
    ASSERT(cs_focused_id() == 123, "Focus should be set");

    /* Re-init should reset state */
    cs_init();
    ASSERT(cs_focused_id() == 0, "Focus should be reset after cs_init");

    /* Error tracking should also be isolated */
    cs_clear_errors();
    cs_record_error(CS_ERR_ALLOC_FAILED);
    ASSERT(cs_get_error_count() == 1, "Should have 1 error");

    cs_clear_errors();
    ASSERT(cs_get_error_count() == 0, "Errors should be cleared");

    PASS();
}

/* ============================================================================
 * Map Provider Tests
 * ============================================================================ */

static void test_tile_layer_raster(void) {
    TEST(tile_layer_raster);

    cs_provider_set_tile_server("http://tiles.example.com");

    const char *url = cs_provider_tile_url(12, 2234, 1456, CS_TILE_LAYER_RASTER);
    ASSERT(url != NULL, "URL should not be NULL");
    ASSERT(strstr(url, ".png") != NULL, "Raster layer should use .png extension");
    ASSERT(strstr(url, "tiles.example.com") != NULL, "URL should contain server");
    ASSERT(strstr(url, "/12/2234/1456") != NULL, "URL should contain z/x/y");

    PASS();
}

static void test_tile_layer_vector(void) {
    TEST(tile_layer_vector);

    cs_provider_set_tile_server("http://tiles.example.com");

    const char *url = cs_provider_tile_url(10, 500, 300, CS_TILE_LAYER_VECTOR);
    ASSERT(url != NULL, "URL should not be NULL");
    ASSERT(strstr(url, ".mvt") != NULL, "Vector layer should use .mvt extension");
    ASSERT(strstr(url, "/10/500/300") != NULL, "URL should contain z/x/y");

    PASS();
}

/* ============================================================================
 * Scroll Container Tests
 * ============================================================================ */

static void test_scroll_default_style(void) {
    TEST(scroll_default_style);

    ASSERT(CS_SCROLL_STYLE_DEFAULT.vertical == true, "Vertical scroll should be enabled by default");
    ASSERT(CS_SCROLL_STYLE_DEFAULT.horizontal == false, "Horizontal scroll should be disabled by default");
    ASSERT(CS_SCROLL_STYLE_DEFAULT.width == 0.0f, "Width 0 should mean grow to fill");

    PASS();
}

static void test_scroll_delta_accumulation(void) {
    TEST(scroll_delta_accumulation);

    cs_init();
    CsState *g = cs_get_state();

    /* Initially zero */
    ASSERT(g->scroll_delta_x == 0.0f, "Initial scroll_delta_x should be 0");
    ASSERT(g->scroll_delta_y == 0.0f, "Initial scroll_delta_y should be 0");

    /* Accumulate deltas */
    cs_set_scroll_delta(10.0f);
    ASSERT(g->scroll_delta_y == 10.0f, "scroll_delta_y should be 10");

    cs_set_scroll_delta(5.0f);
    ASSERT(g->scroll_delta_y == 15.0f, "scroll_delta_y should accumulate to 15");

    cs_set_scroll_delta_xy(3.0f, 2.0f);
    ASSERT(g->scroll_delta_x == 3.0f, "scroll_delta_x should be 3");
    ASSERT(g->scroll_delta_y == 17.0f, "scroll_delta_y should accumulate to 17");

    PASS();
}

static void test_scroll_container_hovered_flag(void) {
    TEST(scroll_container_hovered_flag);

    cs_init();
    CsState *g = cs_get_state();

    /* Initially false */
    ASSERT(g->scroll_container_hovered == false, "Initially no scroll container hovered");
    ASSERT(cs_scroll_container_hovered() == false, "API should return false");

    /* Set via frame begin reset */
    g->scroll_container_hovered = true;
    ASSERT(cs_scroll_container_hovered() == true, "API should return true when set");

    cs_frame_begin();
    ASSERT(g->scroll_container_hovered == false, "frame_begin should reset hovered flag");

    PASS();
}

static void test_scroll_info_not_found(void) {
    TEST(scroll_info_not_found);

    cs_init();

    /* Query non-existent scroll container */
    CsScrollInfo info = cs_scroll_info(CS_ID("nonexistent"));

    ASSERT(info.found == false, "Should not find non-existent container");
    ASSERT(info.at_top == true, "at_top should be true for not found");
    ASSERT(info.at_bottom == true, "at_bottom should be true for not found");

    PASS();
}

static void test_scroll_set_position_not_found(void) {
    TEST(scroll_set_position_not_found);

    cs_init();

    /* Try to set position on non-existent scroll container */
    bool result = cs_scroll_set_position(CS_ID("nonexistent"), 0, 100);

    ASSERT(result == false, "Should return false for non-existent container");

    PASS();
}

/* ============================================================================
 * Scissor Stack Tests
 * ============================================================================ */

static void test_scissor_init(void) {
    TEST(scissor_init);

    CsScissorStack stack;
    cs_scissor_init(&stack, 800, 600);

    ASSERT(stack.depth == 0, "Stack should be empty after init");
    ASSERT(stack.screen_w == 800, "Screen width should be set");
    ASSERT(stack.screen_h == 600, "Screen height should be set");

    CsScissor sc = cs_scissor_current(&stack);
    ASSERT(sc.x == 0 && sc.y == 0, "Current should be origin");
    ASSERT(sc.w == 800 && sc.h == 600, "Current should be full screen");

    PASS();
}

static void test_scissor_push_pop(void) {
    TEST(scissor_push_pop);

    CsScissorStack stack;
    cs_scissor_init(&stack, 800, 600);

    cs_scissor_push(&stack, 100, 100, 200, 200);
    ASSERT(stack.depth == 1, "Depth should be 1");

    CsScissor sc = cs_scissor_current(&stack);
    ASSERT(sc.x == 100 && sc.y == 100, "Scissor position correct");
    ASSERT(sc.w == 200 && sc.h == 200, "Scissor size correct");

    cs_scissor_pop(&stack);
    ASSERT(stack.depth == 0, "Depth should be 0 after pop");

    PASS();
}

static void test_scissor_nesting(void) {
    TEST(scissor_nesting);

    CsScissorStack stack;
    cs_scissor_init(&stack, 800, 600);

    /* Push outer scissor */
    cs_scissor_push(&stack, 100, 100, 400, 400);

    /* Push inner scissor that should be clipped to outer */
    cs_scissor_push(&stack, 200, 200, 500, 500);  /* Extends past outer */

    CsScissor sc = cs_scissor_current(&stack);
    ASSERT(sc.x == 200 && sc.y == 200, "Inner scissor starts at 200,200");
    /* Should be clipped: outer ends at 500 (100+400), so inner can only be 300 wide */
    ASSERT(sc.w == 300, "Width should be clipped to 300");
    ASSERT(sc.h == 300, "Height should be clipped to 300");

    cs_scissor_pop(&stack);
    cs_scissor_pop(&stack);
    ASSERT(stack.depth == 0, "Stack should be empty");

    PASS();
}

static void test_scissor_point_test(void) {
    TEST(scissor_point_test);

    CsScissorStack stack;
    cs_scissor_init(&stack, 800, 600);

    cs_scissor_push(&stack, 100, 100, 200, 200);

    /* Inside scissor */
    ASSERT(cs_scissor_test_point(&stack, 150, 150), "150,150 should be inside");
    ASSERT(cs_scissor_test_point(&stack, 100, 100), "100,100 edge should be inside");
    ASSERT(cs_scissor_test_point(&stack, 299, 299), "299,299 should be inside");

    /* Outside scissor */
    ASSERT(!cs_scissor_test_point(&stack, 50, 50), "50,50 should be outside");
    ASSERT(!cs_scissor_test_point(&stack, 300, 300), "300,300 should be outside");
    ASSERT(!cs_scissor_test_point(&stack, 150, 400), "150,400 should be outside");

    cs_scissor_pop(&stack);

    PASS();
}

static void test_scissor_clip_rect(void) {
    TEST(scissor_clip_rect);

    CsScissorStack stack;
    cs_scissor_init(&stack, 800, 600);

    cs_scissor_push(&stack, 100, 100, 200, 200);

    /* Rectangle fully inside */
    int x = 150, y = 150, w = 50, h = 50;
    bool visible = cs_scissor_clip_rect(&stack, &x, &y, &w, &h);
    ASSERT(visible, "Should be visible");
    ASSERT(x == 150 && y == 150 && w == 50 && h == 50, "Should be unchanged");

    /* Rectangle partially outside */
    x = 250; y = 250; w = 100; h = 100;  /* Extends past 300,300 */
    visible = cs_scissor_clip_rect(&stack, &x, &y, &w, &h);
    ASSERT(visible, "Should still be visible");
    ASSERT(x == 250 && y == 250, "Origin should be preserved");
    ASSERT(w == 50 && h == 50, "Should be clipped to scissor");

    /* Rectangle fully outside */
    x = 400; y = 400; w = 50; h = 50;
    visible = cs_scissor_clip_rect(&stack, &x, &y, &w, &h);
    ASSERT(!visible, "Should not be visible");
    ASSERT(w == 0 && h == 0, "Size should be zeroed");

    cs_scissor_pop(&stack);

    PASS();
}

static void test_scissor_color_blend(void) {
    TEST(scissor_color_blend);

    /* Opaque over opaque */
    uint32_t red = cs_pack_color(255, 0, 0, 255);
    uint32_t blue = cs_pack_color(0, 0, 255, 255);
    uint32_t result = cs_blend_color(red, blue);
    uint8_t r, g, b, a;
    cs_unpack_color(result, &r, &g, &b, &a);
    ASSERT(r == 255 && g == 0 && b == 0 && a == 255, "Opaque over opaque = source");

    /* Transparent over opaque */
    uint32_t transparent = cs_pack_color(0, 0, 0, 0);
    result = cs_blend_color(transparent, blue);
    cs_unpack_color(result, &r, &g, &b, &a);
    ASSERT(r == 0 && g == 0 && b == 255 && a == 255, "Transparent over = dest");

    /* 50% alpha blend */
    uint32_t half_red = cs_pack_color(255, 0, 0, 128);
    result = cs_blend_color(half_red, blue);
    cs_unpack_color(result, &r, &g, &b, &a);
    ASSERT(r > 100 && b > 100, "50% blend should have both colors");

    PASS();
}

static void test_scissor_color_lerp(void) {
    TEST(scissor_color_lerp);

    uint32_t black = cs_pack_color(0, 0, 0, 255);
    uint32_t white = cs_pack_color(255, 255, 255, 255);

    /* t=0 should be first color */
    uint32_t result = cs_lerp_color(black, white, 0.0f);
    uint8_t r, g, b, a;
    cs_unpack_color(result, &r, &g, &b, &a);
    ASSERT(r == 0 && g == 0 && b == 0, "t=0 should be black");

    /* t=1 should be second color */
    result = cs_lerp_color(black, white, 1.0f);
    cs_unpack_color(result, &r, &g, &b, &a);
    ASSERT(r == 255 && g == 255 && b == 255, "t=1 should be white");

    /* t=0.5 should be gray */
    result = cs_lerp_color(black, white, 0.5f);
    cs_unpack_color(result, &r, &g, &b, &a);
    ASSERT(r > 120 && r < 140, "t=0.5 should be ~128");

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
    test_hash_id_null_safe();

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
    test_checkbox_result_init();
    test_toggle_result_init();
    test_slider_result_init();
    test_dropdown_result_init();

    printf("\nStyle Tests:\n");
    test_default_styles_exist();

    printf("\nIntegration Tests:\n");
    test_input_renders();
    test_button_renders();
    test_checkbox_renders();
    test_checkbox_toggle();
    test_checkbox_null_checked();
    test_checkbox_no_label();
    test_checkbox_keyboard_toggle();
    test_toggle_renders();
    test_toggle_switch();
    test_toggle_null_on();
    test_toggle_no_label();
    test_toggle_label_left();
    test_slider_renders();
    test_slider_null_value();
    test_slider_invalid_range();
    test_slider_no_label();
    test_slider_show_value();
    test_slider_keyboard_input_immediate();
    test_dropdown_renders();
    test_dropdown_enter_closes_immediate();
    test_dropdown_null_params();
    test_dropdown_selection_clamp();
    test_dropdown_open_close();
    test_dropdown_default_style();
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

    printf("\nMulti-Map and Hit Testing Tests:\n");
    test_multiple_maps_independent_state();
    test_per_map_overlays();
    test_marker_hit_test();
    test_polyline_hit_test();
    test_draggable_marker();
    test_non_draggable_marker_falls_through();
    test_hovered_overlay_state();

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
    test_tab_preserves_cursor_across_focus();

    printf("\nWidget State Store Tests:\n");
    test_widget_state_persistence();
    test_widget_state_isolation();

    printf("\nError Tracking Tests:\n");
    test_error_tracking_init();
    test_error_tracking_record();
    test_widget_state_stress();
    test_map_state_table_full();

    printf("\nCustom Allocator Tests:\n");
    test_custom_allocator_set();
    test_custom_allocator_used();
    test_default_allocator();

    printf("\nThread-Local Storage Tests:\n");
    test_tls_state_isolation();

    printf("\nMap Provider Tests:\n");
    test_tile_layer_raster();
    test_tile_layer_vector();

    printf("\nScroll Container Tests:\n");
    test_scroll_default_style();
    test_scroll_delta_accumulation();
    test_scroll_container_hovered_flag();
    test_scroll_info_not_found();
    test_scroll_set_position_not_found();

    printf("\nScissor Stack Tests:\n");
    test_scissor_init();
    test_scissor_push_pop();
    test_scissor_nesting();
    test_scissor_point_test();
    test_scissor_clip_rect();
    test_scissor_color_blend();
    test_scissor_color_lerp();

    printf("\n======================================\n");
    printf("Results: %d/%d tests passed\n", tests_passed, tests_run);

    return tests_passed == tests_run ? 0 : 1;
}
