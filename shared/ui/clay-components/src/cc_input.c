/**
 * Clay Components - Text Input Implementation
 */

#include "cc_input.h"
#include "cc_internal.h"
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

/* ============================================================================
 * Component
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
    CcState *g = cc_get_state();

    /* Validate inputs */
    if (!text || !len || max_len <= 0) {
        return result;  /* Return empty result for invalid inputs */
    }

    /* Sanitize length - clamp if corrupted */
    if (*len < 0) *len = 0;
    if (*len > max_len - 1) {
        *len = max_len - 1;
        text[*len] = '\0';
    }

    if (!style) style = &CC_INPUT_STYLE_DEFAULT;

    bool is_focused = (g->focused_id == id);

    /* Display text */
    const char *display_text = (*len == 0 && !is_focused && placeholder)
        ? placeholder
        : text;
    int display_len = (*len == 0 && !is_focused && placeholder)
        ? (int)strlen(placeholder)
        : *len;

    /* Colors - using constants from cc_common.h */
    Clay_Color bg = is_focused
        ? (Clay_Color){CC_COLOR_BG_FOCUSED}
        : (Clay_Color){CC_COLOR_BG_DEFAULT};
    Clay_Color border = is_focused
        ? (Clay_Color){CC_COLOR_BORDER_FOCUSED}
        : (Clay_Color){CC_COLOR_BORDER};
    Clay_Color text_color = (*len == 0 && !is_focused)
        ? (Clay_Color){CC_COLOR_TEXT_MUTED}
        : (Clay_Color){CC_COLOR_TEXT};

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
        .border = { .width = {1, 1, 1, 1}, .color = border }
    };

    /* Check if we need a wrapper for margin/alignment */
    bool has_margin = style->margin.top > 0 || style->margin.bottom > 0 ||
                      style->margin.left > 0 || style->margin.right > 0;
    bool has_align = style->align != CC_ALIGN_AUTO;

    if (has_margin || has_align) {
        /* With wrapper for margin/alignment */
        Clay_ElementId wrapper_id = (Clay_ElementId){.id = id + 0x10000, .stringId = {0}};
        Clay_ChildAlignment child_align = {0};
        if (style->align == CC_ALIGN_START)  child_align.x = CLAY_ALIGN_X_LEFT;
        if (style->align == CC_ALIGN_CENTER) child_align.x = CLAY_ALIGN_X_CENTER;
        if (style->align == CC_ALIGN_END)    child_align.x = CLAY_ALIGN_X_RIGHT;

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

    /* Handle click to focus */
    if (is_hovered && g->pending_click) {
        if (!is_focused) {
            /* Focusing */
            g->focused_id = id;
            g->cursor = *len;  /* Cursor at end */
            g->selection_start = -1;
            g->cursor_visible = true;
            g->cursor_blink = 0.0f;
            result.focused = true;
            is_focused = true;  /* Update for the rest of this call */
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

        /* Clamp cursor to valid range */
        g->cursor = cc_clamp_i(g->cursor, 0, *len);
        if (g->selection_start >= 0) {
            g->selection_start = cc_clamp_i(g->selection_start, 0, *len);
        }
    }

    return result;
}
