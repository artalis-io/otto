/**
 * cs_soft.h - ClayShards Software Renderer
 *
 * Software framebuffer renderer for Clay UI with platform backends.
 * Renders Clay commands to an RGBA pixel buffer using CPU-based rasterization.
 *
 * Features:
 *   - SIMD-optimized span fill (AVX2/SSE2/NEON)
 *   - MSDF text rendering via sh_font
 *   - Rounded rectangles and borders
 *   - Platform backends for window display
 *
 * Supported platforms:
 *   - Headless (testing, offscreen rendering)
 *   - X11 (via XShm)
 *   - Win32 (via DIBSection)
 *   - Wayland (via wl_shm) [planned]
 *
 * Usage:
 *   CsSoftConfig config;
 *   cs_soft_config_init(&config);
 *   config.width = 800;
 *   config.height = 600;
 *
 *   CsSoftRenderer *r = cs_soft_create(&config);
 *
 *   while (!cs_soft_should_close(r)) {
 *       CsSoftEvent event;
 *       while (cs_soft_poll_event(r, &event)) {
 *           // Handle events, route to ClayShards
 *       }
 *
 *       cs_frame_begin();
 *       Clay_BeginLayout();
 *       // ... declare UI ...
 *       Clay_RenderCommandArray commands = Clay_EndLayout();
 *       cs_frame_end(dt);
 *
 *       cs_soft_begin(r);
 *       cs_soft_clear(r, 0xFF1E1E1E);  // Dark gray
 *       cs_soft_render_clay_commands(r, commands.length);
 *       cs_soft_end(r);
 *   }
 *
 *   cs_soft_free(r);
 */

#ifndef CS_SOFT_H
#define CS_SOFT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Platform Selection
 * ============================================================================ */

typedef enum {
    CS_SOFT_PLATFORM_AUTO = 0,    /* Auto-detect best available */
    CS_SOFT_PLATFORM_HEADLESS,    /* No window, just framebuffer */
    CS_SOFT_PLATFORM_COCOA,       /* macOS Cocoa/NSWindow */
    CS_SOFT_PLATFORM_X11,         /* X11 with XShm */
    CS_SOFT_PLATFORM_WAYLAND,     /* Wayland wl_shm */
    CS_SOFT_PLATFORM_WIN32,       /* Win32 GDI/DIB */
    CS_SOFT_PLATFORM_FBDEV        /* Linux framebuffer (no input) */
} CsSoftPlatform;

/* ============================================================================
 * Configuration
 * ============================================================================ */

typedef struct {
    int width;                    /* Window width (0 = auto-detect) */
    int height;                   /* Window height (0 = auto-detect) */
    CsSoftPlatform platform;      /* Backend selection */
    bool double_buffer;           /* Use double buffering (default: true) */
    bool vsync;                   /* Enable VSync (default: true) */
    const char *title;            /* Window title (default: "ClayShards") */
} CsSoftConfig;

/**
 * Initialize configuration with default values.
 */
void cs_soft_config_init(CsSoftConfig *config);

/* ============================================================================
 * Event Types
 * ============================================================================ */

typedef enum {
    CS_SOFT_EVENT_NONE = 0,
    CS_SOFT_EVENT_KEY_DOWN,
    CS_SOFT_EVENT_KEY_UP,
    CS_SOFT_EVENT_CHAR,
    CS_SOFT_EVENT_MOUSE_MOVE,
    CS_SOFT_EVENT_MOUSE_DOWN,
    CS_SOFT_EVENT_MOUSE_UP,
    CS_SOFT_EVENT_SCROLL,
    CS_SOFT_EVENT_RESIZE,
    CS_SOFT_EVENT_CLOSE
} CsSoftEventType;

/* Key codes (subset of common keys) */
typedef enum {
    CS_KEY_UNKNOWN = 0,
    CS_KEY_BACKSPACE = 8,
    CS_KEY_TAB = 9,
    CS_KEY_ENTER = 13,
    CS_KEY_ESCAPE = 27,
    CS_KEY_SPACE = 32,
    CS_KEY_LEFT = 37,
    CS_KEY_UP = 38,
    CS_KEY_RIGHT = 39,
    CS_KEY_DOWN = 40,
    CS_KEY_DELETE = 46,
    CS_KEY_HOME = 36,
    CS_KEY_END = 35,
    CS_KEY_PAGE_UP = 33,
    CS_KEY_PAGE_DOWN = 34,
    CS_KEY_A = 65,
    CS_KEY_C = 67,
    CS_KEY_V = 86,
    CS_KEY_X = 88,
    CS_KEY_Z = 90
} CsKeyCode;

typedef struct {
    CsSoftEventType type;
    union {
        struct {
            int key_code;
            bool shift;
            bool ctrl;
            bool alt;
        } key;
        struct {
            uint32_t codepoint;
        } char_input;
        struct {
            int x, y;
            int button;       /* 0=left, 1=middle, 2=right */
        } mouse;
        struct {
            float delta_x;
            float delta_y;
        } scroll;
        struct {
            int width;
            int height;
        } resize;
    };
} CsSoftEvent;

/* ============================================================================
 * Renderer
 * ============================================================================ */

typedef struct CsSoftRenderer CsSoftRenderer;

/**
 * Create a software renderer with the given configuration.
 *
 * @param config Configuration (NULL = defaults)
 * @return Renderer, or NULL on failure
 */
CsSoftRenderer *cs_soft_create(const CsSoftConfig *config);

/**
 * Free a software renderer and its resources.
 */
void cs_soft_free(CsSoftRenderer *r);

/**
 * Get the renderer's framebuffer dimensions.
 */
void cs_soft_get_size(CsSoftRenderer *r, int *width, int *height);

/**
 * Resize the renderer's framebuffer.
 * Called automatically on window resize events.
 *
 * @return true if resize succeeded
 */
bool cs_soft_resize(CsSoftRenderer *r, int width, int height);

/* ============================================================================
 * Event Handling
 * ============================================================================ */

/**
 * Poll for the next event.
 *
 * @param r Renderer
 * @param event Output event structure
 * @return true if event was returned, false if no events pending
 */
bool cs_soft_poll_event(CsSoftRenderer *r, CsSoftEvent *event);

/**
 * Check if the window should close.
 */
bool cs_soft_should_close(CsSoftRenderer *r);

/**
 * Request the window to close.
 */
void cs_soft_request_close(CsSoftRenderer *r);

/* ============================================================================
 * Rendering
 * ============================================================================ */

/**
 * Begin a new frame.
 * Clears internal state but does not clear the framebuffer.
 */
void cs_soft_begin(CsSoftRenderer *r);

/**
 * Clear the framebuffer to a solid color.
 *
 * @param color Packed RGBA color (r<<24 | g<<16 | b<<8 | a)
 */
void cs_soft_clear(CsSoftRenderer *r, uint32_t color);

/**
 * Render Clay render commands.
 *
 * Iterates through the provided render command array and draws each command.
 * The commands array should be the result of Clay_EndLayout().
 *
 * @param r Renderer
 * @param commands Pointer to Clay_RenderCommandArray from Clay_EndLayout()
 *
 * Note: This function is declared here but requires clay.h to be included
 * before using it. The actual prototype uses void* for the commands parameter
 * to avoid requiring clay.h in this header.
 */
void cs_soft_render_clay_commands(CsSoftRenderer *r, void *commands);

/**
 * End the current frame and present to screen.
 * For double-buffered mode, this swaps buffers.
 */
void cs_soft_end(CsSoftRenderer *r);

/* ============================================================================
 * Direct Primitives (optional, for non-Clay usage)
 * ============================================================================ */

/**
 * Draw a filled rectangle.
 *
 * @param r Renderer
 * @param x X coordinate
 * @param y Y coordinate
 * @param w Width
 * @param h Height
 * @param color Fill color (RGBA)
 * @param radius Corner radius (0 = sharp corners)
 */
void cs_soft_rect(CsSoftRenderer *r, float x, float y, float w, float h,
                  uint32_t color, float radius);

/**
 * Draw a rectangle border (uniform width).
 *
 * @param r Renderer
 * @param x X coordinate
 * @param y Y coordinate
 * @param w Width
 * @param h Height
 * @param color Border color
 * @param width Border width in pixels (all sides)
 */
void cs_soft_border(CsSoftRenderer *r, float x, float y, float w, float h,
                    uint32_t color, float width);

/**
 * Draw a rectangle border with individual side widths.
 *
 * @param r Renderer
 * @param x X coordinate
 * @param y Y coordinate
 * @param w Width
 * @param h Height
 * @param color Border color
 * @param top Top border width
 * @param right Right border width
 * @param bottom Bottom border width
 * @param left Left border width
 */
void cs_soft_border_sides(CsSoftRenderer *r, float x, float y, float w, float h,
                          uint32_t color, float top, float right, float bottom, float left);

/**
 * Draw text.
 *
 * @param r Renderer
 * @param text Text string
 * @param len Text length (-1 = null-terminated)
 * @param x X coordinate
 * @param y Y coordinate
 * @param size Font size in pixels
 * @param color Text color
 */
void cs_soft_text(CsSoftRenderer *r, const char *text, int len,
                  float x, float y, float size, uint32_t color);

/* ============================================================================
 * Buffer Access (for testing and headless mode)
 * ============================================================================ */

/**
 * Get direct access to the pixel buffer.
 *
 * @return Pointer to RGBA pixel data (row-major, 4 bytes per pixel)
 */
uint8_t *cs_soft_get_pixels(CsSoftRenderer *r);

/**
 * Get a pixel at the given coordinates.
 *
 * @return Packed RGBA color, or 0 if out of bounds
 */
uint32_t cs_soft_get_pixel(CsSoftRenderer *r, int x, int y);

#ifdef __cplusplus
}
#endif

#endif /* CS_SOFT_H */
