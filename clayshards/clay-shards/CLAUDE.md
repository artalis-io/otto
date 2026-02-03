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
| `cs_checkbox` | `cs_checkbox.h` | Boolean toggle with checkmark |
| `cs_toggle` | `cs_toggle.h` | On/off switch with sliding knob |
| `cs_slider` | `cs_slider.h` | Value slider with drag/keyboard support |
| `cs_dropdown` | `cs_dropdown.h` | Selection dropdown with keyboard navigation |
| `cs_scroll` | `cs_scroll.h` | Scrollable content container |
| `cs_map` | `cs_map.h` | Slippy map pan/zoom interaction |

## API

### Core (`cs_common.h`)

```c
// Initialize (call once per thread if multi-threaded)
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

// Custom allocator (optional, call before cs_init)
typedef void* (*CsAllocFn)(size_t size, void *user_data);
typedef void* (*CsReallocFn)(void *ptr, size_t size, void *user_data);
typedef void  (*CsFreeFn)(void *ptr, void *user_data);

typedef struct {
    CsAllocFn alloc;
    CsReallocFn realloc;
    CsFreeFn free;
    void *user_data;
} CsAllocator;

void cs_set_allocator(const CsAllocator *allocator);
const CsAllocator* cs_get_allocator(void);

// Error tracking
CsErrorCode cs_get_last_error(void);
int cs_get_error_count(void);
void cs_clear_errors(void);
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

### Scroll (`cs_scroll.h`)

```c
// Scroll container macro - content goes inside the block
CS_SCROLL(CS_ID("list"), 300.0f, NULL) {
    // Scrollable content here
    for (int i = 0; i < 100; i++) {
        render_item(i);
    }
}

// Query scroll state after the block
CsScrollInfo info = cs_scroll_info(CS_ID("list"));
if (info.at_bottom) {
    load_more_items();
}

// Wheel event routing (call from wheel handler)
void cs_set_scroll_delta(float delta_y);

// Check if scroll container is hovered (for event routing)
bool cs_scroll_container_hovered(void);

// Update scroll containers (call during frame, enables momentum scrolling)
void cs_update_scroll_containers(float dt);
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
make test     # Run 83 tests
make clean    # Clean build
```

For WASM builds, include `src/cs_immediate.c` directly in your sources.

## Files

```
include/
├── cs_common.h           # Core API, focus, input routing, allocator
├── cs_clay.h             # Clay integration helpers
├── cs_button.h           # Button component
├── cs_input.h            # Text input component
├── cs_checkbox.h         # Checkbox component
├── cs_toggle.h           # Toggle switch component
├── cs_slider.h           # Slider component
├── cs_dropdown.h         # Dropdown/select component
├── cs_scroll.h           # Scroll container component
├── cs_map.h              # Map interaction component
└── cs_immediate.h        # Convenience header (includes all)

src/
├── cs_common.c           # State management, keyboard handling, allocator
├── cs_clay.c             # Clay initialization, render command accessors
├── cs_button.c           # Button implementation
├── cs_input.c            # Text input implementation
├── cs_checkbox.c         # Checkbox implementation
├── cs_toggle.c           # Toggle switch implementation
├── cs_slider.c           # Slider implementation
├── cs_dropdown.c         # Dropdown/select implementation
├── cs_scroll.c           # Scroll container implementation
├── cs_map.c              # Map pan/zoom, overlays, hit testing
├── cs_map_projection.c   # Web Mercator projection utilities
├── cs_map_simplify.c     # Douglas-Peucker polyline simplification
├── cs_immediate.c        # Amalgamation (includes all .c files)
├── cs_internal.h         # Internal state, TLS macros, error codes
└── cs_map_internal.h     # Map-specific internal types
```

## Thread Safety

ClayShards component state uses thread-local storage:
- Each thread gets isolated UI state (focus, widget state, errors)
- Call `cs_init()` once per thread
- Custom allocators are per-thread

Cross-platform TLS support:
- C11: `_Thread_local`
- GCC/Clang: `__thread`
- MSVC: `__declspec(thread)`

**Clay Integration Limitation:** The `cs_clay.c` module (Clay wrapper) is NOT thread-safe.
Clay itself uses internal global state for layout computation. For multi-threaded apps,
perform all Clay/UI work on a single dedicated thread.

## Custom Allocators

Replace malloc/realloc/free with your own functions (arena allocators, debug allocators):

```c
void* my_alloc(size_t size, void *ctx) { return arena_alloc(ctx, size); }
void* my_realloc(void *p, size_t size, void *ctx) { /* ... */ }
void  my_free(void *p, void *ctx) { /* no-op for arena */ }

CsAllocator arena = {
    .alloc = my_alloc,
    .realloc = my_realloc,
    .free = my_free,
    .user_data = &my_arena
};
cs_set_allocator(&arena);
cs_init();
```

## Related

- [MANIFESTO.md](MANIFESTO.md) - Design principles
- [DESIGN.md](DESIGN.md) - Architecture details
- [CONTRIBUTING.md](CONTRIBUTING.md) - Adding widgets
- [clay-shards-webgl](../clay-shards-webgl/) - WebGL renderer
- [clay-shards-demo](../clay-shards-demo/) - Example application
- [Clay Layout Library](../../vendor/clay/CLAUDE.md)
