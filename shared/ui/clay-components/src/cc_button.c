/**
 * Clay Components - Button Implementation
 */

#include "cc_button.h"

/* ============================================================================
 * Default Styles
 * ============================================================================ */

const CcButtonStyle CC_BUTTON_STYLE_DEFAULT = {
    .min_width = 80.0f,
    .height = 36.0f,
    .font_size = 14.0f,
    .padding_x = 16.0f,
    .padding_y = 8.0f,
    .corner_radius = 4.0f,
    .bg_color = 0xFFFFFFFF,        /* White */
    .bg_hover_color = 0xF9FAFBFF,  /* Gray 50 */
    .bg_pressed_color = 0xF3F4F6FF,/* Gray 100 */
    .bg_disabled_color = 0xF3F4F6FF,
    .text_color = 0x111827FF,      /* Gray 900 */
    .text_disabled_color = 0x9CA3AFFF, /* Gray 400 */
    .border_color = 0xD1D5DBFF,    /* Gray 300 */
    .border_width = 1.0f,
};

const CcButtonStyle CC_BUTTON_STYLE_PRIMARY = {
    .min_width = 80.0f,
    .height = 36.0f,
    .font_size = 14.0f,
    .padding_x = 16.0f,
    .padding_y = 8.0f,
    .corner_radius = 4.0f,
    .bg_color = 0x3B82F6FF,        /* Blue 500 */
    .bg_hover_color = 0x2563EBFF,  /* Blue 600 */
    .bg_pressed_color = 0x1D4ED8FF,/* Blue 700 */
    .bg_disabled_color = 0x93C5FDFF,/* Blue 300 */
    .text_color = 0xFFFFFFFF,      /* White */
    .text_disabled_color = 0xFFFFFFB4,
    .border_color = 0x00000000,    /* Transparent */
    .border_width = 0.0f,
};

const CcButtonStyle CC_BUTTON_STYLE_SECONDARY = {
    .min_width = 80.0f,
    .height = 36.0f,
    .font_size = 14.0f,
    .padding_x = 16.0f,
    .padding_y = 8.0f,
    .corner_radius = 4.0f,
    .bg_color = 0x00000000,        /* Transparent */
    .bg_hover_color = 0xF3F4F6FF,  /* Gray 100 */
    .bg_pressed_color = 0xE5E7EBFF,/* Gray 200 */
    .bg_disabled_color = 0x00000000,
    .text_color = 0x374151FF,      /* Gray 700 */
    .text_disabled_color = 0x9CA3AFFF,
    .border_color = 0xD1D5DBFF,    /* Gray 300 */
    .border_width = 1.0f,
};

const CcButtonStyle CC_BUTTON_STYLE_DANGER = {
    .min_width = 80.0f,
    .height = 36.0f,
    .font_size = 14.0f,
    .padding_x = 16.0f,
    .padding_y = 8.0f,
    .corner_radius = 4.0f,
    .bg_color = 0xEF4444FF,        /* Red 500 */
    .bg_hover_color = 0xDC2626FF,  /* Red 600 */
    .bg_pressed_color = 0xB91C1CFF,/* Red 700 */
    .bg_disabled_color = 0xFCA5A5FF,/* Red 300 */
    .text_color = 0xFFFFFFFF,      /* White */
    .text_disabled_color = 0xFFFFFFB4,
    .border_color = 0x00000000,
    .border_width = 0.0f,
};

const CcButtonStyle CC_BUTTON_STYLE_GHOST = {
    .min_width = 80.0f,
    .height = 36.0f,
    .font_size = 14.0f,
    .padding_x = 16.0f,
    .padding_y = 8.0f,
    .corner_radius = 4.0f,
    .bg_color = 0x00000000,        /* Transparent */
    .bg_hover_color = 0xF3F4F6FF,  /* Gray 100 */
    .bg_pressed_color = 0xE5E7EBFF,/* Gray 200 */
    .bg_disabled_color = 0x00000000,
    .text_color = 0x374151FF,      /* Gray 700 */
    .text_disabled_color = 0x9CA3AFFF,
    .border_color = 0x00000000,
    .border_width = 0.0f,
};

/* ============================================================================
 * API Functions
 * ============================================================================ */

uint32_t cc_button_get_bg_color(const CcButtonStyle *style, CcButtonState state) {
    switch (state) {
        case CC_BUTTON_STATE_HOVERED:  return style->bg_hover_color;
        case CC_BUTTON_STATE_PRESSED:  return style->bg_pressed_color;
        case CC_BUTTON_STATE_DISABLED: return style->bg_disabled_color;
        default:                       return style->bg_color;
    }
}

uint32_t cc_button_get_text_color(const CcButtonStyle *style, CcButtonState state) {
    return state == CC_BUTTON_STATE_DISABLED
        ? style->text_disabled_color
        : style->text_color;
}
