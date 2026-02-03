/**
 * Clay Components - Scroll Container
 *
 * Scrollable content area with automatic clipping.
 * Uses Clay's built-in scroll container functionality.
 *
 * Usage:
 *   CS_SCROLL(CS_ID("list"), 300.0f, NULL) {
 *       // ... render scrollable content ...
 *   }
 *
 * To get scroll state after the block:
 *   CsScrollInfo info = cs_scroll_info(CS_ID("list"));
 *   if (info.at_bottom) load_more_items();
 */

#ifndef CS_SCROLL_H
#define CS_SCROLL_H

#include "cs_common.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * Scroll container style configuration.
 */
typedef struct {
    float width;              /* Viewport width (0 = grow to fill) */
    bool horizontal;          /* Enable horizontal scrolling */
    bool vertical;            /* Enable vertical scrolling (default: true) */
    float corner_radius;      /* Corner radius for container */
    CsMargin margin;          /* Outer margin */
} CsScrollStyle;

/**
 * Scroll container state information.
 * Retrieve with cs_scroll_info() after the scroll block.
 */
typedef struct {
    float scroll_x;           /* Current horizontal scroll offset */
    float scroll_y;           /* Current vertical scroll offset */
    float content_width;      /* Total content width */
    float content_height;     /* Total content height */
    float view_width;         /* Visible viewport width */
    float view_height;        /* Visible viewport height */
    bool hovered;             /* Pointer is over scroll area */
    bool found;               /* Scroll container data was found */
    bool at_top;              /* Scrolled to top */
    bool at_bottom;           /* Scrolled to bottom */
    bool at_left;             /* Scrolled to left edge */
    bool at_right;            /* Scrolled to right edge */
} CsScrollInfo;

/**
 * Internal helper struct for the CS_SCROLL macro.
 */
typedef struct {
    uint32_t id;
    bool loop;
} CsScrollContext;

/**
 * Default scroll style.
 */
extern const CsScrollStyle CS_SCROLL_STYLE_DEFAULT;

/**
 * Get scroll state information for a scroll container.
 * Call after the CS_SCROLL block to retrieve current scroll position.
 *
 * @param id Widget ID used in CS_SCROLL
 * @return Current scroll state
 */
CsScrollInfo cs_scroll_info(uint32_t id);

/**
 * Set scroll wheel delta for this frame.
 *
 * Call this from your wheel event handler. The delta will be applied
 * to any hovered scroll container via Clay_UpdateScrollContainers().
 *
 * @param delta_y Vertical scroll amount (positive = scroll down)
 */
void cs_set_scroll_delta(float delta_y);

/**
 * Set scroll wheel delta with horizontal support.
 *
 * @param delta_x Horizontal scroll amount (positive = scroll right)
 * @param delta_y Vertical scroll amount (positive = scroll down)
 */
void cs_set_scroll_delta_xy(float delta_x, float delta_y);

/**
 * Check if any scroll container is currently hovered.
 *
 * Useful for routing wheel events: if a scroll container is hovered,
 * the wheel should scroll it; otherwise, route to other handlers.
 *
 * @return true if a scroll container is under the pointer
 */
bool cs_scroll_container_hovered(void);

/**
 * Update scroll containers with accumulated delta.
 * Called automatically during frame processing.
 *
 * @param dt Delta time since last frame
 */
void cs_update_scroll_containers(float dt);

/* Internal functions for macro */
CsScrollContext cs_scroll_begin_internal(uint32_t id, float height, const CsScrollStyle *style);
void cs_scroll_end_internal(CsScrollContext *ctx);

/**
 * Scroll container macro.
 *
 * Creates a scrollable container with the specified height.
 * Content rendered inside the block will be clipped and scrollable.
 *
 * @param id Unique widget ID (use CS_ID("name"))
 * @param height Viewport height in pixels
 * @param style Optional style pointer (NULL for defaults)
 *
 * Example:
 *   CS_SCROLL(CS_ID("items"), 200.0f, NULL) {
 *       for (int i = 0; i < 100; i++) {
 *           render_item(i);
 *       }
 *   }
 */
#define CS_SCROLL(id, height, style) \
    for (CsScrollContext _cs_scroll_ctx = cs_scroll_begin_internal(id, height, style); \
         _cs_scroll_ctx.loop; \
         _cs_scroll_ctx.loop = false, cs_scroll_end_internal(&_cs_scroll_ctx))

#endif /* CS_SCROLL_H */
