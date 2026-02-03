/**
 * Clay Components - Checkbox Implementation
 */

#include "cs_checkbox.h"
#include "cs_internal.h"
#include "clay.h"
#include <string.h>

/* ============================================================================
 * Default Style
 * ============================================================================ */

const CsCheckboxStyle CS_CHECKBOX_STYLE_DEFAULT = {
    .size = 18.0f,
    .font_size = 14.0f,
    .gap = 8.0f,
    .corner_radius = 3.0f,
    .border_width = 2.0f,
};

/* ============================================================================
 * Component
 * ============================================================================ */

CsCheckboxResult cs_checkbox(
    uint32_t id,
    bool *checked,
    const char *label,
    const CsCheckboxStyle *style
) {
    CsCheckboxResult result = {0};

    /* Validate required parameter */
    if (!checked) return result;

    CsState *g = cs_get_state();

    if (!style) style = &CS_CHECKBOX_STYLE_DEFAULT;

    /* Register for tab navigation */
    cs_register_focusable(id);

    bool is_focused = (g->focused_id == id);
    bool current_value = *checked;

    /* Clay element ID for the clickable area */
    Clay_ElementId clay_id = (Clay_ElementId){.id = id, .stringId = {0}};

    /* Check hover using previous frame's data */
    bool is_hovered = Clay_PointerOver(clay_id);
    result.hovered = is_hovered;

    /* Colors */
    Clay_Color box_bg = current_value
        ? (Clay_Color){CS_COLOR_BTN_BLUE}
        : (Clay_Color){CS_COLOR_BG_DEFAULT};
    Clay_Color box_border = is_focused
        ? (Clay_Color){CS_COLOR_BORDER_FOCUSED}
        : (is_hovered ? (Clay_Color){CS_COLOR_BTN_BLUE} : (Clay_Color){CS_COLOR_BORDER});
    Clay_Color check_color = (Clay_Color){CS_COLOR_TEXT};
    Clay_Color label_color = (Clay_Color){CS_COLOR_TEXT};

    /* Build the checkbox layout: [box] [label] */
    bool has_margin = cs_has_margin(style->margin);

    Clay_ElementDeclaration row_config = {
        .layout = {
            .sizing = { .width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIT(0) },
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            .childGap = (uint16_t)style->gap,
            .padding = has_margin ? (Clay_Padding){
                .left = (uint16_t)style->margin.left,
                .right = (uint16_t)style->margin.right,
                .top = (uint16_t)style->margin.top,
                .bottom = (uint16_t)style->margin.bottom
            } : (Clay_Padding){0}
        }
    };

    CLAY(clay_id, row_config) {
        /* The checkbox box */
        Clay_ElementId box_id = (Clay_ElementId){.id = id + CS_ID_OFFSET_WRAPPER, .stringId = {0}};
        CLAY(box_id, {
            .layout = {
                .sizing = {
                    .width = CLAY_SIZING_FIXED(style->size),
                    .height = CLAY_SIZING_FIXED(style->size)
                },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = box_bg,
            .cornerRadius = CLAY_CORNER_RADIUS(style->corner_radius),
            .border = {
                .color = box_border,
                .width = {
                    (uint32_t)style->border_width,
                    (uint32_t)style->border_width,
                    (uint32_t)style->border_width,
                    (uint32_t)style->border_width,
                    0
                }
            }
        }) {
            /* Checkmark when checked - using Unicode checkmark character */
            if (current_value) {
                Clay_String check_str = {.chars = "\xE2\x9C\x93", .length = 3}; /* UTF-8 checkmark */
                CLAY_TEXT(check_str, CLAY_TEXT_CONFIG({
                    .fontSize = (uint16_t)(style->size * 0.7f),
                    .textColor = check_color
                }));
            }
        }

        /* Optional label */
        if (label && label[0] != '\0') {
            Clay_String label_str = {.chars = label, .length = (int)strlen(label)};
            CLAY_TEXT(label_str, CLAY_TEXT_CONFIG({
                .fontSize = (uint16_t)style->font_size,
                .textColor = label_color
            }));
        }
    }

    /* Register hit target for click detection (uses previous frame's bounds) */
    Clay_BoundingBox box = Clay_GetElementData(clay_id).boundingBox;
    cs_register_hit_target(id, CS_HIT_BODY, -1, box.x, box.y, box.width, box.height);

    /* Handle click (mouse) */
    if (is_hovered && g->pending_click) {
        *checked = !current_value;
        result.changed = true;
        g->clicked_id = id;
    }

    /* Handle keyboard activation (Space or Enter when focused) */
    if (is_focused && g->pending_enter) {
        *checked = !current_value;
        result.changed = true;
        g->clicked_id = id;
        g->pending_enter = false;  /* Consume the event */
    }

    /* Update result with final value */
    result.checked = *checked;

    /* Mark as non-text element for keyboard navigation */
    CS_MARK_NON_TEXT_IF_FOCUSED(g, is_focused);

    return result;
}
