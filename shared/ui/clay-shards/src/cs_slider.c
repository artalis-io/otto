/**
 * Clay Components - Slider Implementation
 */

#include "cs_slider.h"
#include "cs_internal.h"
#include "clay.h"
#include <string.h>
#include <stdio.h>  /* For snprintf */
#include <math.h>   /* For roundf */

/* ============================================================================
 * Default Style
 * ============================================================================ */

const CsSliderStyle CS_SLIDER_STYLE_DEFAULT = {
    .width = 200.0f,
    .height = 8.0f,
    .thumb_size = 20.0f,
    .font_size = 14.0f,
    .gap = 8.0f,
    .corner_radius = 4.0f,
    .step = 0.0f,
    .show_value = false,
};

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static float clamp_f(float v, float min, float max) {
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

static float snap_to_step(float value, float min, float max, float step) {
    if (step <= 0.0f) return value;
    float range = max - min;
    if (range <= 0.0f) return min;
    float normalized = (value - min) / range;
    float steps = roundf(normalized * (range / step));
    return clamp_f(min + steps * step, min, max);
}

/* ============================================================================
 * Component
 * ============================================================================ */

CsSliderResult cs_slider(
    uint32_t id,
    float *value,
    float min,
    float max,
    const char *label,
    const CsSliderStyle *style
) {
    CsSliderResult result = {0};

    /* Validate required parameter */
    if (!value) return result;

    /* Validate range */
    if (max <= min) {
        result.value = *value;
        return result;
    }

    CsState *g = cs_get_state();

    if (!style) style = &CS_SLIDER_STYLE_DEFAULT;

    /* Register for tab navigation */
    cs_register_focusable(id);

    bool is_focused = (g->focused_id == id);
    float current_value = clamp_f(*value, min, max);

    /* Clay element ID for the clickable track area */
    Clay_ElementId clay_id = (Clay_ElementId){.id = id, .stringId = {0}};

    /* Check hover using previous frame's data */
    bool is_hovered = Clay_PointerOver(clay_id);
    result.hovered = is_hovered;

    /* Check if we're currently dragging this slider */
    bool is_dragging = (g->dragging_id == id);
    result.dragging = is_dragging;

    /* Colors */
    Clay_Color track_bg = (Clay_Color){CS_COLOR_BTN_GRAY};
    Clay_Color track_fill = (Clay_Color){CS_COLOR_BTN_BLUE};
    Clay_Color label_color = (Clay_Color){CS_COLOR_TEXT};
    Clay_Color border_color = (Clay_Color){CS_COLOR_BORDER_FOCUSED};

    /* Calculate thumb position (0.0 to 1.0) */
    float normalized = (current_value - min) / (max - min);
    float track_width = style->width;
    float usable_width = track_width - style->thumb_size;
    float thumb_offset = normalized * usable_width;

    /* Build the slider layout: [label] [track] [value?] */
    bool has_margin = cs_has_margin(style->margin);
    bool has_label = label && label[0] != '\0';

    Clay_ElementDeclaration row_config = {
        .layout = {
            .sizing = { .width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIT(0) },
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            .childGap = (has_label || style->show_value) ? (uint16_t)style->gap : 0,
            .padding = has_margin ? (Clay_Padding){
                .left = (uint16_t)style->margin.left,
                .right = (uint16_t)style->margin.right,
                .top = (uint16_t)style->margin.top,
                .bottom = (uint16_t)style->margin.bottom
            } : (Clay_Padding){0}
        }
    };

    CLAY(clay_id, row_config) {
        /* Optional label */
        if (has_label) {
            Clay_String label_str = {.chars = label, .length = (int)strlen(label)};
            CLAY_TEXT(label_str, CLAY_TEXT_CONFIG({
                .fontSize = (uint16_t)style->font_size,
                .textColor = label_color
            }));
        }

        /* The track container (clickable area) */
        Clay_ElementId track_container_id = (Clay_ElementId){.id = id + CS_ID_OFFSET_WRAPPER, .stringId = {0}};
        float container_height = style->thumb_size > style->height ? style->thumb_size : style->height;

        Clay_ElementDeclaration track_container_config = {
            .layout = {
                .sizing = {
                    .width = CLAY_SIZING_FIXED(track_width),
                    .height = CLAY_SIZING_FIXED(container_height)
                },
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
            }
        };
        if (is_focused) {
            track_container_config.border.color = border_color;
            track_container_config.border.width = (Clay_BorderWidth){1, 1, 1, 1, 0};
            track_container_config.cornerRadius = CLAY_CORNER_RADIUS(style->corner_radius);
        }

        CLAY(track_container_id, track_container_config) {
            /* Track background */
            Clay_ElementId track_bg_id = (Clay_ElementId){.id = id + CS_ID_OFFSET_TEXT_WRAPPER, .stringId = {0}};
            CLAY(track_bg_id, {
                .layout = {
                    .sizing = {
                        .width = CLAY_SIZING_FIXED(track_width),
                        .height = CLAY_SIZING_FIXED(style->height)
                    }
                },
                .backgroundColor = track_bg,
                .cornerRadius = CLAY_CORNER_RADIUS(style->corner_radius)
            }) {
                /* Filled portion (shows current value) */
                float fill_width = thumb_offset + style->thumb_size / 2.0f;
                if (fill_width > 0) {
                    Clay_ElementId fill_id = (Clay_ElementId){.id = id + CS_ID_OFFSET_SLIDER_FILL, .stringId = {0}};
                    CLAY(fill_id, {
                        .layout = {
                            .sizing = {
                                .width = CLAY_SIZING_FIXED(fill_width),
                                .height = CLAY_SIZING_FIXED(style->height)
                            }
                        },
                        .backgroundColor = track_fill,
                        .cornerRadius = CLAY_CORNER_RADIUS(style->corner_radius)
                    }) {}
                }
            }
        }

        /* Thumb is drawn separately for positioning - we'll use the track bounds */

        /* Optional value display */
        if (style->show_value) {
            char value_buf[32];
            int len = snprintf(value_buf, sizeof(value_buf), "%.1f", current_value);
            if (len < 0) len = 0;
            if (len >= (int)sizeof(value_buf)) len = (int)sizeof(value_buf) - 1;
            Clay_String value_str = {.chars = value_buf, .length = len};
            CLAY_TEXT(value_str, CLAY_TEXT_CONFIG({
                .fontSize = (uint16_t)style->font_size,
                .textColor = label_color
            }));
        }
    }

    /* Get track bounds for drag calculation */
    Clay_ElementId track_container_id = (Clay_ElementId){.id = id + CS_ID_OFFSET_WRAPPER, .stringId = {0}};
    Clay_BoundingBox track_box = Clay_GetElementData(track_container_id).boundingBox;

    /* Handle drag start */
    if (is_hovered && g->pointer_down && !is_dragging && g->dragging_id == 0) {
        g->dragging_id = id;
        is_dragging = true;
        result.dragging = true;
    }

    /* Handle dragging - calculate value from pointer position */
    if (is_dragging && g->pointer_down) {
        float rel_x = g->pointer_x - track_box.x - style->thumb_size / 2.0f;
        float new_normalized = rel_x / usable_width;
        new_normalized = clamp_f(new_normalized, 0.0f, 1.0f);
        float new_value = min + new_normalized * (max - min);

        /* Apply step snapping */
        new_value = snap_to_step(new_value, min, max, style->step);

        if (new_value != current_value) {
            *value = new_value;
            result.changed = true;
        }
    }

    /* Handle click (mouse) - jump to position */
    if (is_hovered && g->pending_click && !is_dragging) {
        float rel_x = g->pointer_x - track_box.x - style->thumb_size / 2.0f;
        float new_normalized = rel_x / usable_width;
        new_normalized = clamp_f(new_normalized, 0.0f, 1.0f);
        float new_value = min + new_normalized * (max - min);

        new_value = snap_to_step(new_value, min, max, style->step);

        if (new_value != current_value) {
            *value = new_value;
            result.changed = true;
        }
        g->clicked_id = id;
    }

    /* Handle keyboard - arrow keys when focused */
    if (is_focused) {
        float step_size = style->step > 0.0f ? style->step : (max - min) / 20.0f;
        float new_value = current_value;

        if (g->pending_arrow_left || g->pending_arrow_down) {
            new_value = current_value - step_size;
            new_value = snap_to_step(new_value, min, max, style->step);
            new_value = clamp_f(new_value, min, max);
        } else if (g->pending_arrow_right || g->pending_arrow_up) {
            new_value = current_value + step_size;
            new_value = snap_to_step(new_value, min, max, style->step);
            new_value = clamp_f(new_value, min, max);
        }

        if (new_value != current_value) {
            *value = new_value;
            result.changed = true;
        }
    }

    /* Update result with final value */
    result.value = *value;

    /* Mark as non-text element for keyboard navigation */
    CS_MARK_NON_TEXT_IF_FOCUSED(g, is_focused);

    return result;
}
