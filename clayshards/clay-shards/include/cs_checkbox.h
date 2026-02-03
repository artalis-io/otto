/**
 * Clay Components - Checkbox
 *
 * Immediate mode checkbox component.
 *
 * Usage:
 *   static bool agree = false;
 *   if (cs_checkbox(CS_ID("agree"), &agree, "I agree to the terms", NULL).changed) {
 *       printf("Agreement: %s\n", agree ? "yes" : "no");
 *   }
 */

#ifndef CS_CHECKBOX_H
#define CS_CHECKBOX_H

#include "cs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

typedef struct {
    bool changed;       /* Value changed this frame */
    bool checked;       /* Current value (after change) */
    bool hovered;       /* Is hovered this frame */
} CsCheckboxResult;

typedef struct {
    float size;             /* Box size in pixels (default: 18) */
    float font_size;        /* Label font size (default: 14) */
    float gap;              /* Gap between box and label (default: 8) */
    float corner_radius;    /* Box corner radius (default: 3) */
    float border_width;     /* Border width (default: 2) */
    CsMargin margin;        /* Outer spacing */
} CsCheckboxStyle;

/* Default style */
extern const CsCheckboxStyle CS_CHECKBOX_STYLE_DEFAULT;

/* ============================================================================
 * Component
 * ============================================================================ */

/**
 * Checkbox - immediate mode
 *
 * @param id      Unique identifier (use CS_ID("name"))
 * @param checked Pointer to boolean state (modified on toggle)
 * @param label   Optional label text (NULL for no label)
 * @param style   Style or NULL for default
 * @return        Result with event flags
 */
CsCheckboxResult cs_checkbox(
    uint32_t id,
    bool *checked,
    const char *label,
    const CsCheckboxStyle *style
);

/* Convenience wrapper */
#define cs_checkbox_simple(id, checked, label) cs_checkbox(id, checked, label, NULL)

#ifdef __cplusplus
}
#endif

#endif /* CS_CHECKBOX_H */
