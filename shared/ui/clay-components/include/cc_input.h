/**
 * Clay Components - Text Input
 *
 * Immediate mode text input component.
 *
 * Usage:
 *   static char search[256];
 *   static int search_len = 0;
 *
 *   CcInputResult r = cc_input(CC_ID("search"), search, &search_len, 256, "Search...", NULL);
 *   if (r.submitted) do_search(search);
 */

#ifndef CC_INPUT_H
#define CC_INPUT_H

#include "cc_common.h"

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
} CcInputResult;

typedef struct {
    float width;           /* 0 = auto/grow */
    float height;          /* 0 = auto */
    float font_size;
    float padding;
    float corner_radius;
} CcInputStyle;

/* Default styles */
extern const CcInputStyle CC_INPUT_STYLE_DEFAULT;
extern const CcInputStyle CC_INPUT_STYLE_DARK;

/* ============================================================================
 * Component
 * ============================================================================ */

/**
 * Text input - immediate mode
 *
 * @param id          Unique identifier (use CC_ID("name"))
 * @param text        Your text buffer (modified in place)
 * @param len         Pointer to current length (modified in place)
 * @param max_len     Buffer capacity
 * @param placeholder Placeholder text when empty (can be NULL)
 * @param style       Style or NULL for default
 * @return            Result with event flags
 */
CcInputResult cc_input(
    uint32_t id,
    char *text,
    int *len,
    int max_len,
    const char *placeholder,
    const CcInputStyle *style
);

/* Convenience wrapper with default style */
#define cc_input_simple(id, text, len, max_len, placeholder) \
    cc_input(id, text, len, max_len, placeholder, NULL)

#ifdef __cplusplus
}
#endif

#endif /* CC_INPUT_H */
