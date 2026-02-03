/**
 * Clay Components - Toggle Switch Implementation
 */

#include "cs_toggle.h"
#include "cs_internal.h"
#include "clay.h"
#include <string.h>

/* ============================================================================
 * Default Style
 * ============================================================================ */

const CsToggleStyle CS_TOGGLE_STYLE_DEFAULT = {
    .width = 44.0f,
    .height = 24.0f,
    .knob_padding = 2.0f,
    .font_size = 14.0f,
    .gap = 8.0f,
    .label_left = false,
};

/* ============================================================================
 * Component
 * ============================================================================ */

CsToggleResult cs_toggle(
    uint32_t id,
    bool *on,
    const char *label,
    const CsToggleStyle *style
) {
    CsToggleResult result = {0};

    /* Validate required parameter */
    if (!on) {
        cs_record_error(CS_ERR_INVALID_ARGUMENT);
        return result;
    }

    CsState *g = cs_get_state();

    if (!style) style = &CS_TOGGLE_STYLE_DEFAULT;

    /* Register for tab navigation */
    cs_register_focusable(id);

    bool is_focused = (g->focused_id == id);
    bool current_value = *on;

    /* Clay element ID for the clickable area */
    Clay_ElementId clay_id = (Clay_ElementId){.id = id, .stringId = {0}};

    /* Check hover using previous frame's data */
    bool is_hovered = Clay_PointerOver(clay_id);
    result.hovered = is_hovered;

    /* Colors */
    Clay_Color track_bg = current_value
        ? (Clay_Color){CS_COLOR_BTN_BLUE}
        : (Clay_Color){CS_COLOR_BTN_GRAY};
    Clay_Color track_border = is_focused
        ? (Clay_Color){CS_COLOR_BORDER_FOCUSED}
        : (Clay_Color){0, 0, 0, 0};  /* No border unless focused */
    Clay_Color knob_color = (Clay_Color){CS_COLOR_TEXT};  /* White knob */
    Clay_Color label_color = (Clay_Color){CS_COLOR_TEXT};

    /* Calculate knob size (circular, fits inside track with padding) */
    float knob_size = style->height - (style->knob_padding * 2.0f);

    /* Build the toggle layout: [label?] [track] [label?] */
    bool has_margin = cs_has_margin(style->margin);
    bool has_label = label && label[0] != '\0';

    Clay_ElementDeclaration row_config = {
        .layout = {
            .sizing = { .width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIT(0) },
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            .childGap = has_label ? (uint16_t)style->gap : 0,
            .padding = has_margin ? (Clay_Padding){
                .left = (uint16_t)style->margin.left,
                .right = (uint16_t)style->margin.right,
                .top = (uint16_t)style->margin.top,
                .bottom = (uint16_t)style->margin.bottom
            } : (Clay_Padding){0}
        }
    };

    CLAY(clay_id, row_config) {
        /* Label on left if configured */
        if (has_label && style->label_left) {
            Clay_String label_str = {.chars = label, .length = (int)strlen(label)};
            CLAY_TEXT(label_str, CLAY_TEXT_CONFIG({
                .fontSize = (uint16_t)style->font_size,
                .textColor = label_color
            }));
        }

        /* The toggle track */
        Clay_ElementId track_id = (Clay_ElementId){.id = id + CS_ID_OFFSET_WRAPPER, .stringId = {0}};

        /* Track positioning: knob on left (off) or right (on) */
        Clay_ChildAlignment knob_align = current_value
            ? (Clay_ChildAlignment){ .x = CLAY_ALIGN_X_RIGHT, .y = CLAY_ALIGN_Y_CENTER }
            : (Clay_ChildAlignment){ .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER };

        Clay_ElementDeclaration track_config = {
            .layout = {
                .sizing = {
                    .width = CLAY_SIZING_FIXED(style->width),
                    .height = CLAY_SIZING_FIXED(style->height)
                },
                .padding = {
                    .left = (uint16_t)style->knob_padding,
                    .right = (uint16_t)style->knob_padding,
                    .top = (uint16_t)style->knob_padding,
                    .bottom = (uint16_t)style->knob_padding
                },
                .childAlignment = knob_align
            },
            .backgroundColor = track_bg,
            .cornerRadius = CLAY_CORNER_RADIUS(style->height / 2.0f)
        };
        if (is_focused) {
            track_config.border.color = track_border;
            track_config.border.width = (Clay_BorderWidth){2, 2, 2, 2, 0};
        }

        CLAY(track_id, track_config) {
            /* The knob (circular) */
            Clay_ElementId knob_id = (Clay_ElementId){.id = id + CS_ID_OFFSET_TEXT_WRAPPER, .stringId = {0}};
            CLAY(knob_id, {
                .layout = {
                    .sizing = {
                        .width = CLAY_SIZING_FIXED(knob_size),
                        .height = CLAY_SIZING_FIXED(knob_size)
                    }
                },
                .backgroundColor = knob_color,
                .cornerRadius = CLAY_CORNER_RADIUS(knob_size / 2.0f)
            }) {}
        }

        /* Label on right (default) */
        if (has_label && !style->label_left) {
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
        *on = !current_value;
        result.changed = true;
        g->clicked_id = id;
    }

    /* Handle keyboard activation (Space or Enter when focused) */
    if (is_focused && g->pending_enter) {
        *on = !current_value;
        result.changed = true;
        g->clicked_id = id;
        g->pending_enter = false;  /* Consume the event */
    }

    /* Update result with final value */
    result.on = *on;

    /* Mark as non-text element for keyboard navigation */
    CS_MARK_NON_TEXT_IF_FOCUSED(g, is_focused);

    return result;
}
