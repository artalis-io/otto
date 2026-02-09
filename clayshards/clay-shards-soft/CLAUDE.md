# ClayShards Software Renderer

CPU-based framebuffer renderer for Clay UI with platform backends.

## Overview

Renders Clay render commands to an RGBA pixel buffer using SIMD-optimized
rasterization. Provides platform backends for window creation and display.

## Architecture

```
┌─────────────────────────────────────────┐
│          Application                    │
│  cs_soft_render_clay_commands(r, cmds)  │
├─────────────────────────────────────────┤
│        cs_soft.c (Core Renderer)        │
│  • Rectangle fill (SIMD via sh_render)  │
│  • Text rendering (MSDF via sh_font)    │
│  • Scissor clipping (cs_render)         │
├─────────────────────────────────────────┤
│    Platform Backends                    │
│  • Headless (built-in)                  │
│  • Cocoa/macOS (done)                   │
│  • X11 (planned)                        │
│  • Win32 (planned)                      │
│  • Wayland (planned)                    │
└─────────────────────────────────────────┘
```

## Files

| File | Purpose |
|------|---------|
| `include/cs_soft.h` | Public API |
| `src/cs_soft.c` | Core renderer + headless platform |
| `src/cs_soft_internal.h` | Internal types, platform vtable |
| `src/platforms/cs_soft_cocoa.m` | macOS/Cocoa backend |
| `tests/test_soft.c` | Unit tests |
| `tests/demo_soft.c` | Interactive demo |

## Usage

```c
#define CLAY_IMPLEMENTATION
#include "clay.h"
#include "cs_soft.h"
#include "cs_immediate.h"

int main(void) {
    /* Initialize Clay */
    uint64_t mem_size = Clay_MinMemorySize();
    void *mem = malloc(mem_size);
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(mem_size, mem);
    Clay_Initialize(arena, (Clay_Dimensions){800, 600}, (Clay_ErrorHandler){0});

    /* Initialize ClayShards */
    cs_init();

    /* Create software renderer */
    CsSoftConfig config;
    cs_soft_config_init(&config);
    config.width = 800;
    config.height = 600;
    CsSoftRenderer *r = cs_soft_create(&config);

    /* Main loop */
    while (!cs_soft_should_close(r)) {
        /* Handle events */
        CsSoftEvent event;
        while (cs_soft_poll_event(r, &event)) {
            switch (event.type) {
                case CS_SOFT_EVENT_KEY_DOWN:
                    cs_key_down(event.key.key_code, event.key.shift, event.key.ctrl);
                    break;
                case CS_SOFT_EVENT_MOUSE_DOWN:
                    cs_set_pending_click();
                    Clay_SetPointerState((Clay_Vector2){event.mouse.x, event.mouse.y}, true);
                    break;
                /* ... other events ... */
            }
        }

        /* Run UI frame */
        cs_frame_begin();
        Clay_BeginLayout();

        /* Declare UI */
        CLAY(CLAY_ID("Root"), CLAY_LAYOUT(.padding = {16, 16, 16, 16})) {
            if (cs_button(CS_ID("btn"), "Click Me", NULL).clicked) {
                printf("Button clicked!\n");
            }
        }

        Clay_RenderCommandArray commands = Clay_EndLayout();
        cs_frame_end(0.016f);

        /* Render */
        cs_soft_begin(r);
        cs_soft_clear(r, 0xFF1E1E1E);  /* Dark gray background */
        cs_soft_render_clay_commands(r, &commands);
        cs_soft_end(r);
    }

    cs_soft_free(r);
    free(mem);
    return 0;
}
```

## API

### Configuration

```c
CsSoftConfig config;
cs_soft_config_init(&config);

config.width = 800;           // Window width (0 = auto)
config.height = 600;          // Window height (0 = auto)
config.platform = CS_SOFT_PLATFORM_HEADLESS;  // or AUTO, X11, WIN32
config.double_buffer = true;  // Use double buffering
config.vsync = true;          // Enable VSync
config.title = "My App";      // Window title
```

### Lifecycle

```c
CsSoftRenderer *r = cs_soft_create(&config);  // or NULL for defaults
cs_soft_get_size(r, &width, &height);
cs_soft_resize(r, new_width, new_height);
cs_soft_free(r);
```

### Event Handling

```c
CsSoftEvent event;
while (cs_soft_poll_event(r, &event)) {
    switch (event.type) {
        case CS_SOFT_EVENT_KEY_DOWN:
            // event.key.key_code, event.key.shift, event.key.ctrl
            break;
        case CS_SOFT_EVENT_MOUSE_MOVE:
            // event.mouse.x, event.mouse.y
            break;
        case CS_SOFT_EVENT_SCROLL:
            // event.scroll.delta_x, event.scroll.delta_y
            break;
    }
}

bool closing = cs_soft_should_close(r);
cs_soft_request_close(r);
```

### Rendering

```c
cs_soft_begin(r);
cs_soft_clear(r, 0xFFRRGGBB);  // RGBA (R high byte, A low byte)
cs_soft_render_clay_commands(r, &commands);
cs_soft_end(r);  // Present to screen
```

### Direct Primitives

For non-Clay usage:

```c
cs_soft_rect(r, x, y, w, h, color, radius);
cs_soft_border(r, x, y, w, h, color, width);
cs_soft_text(r, "Hello", -1, x, y, 16.0f, 0xFFFFFFFF);
```

### Buffer Access (Headless/Testing)

```c
uint8_t *pixels = cs_soft_get_pixels(r);  // RGBA, row-major
uint32_t pixel = cs_soft_get_pixel(r, x, y);
```

## Platforms

| Platform | Status | Display Method | Input |
|----------|--------|----------------|-------|
| Headless | Done | None (testing) | None |
| Cocoa/macOS | Done | NSBitmapImageRep | NSEvent |
| X11 | Planned | XShmPutImage | XNextEvent |
| Win32 | Planned | BitBlt/DIB | WndProc |
| Wayland | Planned | wl_shm | wl_seat |
| fbdev | Planned | mmap | /dev/input |

## Color Format

Colors use `cs_pack_color(r, g, b, a)` from `cs_render.h`:
- `(r << 24) | (g << 16) | (b << 8) | a`
- Same as ClayShards/Clay convention

Internally converted to `sh_render` format for SIMD operations.

## Building

```bash
make          # Build static library
make test     # Run unit tests
make clean    # Clean build
```

## Dependencies

- `shared/libshared.a` - SIMD span fill, font rendering
- `clay-shards/libclay_shards.a` - Scissor stack, color utilities
- `vendor/clay/clay.h` - Clay layout library

## Related

- [clay-shards](../clay-shards/) - Immediate mode components
- [clay-shards-tui](../clay-shards-tui/) - Terminal TUI renderer
- [shared/sh_render.h](../../shared/include/sh_render.h) - SIMD primitives
- [shared/sh_font.h](../../shared/include/sh_font.h) - MSDF font rendering
