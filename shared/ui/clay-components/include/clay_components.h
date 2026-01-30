/**
 * Clay UI Components
 *
 * Reusable UI components built on top of Clay layout library.
 * These components provide common UI patterns with full keyboard/mouse support.
 */

#ifndef CLAY_COMPONENTS_H
#define CLAY_COMPONENTS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Text Input Component
 * ============================================================================ */

#define CC_TEXT_INPUT_MAX_LENGTH 1024

typedef struct {
    char text[CC_TEXT_INPUT_MAX_LENGTH];
    int length;
    int cursor;           /* Cursor position (0 = before first char) */
    int selection_start;  /* Selection anchor (-1 = no selection) */
    bool focused;
    bool dirty;           /* Text changed since last check */

    /* Internal state */
    float cursor_blink_timer;
    bool cursor_visible;
} CcTextInput;

typedef struct {
    float width;
    float height;
    float font_size;
    float padding;
    float corner_radius;
    uint32_t bg_color;
    uint32_t bg_focused_color;
    uint32_t text_color;
    uint32_t cursor_color;
    uint32_t selection_color;
    uint32_t border_color;
    uint32_t border_focused_color;
    float border_width;
    const char *placeholder;
} CcTextInputStyle;

/* Default style */
extern const CcTextInputStyle CC_TEXT_INPUT_STYLE_DEFAULT;

/* Initialize a text input */
void cc_text_input_init(CcTextInput *input);

/* Set text programmatically */
void cc_text_input_set_text(CcTextInput *input, const char *text);

/* Get text (null-terminated) */
const char *cc_text_input_get_text(CcTextInput *input);

/* Check and clear dirty flag */
bool cc_text_input_consume_dirty(CcTextInput *input);

/* Update (call each frame for cursor blink) */
void cc_text_input_update(CcTextInput *input, float dt);

/* Render the text input (call within Clay layout) */
void cc_text_input_render(CcTextInput *input, uint32_t id, const CcTextInputStyle *style);

/* Input handling - returns true if event was consumed */
bool cc_text_input_key_down(CcTextInput *input, int key_code, bool shift, bool ctrl);
bool cc_text_input_key_char(CcTextInput *input, uint32_t char_code);
bool cc_text_input_mouse_down(CcTextInput *input, float x, float y, float input_x, float input_width, float font_size);
bool cc_text_input_mouse_drag(CcTextInput *input, float x, float input_x, float input_width, float font_size);

/* Focus management */
void cc_text_input_focus(CcTextInput *input);
void cc_text_input_blur(CcTextInput *input);

/* Key codes (matching JavaScript key codes) */
#define CC_KEY_BACKSPACE    8
#define CC_KEY_TAB          9
#define CC_KEY_ENTER        13
#define CC_KEY_SHIFT        16
#define CC_KEY_CTRL         17
#define CC_KEY_ALT          18
#define CC_KEY_ESCAPE       27
#define CC_KEY_SPACE        32
#define CC_KEY_END          35
#define CC_KEY_HOME         36
#define CC_KEY_LEFT         37
#define CC_KEY_UP           38
#define CC_KEY_RIGHT        39
#define CC_KEY_DOWN         40
#define CC_KEY_DELETE       46
#define CC_KEY_A            65
#define CC_KEY_C            67
#define CC_KEY_V            86
#define CC_KEY_X            88
#define CC_KEY_Z            90

/* ============================================================================
 * Button Component
 * ============================================================================ */

typedef enum {
    CC_BUTTON_STATE_NORMAL,
    CC_BUTTON_STATE_HOVERED,
    CC_BUTTON_STATE_PRESSED,
    CC_BUTTON_STATE_DISABLED
} CcButtonState;

typedef struct {
    float min_width;
    float height;
    float font_size;
    float padding_x;
    float padding_y;
    float corner_radius;
    uint32_t bg_color;
    uint32_t bg_hover_color;
    uint32_t bg_pressed_color;
    uint32_t bg_disabled_color;
    uint32_t text_color;
    uint32_t text_disabled_color;
    uint32_t border_color;
    float border_width;
} CcButtonStyle;

extern const CcButtonStyle CC_BUTTON_STYLE_DEFAULT;
extern const CcButtonStyle CC_BUTTON_STYLE_PRIMARY;
extern const CcButtonStyle CC_BUTTON_STYLE_SECONDARY;

/* Render a button - returns true if clicked */
bool cc_button(uint32_t id, const char *label, const CcButtonStyle *style, bool disabled);

/* ============================================================================
 * Checkbox Component
 * ============================================================================ */

typedef struct {
    float size;
    float corner_radius;
    float check_padding;
    uint32_t bg_color;
    uint32_t bg_checked_color;
    uint32_t check_color;
    uint32_t border_color;
    float border_width;
    float label_gap;
    float font_size;
    uint32_t label_color;
} CcCheckboxStyle;

extern const CcCheckboxStyle CC_CHECKBOX_STYLE_DEFAULT;

/* Render a checkbox - returns true if toggled */
bool cc_checkbox(uint32_t id, const char *label, bool *checked, const CcCheckboxStyle *style);

/* ============================================================================
 * Slider Component
 * ============================================================================ */

typedef struct {
    float width;
    float track_height;
    float thumb_size;
    float corner_radius;
    uint32_t track_color;
    uint32_t track_fill_color;
    uint32_t thumb_color;
    uint32_t thumb_hover_color;
    uint32_t thumb_border_color;
    float thumb_border_width;
} CcSliderStyle;

extern const CcSliderStyle CC_SLIDER_STYLE_DEFAULT;

/* Render a slider - returns true if value changed */
bool cc_slider(uint32_t id, float *value, float min, float max, const CcSliderStyle *style);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/* Pack RGBA color (0-255 each) into uint32_t */
static inline uint32_t cc_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | a;
}

/* Color constants */
#define CC_COLOR_WHITE       cc_color(255, 255, 255, 255)
#define CC_COLOR_BLACK       cc_color(0, 0, 0, 255)
#define CC_COLOR_TRANSPARENT cc_color(0, 0, 0, 0)
#define CC_COLOR_RED         cc_color(239, 68, 68, 255)
#define CC_COLOR_GREEN       cc_color(34, 197, 94, 255)
#define CC_COLOR_BLUE        cc_color(59, 130, 246, 255)
#define CC_COLOR_GRAY_50     cc_color(249, 250, 251, 255)
#define CC_COLOR_GRAY_100    cc_color(243, 244, 246, 255)
#define CC_COLOR_GRAY_200    cc_color(229, 231, 235, 255)
#define CC_COLOR_GRAY_300    cc_color(209, 213, 219, 255)
#define CC_COLOR_GRAY_400    cc_color(156, 163, 175, 255)
#define CC_COLOR_GRAY_500    cc_color(107, 114, 128, 255)
#define CC_COLOR_GRAY_600    cc_color(75, 85, 99, 255)
#define CC_COLOR_GRAY_700    cc_color(55, 65, 81, 255)
#define CC_COLOR_GRAY_800    cc_color(31, 41, 55, 255)
#define CC_COLOR_GRAY_900    cc_color(17, 24, 39, 255)

#ifdef __cplusplus
}
#endif

#endif /* CLAY_COMPONENTS_H */
