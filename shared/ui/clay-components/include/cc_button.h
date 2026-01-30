/**
 * Clay Components - Button
 *
 * Clickable button with multiple style variants.
 */

#ifndef CC_BUTTON_H
#define CC_BUTTON_H

#include "cc_common.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

typedef enum {
    CC_BUTTON_STATE_NORMAL,
    CC_BUTTON_STATE_HOVERED,
    CC_BUTTON_STATE_PRESSED,
    CC_BUTTON_STATE_DISABLED
} CcButtonState;

typedef struct {
    float min_width;
    float height;
    float font_size;
    float padding_x;
    float padding_y;
    float corner_radius;
    uint32_t bg_color;
    uint32_t bg_hover_color;
    uint32_t bg_pressed_color;
    uint32_t bg_disabled_color;
    uint32_t text_color;
    uint32_t text_disabled_color;
    uint32_t border_color;
    float border_width;
} CcButtonStyle;

/* ============================================================================
 * Default Styles
 * ============================================================================ */

extern const CcButtonStyle CC_BUTTON_STYLE_DEFAULT;
extern const CcButtonStyle CC_BUTTON_STYLE_PRIMARY;
extern const CcButtonStyle CC_BUTTON_STYLE_SECONDARY;
extern const CcButtonStyle CC_BUTTON_STYLE_DANGER;
extern const CcButtonStyle CC_BUTTON_STYLE_GHOST;

/* ============================================================================
 * API Functions
 * ============================================================================ */

/* Get colors for current button state */
uint32_t cc_button_get_bg_color(const CcButtonStyle *style, CcButtonState state);
uint32_t cc_button_get_text_color(const CcButtonStyle *style, CcButtonState state);

#ifdef __cplusplus
}
#endif

#endif /* CC_BUTTON_H */
