/**
 * Clay Components - Dropdown Component
 *
 * A dropdown/select widget for choosing from a list of options.
 *
 * Usage:
 *   const char *options[] = {"Option 1", "Option 2", "Option 3"};
 *   int selected = 0;
 *
 *   CsDropdownResult r = cs_dropdown(
 *       CS_ID("my_dropdown"),
 *       &selected,
 *       options, 3,
 *       NULL  // Default style
 *   );
 *   if (r.changed) {
 *       printf("Selected: %s\n", options[selected]);
 *   }
 */

#ifndef CS_DROPDOWN_H
#define CS_DROPDOWN_H

#include "cs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

/**
 * Result returned from cs_dropdown()
 */
typedef struct {
    bool changed;       /* Selection changed this frame */
    bool opened;        /* Dropdown was opened this frame */
    bool closed;        /* Dropdown was closed this frame */
    bool hovered;       /* Is hovered this frame */
    int selected;       /* Current selected index (after change) */
} CsDropdownResult;

/**
 * Style configuration for dropdown
 */
typedef struct {
    float width;            /* Width of dropdown button (0 = fit content) */
    float height;           /* Height of dropdown button */
    float font_size;        /* Font size for text */
    float corner_radius;    /* Corner radius for button and list */
    float gap;              /* Gap between elements */
    float max_height;       /* Maximum height of dropdown list (0 = no limit) */
    CsMargin margin;        /* Outer margin */
} CsDropdownStyle;

/* Default style */
extern const CsDropdownStyle CS_DROPDOWN_STYLE_DEFAULT;

/* ============================================================================
 * Component
 * ============================================================================ */

/**
 * Render a dropdown selector.
 *
 * @param id        Unique identifier (use CS_ID("name"))
 * @param selected  Pointer to selected index (modified on selection)
 * @param options   Array of option labels
 * @param count     Number of options
 * @param style     Style configuration (NULL for defaults)
 * @return          Result with change state and current selection
 */
CsDropdownResult cs_dropdown(
    uint32_t id,
    int *selected,
    const char *const *options,
    int count,
    const CsDropdownStyle *style
);

/**
 * Check if a dropdown is currently open.
 *
 * @param id  The dropdown ID
 * @return    true if the dropdown list is visible
 */
bool cs_dropdown_is_open(uint32_t id);

/**
 * Close a specific dropdown programmatically.
 *
 * @param id  The dropdown ID to close
 */
void cs_dropdown_close(uint32_t id);

/**
 * Close all open dropdowns.
 */
void cs_dropdown_close_all(void);

#ifdef __cplusplus
}
#endif

#endif /* CS_DROPDOWN_H */
