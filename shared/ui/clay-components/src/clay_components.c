/**
 * Clay UI Components Implementation
 *
 * Single compilation unit for all Clay components.
 * Include clay.h before including this file.
 */

#include "clay_components.h"
#include <string.h>

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

static int cc_min_int(int a, int b) { return a < b ? a : b; }
static int cc_max_int(int a, int b) { return a > b ? a : b; }
static int cc_clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ============================================================================
 * Text Input - Default Style
 * ============================================================================ */

const CcTextInputStyle CC_TEXT_INPUT_STYLE_DEFAULT = {
    .width = 200.0f,
    .height = 36.0f,
    .font_size = 14.0f,
    .padding = 10.0f,
    .corner_radius = 4.0f,
    .bg_color = 0xFFFFFFFF,            /* White */
    .bg_focused_color = 0xFFFFFFFF,
    .text_color = 0x111827FF,          /* Gray 900 */
    .cursor_color = 0x3B82F6FF,        /* Blue 500 */
    .selection_color = 0x3B82F650,     /* Blue 500 with alpha */
    .border_color = 0xD1D5DBFF,        /* Gray 300 */
    .border_focused_color = 0x3B82F6FF,
    .border_width = 1.0f,
    .placeholder = NULL,
};

/* ============================================================================
 * Text Input - Implementation
 * ============================================================================ */

static int ti_selection_start(CcTextInput *input) {
    if (input->selection_start < 0) return input->cursor;
    return cc_min_int(input->cursor, input->selection_start);
}

static int ti_selection_end(CcTextInput *input) {
    if (input->selection_start < 0) return input->cursor;
    return cc_max_int(input->cursor, input->selection_start);
}

static bool ti_has_selection(CcTextInput *input) {
    return input->selection_start >= 0 && input->selection_start != input->cursor;
}

static void ti_delete_selection(CcTextInput *input) {
    if (!ti_has_selection(input)) return;

    int start = ti_selection_start(input);
    int end = ti_selection_end(input);
    int len = end - start;

    memmove(&input->text[start], &input->text[end], input->length - end + 1);
    input->length -= len;
    input->cursor = start;
    input->selection_start = -1;
    input->dirty = true;
}

static void ti_insert_char(CcTextInput *input, char c) {
    if (input->length >= CC_TEXT_INPUT_MAX_LENGTH - 1) return;

    ti_delete_selection(input);

    memmove(&input->text[input->cursor + 1], &input->text[input->cursor],
            input->length - input->cursor + 1);
    input->text[input->cursor] = c;
    input->cursor++;
    input->length++;
    input->dirty = true;
}

void cc_text_input_init(CcTextInput *input) {
    memset(input, 0, sizeof(*input));
    input->selection_start = -1;
    input->cursor_visible = true;
}

void cc_text_input_set_text(CcTextInput *input, const char *text) {
    int len = (int)strlen(text);
    if (len > CC_TEXT_INPUT_MAX_LENGTH - 1) {
        len = CC_TEXT_INPUT_MAX_LENGTH - 1;
    }
    memcpy(input->text, text, len);
    input->text[len] = '\0';
    input->length = len;
    input->cursor = len;
    input->selection_start = -1;
    input->dirty = true;
}

const char *cc_text_input_get_text(CcTextInput *input) {
    return input->text;
}

bool cc_text_input_consume_dirty(CcTextInput *input) {
    bool was_dirty = input->dirty;
    input->dirty = false;
    return was_dirty;
}

void cc_text_input_update(CcTextInput *input, float dt) {
    if (!input->focused) {
        input->cursor_visible = false;
        return;
    }

    input->cursor_blink_timer += dt;
    if (input->cursor_blink_timer >= 0.5f) {
        input->cursor_blink_timer = 0.0f;
        input->cursor_visible = !input->cursor_visible;
    }
}

void cc_text_input_focus(CcTextInput *input) {
    input->focused = true;
    input->cursor_visible = true;
    input->cursor_blink_timer = 0.0f;
}

void cc_text_input_blur(CcTextInput *input) {
    input->focused = false;
    input->selection_start = -1;
}

bool cc_text_input_key_char(CcTextInput *input, uint32_t char_code) {
    if (!input->focused) return false;
    if (char_code < 32 || char_code > 126) return false;

    ti_insert_char(input, (char)char_code);
    input->cursor_visible = true;
    input->cursor_blink_timer = 0.0f;
    return true;
}

bool cc_text_input_key_down(CcTextInput *input, int key_code, bool shift, bool ctrl) {
    if (!input->focused) return false;

    input->cursor_visible = true;
    input->cursor_blink_timer = 0.0f;

    switch (key_code) {
        case CC_KEY_LEFT:
            if (ctrl) {
                while (input->cursor > 0 && input->text[input->cursor - 1] == ' ')
                    input->cursor--;
                while (input->cursor > 0 && input->text[input->cursor - 1] != ' ')
                    input->cursor--;
            } else if (ti_has_selection(input) && !shift) {
                input->cursor = ti_selection_start(input);
            } else if (input->cursor > 0) {
                input->cursor--;
            }
            if (!shift) input->selection_start = -1;
            else if (input->selection_start < 0) input->selection_start = input->cursor + 1;
            return true;

        case CC_KEY_RIGHT:
            if (ctrl) {
                while (input->cursor < input->length && input->text[input->cursor] != ' ')
                    input->cursor++;
                while (input->cursor < input->length && input->text[input->cursor] == ' ')
                    input->cursor++;
            } else if (ti_has_selection(input) && !shift) {
                input->cursor = ti_selection_end(input);
            } else if (input->cursor < input->length) {
                input->cursor++;
            }
            if (!shift) input->selection_start = -1;
            else if (input->selection_start < 0) input->selection_start = input->cursor - 1;
            return true;

        case CC_KEY_HOME:
            if (shift && input->selection_start < 0) {
                input->selection_start = input->cursor;
            }
            input->cursor = 0;
            if (!shift) input->selection_start = -1;
            return true;

        case CC_KEY_END:
            if (shift && input->selection_start < 0) {
                input->selection_start = input->cursor;
            }
            input->cursor = input->length;
            if (!shift) input->selection_start = -1;
            return true;

        case CC_KEY_BACKSPACE:
            if (ti_has_selection(input)) {
                ti_delete_selection(input);
            } else if (input->cursor > 0) {
                if (ctrl) {
                    int start = input->cursor;
                    while (input->cursor > 0 && input->text[input->cursor - 1] == ' ')
                        input->cursor--;
                    while (input->cursor > 0 && input->text[input->cursor - 1] != ' ')
                        input->cursor--;
                    memmove(&input->text[input->cursor], &input->text[start],
                            input->length - start + 1);
                    input->length -= (start - input->cursor);
                } else {
                    memmove(&input->text[input->cursor - 1], &input->text[input->cursor],
                            input->length - input->cursor + 1);
                    input->cursor--;
                    input->length--;
                }
                input->dirty = true;
            }
            return true;

        case CC_KEY_DELETE:
            if (ti_has_selection(input)) {
                ti_delete_selection(input);
            } else if (input->cursor < input->length) {
                if (ctrl) {
                    int end = input->cursor;
                    while (end < input->length && input->text[end] == ' ')
                        end++;
                    while (end < input->length && input->text[end] != ' ')
                        end++;
                    memmove(&input->text[input->cursor], &input->text[end],
                            input->length - end + 1);
                    input->length -= (end - input->cursor);
                } else {
                    memmove(&input->text[input->cursor], &input->text[input->cursor + 1],
                            input->length - input->cursor);
                    input->length--;
                }
                input->dirty = true;
            }
            return true;

        case CC_KEY_A:
            if (ctrl) {
                input->selection_start = 0;
                input->cursor = input->length;
                return true;
            }
            break;

        case CC_KEY_ESCAPE:
            cc_text_input_blur(input);
            return true;
    }

    return false;
}

bool cc_text_input_mouse_down(CcTextInput *input, float x, float y,
                               float input_x, float input_width, float font_size) {
    (void)y;
    (void)input_width;

    float padding = 10.0f;
    float text_start = input_x + padding;
    float char_width = font_size * 0.6f;
    float rel_x = x - text_start;

    int idx = 0;
    if (rel_x > 0) {
        idx = (int)(rel_x / char_width + 0.5f);
    }
    idx = cc_clamp_int(idx, 0, input->length);

    input->cursor = idx;
    input->selection_start = idx;
    input->cursor_visible = true;
    input->cursor_blink_timer = 0.0f;
    cc_text_input_focus(input);
    return true;
}

bool cc_text_input_mouse_drag(CcTextInput *input, float x,
                               float input_x, float input_width, float font_size) {
    if (!input->focused) return false;
    (void)input_width;

    float padding = 10.0f;
    float text_start = input_x + padding;
    float char_width = font_size * 0.6f;
    float rel_x = x - text_start;

    int idx = 0;
    if (rel_x > 0) {
        idx = (int)(rel_x / char_width + 0.5f);
    }
    idx = cc_clamp_int(idx, 0, input->length);

    input->cursor = idx;
    input->cursor_visible = true;
    input->cursor_blink_timer = 0.0f;
    return true;
}

/* ============================================================================
 * Button - Default Styles
 * ============================================================================ */

const CcButtonStyle CC_BUTTON_STYLE_DEFAULT = {
    .min_width = 80.0f,
    .height = 36.0f,
    .font_size = 14.0f,
    .padding_x = 16.0f,
    .padding_y = 8.0f,
    .corner_radius = 4.0f,
    .bg_color = 0xFFFFFFFF,
    .bg_hover_color = 0xF9FAFBFF,
    .bg_pressed_color = 0xF3F4F6FF,
    .bg_disabled_color = 0xF3F4F6FF,
    .text_color = 0x111827FF,
    .text_disabled_color = 0x9CA3AFFF,
    .border_color = 0xD1D5DBFF,
    .border_width = 1.0f,
};

const CcButtonStyle CC_BUTTON_STYLE_PRIMARY = {
    .min_width = 80.0f,
    .height = 36.0f,
    .font_size = 14.0f,
    .padding_x = 16.0f,
    .padding_y = 8.0f,
    .corner_radius = 4.0f,
    .bg_color = 0x3B82F6FF,
    .bg_hover_color = 0x2563EBFF,
    .bg_pressed_color = 0x1D4ED8FF,
    .bg_disabled_color = 0x93C5FDFF,
    .text_color = 0xFFFFFFFF,
    .text_disabled_color = 0xFFFFFFB4,
    .border_color = 0x00000000,
    .border_width = 0.0f,
};

const CcButtonStyle CC_BUTTON_STYLE_SECONDARY = {
    .min_width = 80.0f,
    .height = 36.0f,
    .font_size = 14.0f,
    .padding_x = 16.0f,
    .padding_y = 8.0f,
    .corner_radius = 4.0f,
    .bg_color = 0x00000000,
    .bg_hover_color = 0xF3F4F6FF,
    .bg_pressed_color = 0xE5E7EBFF,
    .bg_disabled_color = 0x00000000,
    .text_color = 0x374151FF,
    .text_disabled_color = 0x9CA3AFFF,
    .border_color = 0xD1D5DBFF,
    .border_width = 1.0f,
};

/* ============================================================================
 * Checkbox - Default Style
 * ============================================================================ */

const CcCheckboxStyle CC_CHECKBOX_STYLE_DEFAULT = {
    .size = 20.0f,
    .corner_radius = 4.0f,
    .check_padding = 4.0f,
    .bg_color = 0xFFFFFFFF,
    .bg_checked_color = 0x3B82F6FF,
    .check_color = 0xFFFFFFFF,
    .border_color = 0xD1D5DBFF,
    .border_width = 1.0f,
    .label_gap = 8.0f,
    .font_size = 14.0f,
    .label_color = 0x111827FF,
};

/* ============================================================================
 * Slider - Default Style
 * ============================================================================ */

const CcSliderStyle CC_SLIDER_STYLE_DEFAULT = {
    .width = 200.0f,
    .track_height = 4.0f,
    .thumb_size = 16.0f,
    .corner_radius = 2.0f,
    .track_color = 0xE5E7EBFF,
    .track_fill_color = 0x3B82F6FF,
    .thumb_color = 0xFFFFFFFF,
    .thumb_hover_color = 0xF9FAFBFF,
    .thumb_border_color = 0xD1D5DBFF,
    .thumb_border_width = 1.0f,
};

/* Note: Button, Checkbox, and Slider rendering would be implemented similarly.
 * They require Clay context which is application-specific.
 * The styles and data structures are provided for use in your Clay layouts.
 */
