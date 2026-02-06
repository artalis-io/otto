/**
 * Clay Components - Text Input Implementation
 */

#include "cs_input.h"
#include "cs_internal.h"
#include "cs_clay.h"
#include "clay.h"
#include <string.h>

/* ============================================================================
 * Default Styles
 * ============================================================================ */

const CsInputStyle CC_INPUT_STYLE_DEFAULT = {
    .width = 200.0f,
    .height = 32.0f,
    .font_size = 14.0f,
    .padding = 8.0f,
    .corner_radius = 4.0f,
};

const CsInputStyle CC_INPUT_STYLE_DARK = {
    .width = 200.0f,
    .height = 32.0f,
    .font_size = 14.0f,
    .padding = 8.0f,
    .corner_radius = 4.0f,
};

/* ============================================================================
 * Component
 * ============================================================================ */

CsInputResult cs_input(
    uint32_t id,
    char *text,
    int *len,
    int max_len,
    const char *placeholder,
    const CsInputStyle *style
) {
    CsInputResult result = {0};
    CsState *g = cs_get_state();

    /* Validate inputs */
    if (!text || !len || max_len <= 0) {
        cs_record_error(CS_ERR_INVALID_ARGUMENT);
        return result;
    }

    /* Sanitize length - clamp if corrupted */
    if (*len < 0) *len = 0;
    if (*len > max_len - 1) {
        *len = max_len - 1;
        text[*len] = '\0';
    }

    if (!style) style = &CC_INPUT_STYLE_DEFAULT;

    /* Register for tab navigation */
    cs_register_focusable(id);

    bool is_focused = (g->focused_id == id);

    /* Display text */
    const char *display_text = (*len == 0 && !is_focused && placeholder)
        ? placeholder
        : text;
    int display_len = (*len == 0 && !is_focused && placeholder)
        ? (int)strnlen(placeholder, CS_MAX_LABEL_LEN)
        : *len;

    /* Colors - using constants from cs_common.h */
    /* Use brighter gray for focus visibility in TUI mode */
    Clay_Color bg = is_focused
        ? (Clay_Color){CS_COLOR_BTN_GRAY_FOCUS}
        : (Clay_Color){CS_COLOR_BG_DEFAULT};
    Clay_Color border = is_focused
        ? (Clay_Color){CS_COLOR_BORDER_FOCUSED}
        : (Clay_Color){CS_COLOR_BORDER};
    Clay_Color text_color = (*len == 0 && !is_focused)
        ? (Clay_Color){CS_COLOR_TEXT_MUTED}
        : (Clay_Color){CS_COLOR_TEXT};

    /* Build Clay element */
    Clay_ElementId clay_id = (Clay_ElementId){.id = id, .stringId = {0}};

    /* Determine sizing */
    Clay_SizingAxis width_sizing = CLAY_SIZING_FIT(0);
    Clay_SizingAxis height_sizing = CLAY_SIZING_FIT(0);
    if (style->grow) {
        width_sizing = CLAY_SIZING_GROW(0);
    } else if (style->width > 0) {
        width_sizing = CLAY_SIZING_FIXED(style->width);
    }
    if (style->height > 0) {
        height_sizing = CLAY_SIZING_FIXED(style->height);
    }

    /* Build input config */
    Clay_ElementDeclaration input_config = {
        .layout = {
            .sizing = { .width = width_sizing, .height = height_sizing },
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
        .border = { .width = {1, 1, 1, 1, 0}, .color = border }
    };

    /* Check if we need a wrapper for margin/alignment */
    bool has_margin = style->margin.top > 0 || style->margin.bottom > 0 ||
                      style->margin.left > 0 || style->margin.right > 0;
    bool has_align = style->align != CS_ALIGN_AUTO;

    if (has_margin || has_align) {
        /* With wrapper for margin/alignment */
        Clay_ElementId wrapper_id = (Clay_ElementId){.id = id + CS_ID_OFFSET_WRAPPER, .stringId = {0}};
        Clay_ChildAlignment child_align = {0};
        if (style->align == CS_ALIGN_START)  child_align.x = CLAY_ALIGN_X_LEFT;
        if (style->align == CS_ALIGN_CENTER) child_align.x = CLAY_ALIGN_X_CENTER;
        if (style->align == CS_ALIGN_END)    child_align.x = CLAY_ALIGN_X_RIGHT;

        CLAY(wrapper_id, {
            .layout = {
                .sizing = { .width = has_align ? CLAY_SIZING_GROW(0) : CLAY_SIZING_FIT(0) },
                .padding = {
                    .left = (uint16_t)style->margin.left,
                    .right = (uint16_t)style->margin.right,
                    .top = (uint16_t)style->margin.top,
                    .bottom = (uint16_t)style->margin.bottom
                },
                .childAlignment = child_align
            }
        }) {
            CLAY(clay_id, input_config) {
                if (display_len > 0) {
                    Clay_String text_str = {.chars = display_text, .length = display_len};
                    CLAY_TEXT(text_str, CLAY_TEXT_CONFIG({
                        .fontSize = (uint16_t)style->font_size,
                        .textColor = text_color
                    }));
                }
            }
        }
    } else {
        /* No wrapper needed */
        CLAY(clay_id, input_config) {
            if (display_len > 0) {
                Clay_String text_str = {.chars = display_text, .length = display_len};
                CLAY_TEXT(text_str, CLAY_TEXT_CONFIG({
                    .fontSize = (uint16_t)style->font_size,
                    .textColor = text_color
                }));
            }
        }
    }

    /* Check interaction */
    bool is_hovered = Clay_PointerOver(clay_id);

    if (is_hovered) {
        g->hovered_id = id;
    }

    /* Handle click to focus and/or position cursor */
    if (is_hovered && g->pending_click) {
        /* Get bounding box for cursor calculation */
        Clay_BoundingBox box = Clay_GetElementData(clay_id).boundingBox;

        if (!is_focused) {
            /* First click - focus the input */
            g->focused_id = id;
            result.focused = true;
            is_focused = true;
        }

        /* Calculate cursor position from click X */
        CsWidgetState *w = cs_widget_state(id);
        if (w) {
            float click_x = cs_pointer_x();
            float text_start_x = box.x + style->padding;
            float x_offset = click_x - text_start_x;

            w->cursor = cs_clay_x_to_cursor(text, *len, x_offset, style->font_size);
            w->selection_start = -1;
            w->cursor_visible = true;
            w->cursor_blink = 0.0f;
        }
        g->clicked_id = id;
    }

    /* Track state for focused element */
    if (is_focused) {
        /* Store bounds for cursor rendering */
        Clay_BoundingBox box = Clay_GetElementData(clay_id).boundingBox;
        g->focused_x = box.x;
        g->focused_y = box.y;
        g->focused_w = box.width;
        g->focused_h = box.height;

        /* Set active buffer for keyboard input */
        g->active_text = text;
        g->active_len = len;
        g->active_max_len = max_len;

        /* Clamp cursor to valid range (uses widget state store) */
        CsWidgetState *w = cs_widget_state(id);
        if (w) {
            w->cursor = cs_clamp_i(w->cursor, 0, *len);
            if (w->selection_start >= 0) {
                w->selection_start = cs_clamp_i(w->selection_start, 0, *len);
            }
        }
    }

    return result;
}
