/**
 * Clay Components - Text Input Implementation
 */

#include "cc_text_input.h"
#include <string.h>

/* ============================================================================
 * Default Styles
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
    .placeholder_color = 0x9CA3AFFF,   /* Gray 400 */
    .cursor_color = 0x3B82F6FF,        /* Blue 500 */
    .selection_color = 0x3B82F650,     /* Blue 500 with alpha */
    .border_color = 0xD1D5DBFF,        /* Gray 300 */
    .border_focused_color = 0x3B82F6FF,/* Blue 500 */
    .border_width = 1.0f,
};

const CcTextInputStyle CC_TEXT_INPUT_STYLE_DARK = {
    .width = 200.0f,
    .height = 36.0f,
    .font_size = 14.0f,
    .padding = 10.0f,
    .corner_radius = 4.0f,
    .bg_color = 0x374151FF,            /* Gray 700 */
    .bg_focused_color = 0x4B5563FF,    /* Gray 600 */
    .text_color = 0xFFFFFFFF,          /* White */
    .placeholder_color = 0x9CA3AFFF,   /* Gray 400 */
    .cursor_color = 0x60A5FAFF,        /* Blue 400 */
    .selection_color = 0x3B82F650,     /* Blue 500 with alpha */
    .border_color = 0x6B7280FF,        /* Gray 500 */
    .border_focused_color = 0x60A5FAFF,/* Blue 400 */
    .border_width = 1.0f,
};

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

static int selection_start_pos(const CcTextInput *input) {
    if (input->selection_start < 0) return input->cursor;
    return cc_min_i(input->cursor, input->selection_start);
}

static int selection_end_pos(const CcTextInput *input) {
    if (input->selection_start < 0) return input->cursor;
    return cc_max_i(input->cursor, input->selection_start);
}

static void delete_selection(CcTextInput *input) {
    if (!cc_text_input_has_selection(input)) return;

    int start = selection_start_pos(input);
    int end = selection_end_pos(input);
    int len = end - start;

    memmove(&input->text[start], &input->text[end], input->length - end + 1);
    input->length -= len;
    input->cursor = start;
    input->selection_start = -1;
    input->dirty = true;
}

static void insert_char(CcTextInput *input, char c) {
    if (input->length >= CC_TEXT_INPUT_MAX_LENGTH - 1) return;

    delete_selection(input);

    memmove(&input->text[input->cursor + 1], &input->text[input->cursor],
            input->length - input->cursor + 1);
    input->text[input->cursor] = c;
    input->cursor++;
    input->length++;
    input->dirty = true;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

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

const char *cc_text_input_get_text(const CcTextInput *input) {
    return input->text;
}

int cc_text_input_get_length(const CcTextInput *input) {
    return input->length;
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

    input->cursor_blink += dt;
    if (input->cursor_blink >= 0.5f) {
        input->cursor_blink = 0.0f;
        input->cursor_visible = !input->cursor_visible;
    }
}

void cc_text_input_focus(CcTextInput *input) {
    input->focused = true;
    input->cursor_visible = true;
    input->cursor_blink = 0.0f;
}

void cc_text_input_blur(CcTextInput *input) {
    input->focused = false;
    input->selection_start = -1;
}

bool cc_text_input_is_focused(const CcTextInput *input) {
    return input->focused;
}

int cc_text_input_get_cursor(const CcTextInput *input) {
    return input->cursor;
}

int cc_text_input_get_selection_start(const CcTextInput *input) {
    return input->selection_start;
}

bool cc_text_input_cursor_visible(const CcTextInput *input) {
    return input->cursor_visible;
}

bool cc_text_input_has_selection(const CcTextInput *input) {
    return input->selection_start >= 0 && input->selection_start != input->cursor;
}

bool cc_text_input_key_char(CcTextInput *input, uint32_t char_code) {
    if (!input->focused) return false;
    if (char_code < 32 || char_code > 126) return false;

    insert_char(input, (char)char_code);
    input->cursor_visible = true;
    input->cursor_blink = 0.0f;
    return true;
}

bool cc_text_input_key_down(CcTextInput *input, int key_code, bool shift, bool ctrl) {
    if (!input->focused) return false;

    input->cursor_visible = true;
    input->cursor_blink = 0.0f;

    bool has_sel = cc_text_input_has_selection(input);

    switch (key_code) {
        case CC_KEY_LEFT:
            if (ctrl) {
                /* Move to start of word */
                while (input->cursor > 0 && input->text[input->cursor - 1] == ' ')
                    input->cursor--;
                while (input->cursor > 0 && input->text[input->cursor - 1] != ' ')
                    input->cursor--;
            } else if (has_sel && !shift) {
                input->cursor = selection_start_pos(input);
            } else if (input->cursor > 0) {
                input->cursor--;
            }
            if (!shift) input->selection_start = -1;
            else if (input->selection_start < 0) input->selection_start = input->cursor + 1;
            return true;

        case CC_KEY_RIGHT:
            if (ctrl) {
                /* Move to end of word */
                while (input->cursor < input->length && input->text[input->cursor] != ' ')
                    input->cursor++;
                while (input->cursor < input->length && input->text[input->cursor] == ' ')
                    input->cursor++;
            } else if (has_sel && !shift) {
                input->cursor = selection_end_pos(input);
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
            if (has_sel) {
                delete_selection(input);
            } else if (input->cursor > 0) {
                if (ctrl) {
                    /* Delete word */
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
            if (has_sel) {
                delete_selection(input);
            } else if (input->cursor < input->length) {
                if (ctrl) {
                    /* Delete word forward */
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
                /* Select all */
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

bool cc_text_input_click(CcTextInput *input, float local_x, float char_width) {
    int idx = (int)(local_x / char_width + 0.5f);
    idx = cc_clamp_i(idx, 0, input->length);

    input->cursor = idx;
    input->selection_start = idx;  /* Start selection at click point */
    input->cursor_visible = true;
    input->cursor_blink = 0.0f;
    cc_text_input_focus(input);
    return true;
}

bool cc_text_input_drag(CcTextInput *input, float local_x, float char_width) {
    if (!input->focused) return false;

    int idx = (int)(local_x / char_width + 0.5f);
    idx = cc_clamp_i(idx, 0, input->length);

    input->cursor = idx;
    input->cursor_visible = true;
    input->cursor_blink = 0.0f;
    return true;
}

void cc_text_input_paste(CcTextInput *input, const char *text) {
    if (!input->focused) return;

    delete_selection(input);

    int len = (int)strlen(text);
    int available = CC_TEXT_INPUT_MAX_LENGTH - 1 - input->length;
    if (len > available) len = available;

    memmove(&input->text[input->cursor + len], &input->text[input->cursor],
            input->length - input->cursor + 1);
    memcpy(&input->text[input->cursor], text, len);
    input->cursor += len;
    input->length += len;
    input->dirty = true;
}

int cc_text_input_get_selected_text(const CcTextInput *input, char *out, int max_len) {
    if (!cc_text_input_has_selection(input)) {
        if (max_len > 0) out[0] = '\0';
        return 0;
    }

    int start = selection_start_pos(input);
    int end = selection_end_pos(input);
    int len = end - start;

    if (len > max_len - 1) len = max_len - 1;

    memcpy(out, &input->text[start], len);
    out[len] = '\0';
    return len;
}
