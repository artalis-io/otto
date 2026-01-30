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

    if (!style) style = &CC_INPUT_STYLE_DEFAULT;

    bool is_focused = (g->focused_id == id);
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
