/**
 * Clay Components - Text Input
 *
 * Immediate mode text input component.
 *
 * Usage:
 *   static char search[256];
 *   static int search_len = 0;
 *
 *   CsInputResult r = cs_input(CS_ID("search"), search, &search_len, 256, "Search...", NULL);
 *   if (r.submitted) do_search(search);
 */

#ifndef CS_INPUT_H
#define CS_INPUT_H

#include "cs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

typedef struct {
    bool changed;      /* Text was modified this frame */
    bool submitted;    /* Enter was pressed */
    bool focused;      /* Gained focus this frame */
    bool blurred;      /* Lost focus this frame */
} CsInputResult;

typedef struct {
    float width;           /* Fixed width (0 = auto) */
    float height;          /* Fixed height (0 = auto) */
    float font_size;
    float padding;
    float corner_radius;
    /* Layout */
    CsMargin margin;       /* Outer spacing */
    CsAlign align;         /* Self-alignment within parent */
    bool grow;             /* Grow to fill available width */
} CsInputStyle;

/* Default styles */
extern const CsInputStyle CC_INPUT_STYLE_DEFAULT;
extern const CsInputStyle CC_INPUT_STYLE_DARK;

/* ============================================================================
 * Component
 * ============================================================================ */

/**
 * Text input - immediate mode
 *
 * @param id          Unique identifier (use CS_ID("name"))
 * @param text        Your text buffer (modified in place)
 * @param len         Pointer to current length (modified in place)
 * @param max_len     Buffer capacity
 * @param placeholder Placeholder text when empty (can be NULL)
 * @param style       Style or NULL for default
 * @return            Result with event flags
 */
CsInputResult cs_input(
    uint32_t id,
    char *text,
    int *len,
    int max_len,
    const char *placeholder,
    const CsInputStyle *style
);

/* Convenience wrapper with default style */
#define cs_input_simple(id, text, len, max_len, placeholder) \
    cs_input(id, text, len, max_len, placeholder, NULL)

#ifdef __cplusplus
}
#endif

#endif /* CS_INPUT_H */
