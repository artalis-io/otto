/**
 * Clay Components - Slider
 *
 * Immediate mode slider component for numeric ranges.
 *
 * Usage:
 *   static float volume = 0.5f;
 *   if (cs_slider(CS_ID("volume"), &volume, 0.0f, 1.0f, "Volume", NULL).changed) {
 *       set_volume(volume);
 *   }
 */

#ifndef CS_SLIDER_H
#define CS_SLIDER_H

#include "cs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

typedef struct {
    bool changed;       /* Value changed this frame */
    bool dragging;      /* Currently being dragged */
    bool hovered;       /* Is hovered this frame */
    float value;        /* Current value (after change) */
} CsSliderResult;

typedef struct {
    float width;            /* Track width (default: 200) */
    float height;           /* Track height (default: 8) */
    float thumb_size;       /* Thumb diameter (default: 20) */
    float font_size;        /* Label font size (default: 14) */
    float gap;              /* Gap between slider and label (default: 8) */
    float corner_radius;    /* Track corner radius (default: 4) */
    float step;             /* Step size (0 = continuous, default: 0) */
    bool show_value;        /* Show numeric value (default: false) */
    CsMargin margin;        /* Outer spacing */
} CsSliderStyle;

/* Default style */
extern const CsSliderStyle CS_SLIDER_STYLE_DEFAULT;

/* ============================================================================
 * Component
 * ============================================================================ */

/**
 * Slider - immediate mode
 *
 * @param id    Unique identifier (use CS_ID("name"))
 * @param value Pointer to float value (modified on change)
 * @param min   Minimum value
 * @param max   Maximum value
 * @param label Optional label text (NULL for no label)
 * @param style Style or NULL for default
 * @return      Result with event flags
 */
CsSliderResult cs_slider(
    uint32_t id,
    float *value,
    float min,
    float max,
    const char *label,
    const CsSliderStyle *style
);

/* Convenience wrapper */
#define cs_slider_simple(id, value, min, max) cs_slider(id, value, min, max, NULL, NULL)

#ifdef __cplusplus
}
#endif

#endif /* CS_SLIDER_H */
