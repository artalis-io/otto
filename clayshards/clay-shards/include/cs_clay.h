/**
 * cs_clay.h - Generic Clay Integration for Immediate Mode Components
 *
 * Provides reusable Clay initialization, callbacks, and render command
 * accessors. Applications include this instead of duplicating boilerplate.
 *
 * Usage:
 *   #define CLAY_IMPLEMENTATION
 *   #include "clay.h"
 *   #include "cs_clay.h"
 *
 *   // In your init:
 *   CsClayConfig cfg = cs_clay_default_config();
 *   cs_clay_init(&cfg, width, height);
 *
 *   // In your frame:
 *   cs_clay_begin_frame();
 *   // ... your layout code ...
 *   int count = cs_clay_end_frame(dt);
 */

#ifndef CS_CLAY_H
#define CS_CLAY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration
 * ============================================================================ */

typedef struct {
    uint8_t *memory;            /* Clay arena memory (NULL = use internal) */
    uint32_t memory_size;       /* Arena size in bytes */
    float char_width_ratio;     /* Text width = fontSize * ratio (default 0.6) */
} CsClayConfig;

/* Returns config with sensible defaults */
CsClayConfig cs_clay_default_config(void);

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/* Initialize Clay with config. Returns true on success. */
bool cs_clay_init(const CsClayConfig *config, int width, int height);

/* Update layout dimensions (call on resize) */
void cs_clay_resize(int width, int height);

/* Begin frame - call cs_frame_begin() and Clay_BeginLayout() */
void cs_clay_begin_frame(void);

/* End frame - call Clay_EndLayout() and cs_frame_end(). Returns command count. */
int cs_clay_end_frame(float dt);

/* Check if initialized */
bool cs_clay_is_initialized(void);

/* Get last error message (or NULL) */
const char *cs_clay_get_error(void);

/* ============================================================================
 * Render Command Accessors
 *
 * These provide JavaScript-friendly access to Clay render commands.
 * Command types: 0=None, 1=Rectangle, 2=Border, 3=Text, 4=Image,
 *                5=ScissorStart, 6=ScissorEnd, 7=Custom
 * ============================================================================ */

int      cs_clay_cmd_count(void);
int      cs_clay_cmd_type(int index);
float    cs_clay_cmd_x(int index);
float    cs_clay_cmd_y(int index);
float    cs_clay_cmd_w(int index);
float    cs_clay_cmd_h(int index);

/* Rectangle commands */
uint32_t cs_clay_cmd_rect_color(int index);
float    cs_clay_cmd_rect_radius(int index);

/* Text commands */
const char *cs_clay_cmd_text_str(int index);
int         cs_clay_cmd_text_len(int index);
uint32_t    cs_clay_cmd_text_color(int index);
int         cs_clay_cmd_text_size(int index);

/* Border commands */
uint32_t cs_clay_cmd_border_color(int index);
float    cs_clay_cmd_border_radius(int index);
int      cs_clay_cmd_border_width(int index);

/* ============================================================================
 * Pointer/Input Helpers
 * ============================================================================ */

/* Update Clay pointer state */
void cs_clay_set_pointer(float x, float y, bool is_down);

/* Check if pointer is over a Clay element by string ID */
bool cs_clay_pointer_over(const char *element_id);

/* ============================================================================
 * Font Metrics
 *
 * Call these from JS after loading font to enable accurate text measurement.
 * ============================================================================ */

/* Set glyph advance width (normalized: 1.0 = em size).
 * Call once per glyph after font loads. */
void cs_clay_set_glyph_advance(int unicode, float advance);

/* Set all glyphs at once from a buffer.
 * advances[i] = advance for unicode i, for i in [0, count).
 * More efficient than individual calls. */
void cs_clay_set_glyph_advances(const float *advances, int count);

/* Check if font metrics have been loaded */
bool cs_clay_has_font_metrics(void);

/* Calculate cursor position from X offset within text.
 * x_offset is relative to start of text, font_size in pixels.
 * Returns character index closest to x_offset. */
int cs_clay_x_to_cursor(const char *text, int len, float x_offset, float font_size);

#ifdef __cplusplus
}
#endif

#endif /* CS_CLAY_H */
