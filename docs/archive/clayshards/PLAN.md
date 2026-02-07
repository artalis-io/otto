# ClayShards TUI Implementation Plan

## Overview

Text UI renderer for ClayShards that processes Clay render commands and outputs ANSI escape sequences. Follows the same architecture as `clay-shards-webgl` but targets modern terminal emulators.

## Architecture

```
┌─────────────────────────────────────────┐
│          Application                    │
│   wasm.app_frame() → render commands    │
├─────────────────────────────────────────┤
│         ClayShards TUI Renderer         │
│  • cs_tui_render_commands()             │
│  • Box-drawing characters               │
│  • ANSI color sequences                 │
│  • Differential updates (dirty regions) │
├─────────────────────────────────────────┤
│           Terminal Output               │
│  • stdout with ANSI escape codes        │
│  • Optional raw mode for input          │
└─────────────────────────────────────────┘
```

## Target Terminals

| Platform | Terminal | Minimum Version |
|----------|----------|-----------------|
| macOS | Terminal.app, iTerm2, Alacritty | Built-in |
| Linux | gnome-terminal, xterm, Alacritty | Any modern |
| Windows | WSL2 (recommended), Windows Terminal | WSL2 or WT 1.0+ |

**Capabilities required:**
- ANSI escape sequences (CSI)
- 256-color or true color (24-bit)
- UTF-8 support (box-drawing, Braille for map)
- Alternate screen buffer (optional, for clean exit)

## Phase 1: Core Renderer (2-3 days)

### Files to Create

```
clayshards/clay-shards-tui/
├── include/
│   └── cs_tui.h           # Public API
├── src/
│   ├── cs_tui.c           # Main renderer
│   ├── cs_tui_buffer.c    # Character buffer management
│   ├── cs_tui_color.c     # Color conversion (Clay_Color → ANSI)
│   ├── cs_tui_box.c       # Box-drawing character selection
│   └── cs_tui_internal.h  # Internal state
├── tests/
│   └── test_tui.c         # Unit tests
├── Makefile
└── CLAUDE.md
```

### API Design

```c
/* cs_tui.h - TUI renderer API */

#ifndef CS_TUI_H
#define CS_TUI_H

#include "clay.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Color modes */
typedef enum {
    CS_TUI_COLOR_AUTO = 0, /* Auto-detect: try true color, fallback to 256 */
    CS_TUI_COLOR_16,       /* Basic 16 colors (legacy) */
    CS_TUI_COLOR_256,      /* 256-color palette */
    CS_TUI_COLOR_TRUE      /* 24-bit true color */
} CsTuiColorMode;

/* Box-drawing style */
typedef enum {
    CS_TUI_BOX_ASCII,     /* +--+, |, corners */
    CS_TUI_BOX_LIGHT,     /* ─│┐┘└┌ thin lines */
    CS_TUI_BOX_HEAVY,     /* ━┃┓┛┗┏ thick lines */
    CS_TUI_BOX_DOUBLE,    /* ═║╗╝╚╔ double lines */
    CS_TUI_BOX_ROUNDED    /* ─│╮╯╰╭ rounded corners */
} CsTuiBoxStyle;

/* Renderer configuration */
typedef struct {
    int width;                 /* Terminal width in columns (0 = auto-detect) */
    int height;                /* Terminal height in rows (0 = auto-detect) */
    CsTuiColorMode color_mode; /* Color depth */
    CsTuiBoxStyle box_style;   /* Box-drawing character set */
    bool alternate_screen;     /* Use alternate screen buffer */
    bool hide_cursor;          /* Hide cursor during rendering */
    bool differential;         /* Only update changed regions */
} CsTuiConfig;

/* Opaque renderer context */
typedef struct CsTuiRenderer CsTuiRenderer;

/* Initialize default config */
void cs_tui_config_init(CsTuiConfig *config);

/* Create/destroy renderer */
CsTuiRenderer *cs_tui_create(const CsTuiConfig *config);
void cs_tui_free(CsTuiRenderer *r);

/* Resize handling */
void cs_tui_resize(CsTuiRenderer *r, int width, int height);
void cs_tui_get_size(CsTuiRenderer *r, int *width, int *height);

/* Render Clay commands to terminal */
void cs_tui_begin(CsTuiRenderer *r);
void cs_tui_render_commands(CsTuiRenderer *r,
                            Clay_RenderCommandArray commands);
void cs_tui_end(CsTuiRenderer *r);

/* Clear screen */
void cs_tui_clear(CsTuiRenderer *r, Clay_Color color);

/* Cursor management (for focused inputs) */
void cs_tui_set_cursor(CsTuiRenderer *r, int x, int y, bool visible);

/* Flush output buffer to stdout */
void cs_tui_flush(CsTuiRenderer *r);

/* Query if we're in a terminal (vs redirected) */
bool cs_tui_is_tty(void);

/* Terminal capability detection */
CsTuiColorMode cs_tui_detect_color_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* CS_TUI_H */
```

### Internal Buffer Structure

```c
/* cs_tui_internal.h */

/* Single cell in the character buffer */
typedef struct {
    uint32_t codepoint;    /* Unicode codepoint (0 = empty) */
    uint8_t fg_r, fg_g, fg_b;
    uint8_t bg_r, bg_g, bg_b;
    uint8_t flags;         /* Bold, underline, etc. */
} CsTuiCell;

/* Double-buffered screen */
typedef struct {
    CsTuiCell *front;      /* Currently displayed */
    CsTuiCell *back;       /* Being rendered to */
    int width;
    int height;
} CsTuiBuffer;

struct CsTuiRenderer {
    CsTuiConfig config;
    CsTuiBuffer buffer;

    /* Scissor stack */
    struct {
        int x, y, w, h;
    } scissor_stack[16];
    int scissor_depth;

    /* Output buffer (avoid many write() calls) */
    char *output_buf;
    size_t output_len;
    size_t output_cap;

    /* Cursor state */
    int cursor_x, cursor_y;
    bool cursor_visible;
};
```

### Render Command Mapping

| Clay Command | TUI Rendering |
|--------------|---------------|
| `RECTANGLE` | Fill region with bg color, draw box border if cornerRadius > 0 |
| `TEXT` | Write text at position with fg color |
| `BORDER` | Draw box-drawing characters for border |
| `IMAGE` | Show placeholder "[IMAGE]" or use Braille/ASCII art |
| `SCISSOR_START` | Push clip region (clamp cell writes) |
| `SCISSOR_END` | Pop clip region |
| `CUSTOM` | Dispatch to custom handler (e.g., map component) |

### Color Conversion

```c
/* Clay_Color (RGBA 0-255) → ANSI escape sequence */

/* True color (24-bit) */
#define ANSI_FG_TRUE "\x1b[38;2;%d;%d;%dm"
#define ANSI_BG_TRUE "\x1b[48;2;%d;%d;%dm"

/* 256-color palette */
static int rgb_to_256(uint8_t r, uint8_t g, uint8_t b) {
    /* 216 color cube: 6x6x6 */
    if (r == g && g == b) {
        /* Grayscale ramp: 232-255 */
        return 232 + (r * 23 / 255);
    }
    int ri = (r * 5) / 255;
    int gi = (g * 5) / 255;
    int bi = (b * 5) / 255;
    return 16 + 36 * ri + 6 * gi + bi;
}

/* Auto-detection: true color with 256-color fallback */
CsTuiColorMode cs_tui_detect_color_mode(void) {
    /* Check COLORTERM for true color support */
    const char *colorterm = getenv("COLORTERM");
    if (colorterm) {
        if (strcmp(colorterm, "truecolor") == 0 ||
            strcmp(colorterm, "24bit") == 0) {
            return CS_TUI_COLOR_TRUE;
        }
    }

    /* Check TERM for common true-color terminals */
    const char *term = getenv("TERM");
    if (term) {
        /* iTerm2, Alacritty, kitty, etc. */
        if (strstr(term, "256color") || strstr(term, "truecolor") ||
            strstr(term, "alacritty") || strstr(term, "kitty")) {
            return CS_TUI_COLOR_TRUE;
        }
    }

    /* Default fallback: 256-color (widely supported) */
    return CS_TUI_COLOR_256;
}
```

### Box-Drawing Character Selection

```c
/* Corner radius → box style */
/* cornerRadius == 0: no border */
/* cornerRadius > 0: use CS_TUI_BOX_ROUNDED or configured style */

static const char *box_light[] = {
    "─", "│", "┌", "┐", "└", "┘", "├", "┤", "┬", "┴", "┼"
};

static const char *box_rounded[] = {
    "─", "│", "╭", "╮", "╰", "╯", "├", "┤", "┬", "┴", "┼"
};

/* Select corner based on position */
typedef enum {
    BOX_TOP_LEFT, BOX_TOP_RIGHT, BOX_BOTTOM_LEFT, BOX_BOTTOM_RIGHT,
    BOX_HORIZONTAL, BOX_VERTICAL
} BoxPart;
```

### Update Triggers

**Full redraw triggers:**
- Terminal resize (SIGWINCH)
- First render
- `cs_tui_clear()` called

**Differential update triggers:**
- Normal frame (compare front/back buffers)
- Cursor position change

**Detection:**
```c
/* In cs_tui_end(), compare buffers and emit only changed cells */
void cs_tui_flush_differential(CsTuiRenderer *r) {
    for (int y = 0; y < r->buffer.height; y++) {
        for (int x = 0; x < r->buffer.width; x++) {
            CsTuiCell *front = &r->buffer.front[y * r->buffer.width + x];
            CsTuiCell *back = &r->buffer.back[y * r->buffer.width + x];

            if (memcmp(front, back, sizeof(CsTuiCell)) != 0) {
                /* Move cursor and emit cell */
                cs_tui_emit_cell(r, x, y, back);
                *front = *back;
            }
        }
    }
}
```

## Phase 2: Widget Tests (1-2 days)

### Test Approach

Widget tests verify that components render correctly to the character buffer. Tests run without a real terminal by capturing the buffer state.

```c
/* test_tui.c */

#include "cs_tui.h"
#include "../clay-shards/include/cs_immediate.h"
#include "../../vendor/clay/clay.h"

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)
#define ASSERT(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

/* Test helper: read cell at position */
static CsTuiCell *get_cell(CsTuiRenderer *r, int x, int y);

/* Test helper: check if region contains expected text */
static bool buffer_contains_text(CsTuiRenderer *r,
                                  int x, int y, const char *expected);

/* Test helper: check cell background color */
static bool cell_has_bg(CsTuiCell *c, uint8_t r, uint8_t g, uint8_t b);
```

### Widget Test Cases

#### Button Tests
```c
static void test_button_renders_label(void) {
    TEST(button_renders_label);

    CsTuiRenderer *r = cs_tui_create(&test_config);

    /* Run one Clay frame with button */
    Clay_BeginLayout();
    cs_frame_begin();

    cs_button(CS_ID("test_btn"), "Click Me", NULL);

    Clay_RenderCommandArray cmds = Clay_EndLayout();
    cs_frame_end(0.016f);

    cs_tui_begin(r);
    cs_tui_render_commands(r, cmds);
    cs_tui_end(r);

    /* Verify button label is in buffer */
    ASSERT(buffer_contains_text(r, 0, 0, "Click Me"),
           "Button label not found");

    cs_tui_free(r);
    PASS();
}

static void test_button_hover_changes_color(void) {
    TEST(button_hover_changes_color);

    /* Set mouse over button position */
    /* Render, check background color differs from non-hovered */

    PASS();
}
```

#### Input Tests
```c
static void test_input_shows_text(void) {
    TEST(input_shows_text);

    char buf[64] = "Hello";
    int len = 5;

    /* Render input with text */
    /* Verify "Hello" appears in buffer */

    PASS();
}

static void test_input_shows_placeholder(void) {
    TEST(input_shows_placeholder);

    char buf[64] = "";
    int len = 0;

    /* Render input with placeholder */
    /* Verify placeholder text appears (possibly dimmed) */

    PASS();
}

static void test_input_cursor_position(void) {
    TEST(input_cursor_position);

    /* Focus input, verify cursor renders at correct position */

    PASS();
}
```

#### Checkbox/Toggle Tests
```c
static void test_checkbox_unchecked(void) {
    TEST(checkbox_unchecked);

    /* Render unchecked checkbox */
    /* Verify "[ ]" or "☐" appears */

    PASS();
}

static void test_checkbox_checked(void) {
    TEST(checkbox_checked);

    /* Render checked checkbox */
    /* Verify "[x]" or "☑" appears */

    PASS();
}

static void test_toggle_off(void) {
    TEST(toggle_off);

    /* Render off toggle */
    /* Verify "○──" or similar */

    PASS();
}

static void test_toggle_on(void) {
    TEST(toggle_on);

    /* Render on toggle */
    /* Verify "──●" or similar */

    PASS();
}
```

#### Slider Tests
```c
static void test_slider_renders_track(void) {
    TEST(slider_renders_track);

    /* Verify track characters appear */

    PASS();
}

static void test_slider_handle_position(void) {
    TEST(slider_handle_position);

    /* Set value to 0.5, verify handle at center */

    PASS();
}
```

#### Dropdown Tests
```c
static void test_dropdown_closed(void) {
    TEST(dropdown_closed);

    /* Verify selected item and "▼" indicator */

    PASS();
}

static void test_dropdown_open(void) {
    TEST(dropdown_open);

    /* Open dropdown, verify all options visible */

    PASS();
}
```

#### Scroll Tests
```c
static void test_scroll_clips_content(void) {
    TEST(scroll_clips_content);

    /* Add more content than visible */
    /* Verify only visible portion renders */

    PASS();
}

static void test_scroll_scrollbar_visible(void) {
    TEST(scroll_scrollbar_visible);

    /* Verify scrollbar indicator appears */

    PASS();
}
```

### Test Infrastructure

```c
/* Minimal test config for headless testing */
static CsTuiConfig test_config = {
    .width = 80,
    .height = 24,
    .color_mode = CS_TUI_COLOR_256,
    .box_style = CS_TUI_BOX_LIGHT,
    .alternate_screen = false,
    .hide_cursor = false,
    .differential = false  /* Always full render for tests */
};

static int tests_run = 0;
static int tests_passed = 0;

int main(void) {
    printf("ClayShards TUI Tests\n\n");

    /* Initialize Clay and ClayShards */
    uint64_t mem_size = Clay_MinMemorySize();
    void *mem = malloc(mem_size);
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(mem_size, mem);
    Clay_Initialize(arena, (Clay_Dimensions){80, 24}, (Clay_ErrorHandler){0});
    cs_init();

    /* Run tests */
    printf("Button tests:\n");
    test_button_renders_label();
    test_button_hover_changes_color();

    printf("\nInput tests:\n");
    test_input_shows_text();
    test_input_shows_placeholder();
    test_input_cursor_position();

    printf("\nCheckbox/Toggle tests:\n");
    test_checkbox_unchecked();
    test_checkbox_checked();
    test_toggle_off();
    test_toggle_on();

    printf("\nSlider tests:\n");
    test_slider_renders_track();
    test_slider_handle_position();

    printf("\nDropdown tests:\n");
    test_dropdown_closed();
    test_dropdown_open();

    printf("\nScroll tests:\n");
    test_scroll_clips_content();
    test_scroll_scrollbar_visible();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);

    free(mem);
    return (tests_passed == tests_run) ? 0 : 1;
}
```

## Phase 3: Input Handling (1-2 days)

**Note:** Mouse support is deferred to a future phase. The input architecture is designed
to be modular so mouse handling can be added later without breaking changes.

### Raw Terminal Mode

```c
/* cs_tui_input.h */

#include <termios.h>

typedef struct {
    struct termios original;
    bool raw_mode;
    /* Future: mouse state will be added here */
} CsTuiInput;

/* Enable raw mode for immediate key detection */
void cs_tui_input_enable_raw(CsTuiInput *input);

/* Restore original terminal settings */
void cs_tui_input_restore(CsTuiInput *input);

/* Non-blocking key read (returns 0 if no key) */
int cs_tui_input_read_key(CsTuiInput *input);

/* Parse escape sequences to key codes */
typedef enum {
    CT_KEY_NONE = 0,
    CT_KEY_UP, CT_KEY_DOWN, CT_KEY_LEFT, CT_KEY_RIGHT,
    CT_KEY_HOME, CT_KEY_END,
    CT_KEY_TAB, CT_KEY_SHIFT_TAB,
    CT_KEY_ENTER, CT_KEY_ESCAPE,
    CT_KEY_BACKSPACE, CT_KEY_DELETE,
    CT_KEY_CHAR  /* Regular character in codepoint field */
} CsTuiKeyType;

typedef struct {
    CsTuiKeyType type;
    uint32_t codepoint;  /* For CT_KEY_CHAR */
    bool shift, ctrl, alt;
} CsTuiKeyEvent;

/* Parse raw input to key event */
bool cs_tui_parse_key(const char *seq, int len, CsTuiKeyEvent *out);
```

### Mouse Support (Deferred - Future Phase)

Architecture placeholder for future mouse support. The input module is designed
to accommodate this without breaking changes.

```c
/* Future: Enable terminal mouse reporting (SGR 1006 mode) */
void cs_tui_input_enable_mouse(CsTuiInput *input);
void cs_tui_input_disable_mouse(CsTuiInput *input);

typedef struct {
    int x, y;                    /* Cell coordinates */
    bool button1, button2, button3;
    bool drag;
    bool release;
} CsTuiMouseEvent;

/* Future: Parse mouse escape sequence */
bool cs_tui_parse_mouse(const char *seq, int len, CsTuiMouseEvent *out);
```

**Implementation notes for future:**
- Use SGR 1006 extended mouse mode for coordinates > 223
- Support drag events for slider/scroll components
- Map terminal cell coordinates to Clay coordinate space

## Phase 4: Map Component Integration (Separate Phase)

### Design

The map component will use Carta's ASCII renderer (`ct_ascii.h`) for tile rendering.

```c
/* Custom render handler for CLAY_RENDER_COMMAND_TYPE_CUSTOM */

typedef struct {
    double lat, lon;
    int zoom;
    int width, height;  /* In characters */
} CsTuiMapState;

void cs_tui_render_map(CsTuiRenderer *r,
                       Clay_BoundingBox bounds,
                       CsTuiMapState *state);
```

### Map Rendering Configuration

```c
/* Map-specific rendering options */
typedef struct {
    CTAsciiCharset charset;      /* Default: CT_ASCII_BRAILLE */
    CTAsciiCharset fallback;     /* Default: CT_ASCII_SIMPLE (ASCII-only) */
    bool auto_fallback;          /* Auto-detect Unicode support */
    bool color;                  /* Use ANSI colors for features */
} CsTuiMapConfig;

void cs_tui_map_config_init(CsTuiMapConfig *config) {
    config->charset = CT_ASCII_BRAILLE;    /* Max detail: 2x4 dots per char */
    config->fallback = CT_ASCII_SIMPLE;    /* ASCII-only fallback */
    config->auto_fallback = true;          /* Detect Unicode support */
    config->color = true;                  /* Color-coded features */
}
```

### Implementation Notes

1. **Tile fetching**: Use existing `cs_map` provider to get tile data
2. **Rendering**: Convert each visible tile to ASCII using `ct_render_ascii()`
3. **Character sets**:
   - `CT_ASCII_BRAILLE` for maximum detail (2x4 dots per character) - default
   - `CT_ASCII_BLOCKS` for Unicode without Braille
   - `CT_ASCII_SIMPLE` for pure ASCII fallback
4. **Auto-fallback**: Detect if terminal supports Unicode, fall back to ASCII if not
5. **Colors**: Use ANSI colors for land/water/road distinction
6. **Labels**: Overlay city/feature names on map

### Phase 4 Files

```
clayshards/clay-shards-tui/src/
├── cs_tui_map.c        # Map renderer using Carta
└── cs_tui_map.h        # Map component API
```

## Memory Management

Following `/c-audit` patterns:

### Ownership

```c
/* cs_tui_create() returns owned pointer */
CsTuiRenderer *r = cs_tui_create(&config);
/* ... use renderer ... */
cs_tui_free(r);  /* Caller must free */
```

### Buffer Allocation

```c
/* Use calloc for overflow-safe allocation */
buffer->front = calloc(width * height, sizeof(CsTuiCell));
if (!buffer->front) {
    return CS_TUI_ERR_ALLOC;
}
```

### String Safety

```c
/* Use bounded string operations */
size_t cs_tui_emit_text(CsTuiRenderer *r, const char *text, size_t len) {
    /* Don't trust text to be null-terminated */
    for (size_t i = 0; i < len; i++) {
        /* Process each byte */
    }
}
```

## Error Handling

```c
typedef enum {
    CS_TUI_OK = 0,
    CS_TUI_ERR_NULL,
    CS_TUI_ERR_ALLOC,
    CS_TUI_ERR_NOT_TTY,
    CS_TUI_ERR_BOUNDS
} CsTuiError;
```

## Build Integration

### Makefile

```makefile
# clayshards/clay-shards-tui/Makefile

CC ?= cc
CFLAGS = -std=c11 -Wall -Wextra -Werror -O2
CFLAGS += -I./include -I../clay-shards/include -I../../vendor/clay

SRCS = src/cs_tui.c src/cs_tui_buffer.c src/cs_tui_color.c src/cs_tui_box.c
OBJS = $(SRCS:.c=.o)
LIB = libclaytui.a

all: $(LIB) test

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

test: tests/test_tui.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lclaytui -L../clay-shards -lclayshards

clean:
	rm -f $(OBJS) $(LIB) test

.PHONY: all test clean
```

### Root Makefile Integration

```makefile
# Add to otto/Makefile

clay-shards-tui:
	$(MAKE) -C clayshards/clay-shards-tui

test-clay-shards-tui:
	$(MAKE) -C clayshards/clay-shards-tui test
	./clayshards/clay-shards-tui/test
```

## Timeline Summary

| Phase | Duration | Deliverables |
|-------|----------|--------------|
| 1. Core Renderer | 2-3 days | `cs_tui.h`, buffer management, true color + 256 fallback, configurable box drawing |
| 2. Widget Tests | 1-2 days | 15-20 tests covering all ClayShards widgets |
| 3. Input Handling | 1-2 days | Raw mode, key parsing (mouse deferred, architecture ready) |
| 4. Map Component | Separate | Carta ASCII integration, Braille default + ASCII fallback |

**Total Phase 1-3**: 4-7 days

### What's Configurable

| Setting | Options | Default |
|---------|---------|---------|
| Color mode | AUTO, 16, 256, TRUE | AUTO (true color → 256 fallback) |
| Box style | ASCII, LIGHT, HEAVY, DOUBLE, ROUNDED | LIGHT |
| Map charset | BRAILLE, BLOCKS, EXTENDED, SIMPLE | BRAILLE (auto-fallback to SIMPLE) |
| Differential updates | on/off | on |
| Alternate screen | on/off | off |

## Design Decisions

1. **Box-drawing**: Fully configurable via `CsTuiBoxStyle`. Default: `CS_TUI_BOX_LIGHT`
2. **Color mode**: Configurable. Default: true color with automatic 256-color fallback
3. **Mouse support**: Deferred to future phase. Architecture keeps input handling modular for easy addition
4. **Map detail**: Braille (`CT_ASCII_BRAILLE`) with ASCII fallback. Configurable via `CTAsciiCharset`

## References

- [Clay Layout Library](../../vendor/clay/CLAUDE.md)
- [ClayShards Components](../clay-shards/CLAUDE.md)
- [ClayShards WebGL Renderer](../clay-shards-webgl/CLAUDE.md)
- [ClayShards Manifesto](../clay-shards/MANIFESTO.md)
- [Carta ASCII Renderer](../../carta/include/ct_ascii.h)
- [ANSI Escape Codes](https://en.wikipedia.org/wiki/ANSI_escape_code)
