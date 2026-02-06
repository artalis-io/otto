/**
 * Clay Components - Dropdown Implementation
 */

#include "cs_dropdown.h"
#include "cs_internal.h"
#include "clay.h"
#include <string.h>

/* ============================================================================
 * Default Style
 * ============================================================================ */

const CsDropdownStyle CS_DROPDOWN_STYLE_DEFAULT = {
    .width = 200.0f,
    .height = 32.0f,
    .font_size = 14.0f,
    .corner_radius = 4.0f,
    .gap = 4.0f,
    .max_height = 200.0f,
};

/* ============================================================================
 * State Management
 * ============================================================================ */

/*
 * ARCHITECTURE: Single-open dropdown design.
 *
 * Only one dropdown can be open at a time per thread. Opening a new dropdown
 * automatically closes any previously open dropdown. This is a deliberate
 * design choice for simplicity - most UIs only need one dropdown open at once.
 *
 * Limitations:
 *   - Nested/hierarchical dropdown menus are not supported
 *   - Opening dropdown B while dropdown A is open will close A
 *   - Multi-level menus require alternative UI patterns (e.g., tree view)
 *
 * If hierarchical dropdowns are needed in the future, this could be refactored
 * to use a stack of open dropdown IDs instead of a single ID.
 */
static CS_THREAD_LOCAL uint32_t tls_open_dropdown_id = 0;

bool cs_dropdown_is_open(uint32_t id) {
    return tls_open_dropdown_id == id && id != 0;
}

void cs_dropdown_close(uint32_t id) {
    if (tls_open_dropdown_id == id) {
        tls_open_dropdown_id = 0;
    }
}

void cs_dropdown_close_all(void) {
    tls_open_dropdown_id = 0;
}

/* ============================================================================
 * Component
 * ============================================================================ */

CsDropdownResult cs_dropdown(
    uint32_t id,
    int *selected,
    const char *const *options,
    int count,
    const CsDropdownStyle *style
) {
    CsDropdownResult result = {0};

    /* Validate required parameters */
    if (!selected || !options || count <= 0) {
        cs_record_error(CS_ERR_INVALID_ARGUMENT);
        return result;
    }

    /* Clamp selected to valid range */
    if (*selected < 0) *selected = 0;
    if (*selected >= count) *selected = count - 1;

    CsState *g = cs_get_state();

    if (!style) style = &CS_DROPDOWN_STYLE_DEFAULT;

    /* Register for tab navigation */
    cs_register_focusable(id);

    bool is_focused = (g->focused_id == id);
    bool is_open = cs_dropdown_is_open(id);
    int current_selected = *selected;

    result.selected = current_selected;

    /* Clay element ID for the button */
    Clay_ElementId clay_id = (Clay_ElementId){.id = id, .stringId = {0}};

    /* Check hover using previous frame's data */
    bool is_hovered = Clay_PointerOver(clay_id);
    result.hovered = is_hovered;

    /* Focus follows hover - update immediately for this frame */
    if (is_hovered) {
        g->focused_id = id;
        is_focused = true;
    }

    /* ========================================================================
     * INPUT HANDLING - Process all input BEFORE rendering
     * ======================================================================== */

    /* Handle keyboard when focused - BEFORE rendering so state is up-to-date */
    if (is_focused) {
        /* Enter/Space toggles open state */
        if (g->pending_enter) {
            if (is_open) {
                tls_open_dropdown_id = 0;
                is_open = false;  /* Update for rendering */
                result.closed = true;
            } else {
                tls_open_dropdown_id = id;
                is_open = true;   /* Update for rendering */
                result.opened = true;
            }
        }

        /* Escape closes dropdown */
        if (g->pending_escape && is_open) {
            tls_open_dropdown_id = 0;
            is_open = false;      /* Update for rendering */
            result.closed = true;
        }

        /* Arrow keys navigate selection */
        if (g->pending_arrow_up) {
            if (!is_open) {
                /* Open dropdown when pressing arrow on closed dropdown */
                tls_open_dropdown_id = id;
                is_open = true;   /* Update for rendering */
                result.opened = true;
            } else if (current_selected > 0) {
                *selected = current_selected - 1;
                current_selected = *selected;  /* Update for rendering */
                result.changed = true;
                result.selected = *selected;
            }
        }
        if (g->pending_arrow_down) {
            if (!is_open) {
                /* Open dropdown when pressing arrow on closed dropdown */
                tls_open_dropdown_id = id;
                is_open = true;   /* Update for rendering */
                result.opened = true;
            } else if (current_selected < count - 1) {
                *selected = current_selected + 1;
                current_selected = *selected;  /* Update for rendering */
                result.changed = true;
                result.selected = *selected;
            }
        }
    }

    /* Handle button click to toggle open/close - BEFORE rendering */
    if (is_hovered && g->pending_click && !result.closed) {
        if (is_open) {
            tls_open_dropdown_id = 0;
            is_open = false;      /* Update for rendering */
            result.closed = true;
        } else {
            /* Close any other open dropdown first */
            tls_open_dropdown_id = id;
            is_open = true;       /* Update for rendering */
            result.opened = true;
        }
        g->clicked_id = id;
        g->focused_id = id;  /* Focus on click */
    }

    /* Close dropdown if clicked elsewhere (not on button or list) - BEFORE rendering */
    if (is_open && g->pending_click && !is_hovered && !result.closed) {
        /* Check if click was on any list item */
        bool clicked_on_list = false;
        for (int i = 0; i < count; i++) {
            Clay_ElementId item_id = (Clay_ElementId){.id = id + CS_ID_OFFSET_DROPDOWN_ITEM + (uint32_t)i, .stringId = {0}};
            if (Clay_PointerOver(item_id)) {
                clicked_on_list = true;
                break;
            }
        }
        if (!clicked_on_list) {
            tls_open_dropdown_id = 0;
            is_open = false;      /* Update for rendering */
            result.closed = true;
        }
    }

    /* ========================================================================
     * RENDERING - Now render with the updated state
     * ======================================================================== */

    /* Colors - show focus state via background, not just border */
    Clay_Color bg_color = is_hovered
        ? (Clay_Color){CS_COLOR_BG_HOVER}
        : is_focused
            ? (Clay_Color){CS_COLOR_BG_FOCUSED}
            : (Clay_Color){CS_COLOR_BG_DEFAULT};
    Clay_Color text_color = (Clay_Color){CS_COLOR_TEXT};
    Clay_Color border_color = is_focused
        ? (Clay_Color){CS_COLOR_BORDER_FOCUSED}
        : (Clay_Color){CS_COLOR_BORDER};
    Clay_Color list_bg = (Clay_Color){CS_COLOR_BG_DEFAULT};
    Clay_Color item_hover_bg = (Clay_Color){CS_COLOR_BG_HOVER};

    /* Build margin */
    bool has_margin = cs_has_margin(style->margin);

    /* Container for dropdown (button + list) */
    Clay_ElementId container_id = (Clay_ElementId){.id = id + CS_ID_OFFSET_WRAPPER, .stringId = {0}};

    Clay_ElementDeclaration container_config = {
        .layout = {
            .sizing = {
                .width = style->width > 0 ? CLAY_SIZING_FIXED(style->width) : CLAY_SIZING_FIT(0),
                .height = CLAY_SIZING_FIT(0)
            },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = has_margin ? (Clay_Padding){
                .left = (uint16_t)style->margin.left,
                .right = (uint16_t)style->margin.right,
                .top = (uint16_t)style->margin.top,
                .bottom = (uint16_t)style->margin.bottom
            } : (Clay_Padding){0}
        }
    };

    CLAY(container_id, container_config) {
        /* Dropdown button */
        Clay_ElementDeclaration button_config = {
            .layout = {
                .sizing = {
                    .width = CLAY_SIZING_GROW(0),
                    .height = CLAY_SIZING_FIXED(style->height)
                },
                .padding = {8, 8, 8, 8},
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childGap = 8
            },
            .backgroundColor = bg_color,
            .cornerRadius = CLAY_CORNER_RADIUS(style->corner_radius),
            .border = {
                .color = border_color,
                .width = {1, 1, 1, 1, 0}
            }
        };

        CLAY(clay_id, button_config) {
            /* Current selection text */
            const char *label = options[current_selected];
            Clay_String label_str = {.chars = label, .length = (int)strnlen(label, CS_MAX_LABEL_LEN)};

            /* Text container takes remaining space */
            Clay_ElementId text_id = (Clay_ElementId){.id = id + CS_ID_OFFSET_TEXT_WRAPPER, .stringId = {0}};
            CLAY(text_id, {
                .layout = {
                    .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0) }
                }
            }) {
                CLAY_TEXT(label_str, CLAY_TEXT_CONFIG({
                    .fontSize = (uint16_t)style->font_size,
                    .textColor = text_color
                }));
            }

            /* Arrow indicator */
            Clay_ElementId arrow_id = (Clay_ElementId){.id = id + CS_ID_OFFSET_DROPDOWN_ARROW, .stringId = {0}};
            const char *arrow = is_open ? "\xE2\x96\xB2" : "\xE2\x96\xBC";  /* Unicode triangles */
            Clay_String arrow_str = {.chars = arrow, .length = 3};
            CLAY(arrow_id, {
                .layout = {
                    .sizing = { .width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIT(0) }
                }
            }) {
                CLAY_TEXT(arrow_str, CLAY_TEXT_CONFIG({
                    .fontSize = (uint16_t)(style->font_size * 0.7f),
                    .textColor = text_color
                }));
            }

            /* Dropdown list - FLOATING to overlay on top of other content */
            if (is_open) {
                Clay_ElementId list_id = (Clay_ElementId){.id = id + CS_ID_OFFSET_DROPDOWN_LIST, .stringId = {0}};

                /* Calculate list width - match button width */
                float list_width = style->width > 0 ? style->width : 120.0f;

                /* TUI-friendly: smaller offset and padding when height is small */
                bool tui_mode = style->height <= 3;
                Clay_Vector2 list_offset = tui_mode ? (Clay_Vector2){0, 1} : (Clay_Vector2){0, 2};
                Clay_Padding list_pad = tui_mode ? (Clay_Padding){1, 1, 0, 0} : (Clay_Padding){4, 4, 4, 4};

                Clay_ElementDeclaration list_config = {
                    .floating = {
                        .attachTo = CLAY_ATTACH_TO_PARENT,
                        .attachPoints = {
                            .element = CLAY_ATTACH_POINT_LEFT_TOP,
                            .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM
                        },
                        .offset = list_offset,
                        .zIndex = 1000     /* Ensure it's on top */
                    },
                    .layout = {
                        .sizing = {
                            .width = CLAY_SIZING_FIXED(list_width),
                            .height = style->max_height > 0
                                ? CLAY_SIZING_FIT(.max = (float)style->max_height)
                                : CLAY_SIZING_FIT(.max = 0)
                        },
                        .layoutDirection = CLAY_TOP_TO_BOTTOM,
                        .padding = list_pad
                    },
                    .backgroundColor = list_bg,
                    .cornerRadius = CLAY_CORNER_RADIUS(style->corner_radius),
                    .border = {
                        .color = border_color,
                        .width = {1, 1, 1, 1, 0}
                    }
                };

                CLAY(list_id, list_config) {
                    for (int i = 0; i < count; i++) {
                        Clay_ElementId item_id = (Clay_ElementId){.id = id + CS_ID_OFFSET_DROPDOWN_ITEM + (uint32_t)i, .stringId = {0}};
                        bool item_hovered = Clay_PointerOver(item_id);
                        bool item_selected = (i == current_selected);

                        /* Use list_bg for default to properly overwrite underlying content */
                        /* When focused (keyboard nav), highlight selected item more prominently */
                        Clay_Color item_bg = item_hovered ? item_hover_bg
                                           : item_selected ? (Clay_Color){CS_COLOR_BTN_BLUE_FOCUS}
                                           : list_bg;

                        /* TUI-friendly sizing: use height directly if small (<= 3),
                         * otherwise subtract padding for pixel mode */
                        float item_h = style->height <= 3 ? style->height : style->height - 4;
                        if (item_h < 1) item_h = 1;
                        /* TUI-friendly padding: minimal if height is small */
                        Clay_Padding item_pad = style->height <= 3
                            ? (Clay_Padding){1, 1, 0, 0}
                            : (Clay_Padding){8, 8, 4, 4};

                        CLAY(item_id, {
                            .layout = {
                                .sizing = {
                                    .width = CLAY_SIZING_GROW(0),
                                    .height = CLAY_SIZING_FIXED(item_h)
                                },
                                .padding = item_pad,
                                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
                            },
                            .backgroundColor = item_bg,
                            .cornerRadius = CLAY_CORNER_RADIUS(style->corner_radius > 2 ? style->corner_radius - 2 : 0)
                        }) {
                            const char *opt_label = options[i];
                            Clay_String opt_str = {.chars = opt_label, .length = (int)strnlen(opt_label, CS_MAX_LABEL_LEN)};
                            CLAY_TEXT(opt_str, CLAY_TEXT_CONFIG({
                                .fontSize = (uint16_t)style->font_size,
                                .textColor = text_color
                            }));
                        }

                        /* Check for item click */
                        if (item_hovered && g->pending_click) {
                            if (i != current_selected) {
                                *selected = i;
                                result.changed = true;
                                result.selected = i;
                            }
                            /* Close after selection */
                            tls_open_dropdown_id = 0;
                            result.closed = true;
                            g->clicked_id = item_id.id;
                        }
                    }
                }
            }
        }
    }

    /* Register hit targets for click detection (uses previous frame's bounds) */
    /* Items are registered at dropdown z-level so they win over the button */
    if (is_open) {
        cs_push_z_index(CS_Z_DROPDOWN);
        for (int i = 0; i < count; i++) {
            Clay_ElementId item_id = (Clay_ElementId){.id = id + CS_ID_OFFSET_DROPDOWN_ITEM + (uint32_t)i, .stringId = {0}};
            Clay_BoundingBox item_box = Clay_GetElementData(item_id).boundingBox;
            cs_register_hit_target(id, CS_HIT_ITEM, (int16_t)i, item_box.x, item_box.y, item_box.width, item_box.height);
        }
        cs_pop_z_index();
    }
    /* Button registered at base z-level */
    Clay_BoundingBox button_box = Clay_GetElementData(clay_id).boundingBox;
    cs_register_hit_target(id, CS_HIT_BODY, -1, button_box.x, button_box.y, button_box.width, button_box.height);

    /* Mark as non-text element for keyboard navigation */
    CS_MARK_NON_TEXT_IF_FOCUSED(g, is_focused);

    return result;
}
