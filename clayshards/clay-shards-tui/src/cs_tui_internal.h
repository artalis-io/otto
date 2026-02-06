/**
 * cs_tui_internal.h - Internal types and macros
 */

#ifndef CS_TUI_INTERNAL_H
#define CS_TUI_INTERNAL_H

#include "../include/cs_tui.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define CS_TUI_MAX_SCISSOR_DEPTH 16
#define CS_TUI_OUTPUT_INITIAL_CAP 4096

/* ============================================================================
 * Cell Structure
 * ============================================================================ */

/*
 * Single cell in the character buffer.
 * Packed to exactly 16 bytes for predictable WASM memory layout.
 *
 * Memory layout (for JS to read from WASM):
 *   Offset  Size  Field
 *   0       4     codepoint (uint32)
 *   4       1     fg_r
 *   5       1     fg_g
 *   6       1     fg_b
 *   7       1     bg_r
 *   8       1     bg_g
 *   9       1     bg_b
 *   10      1     flags
 *   11      1     _pad1 (padding)
 *   12      2     z_index (int16)
 *   14      2     _pad2 (padding)
 *   Total: 16 bytes
 */
#ifdef __EMSCRIPTEN__
#define CS_TUI_CELL_PACKED __attribute__((packed, aligned(4)))
#else
#define CS_TUI_CELL_PACKED
#endif

typedef struct CS_TUI_CELL_PACKED {
    uint32_t codepoint;     /* Unicode codepoint (0 = space) */
    uint8_t fg_r, fg_g, fg_b;
    uint8_t bg_r, bg_g, bg_b;
    uint8_t flags;          /* Reserved for bold, underline, etc. */
    uint8_t _pad1;          /* Explicit padding for alignment */
    int16_t z_index;        /* Z-index for layering (higher = on top) */
    uint16_t _pad2;         /* Padding to 16 bytes */
} CsTuiCell;

/* Compile-time size assertion */
_Static_assert(sizeof(CsTuiCell) == 16, "CsTuiCell must be exactly 16 bytes");

/* Cell flags (future use) */
#define CS_TUI_CELL_BOLD      0x01
#define CS_TUI_CELL_UNDERLINE 0x02
#define CS_TUI_CELL_INVERSE   0x04

/* ============================================================================
 * Buffer Structure
 * ============================================================================ */

typedef struct {
    CsTuiCell *front;       /* Currently displayed */
    CsTuiCell *back;        /* Being rendered to */
    int width;
    int height;
} CsTuiBuffer;

/* ============================================================================
 * Scissor Region
 * ============================================================================ */

typedef struct {
    int x, y, w, h;
} CsTuiScissor;

/* ============================================================================
 * Renderer Structure
 * ============================================================================ */

struct CsTuiRenderer {
    CsTuiConfig config;
    CsTuiBuffer buffer;
    CsTuiColorMode effective_color_mode;  /* Resolved from AUTO */

    /* Scissor stack */
    CsTuiScissor scissor_stack[CS_TUI_MAX_SCISSOR_DEPTH];
    int scissor_depth;

    /* Output buffer (batches write() calls) */
    char *output_buf;
    size_t output_len;
    size_t output_cap;

    /* Cursor state */
    int cursor_x, cursor_y;
    bool cursor_visible;

    /* Z-index tracking for layered rendering */
    int16_t current_z_index;  /* Z-index of current render command */

    /* Flags */
    bool needs_full_redraw;
    bool alternate_screen_active;
    bool cursor_hidden;
    bool buffer_dirty;          /* Set when buffer changes, cleared by cs_tui_buffer_dirty() */
};

/* ============================================================================
 * Internal Functions
 * ============================================================================ */

/* Buffer management */
static inline CsTuiCell *cs_tui_cell_at(CsTuiBuffer *buf, int x, int y) {
    if (x < 0 || x >= buf->width || y < 0 || y >= buf->height) {
        return NULL;
    }
    return &buf->back[y * buf->width + x];
}

static inline CsTuiCell *cs_tui_front_cell_at(CsTuiBuffer *buf, int x, int y) {
    if (x < 0 || x >= buf->width || y < 0 || y >= buf->height) {
        return NULL;
    }
    return &buf->front[y * buf->width + x];
}

/* Output buffer */
void cs_tui_output_init(CsTuiRenderer *r);
void cs_tui_output_free(CsTuiRenderer *r);
void cs_tui_output_append(CsTuiRenderer *r, const char *str, size_t len);
void cs_tui_output_printf(CsTuiRenderer *r, const char *fmt, ...);
void cs_tui_output_flush(CsTuiRenderer *r);

/* Color conversion */
void cs_tui_emit_fg_color(CsTuiRenderer *r, uint8_t r_, uint8_t g, uint8_t b);
void cs_tui_emit_bg_color(CsTuiRenderer *r, uint8_t r_, uint8_t g, uint8_t b);
int cs_tui_rgb_to_256(uint8_t r, uint8_t g, uint8_t b);

/* Box drawing */
const char *cs_tui_box_char(CsTuiBoxStyle style, int part);

/* Box parts */
enum {
    CS_TUI_BOX_HORIZ = 0,
    CS_TUI_BOX_VERT,
    CS_TUI_BOX_TOP_LEFT,
    CS_TUI_BOX_TOP_RIGHT,
    CS_TUI_BOX_BOTTOM_LEFT,
    CS_TUI_BOX_BOTTOM_RIGHT,
    CS_TUI_BOX_TEE_LEFT,
    CS_TUI_BOX_TEE_RIGHT,
    CS_TUI_BOX_TEE_TOP,
    CS_TUI_BOX_TEE_BOTTOM,
    CS_TUI_BOX_CROSS
};

/* Clipping */
bool cs_tui_clip_point(CsTuiRenderer *r, int x, int y);
bool cs_tui_clip_rect(CsTuiRenderer *r, int *x, int *y, int *w, int *h);

/* UTF-8 helpers */
int cs_tui_utf8_encode(uint32_t codepoint, char *out);
int cs_tui_utf8_decode(const char *str, int len, uint32_t *out);

/* ANSI sequences */
#define CS_TUI_ESC "\x1b"
#define CS_TUI_CSI "\x1b["

/* Cursor movement */
#define CS_TUI_CURSOR_HOME     CS_TUI_CSI "H"
#define CS_TUI_CURSOR_POS(y,x) CS_TUI_CSI "%d;%dH", (y)+1, (x)+1
#define CS_TUI_CURSOR_HIDE     CS_TUI_CSI "?25l"
#define CS_TUI_CURSOR_SHOW     CS_TUI_CSI "?25h"

/* Screen control */
#define CS_TUI_CLEAR_SCREEN    CS_TUI_CSI "2J"
#define CS_TUI_ALT_SCREEN_ON   CS_TUI_CSI "?1049h"
#define CS_TUI_ALT_SCREEN_OFF  CS_TUI_CSI "?1049l"

/* Color */
#define CS_TUI_RESET           CS_TUI_CSI "0m"
#define CS_TUI_FG_256          CS_TUI_CSI "38;5;%dm"
#define CS_TUI_BG_256          CS_TUI_CSI "48;5;%dm"
#define CS_TUI_FG_TRUE         CS_TUI_CSI "38;2;%d;%d;%dm"
#define CS_TUI_BG_TRUE         CS_TUI_CSI "48;2;%d;%d;%dm"

#endif /* CS_TUI_INTERNAL_H */
