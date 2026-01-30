/**
 * Clay Components - Text Input
 *
 * Single-line text input with cursor, selection, and full keyboard support.
 */

#ifndef CC_TEXT_INPUT_H
#define CC_TEXT_INPUT_H

#include "cc_common.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

#define CC_TEXT_INPUT_MAX_LENGTH 1024

/* Key codes (matching JavaScript keyCodes) */
#define CC_KEY_BACKSPACE    8
#define CC_KEY_TAB          9
#define CC_KEY_ENTER        13
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

/* ============================================================================
 * Types
 * ============================================================================ */

typedef struct {
    char text[CC_TEXT_INPUT_MAX_LENGTH];
    int length;
    int cursor;           /* Cursor position (0 = before first char) */
    int selection_start;  /* Selection anchor (-1 = no selection) */
    bool focused;
    float cursor_blink;   /* Timer for cursor blinking */
    bool cursor_visible;  /* Current blink state */
    bool dirty;           /* Text changed since last check */
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
    uint32_t placeholder_color;
    uint32_t cursor_color;
    uint32_t selection_color;
    uint32_t border_color;
    uint32_t border_focused_color;
    float border_width;
} CcTextInputStyle;

/* ============================================================================
 * Default Style
 * ============================================================================ */

extern const CcTextInputStyle CC_TEXT_INPUT_STYLE_DEFAULT;
extern const CcTextInputStyle CC_TEXT_INPUT_STYLE_DARK;

/* ============================================================================
 * API Functions
 * ============================================================================ */

/* Initialize a text input to default state */
void cc_text_input_init(CcTextInput *input);

/* Set text programmatically */
void cc_text_input_set_text(CcTextInput *input, const char *text);

/* Get text (null-terminated) */
const char *cc_text_input_get_text(const CcTextInput *input);

/* Get text length */
int cc_text_input_get_length(const CcTextInput *input);

/* Check and clear dirty flag (returns true if text changed) */
bool cc_text_input_consume_dirty(CcTextInput *input);

/* Update cursor blink (call each frame with delta time in seconds) */
void cc_text_input_update(CcTextInput *input, float dt);

/* Focus management */
void cc_text_input_focus(CcTextInput *input);
void cc_text_input_blur(CcTextInput *input);
bool cc_text_input_is_focused(const CcTextInput *input);

/* Cursor state (for rendering) */
int cc_text_input_get_cursor(const CcTextInput *input);
int cc_text_input_get_selection_start(const CcTextInput *input);
bool cc_text_input_cursor_visible(const CcTextInput *input);
bool cc_text_input_has_selection(const CcTextInput *input);

/* Input handling - returns true if event was consumed */
bool cc_text_input_key_char(CcTextInput *input, uint32_t char_code);
bool cc_text_input_key_down(CcTextInput *input, int key_code, bool shift, bool ctrl);

/* Mouse handling */
bool cc_text_input_click(CcTextInput *input, float local_x, float char_width);
bool cc_text_input_drag(CcTextInput *input, float local_x, float char_width);

/* Clipboard operations (text provided by JS layer) */
void cc_text_input_paste(CcTextInput *input, const char *text);
int cc_text_input_get_selected_text(const CcTextInput *input, char *out, int max_len);

#ifdef __cplusplus
}
#endif

#endif /* CC_TEXT_INPUT_H */
