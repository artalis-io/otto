/**
 * Clay Components - Button
 *
 * Immediate mode button component.
 *
 * Usage:
 *   if (cs_button(CS_ID("submit"), "Submit", NULL).clicked) {
 *       handle_submit();
 *   }
 */

#ifndef CS_BUTTON_H
#define CS_BUTTON_H

#include "cs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

typedef struct {
    bool clicked;      /* Was clicked this frame */
    bool hovered;      /* Is hovered this frame */
} CsButtonResult;

typedef enum {
    CS_BTN_DEFAULT,
    CS_BTN_PRIMARY,
    CS_BTN_SECONDARY,
    CS_BTN_DANGER,
    CS_BTN_GHOST,
} CsButtonVariant;

typedef struct {
    CsButtonVariant variant;
    float font_size;
    float padding_x;
    float padding_y;
    float corner_radius;
    /* Layout */
    float width;        /* Fixed width (0 = auto) */
    float height;       /* Fixed height (0 = auto) */
    CsMargin margin;    /* Outer spacing */
    CsAlign align;      /* Self-alignment within parent */
    bool grow;          /* Grow to fill available width */
    /* Fine-tuning for icon buttons */
    float text_offset_y; /* Vertical offset for text (positive = down) */
} CsButtonStyle;

/* Default style */
extern const CsButtonStyle CC_BUTTON_STYLE_DEFAULT;

/* ============================================================================
 * Component
 * ============================================================================ */

/**
 * Button - immediate mode
 *
 * @param id    Unique identifier (use CS_ID("name"))
 * @param label Button text
 * @param style Style or NULL for default
 * @return      Result with event flags
 */
CsButtonResult cs_button(
    uint32_t id,
    const char *label,
    const CsButtonStyle *style
);

/* Convenience wrappers */
#define cs_button_simple(id, label) cs_button(id, label, NULL)

#define cs_button_primary(id, label) \
    cs_button(id, label, &(CsButtonStyle){CS_BTN_PRIMARY, 14, 16, 8, 4})

#define cs_button_danger(id, label) \
    cs_button(id, label, &(CsButtonStyle){CS_BTN_DANGER, 14, 16, 8, 4})

#define cs_button_ghost(id, label) \
    cs_button(id, label, &(CsButtonStyle){CS_BTN_GHOST, 14, 16, 8, 4})

#ifdef __cplusplus
}
#endif

#endif /* CS_BUTTON_H */
