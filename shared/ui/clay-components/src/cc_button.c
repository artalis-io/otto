/**
 * Clay Components - Button Implementation
 */

#include "cc_button.h"
#include "cc_internal.h"
#include "clay.h"
#include <string.h>

/* ============================================================================
 * Default Style
 * ============================================================================ */

const CcButtonStyle CC_BUTTON_STYLE_DEFAULT = {
    .variant = CC_BTN_DEFAULT,
    .font_size = 14.0f,
    .padding_x = 16.0f,
    .padding_y = 8.0f,
    .corner_radius = 4.0f,
};

/* ============================================================================
 * Component
 * ============================================================================ */

CcButtonResult cc_button(
    uint32_t id,
    const char *label,
    const CcButtonStyle *style
) {
    CcButtonResult result = {0};
    CcState *g = cc_get_state();

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

    /* Check hover using previous frame's data (standard immediate mode pattern) */
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

    /* Check click */
    if (is_hovered && g->pending_click) {
        result.clicked = true;
        g->clicked_id = id;

        /* Blur any focused input when clicking a button */
        if (g->focused_id != 0) {
            g->focused_id = 0;
        }
    }

    return result;
}
