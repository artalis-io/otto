/**
 * Clay Components - Button Implementation
 */

#include "cs_button.h"
#include "cs_internal.h"
#include "clay.h"
#include <string.h>

/* ============================================================================
 * Default Style
 * ============================================================================ */

const CsButtonStyle CC_BUTTON_STYLE_DEFAULT = {
    .variant = CS_BTN_DEFAULT,
    .font_size = 14.0f,
    .padding_x = 16.0f,
    .padding_y = 8.0f,
    .corner_radius = 4.0f,
};

/* ============================================================================
 * Component
 * ============================================================================ */

CsButtonResult cs_button(
    uint32_t id,
    const char *label,
    const CsButtonStyle *style
) {
    CsButtonResult result = {0};

    /* Validate required parameter */
    if (!label) {
        cs_record_error(CS_ERR_INVALID_ARGUMENT);
        return result;
    }

    CsState *g = cs_get_state();

    if (!style) style = &CC_BUTTON_STYLE_DEFAULT;

    /* Register for tab navigation */
    cs_register_focusable(id);

    bool is_focused = (g->focused_id == id);

    /* Colors based on variant */
    Clay_Color bg, bg_hover, text_color;

    switch (style->variant) {
        case CS_BTN_PRIMARY:
            bg = (Clay_Color){CS_COLOR_BTN_BLUE};
            bg_hover = (Clay_Color){CS_COLOR_BTN_BLUE_HOVER};
            text_color = (Clay_Color){CS_COLOR_TEXT};
            break;
        case CS_BTN_DANGER:
            bg = (Clay_Color){CS_COLOR_BTN_RED};
            bg_hover = (Clay_Color){CS_COLOR_BTN_RED_HOVER};
            text_color = (Clay_Color){CS_COLOR_TEXT};
            break;
        case CS_BTN_SECONDARY:
            bg = (Clay_Color){CS_COLOR_BTN_GRAY};
            bg_hover = (Clay_Color){CS_COLOR_BTN_GRAY_HOVER};
            text_color = (Clay_Color){CS_COLOR_TEXT};
            break;
        case CS_BTN_GHOST:
            bg = (Clay_Color){0, 0, 0, 0};
            bg_hover = (Clay_Color){CS_COLOR_BTN_GHOST_HOVER};
            text_color = (Clay_Color){CS_COLOR_TEXT};
            break;
        default: /* CS_BTN_DEFAULT */
            bg = (Clay_Color){CS_COLOR_BG_HOVER};
            bg_hover = (Clay_Color){CS_COLOR_BORDER};
            text_color = (Clay_Color){CS_COLOR_TEXT};
            break;
    }

    Clay_ElementId clay_id = (Clay_ElementId){.id = id, .stringId = {0}};

    /* Check hover using previous frame's data (standard immediate mode pattern) */
    bool is_hovered = Clay_PointerOver(clay_id);
    result.hovered = is_hovered;

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

    /* Build button config */
    Clay_ElementDeclaration btn_config = {
        .layout = {
            .sizing = { .width = width_sizing, .height = height_sizing },
            .padding = {
                .left = (uint16_t)style->padding_x,
                .right = (uint16_t)style->padding_x,
                .top = (uint16_t)style->padding_y,
                .bottom = (uint16_t)style->padding_y
            },
            .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
        },
        .backgroundColor = is_hovered ? bg_hover : bg,
        .cornerRadius = CLAY_CORNER_RADIUS(style->corner_radius)
    };

    /* Add focus ring if focused */
    if (is_focused) {
        btn_config.border.color = (Clay_Color){CS_COLOR_BORDER_FOCUSED};
        btn_config.border.width = (Clay_BorderWidth){2, 2, 2, 2, 0};
    }

    /* Check if we need a wrapper for margin/alignment */
    bool has_margin = cs_has_margin(style->margin);
    bool has_align = style->align != CS_ALIGN_AUTO;

    /* Text offset for icon buttons (positive = down) */
    bool has_text_offset = (style->text_offset_y != 0.0f);
    uint16_t text_pad_top = 0, text_pad_bottom = 0;
    if (style->text_offset_y > 0) {
        text_pad_top = (uint16_t)style->text_offset_y;
    } else if (style->text_offset_y < 0) {
        text_pad_bottom = (uint16_t)(-style->text_offset_y);
    }

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
            CLAY(clay_id, btn_config) {
                if (has_text_offset) {
                    Clay_ElementId text_wrapper_id = (Clay_ElementId){.id = id + CS_ID_OFFSET_TEXT_WRAPPER, .stringId = {0}};
                    CLAY(text_wrapper_id, {
                        .layout = { .padding = { .top = text_pad_top, .bottom = text_pad_bottom } }
                    }) {
                        Clay_String label_str = {.chars = label, .length = (int)strnlen(label, CS_MAX_LABEL_LEN)};
                        CLAY_TEXT(label_str, CLAY_TEXT_CONFIG({
                            .fontSize = (uint16_t)style->font_size,
                            .textColor = text_color
                        }));
                    }
                } else {
                    Clay_String label_str = {.chars = label, .length = (int)strnlen(label, CS_MAX_LABEL_LEN)};
                    CLAY_TEXT(label_str, CLAY_TEXT_CONFIG({
                        .fontSize = (uint16_t)style->font_size,
                        .textColor = text_color
                    }));
                }
            }
        }
    } else {
        /* No wrapper needed */
        CLAY(clay_id, btn_config) {
            if (has_text_offset) {
                Clay_ElementId text_wrapper_id = (Clay_ElementId){.id = id + CS_ID_OFFSET_TEXT_WRAPPER, .stringId = {0}};
                CLAY(text_wrapper_id, {
                    .layout = { .padding = { .top = text_pad_top, .bottom = text_pad_bottom } }
                }) {
                    Clay_String label_str = {.chars = label, .length = (int)strnlen(label, CS_MAX_LABEL_LEN)};
                    CLAY_TEXT(label_str, CLAY_TEXT_CONFIG({
                        .fontSize = (uint16_t)style->font_size,
                        .textColor = text_color
                    }));
                }
            } else {
                Clay_String label_str = {.chars = label, .length = (int)strnlen(label, CS_MAX_LABEL_LEN)};
                CLAY_TEXT(label_str, CLAY_TEXT_CONFIG({
                    .fontSize = (uint16_t)style->font_size,
                    .textColor = text_color
                }));
            }
        }
    }

    /* Register hit target for click detection (uses previous frame's bounds) */
    Clay_BoundingBox box = Clay_GetElementData(clay_id).boundingBox;
    cs_register_hit_target(id, CS_HIT_BODY, -1, box.x, box.y, box.width, box.height);

    /* Check click (mouse) */
    if (is_hovered && g->pending_click) {
        result.clicked = true;
        g->clicked_id = id;
    }

    /* Check keyboard activation (Enter key when focused) */
    if (is_focused && g->pending_enter) {
        result.clicked = true;
        g->clicked_id = id;
        g->pending_enter = false;  /* Consume the event */
    }

    /* Mark as non-text element so cs_key_down knows Enter means "activate" */
    CS_MARK_NON_TEXT_IF_FOCUSED(g, is_focused);

    return result;
}
