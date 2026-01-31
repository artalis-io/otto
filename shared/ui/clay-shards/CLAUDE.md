# ClayShards

Immediate-mode UI components in C11, built on Clay layout, render anywhere.

## Architecture

ClayShards provides **immediate mode interaction** on top of **Clay's declarative layout**:

```
┌─────────────────────────────────────────┐
│         Application Code                │
│  if (cs_button(...).clicked) { ... }    │
├─────────────────────────────────────────┤
│            ClayShards                   │
│  • Immediate mode API                   │
│  • Focus/hover/click handling           │
│  • Internally builds Clay elements      │
├─────────────────────────────────────────┤
│            Clay Layout                  │
│  • Declarative element tree             │
│  • Layout computation                   │
└─────────────────────────────────────────┘
```

Components are called within Clay blocks and internally use Clay's API:

```c
CLAY(CLAY_ID("Panel"), {...}) {
    // cs_button internally calls CLAY() to add elements
    if (cs_button(CS_ID("submit"), "Submit", NULL).clicked) {
        submit_form();
    }
}
```

## Components

| Component | Header | Description |
|-----------|--------|-------------|
| `cs_button` | `cs_button.h` | Clickable button with variants |
| `cs_input` | `cs_input.h` | Text input with cursor/selection/clipboard |
| `cs_map` | `cs_map.h` | Slippy map pan/zoom interaction |

## API

### Core (`cs_common.h`)

```c
// Initialize (call once)
void cs_init(void);

// Frame lifecycle
void cs_frame_begin(void);
void cs_frame_end(float dt);

// Focus management
uint32_t cs_focused_id(void);
void cs_focus(uint32_t id);
void cs_blur(void);

// Input routing (call from event handlers)
bool cs_key_down(int key_code, bool shift, bool ctrl);
bool cs_key_char(uint32_t char_code);
void cs_set_pending_click(void);

// Tab navigation
void cs_register_focusable(uint32_t id);
bool cs_focus_next(void);
bool cs_focus_prev(void);

// Cursor state (for rendering)
int cs_cursor_pos(void);
int cs_selection_start(void);
bool cs_cursor_visible(void);
const char* cs_focused_text(void);
int cs_focused_text_len(void);
```

### Button (`cs_button.h`)

```c
typedef struct {
    bool clicked;
    bool hovered;
} CsButtonResult;

CsButtonResult cs_button(uint32_t id, const char *label, const CsButtonStyle *style);
```

### Input (`cs_input.h`)

```c
typedef struct {
    bool changed;
    bool submitted;
    bool focused;
    bool blurred;
} CsInputResult;

CsInputResult cs_input(
    uint32_t id,
    char *text,          // Your buffer (modified in place)
    int *len,            // Current length (modified in place)
    int max_len,         // Buffer capacity
    const char *placeholder,
    const CsInputStyle *style
);
```

### Map (`cs_map.h`)

```c
typedef struct {
    bool panned;
    bool zoomed;
    bool clicked;
    double click_lat, click_lon;
} CsMapResult;

CsMapResult cs_map(
    uint32_t id,
    double *lat, double *lon, int *zoom,  // Your state (modified on interaction)
    float width, float height,
    const CsMapStyle *style
);
```

## Usage

```c
#define CLAY_IMPLEMENTATION
#include "clay.h"
#include "cs_immediate.h"

static char search[256];
static int search_len = 0;

void render(float dt) {
    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("App"), CLAY_LAYOUT(.padding = {16, 16, 16, 16})) {
        // Button
        if (cs_button(CS_ID("click_me"), "Click Me", NULL).clicked) {
            printf("Clicked!\n");
        }

        // Text input
        CsInputResult r = cs_input(
            CS_ID("search"), search, &search_len, sizeof(search),
            "Search...", NULL
        );
        if (r.submitted) {
            do_search(search);
        }
    }

    Clay_RenderCommandArray commands = Clay_EndLayout();
    cs_frame_end(dt);

    // Render commands via your backend...
}
```

## WASM Exports

When building for WASM, these functions are exported (via `EMSCRIPTEN_KEEPALIVE`):

```
cs_focused_id, cs_cursor_pos, cs_selection_start, cs_cursor_visible,
cs_focused_bounds, cs_focused_text, cs_focused_text_len,
cs_key_down, cs_key_char, cs_blur, cs_set_pending_click,
cs_register_focusable, cs_focus_next, cs_focus_prev
```

Add to your Makefile's `EXPORTED_FUNCTIONS`.

## Building

```bash
make          # Build static library
make test     # Run 37 tests
make clean    # Clean build
```

For WASM builds, include `src/cs_immediate.c` directly in your sources.

## Files

```
include/
├── cs_common.h      # Core API, focus, input routing
├── cs_clay.h        # Clay integration helpers
├── cs_button.h      # Button component
├── cs_input.h       # Text input component
├── cs_map.h         # Map interaction component
└── cs_immediate.h   # Convenience header (includes all)

src/
├── cs_common.c      # State management, keyboard handling
├── cs_clay.c        # Clay initialization, render command accessors
├── cs_button.c      # Button implementation
├── cs_input.c       # Text input implementation
├── cs_map.c         # Map pan/zoom implementation
├── cs_immediate.c   # Includes all .c files
└── cs_internal.h    # Internal state structure
```

## Related

- [MANIFESTO.md](MANIFESTO.md) - Design principles
- [DESIGN.md](DESIGN.md) - Architecture details
- [CONTRIBUTING.md](CONTRIBUTING.md) - Adding widgets
- [clay-shards-webgl](../clay-shards-webgl/) - WebGL renderer
- [clay-shards-demo](../clay-shards-demo/) - Example application
- [Clay Layout Library](../../../vendor/clay/CLAUDE.md)
