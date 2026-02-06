# ClayShards TUI

Text UI renderer for ClayShards and Clay UI applications in the terminal.

## Overview

Renders Clay's render commands to ANSI escape sequences for modern terminal emulators:
- Box-drawing characters for rectangles with rounded corners
- 256-color and true color support
- Text rendering with foreground/background colors
- Scissor clipping via cell-level bounds checking
- Differential updates (only redraw changed cells)
- ASCII/Braille map tiles via Carta integration (Phase 4)

## Architecture

```
┌─────────────────────────────────────┐
│          Application                │
│   renderer.render_commands()        │
├─────────────────────────────────────┤
│        CsTuiRenderer                │
│  • cs_tui_render_rect()             │
│  • cs_tui_render_text()             │
│  • cs_tui_render_border()           │
├─────────────────────────────────────┤
│          Terminal                   │
│  • ANSI escape sequences            │
│  • Box-drawing characters           │
└─────────────────────────────────────┘
```

## Files

| File | Purpose |
|------|---------|
| `include/cs_tui.h` | Public API |
| `src/cs_tui.c` | Main renderer implementation |
| `src/cs_tui_buffer.c` | Double-buffered character grid |
| `src/cs_tui_color.c` | Clay_Color → ANSI color conversion |
| `src/cs_tui_box.c` | Box-drawing character selection |
| `src/cs_tui_internal.h` | Internal types and macros |
| `tests/test_tui.c` | Unit tests |

## Usage

```c
#include "cs_tui.h"
#include "cs_immediate.h"

int main(void) {
    /* Initialize Clay and ClayShards */
    uint64_t mem_size = Clay_MinMemorySize();
    void *mem = malloc(mem_size);
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(mem_size, mem);
    Clay_Initialize(arena, (Clay_Dimensions){80, 24}, (Clay_ErrorHandler){0});
    cs_init();

    /* Create TUI renderer */
    CsTuiConfig config;
    cs_tui_config_init(&config);
    CsTuiRenderer *r = cs_tui_create(&config);

    /* Main loop */
    while (!quit) {
        /* Handle input */
        /* ... */

        /* Run ClayShards frame */
        cs_frame_begin();
        Clay_BeginLayout();

        /* Declare UI */
        CLAY(CLAY_ID("Root"), ...) {
            if (cs_button(CS_ID("btn"), "Click Me", NULL).clicked) {
                /* Handle click */
            }
        }

        Clay_RenderCommandArray commands = Clay_EndLayout();
        cs_frame_end(dt);

        /* Render to terminal */
        cs_tui_begin(r);
        cs_tui_render_commands(r, commands);
        cs_tui_end(r);
        cs_tui_flush(r);
    }

    cs_tui_free(r);
    free(mem);
    return 0;
}
```

## API

### CsTuiRenderer

```c
CsTuiConfig config;
cs_tui_config_init(&config);

CsTuiRenderer *r = cs_tui_create(&config);

cs_tui_resize(r, width, height);
cs_tui_get_size(r, &width, &height);

cs_tui_begin(r);
cs_tui_clear(r, (Clay_Color){30, 30, 30, 255});
cs_tui_render_commands(r, commands);
cs_tui_end(r);
cs_tui_flush(r);

cs_tui_free(r);
```

### Configuration

```c
typedef struct {
    int width;                 /* Terminal width (0 = auto) */
    int height;                /* Terminal height (0 = auto) */
    CsTuiColorMode color_mode; /* AUTO, COLOR_16, COLOR_256, COLOR_TRUE */
    CsTuiBoxStyle box_style;   /* ASCII, LIGHT, HEAVY, DOUBLE, ROUNDED */
    bool alternate_screen;     /* Use alternate screen buffer */
    bool hide_cursor;          /* Hide cursor during rendering */
    bool differential;         /* Only update changed cells */
} CsTuiConfig;
```

### Color Mode Auto-Detection

Default: `CS_TUI_COLOR_AUTO` - tries true color, falls back to 256-color.

Detection checks:
1. `COLORTERM=truecolor` or `COLORTERM=24bit` → true color
2. `TERM` contains `256color`, `alacritty`, `kitty` → true color
3. Otherwise → 256-color fallback

## Render Command Mapping

| Clay Command | TUI Rendering |
|--------------|---------------|
| `RECTANGLE` | Fill cells with bg color, box-drawing border |
| `TEXT` | Write characters with fg color |
| `BORDER` | Box-drawing characters |
| `IMAGE` | Placeholder "[IMAGE]" or ASCII art |
| `SCISSOR_START` | Push clip region |
| `SCISSOR_END` | Pop clip region |
| `CUSTOM` | Dispatch to custom handler |

## Box-Drawing Styles

```
ASCII:    +--+   LIGHT:   ┌──┐   ROUNDED: ╭──╮
          |  |            │  │            │  │
          +--+            └──┘            ╰──╯

HEAVY:    ┏━━┓   DOUBLE:  ╔══╗
          ┃  ┃            ║  ║
          ┗━━┛            ╚══╝
```

## Color Modes

| Mode | Escape | Example |
|------|--------|---------|
| 16-color | `\x1b[31m` | Basic ANSI colors |
| 256-color | `\x1b[38;5;196m` | 6x6x6 color cube + grayscale |
| True color | `\x1b[38;2;255;0;0m` | 24-bit RGB |

## Terminal Compatibility

| Platform | Terminal | Status |
|----------|----------|--------|
| macOS | Terminal.app | ✓ |
| macOS | iTerm2 | ✓ |
| macOS | Alacritty | ✓ |
| Linux | gnome-terminal | ✓ |
| Linux | xterm | ✓ |
| Windows | WSL2 | ✓ |
| Windows | Windows Terminal | ✓ |

## Building

```bash
make          # Build library and tests
make test     # Run tests
make clean    # Clean build
```

## Map Component (Phase 4)

Uses Carta's ASCII renderer with configurable character sets:

| Charset | Detail | Unicode Required |
|---------|--------|------------------|
| `CT_ASCII_BRAILLE` | 2x4 dots per char (default) | Yes |
| `CT_ASCII_BLOCKS` | Block elements | Yes |
| `CT_ASCII_SIMPLE` | Basic ASCII (fallback) | No |

Auto-fallback detects Unicode support and downgrades gracefully.

## Related

- [clay-shards](../clay-shards/) - Immediate mode components
- [clay-shards-webgl](../clay-shards-webgl/) - WebGL renderer (reference)
- [Clay Library](../../vendor/clay/)
- [Carta ASCII](../../carta/include/ct_ascii.h) - Map tile ASCII rendering
