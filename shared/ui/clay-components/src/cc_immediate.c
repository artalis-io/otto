/**
 * Clay Components - Immediate Mode Implementation
 */

#include "cc_immediate.h"
#include "clay.h"
#include <string.h>

/* ============================================================================
 * Default Styles
 * ============================================================================ */

const CcInputStyle CC_INPUT_STYLE_DEFAULT = {
    .width = 200.0f,
    .height = 32.0f,
    .font_size = 14.0f,
    .padding = 8.0f,
    .corner_radius = 4.0f,
};

const CcInputStyle CC_INPUT_STYLE_DARK = {
    .width = 200.0f,
    .height = 32.0f,
    .font_size = 14.0f,
    .padding = 8.0f,
    .corner_radius = 4.0f,
};

const CcButtonStyle CC_BUTTON_STYLE_DEFAULT = {
    .variant = CC_BTN_DEFAULT,
    .font_size = 14.0f,
    .padding_x = 16.0f,
    .padding_y = 8.0f,
    .corner_radius = 4.0f,
};

/* ============================================================================
 * Global State
 * ============================================================================ */

/* Focus state - only one element can be focused */
static struct {
    uint32_t focused_id;        /* Currently focused element (0 = none) */

    /* Text input state for focused element */
    int cursor;                 /* Cursor position */
    int selection_start;        /* Selection anchor (-1 = no selection) */
    float cursor_blink;         /* Blink timer */
    bool cursor_visible;        /* Current blink state */

    /* Current frame state */
    uint32_t clicked_id;        /* Element clicked this frame */
    uint32_t hovered_id;        /* Element hovered this frame */
    bool pending_click;         /* Click event pending this frame */

    /* Focused element bounds (set during render) */
    float focused_x, focused_y, focused_w, focused_h;

    /* Active input buffer reference (only valid during frame) */
    char *active_text;
    int *active_len;
    int active_max_len;
} g_cc = {
    .focused_id = 0,
    .cursor = 0,
    .selection_start = -1,
    .cursor_blink = 0.0f,
    .cursor_visible = true,
    .pending_click = false,
};

/* ============================================================================
 * ID Generation
 * ============================================================================ */

uint32_t cc_hash_id(const char *str) {
    /* FNV-1a hash */
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= 16777619u;
    }
    return hash ? hash : 1; /* Never return 0 */
}

/* ============================================================================
 * Core API
 * ============================================================================ */

void cc_init(void) {
    memset(&g_cc, 0, sizeof(g_cc));
    g_cc.selection_start = -1;
    g_cc.cursor_visible = true;
}

void cc_frame_begin(void) {
    g_cc.clicked_id = 0;
    g_cc.hovered_id = 0;
    g_cc.pending_click = false;
    g_cc.active_text = NULL;
    g_cc.active_len = NULL;
}

/* Set pending click for this frame (call from JS on mousedown) */
void cc_set_pending_click(void) {
    g_cc.pending_click = true;
}

void cc_frame_end(float dt) {
    /* Update cursor blink */
    if (g_cc.focused_id != 0) {
        g_cc.cursor_blink += dt;
        if (g_cc.cursor_blink >= 0.53f) {
            g_cc.cursor_blink = 0.0f;
            g_cc.cursor_visible = !g_cc.cursor_visible;
        }
    }
}

/* ============================================================================
 * Focus Management
 * ============================================================================ */

uint32_t cc_focused_id(void) {
    return g_cc.focused_id;
}

void cc_focus(uint32_t id) {
    if (g_cc.focused_id != id) {
        g_cc.focused_id = id;
        g_cc.cursor = 0;
        g_cc.selection_start = -1;
        g_cc.cursor_visible = true;
        g_cc.cursor_blink = 0.0f;
    }
}

void cc_blur(void) {
    g_cc.focused_id = 0;
    g_cc.selection_start = -1;
}

int cc_cursor_pos(void) {
    return g_cc.cursor;
}

int cc_selection_start(void) {
    return g_cc.selection_start;
}

bool cc_cursor_visible(void) {
    return g_cc.cursor_visible;
}

bool cc_focused_bounds(float *x, float *y, float *w, float *h) {
    if (g_cc.focused_id == 0) return false;
    if (x) *x = g_cc.focused_x;
    if (y) *y = g_cc.focused_y;
    if (w) *w = g_cc.focused_w;
    if (h) *h = g_cc.focused_h;
    return true;
}

/* ============================================================================
 * Input Routing
 * ============================================================================ */

static int min_i(int a, int b) { return a < b ? a : b; }
static int max_i(int a, int b) { return a > b ? a : b; }
static int clamp_i(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static bool has_selection(void) {
    return g_cc.selection_start >= 0 && g_cc.selection_start != g_cc.cursor;
}

static void delete_selection(char *text, int *len) {
    if (!has_selection()) return;

    int start = min_i(g_cc.cursor, g_cc.selection_start);
    int end = max_i(g_cc.cursor, g_cc.selection_start);

    memmove(&text[start], &text[end], *len - end + 1);
    *len -= (end - start);
    g_cc.cursor = start;
    g_cc.selection_start = -1;
}

bool cc_key_char(uint32_t char_code) {
    if (g_cc.focused_id == 0) return false;
    if (!g_cc.active_text || !g_cc.active_len) return false;
    if (char_code < 32 || char_code > 126) return false;
    if (*g_cc.active_len >= g_cc.active_max_len - 1) return false;

    char *text = g_cc.active_text;
    int *len = g_cc.active_len;

    delete_selection(text, len);

    /* Insert character */
    memmove(&text[g_cc.cursor + 1], &text[g_cc.cursor], *len - g_cc.cursor + 1);
    text[g_cc.cursor] = (char)char_code;
    g_cc.cursor++;
    (*len)++;

    g_cc.cursor_visible = true;
    g_cc.cursor_blink = 0.0f;

    return true;
}

bool cc_key_down(int key_code, bool shift, bool ctrl) {
    if (g_cc.focused_id == 0) return false;
    if (!g_cc.active_text || !g_cc.active_len) return false;

    char *text = g_cc.active_text;
    int *len = g_cc.active_len;
    bool sel = has_selection();

    g_cc.cursor_visible = true;
    g_cc.cursor_blink = 0.0f;

    switch (key_code) {
        case 37: /* Left */
            if (ctrl) {
                while (g_cc.cursor > 0 && text[g_cc.cursor - 1] == ' ') g_cc.cursor--;
                while (g_cc.cursor > 0 && text[g_cc.cursor - 1] != ' ') g_cc.cursor--;
            } else if (sel && !shift) {
                g_cc.cursor = min_i(g_cc.cursor, g_cc.selection_start);
            } else if (g_cc.cursor > 0) {
                g_cc.cursor--;
            }
            if (!shift) g_cc.selection_start = -1;
            else if (g_cc.selection_start < 0) g_cc.selection_start = g_cc.cursor + 1;
            return true;

        case 39: /* Right */
            if (ctrl) {
                while (g_cc.cursor < *len && text[g_cc.cursor] != ' ') g_cc.cursor++;
                while (g_cc.cursor < *len && text[g_cc.cursor] == ' ') g_cc.cursor++;
            } else if (sel && !shift) {
                g_cc.cursor = max_i(g_cc.cursor, g_cc.selection_start);
            } else if (g_cc.cursor < *len) {
                g_cc.cursor++;
            }
            if (!shift) g_cc.selection_start = -1;
            else if (g_cc.selection_start < 0) g_cc.selection_start = g_cc.cursor - 1;
            return true;

        case 36: /* Home */
            if (shift && g_cc.selection_start < 0) g_cc.selection_start = g_cc.cursor;
            g_cc.cursor = 0;
            if (!shift) g_cc.selection_start = -1;
            return true;

        case 35: /* End */
            if (shift && g_cc.selection_start < 0) g_cc.selection_start = g_cc.cursor;
            g_cc.cursor = *len;
            if (!shift) g_cc.selection_start = -1;
            return true;

        case 8: /* Backspace */
            if (sel) {
                delete_selection(text, len);
            } else if (g_cc.cursor > 0) {
                if (ctrl) {
                    int start = g_cc.cursor;
                    while (g_cc.cursor > 0 && text[g_cc.cursor - 1] == ' ') g_cc.cursor--;
                    while (g_cc.cursor > 0 && text[g_cc.cursor - 1] != ' ') g_cc.cursor--;
                    memmove(&text[g_cc.cursor], &text[start], *len - start + 1);
                    *len -= (start - g_cc.cursor);
                } else {
                    memmove(&text[g_cc.cursor - 1], &text[g_cc.cursor], *len - g_cc.cursor + 1);
                    g_cc.cursor--;
                    (*len)--;
                }
            }
            return true;

        case 46: /* Delete */
            if (sel) {
                delete_selection(text, len);
            } else if (g_cc.cursor < *len) {
                if (ctrl) {
                    int end = g_cc.cursor;
                    while (end < *len && text[end] == ' ') end++;
                    while (end < *len && text[end] != ' ') end++;
                    memmove(&text[g_cc.cursor], &text[end], *len - end + 1);
                    *len -= (end - g_cc.cursor);
                } else {
                    memmove(&text[g_cc.cursor], &text[g_cc.cursor + 1], *len - g_cc.cursor);
                    (*len)--;
                }
            }
            return true;

        case 65: /* A - select all */
            if (ctrl) {
                g_cc.selection_start = 0;
                g_cc.cursor = *len;
                return true;
            }
            break;

        case 27: /* Escape */
            cc_blur();
            return true;

        case 13: /* Enter */
            return true; /* Handled by component to set submitted flag */
    }

    return false;
}

bool cc_click(float x, float y) {
    (void)x; (void)y;
    /* Set pending click - components will check is_hovered && pending_click */
    g_cc.pending_click = true;
    return g_cc.clicked_id != 0;
}

/* ============================================================================
 * Text Input Component
 * ============================================================================ */

CcInputResult cc_input(
    uint32_t id,
    char *text,
    int *len,
    int max_len,
    const char *placeholder,
    const CcInputStyle *style
) {
    CcInputResult result = {0};

    if (!style) style = &CC_INPUT_STYLE_DEFAULT;

    bool is_focused = (g_cc.focused_id == id);
    bool is_hovered = false;

    /* Display text */
    const char *display_text = (*len == 0 && !is_focused && placeholder)
        ? placeholder
        : text;
    int display_len = (*len == 0 && !is_focused && placeholder)
        ? (int)strlen(placeholder)
        : *len;

    /* Colors */
    Clay_Color bg = is_focused
        ? (Clay_Color){60, 60, 60, 255}
        : (Clay_Color){50, 50, 50, 255};
    Clay_Color border = is_focused
        ? (Clay_Color){66, 133, 244, 255}
        : (Clay_Color){100, 100, 100, 255};
    Clay_Color text_color = (*len == 0 && !is_focused)
        ? (Clay_Color){120, 120, 120, 255}
        : (Clay_Color){255, 255, 255, 255};

    /* Build Clay element */
    Clay_ElementId clay_id = (Clay_ElementId){.id = id, .stringId = {0}};

    CLAY(clay_id, {
        .layout = {
            .sizing = {
                .width = style->width > 0
                    ? CLAY_SIZING_FIXED(style->width)
                    : CLAY_SIZING_GROW(0),
                .height = CLAY_SIZING_FIXED(style->height)
            },
            .padding = {
                .left = (uint16_t)style->padding,
                .right = (uint16_t)style->padding,
                .top = 4,
                .bottom = 4
            },
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
        },
        .backgroundColor = bg,
        .cornerRadius = CLAY_CORNER_RADIUS(style->corner_radius),
        .border = {
            .width = {1, 1, 1, 1},
            .color = border
        }
    }) {
        if (display_len > 0) {
            Clay_String text_str = {.chars = display_text, .length = display_len};
            CLAY_TEXT(text_str, CLAY_TEXT_CONFIG({
                .fontSize = (uint16_t)style->font_size,
                .textColor = text_color
            }));
        }
    }

    /* Check interaction */
    is_hovered = Clay_PointerOver(clay_id);

    if (is_hovered) {
        g_cc.hovered_id = id;
    }

    /* Handle click to focus */
    if (is_hovered && g_cc.pending_click) {
        if (!is_focused) {
            /* Focusing */
            g_cc.focused_id = id;
            g_cc.cursor = *len;  /* Cursor at end */
            g_cc.selection_start = -1;
            g_cc.cursor_visible = true;
            g_cc.cursor_blink = 0.0f;
            result.focused = true;
        }
        g_cc.clicked_id = id;
    }

    /* Track state for focused element */
    if (is_focused) {
        /* Store bounds for cursor rendering */
        Clay_BoundingBox box = Clay_GetElementData(clay_id).boundingBox;
        g_cc.focused_x = box.x;
        g_cc.focused_y = box.y;
        g_cc.focused_w = box.width;
        g_cc.focused_h = box.height;

        /* Set active buffer for keyboard input */
        g_cc.active_text = text;
        g_cc.active_len = len;
        g_cc.active_max_len = max_len;

        /* Clamp cursor to valid range */
        g_cc.cursor = clamp_i(g_cc.cursor, 0, *len);
        if (g_cc.selection_start >= 0) {
            g_cc.selection_start = clamp_i(g_cc.selection_start, 0, *len);
        }
    }

    return result;
}

/* ============================================================================
 * Button Component
 * ============================================================================ */

CcButtonResult cc_button(
    uint32_t id,
    const char *label,
    const CcButtonStyle *style
) {
    CcButtonResult result = {0};

    if (!style) style = &CC_BUTTON_STYLE_DEFAULT;

    /* Colors based on variant */
    Clay_Color bg, bg_hover, text_color;

    switch (style->variant) {
        case CC_BTN_PRIMARY:
            bg = (Clay_Color){59, 130, 246, 255};       /* Blue 500 */
            bg_hover = (Clay_Color){37, 99, 235, 255};  /* Blue 600 */
            text_color = (Clay_Color){255, 255, 255, 255};
            break;
        case CC_BTN_DANGER:
            bg = (Clay_Color){239, 68, 68, 255};        /* Red 500 */
            bg_hover = (Clay_Color){220, 38, 38, 255};  /* Red 600 */
            text_color = (Clay_Color){255, 255, 255, 255};
            break;
        case CC_BTN_SECONDARY:
            bg = (Clay_Color){75, 85, 99, 255};         /* Gray 600 */
            bg_hover = (Clay_Color){55, 65, 81, 255};   /* Gray 700 */
            text_color = (Clay_Color){255, 255, 255, 255};
            break;
        case CC_BTN_GHOST:
            bg = (Clay_Color){0, 0, 0, 0};
            bg_hover = (Clay_Color){55, 65, 81, 128};
            text_color = (Clay_Color){255, 255, 255, 255};
            break;
        default: /* CC_BTN_DEFAULT */
            bg = (Clay_Color){80, 80, 80, 255};
            bg_hover = (Clay_Color){100, 100, 100, 255};
            text_color = (Clay_Color){255, 255, 255, 255};
            break;
    }

    Clay_ElementId clay_id = (Clay_ElementId){.id = id, .stringId = {0}};

    bool is_hovered = Clay_PointerOver(clay_id);
    result.hovered = is_hovered;

    CLAY(clay_id, {
        .layout = {
            .padding = {
                .left = (uint16_t)style->padding_x,
                .right = (uint16_t)style->padding_x,
                .top = (uint16_t)style->padding_y,
                .bottom = (uint16_t)style->padding_y
            },
            .childAlignment = {
                .x = CLAY_ALIGN_X_CENTER,
                .y = CLAY_ALIGN_Y_CENTER
            }
        },
        .backgroundColor = is_hovered ? bg_hover : bg,
        .cornerRadius = CLAY_CORNER_RADIUS(style->corner_radius)
    }) {
        Clay_String label_str = {.chars = label, .length = (int)strlen(label)};
        CLAY_TEXT(label_str, CLAY_TEXT_CONFIG({
            .fontSize = (uint16_t)style->font_size,
            .textColor = text_color
        }));
    }

    /* Check click - set by cc_click() if this element was hovered when clicked */
    if (is_hovered && g_cc.pending_click) {
        result.clicked = true;
        g_cc.clicked_id = id;

        /* Blur any focused input when clicking a button */
        if (g_cc.focused_id != 0) {
            g_cc.focused_id = 0;
        }
    }

    return result;
}
