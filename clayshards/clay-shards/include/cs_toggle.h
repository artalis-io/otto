/**
 * Clay Components - Toggle Switch
 *
 * Immediate mode toggle switch component (styled boolean input).
 *
 * Usage:
 *   static bool enabled = false;
 *   if (cs_toggle(CS_ID("dark_mode"), &enabled, "Dark Mode", NULL).changed) {
 *       apply_theme(enabled);
 *   }
 */

#ifndef CS_TOGGLE_H
#define CS_TOGGLE_H

#include "cs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

typedef struct {
    bool changed;       /* Value changed this frame */
    bool on;            /* Current value (after change) */
    bool hovered;       /* Is hovered this frame */
} CsToggleResult;

typedef struct {
    float width;            /* Track width (default: 44) */
    float height;           /* Track height (default: 24) */
    float knob_padding;     /* Padding around knob (default: 2) */
    float font_size;        /* Label font size (default: 14) */
    float gap;              /* Gap between toggle and label (default: 8) */
    bool label_left;        /* Place label on left side (default: false = right) */
    CsMargin margin;        /* Outer spacing */
} CsToggleStyle;

/* Default style */
extern const CsToggleStyle CS_TOGGLE_STYLE_DEFAULT;

/* ============================================================================
 * Component
 * ============================================================================ */

/**
 * Toggle switch - immediate mode
 *
 * @param id    Unique identifier (use CS_ID("name"))
 * @param on    Pointer to boolean state (modified on toggle)
 * @param label Optional label text (NULL for no label)
 * @param style Style or NULL for default
 * @return      Result with event flags
 */
CsToggleResult cs_toggle(
    uint32_t id,
    bool *on,
    const char *label,
    const CsToggleStyle *style
);

/* Convenience wrapper */
#define cs_toggle_simple(id, on, label) cs_toggle(id, on, label, NULL)

#ifdef __cplusplus
}
#endif

#endif /* CS_TOGGLE_H */
