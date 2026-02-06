/**
 * cs_tui.h - ClayShards TUI Renderer
 *
 * Text UI renderer that processes Clay render commands and outputs
 * ANSI escape sequences for modern terminal emulators.
 */

#ifndef CS_TUI_H
#define CS_TUI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

/* Color modes */
typedef enum {
    CS_TUI_COLOR_AUTO = 0,  /* Auto-detect: try true color, fallback to 256 */
    CS_TUI_COLOR_16,        /* Basic 16 colors (legacy) */
    CS_TUI_COLOR_256,       /* 256-color palette */
    CS_TUI_COLOR_TRUE       /* 24-bit true color */
} CsTuiColorMode;

/* Box-drawing character style */
typedef enum {
    CS_TUI_BOX_ASCII = 0,   /* +--+, |  (ASCII only) */
    CS_TUI_BOX_LIGHT,       /* ─│┐┘└┌ (thin lines, default) */
    CS_TUI_BOX_HEAVY,       /* ━┃┓┛┗┏ (thick lines) */
    CS_TUI_BOX_DOUBLE,      /* ═║╗╝╚╔ (double lines) */
    CS_TUI_BOX_ROUNDED      /* ─│╮╯╰╭ (rounded corners) */
} CsTuiBoxStyle;

/* RGBA color (same as Clay_Color for compatibility) */
typedef struct {
    uint8_t r, g, b, a;
} CsTuiColor;

/* Renderer configuration */
typedef struct {
    int width;                  /* Terminal width in columns (0 = auto-detect) */
    int height;                 /* Terminal height in rows (0 = auto-detect) */
    CsTuiColorMode color_mode;  /* Color depth */
    CsTuiBoxStyle box_style;    /* Box-drawing character set */
    bool alternate_screen;      /* Use alternate screen buffer */
    bool hide_cursor;           /* Hide cursor during rendering */
    bool differential;          /* Only update changed regions */
    bool headless;              /* Headless mode: no terminal I/O, use dump_buffer() */
} CsTuiConfig;

/* Error codes */
typedef enum {
    CS_TUI_OK = 0,
    CS_TUI_ERR_NULL,
    CS_TUI_ERR_ALLOC,
    CS_TUI_ERR_NOT_TTY,
    CS_TUI_ERR_BOUNDS
} CsTuiError;

/* Opaque renderer context */
typedef struct CsTuiRenderer CsTuiRenderer;

/* ============================================================================
 * Configuration
 * ============================================================================ */

/**
 * Initialize config with sensible defaults.
 */
void cs_tui_config_init(CsTuiConfig *config);

/**
 * Detect terminal color capability.
 * Checks COLORTERM and TERM environment variables.
 */
CsTuiColorMode cs_tui_detect_color_mode(void);

/**
 * Check if stdout is a terminal (not redirected).
 */
bool cs_tui_is_tty(void);

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/**
 * Create a TUI renderer.
 *
 * @param config Configuration (NULL for defaults)
 * @return Renderer instance, or NULL on error
 */
CsTuiRenderer *cs_tui_create(const CsTuiConfig *config);

/**
 * Free renderer and restore terminal state.
 */
void cs_tui_free(CsTuiRenderer *r);

/* ============================================================================
 * Size Management
 * ============================================================================ */

/**
 * Handle terminal resize.
 * Call this when SIGWINCH is received or dimensions change.
 *
 * @param width New width (0 = auto-detect)
 * @param height New height (0 = auto-detect)
 */
void cs_tui_resize(CsTuiRenderer *r, int width, int height);

/**
 * Get current terminal dimensions.
 */
void cs_tui_get_size(CsTuiRenderer *r, int *width, int *height);

/* ============================================================================
 * Rendering
 * ============================================================================ */

/**
 * Begin a new frame.
 * Clears the back buffer.
 */
void cs_tui_begin(CsTuiRenderer *r);

/**
 * Clear with a background color.
 */
void cs_tui_clear(CsTuiRenderer *r, CsTuiColor color);

/**
 * Render a filled rectangle.
 *
 * @param x Column position
 * @param y Row position
 * @param w Width in columns
 * @param h Height in rows
 * @param bg Background color
 * @param border_color Border color (NULL for no border)
 * @param corner_radius Corner radius (0 for square, >0 for rounded)
 */
void cs_tui_rect(CsTuiRenderer *r, int x, int y, int w, int h,
                 CsTuiColor bg, const CsTuiColor *border_color, int corner_radius);

/**
 * Render text.
 *
 * @param x Column position
 * @param y Row position
 * @param text UTF-8 text string
 * @param len Text length in bytes (-1 for strlen)
 * @param fg Foreground color
 * @param bg Background color (NULL for transparent)
 */
void cs_tui_text(CsTuiRenderer *r, int x, int y, const char *text, int len,
                 CsTuiColor fg, const CsTuiColor *bg);

/**
 * Render a border (outline only, no fill).
 */
void cs_tui_border(CsTuiRenderer *r, int x, int y, int w, int h,
                   CsTuiColor color, int corner_radius);

/**
 * Push a scissor (clip) region.
 * All subsequent drawing will be clipped to this region.
 */
void cs_tui_scissor_push(CsTuiRenderer *r, int x, int y, int w, int h);

/**
 * Pop the current scissor region.
 */
void cs_tui_scissor_pop(CsTuiRenderer *r);

/**
 * End frame and flush to terminal.
 * Performs differential update if enabled.
 */
void cs_tui_end(CsTuiRenderer *r);

/**
 * Force full redraw (disables differential for this frame).
 */
void cs_tui_invalidate(CsTuiRenderer *r);

/* ============================================================================
 * Cursor
 * ============================================================================ */

/**
 * Set cursor position and visibility.
 * Use for text input cursor rendering.
 */
void cs_tui_set_cursor(CsTuiRenderer *r, int x, int y, bool visible);

/* ============================================================================
 * Clay Integration
 * ============================================================================ */

/**
 * Render Clay render commands.
 *
 * This is the main entry point for Clay UI rendering.
 * Processes all commands and maps them to TUI primitives.
 *
 * @param r Renderer
 * @param commands Clay render command array pointer
 * @param count Number of commands
 *
 * Note: For native C usage, pass Clay_RenderCommandArray.internalArray
 * and Clay_RenderCommandArray.length.
 */
void cs_tui_render_clay_commands(CsTuiRenderer *r, const void *commands, int count);

/* ============================================================================
 * Debug / Testing
 * ============================================================================ */

/**
 * Get the character at a cell position (for testing).
 * Returns 0 if out of bounds.
 */
uint32_t cs_tui_get_cell_char(CsTuiRenderer *r, int x, int y);

/**
 * Get the foreground color at a cell position (for testing).
 */
CsTuiColor cs_tui_get_cell_fg(CsTuiRenderer *r, int x, int y);

/**
 * Get the background color at a cell position (for testing).
 */
CsTuiColor cs_tui_get_cell_bg(CsTuiRenderer *r, int x, int y);

/**
 * Check if a text string exists at position (for testing).
 */
bool cs_tui_buffer_contains(CsTuiRenderer *r, int x, int y, const char *text);

/**
 * Dump buffer to a string (for testing/debugging).
 * Returns allocated string that caller must free.
 */
char *cs_tui_dump_buffer(CsTuiRenderer *r);

#ifdef __cplusplus
}
#endif

#endif /* CS_TUI_H */
