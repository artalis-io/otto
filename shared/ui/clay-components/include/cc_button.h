/**
 * Clay Components - Button
 *
 * Immediate mode button component.
 *
 * Usage:
 *   if (cc_button(CC_ID("submit"), "Submit", NULL).clicked) {
 *       handle_submit();
 *   }
 */

#ifndef CC_BUTTON_H
#define CC_BUTTON_H

#include "cc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

typedef struct {
    bool clicked;      /* Was clicked this frame */
    bool hovered;      /* Is hovered this frame */
} CcButtonResult;

typedef enum {
    CC_BTN_DEFAULT,
    CC_BTN_PRIMARY,
    CC_BTN_SECONDARY,
    CC_BTN_DANGER,
    CC_BTN_GHOST,
} CcButtonVariant;

typedef struct {
    CcButtonVariant variant;
    float font_size;
    float padding_x;
    float padding_y;
    float corner_radius;
} CcButtonStyle;

/* Default style */
extern const CcButtonStyle CC_BUTTON_STYLE_DEFAULT;

/* ============================================================================
 * Component
 * ============================================================================ */

/**
 * Button - immediate mode
 *
 * @param id    Unique identifier (use CC_ID("name"))
 * @param label Button text
 * @param style Style or NULL for default
 * @return      Result with event flags
 */
CcButtonResult cc_button(
    uint32_t id,
    const char *label,
    const CcButtonStyle *style
);

/* Convenience wrappers */
#define cc_button_simple(id, label) cc_button(id, label, NULL)

#define cc_button_primary(id, label) \
    cc_button(id, label, &(CcButtonStyle){CC_BTN_PRIMARY, 14, 16, 8, 4})

#define cc_button_danger(id, label) \
    cc_button(id, label, &(CcButtonStyle){CC_BTN_DANGER, 14, 16, 8, 4})

#define cc_button_ghost(id, label) \
    cc_button(id, label, &(CcButtonStyle){CC_BTN_GHOST, 14, 16, 8, 4})

#ifdef __cplusplus
}
#endif

#endif /* CC_BUTTON_H */
