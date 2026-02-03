/**
 * Clay Components - Scroll Container Implementation
 *
 * Wraps Clay's built-in scroll functionality with a ClayShards-style API.
 */

#include "cs_scroll.h"
#include "cs_internal.h"
#include "clay.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define CS_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define CS_EXPORT
#endif

/* ============================================================================
 * Default Style
 * ============================================================================ */

const CsScrollStyle CS_SCROLL_STYLE_DEFAULT = {
    .width = 0.0f,            /* Grow to fill */
    .horizontal = false,
    .vertical = true,
    .corner_radius = 0.0f,
};

/* ============================================================================
 * Scroll Delta Management
 * ============================================================================ */

CS_EXPORT void cs_set_scroll_delta(float delta_y) {
    CsState *g = cs_get_state();
    g->scroll_delta_y += delta_y;
}

CS_EXPORT void cs_set_scroll_delta_xy(float delta_x, float delta_y) {
    CsState *g = cs_get_state();
    g->scroll_delta_x += delta_x;
    g->scroll_delta_y += delta_y;
}

CS_EXPORT bool cs_scroll_container_hovered(void) {
    CsState *g = cs_get_state();
    return g->scroll_container_hovered;
}

/* ============================================================================
 * Scroll Container Update
 * ============================================================================ */

void cs_update_scroll_containers(float dt) {
    CsState *g = cs_get_state();

    Clay_Vector2 delta = { g->scroll_delta_x, g->scroll_delta_y };

    /* Update Clay's scroll containers with accumulated delta */
    Clay_UpdateScrollContainers(
        true,   /* Enable drag scrolling for touch devices */
        delta,
        dt
    );

    /* Reset delta after processing */
    g->scroll_delta_x = 0.0f;
    g->scroll_delta_y = 0.0f;
}

/* ============================================================================
 * Scroll Info Retrieval
 * ============================================================================ */

CsScrollInfo cs_scroll_info(uint32_t id) {
    CsScrollInfo info = {0};

    Clay_ElementId clay_id = (Clay_ElementId){.id = id, .stringId = {0}};
    Clay_ScrollContainerData data = Clay_GetScrollContainerData(clay_id);

    if (!data.found) {
        info.found = false;
        info.at_top = true;
        info.at_bottom = true;
        info.at_left = true;
        info.at_right = true;
        return info;
    }

    info.found = true;

    if (data.scrollPosition) {
        /* Clay uses negative values for scroll offset */
        info.scroll_x = -data.scrollPosition->x;
        info.scroll_y = -data.scrollPosition->y;
    }

    info.content_width = data.contentDimensions.width;
    info.content_height = data.contentDimensions.height;
    info.view_width = data.scrollContainerDimensions.width;
    info.view_height = data.scrollContainerDimensions.height;

    /* Check hover state */
    info.hovered = Clay_PointerOver(clay_id);

    /* Calculate edge states with small tolerance for float comparison */
    const float epsilon = 0.5f;
    info.at_top = (info.scroll_y <= epsilon);
    info.at_left = (info.scroll_x <= epsilon);

    float max_scroll_y = info.content_height - info.view_height;
    float max_scroll_x = info.content_width - info.view_width;

    info.at_bottom = (max_scroll_y <= epsilon) || (info.scroll_y >= max_scroll_y - epsilon);
    info.at_right = (max_scroll_x <= epsilon) || (info.scroll_x >= max_scroll_x - epsilon);

    return info;
}

/* ============================================================================
 * Scroll Container Macro Support
 * ============================================================================ */

CsScrollContext cs_scroll_begin_internal(uint32_t id, float height, const CsScrollStyle *style) {
    CsScrollContext ctx = { .id = id, .loop = true };
    CsState *g = cs_get_state();

    if (!style) {
        style = &CS_SCROLL_STYLE_DEFAULT;
    }

    /* Validate height */
    float view_height = height;
    if (view_height <= 0.0f) {
        view_height = 200.0f;  /* Sensible default */
    }

    /* Track active scroll container */
    g->active_scroll_id = id;

    /* Build sizing configuration */
    Clay_SizingAxis width_sizing;
    if (style->width > 0.0f) {
        width_sizing = CLAY_SIZING_FIXED(style->width);
    } else {
        width_sizing = CLAY_SIZING_GROW(0);
    }

    /* Build padding from margin */
    Clay_Padding padding = {0};
    if (cs_has_margin(style->margin)) {
        padding.left = (uint16_t)style->margin.left;
        padding.right = (uint16_t)style->margin.right;
        padding.top = (uint16_t)style->margin.top;
        padding.bottom = (uint16_t)style->margin.bottom;
    }

    /* Create Clay element ID */
    Clay_ElementId clay_id = (Clay_ElementId){.id = id, .stringId = {0}};

    /* Open the Clay element with ID */
    Clay__OpenElementWithId(clay_id);

    /* Configure the element with clip for scrolling */
    Clay__ConfigureOpenElement((Clay_ElementDeclaration){
        .layout = {
            .sizing = {
                .width = width_sizing,
                .height = CLAY_SIZING_FIXED(view_height)
            },
            .padding = padding,
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        },
        .clip = {
            .horizontal = style->horizontal,
            .vertical = style->vertical,
            .childOffset = Clay_GetScrollOffset()
        },
        .cornerRadius = CLAY_CORNER_RADIUS(style->corner_radius)
    });

    /* Track if this container is hovered */
    if (Clay_PointerOver(clay_id)) {
        g->scroll_container_hovered = true;
    }

    return ctx;
}

void cs_scroll_end_internal(CsScrollContext *ctx) {
    CsState *g = cs_get_state();

    /* Close the Clay element */
    Clay__CloseElement();

    /* Clear active scroll container */
    g->active_scroll_id = 0;

    (void)ctx;  /* Currently unused, but available for future state */
}
