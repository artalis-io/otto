# Clay Components

Immediate mode UI components built on top of Clay layout engine.

## Architecture

This library provides **immediate mode interaction** on top of **Clay's declarative layout**:

```
┌─────────────────────────────────────────┐
│         Application Code                │
│  if (cc_button(...).clicked) { ... }    │
├─────────────────────────────────────────┤
│         clay-components                 │
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
    // cc_button internally calls CLAY() to add elements
    if (cc_button(CC_ID("submit"), "Submit", NULL).clicked) {
        submit_form();
    }
}
```

## Components

| Component | Header | Description |
|-----------|--------|-------------|
| `cc_button` | `cc_button.h` | Clickable button with variants |
| `cc_input` | `cc_input.h` | Text input with cursor/selection |
| `cc_map` | `cc_map.h` | Slippy map pan/zoom interaction |

## API

### Core (`cc_common.h`)

```c
// Initialize (call once)
void cc_init(void);

// Frame lifecycle
void cc_frame_begin(void);
void cc_frame_end(float dt);

// Focus management
uint32_t cc_focused_id(void);
void cc_focus(uint32_t id);
void cc_blur(void);

// Input routing (call from event handlers)
bool cc_key_down(int key_code, bool shift, bool ctrl);
bool cc_key_char(uint32_t char_code);
void cc_set_pending_click(void);

// Cursor state (for rendering)
int cc_cursor_pos(void);
int cc_selection_start(void);
bool cc_cursor_visible(void);
const char* cc_focused_text(void);
int cc_focused_text_len(void);
```

### Button (`cc_button.h`)

```c
typedef struct {
    bool clicked;
    bool hovered;
} CcButtonResult;

CcButtonResult cc_button(uint32_t id, const char *label, const CcButtonStyle *style);
```

### Input (`cc_input.h`)

```c
typedef struct {
    bool changed;
    bool submitted;
    bool focused;
    bool blurred;
} CcInputResult;

CcInputResult cc_input(
    uint32_t id,
    char *text,          // Your buffer (modified in place)
    int *len,            // Current length (modified in place)
    int max_len,         // Buffer capacity
    const char *placeholder,
    const CcInputStyle *style
);
```

### Map (`cc_map.h`)

```c
typedef struct {
    bool panned;
    bool zoomed;
    bool clicked;
    double click_lat, click_lon;
} CcMapResult;

CcMapResult cc_map(
    uint32_t id,
    double *lat, double *lon, int *zoom,  // Your state (modified on interaction)
    float width, float height,
    const CcMapStyle *style
);
```

## Usage

```c
#define CLAY_IMPLEMENTATION
#include "clay.h"
#include "cc_immediate.h"

static char search[256];
static int search_len = 0;

void render(void) {
    cc_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("App"), {...}) {
        // Button
        if (cc_button(CC_ID("click_me"), "Click Me", NULL).clicked) {
            printf("Clicked!\n");
        }

        // Text input
        CcInputResult r = cc_input(
            CC_ID("search"), search, &search_len, sizeof(search),
            "Search...", NULL
        );
        if (r.submitted) {
            do_search(search);
        }
    }

    Clay_RenderCommandArray commands = Clay_EndLayout();
    cc_frame_end(dt);

    // Render commands...
}
```

## WASM Exports

When building for WASM, these functions are exported (via `EMSCRIPTEN_KEEPALIVE`):

```
cc_focused_id, cc_cursor_pos, cc_selection_start, cc_cursor_visible,
cc_focused_bounds, cc_focused_text, cc_focused_text_len,
cc_key_down, cc_key_char, cc_blur, cc_set_pending_click
```

Add to your Makefile's `EXPORTED_FUNCTIONS`.

## Building

```bash
make          # Build static library
make test     # Run tests
make clean    # Clean build
```

For WASM builds, include `src/cc_immediate.c` directly in your sources.

## Files

```
include/
├── cc_common.h      # Core API, focus, input routing
├── cc_button.h      # Button component
├── cc_input.h       # Text input component
├── cc_map.h         # Map interaction component
└── cc_immediate.h   # Convenience header (includes all)

src/
├── cc_common.c      # State management, keyboard handling
├── cc_button.c      # Button implementation
├── cc_input.c       # Text input implementation
├── cc_map.c         # Map pan/zoom implementation
├── cc_immediate.c   # Includes all .c files
└── cc_internal.h    # Internal state structure
```

## Related

- [Clay Layout Library](../../../vendor/clay/CLAUDE.md)
- [clay-renderer-webgl](../clay-renderer-webgl/)
- [cc-map-demo](../cc-map-demo/)
- [Architecture Guide](../../../.claude/skills/clay-ui-architecture.md)
