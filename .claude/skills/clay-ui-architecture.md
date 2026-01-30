# Clay UI Architecture

## Overview

The OTTO platform uses a hybrid UI architecture combining **Clay** (declarative layout) with **immediate mode components** (interaction handling). This provides the best of both worlds: efficient layout computation with intuitive interaction code.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Application Code                        │
│  ┌───────────────────────────────────────────────────────┐  │
│  │  CLAY(CLAY_ID("Panel"), {...}) {                      │  │
│  │      if (cc_button(CC_ID("btn"), "Click").clicked) {  │  │
│  │          do_something();                              │  │
│  │      }                                                │  │
│  │      cc_input(CC_ID("search"), buf, &len, ...);       │  │
│  │  }                                                    │  │
│  └───────────────────────────────────────────────────────┘  │
├─────────────────────────────────────────────────────────────┤
│                   clay-components (cc_*)                    │
│  • Immediate mode API (call function, check result)         │
│  • Manages interaction state (focus, hover, click)          │
│  • Internally builds Clay elements                          │
├─────────────────────────────────────────────────────────────┤
│                        Clay Layout                          │
│  • Declarative element tree                                 │
│  • Flexbox-like layout computation                          │
│  • Produces render commands                                 │
├─────────────────────────────────────────────────────────────┤
│                        Renderers                            │
│  • clay-renderer-webgl (browser)                            │
│  • Future: SDL, raylib, Sokol, native                       │
└─────────────────────────────────────────────────────────────┘
```

## How It Works

### 1. Clay = Layout Engine (Declarative)

Clay handles the visual structure:
- Element positioning and sizing
- Flexbox-like layout (padding, gaps, alignment)
- Floating elements, scrolling
- Produces renderer-agnostic commands

```c
CLAY(CLAY_ID("Panel"), {
    .layout = {
        .layoutDirection = CLAY_TOP_TO_BOTTOM,
        .padding = CLAY_PADDING_ALL(16),
        .childGap = 8
    },
    .backgroundColor = {40, 40, 40, 255},
    .cornerRadius = CLAY_CORNER_RADIUS(8)
}) {
    CLAY_TEXT(CLAY_STRING("Title"), CLAY_TEXT_CONFIG({...}));
}
```

### 2. cc_* = Interaction Wrappers (Immediate Mode)

Components handle user interaction:
- Focus management
- Keyboard input routing
- Click/hover detection
- Return results for caller to act on

```c
// Immediate mode pattern: call, check result, react
CcButtonResult r = cc_button(CC_ID("submit"), "Submit", NULL);
if (r.clicked) {
    submit_form();
}
```

### 3. Components Build Clay Elements Internally

Inside `cc_button()`:

```c
CcButtonResult cc_button(uint32_t id, const char *label, ...) {
    // Check hover using Clay's data from previous frame
    bool is_hovered = Clay_PointerOver(clay_id);

    // Build Clay element (becomes child of current CLAY block)
    CLAY(clay_id, {
        .backgroundColor = is_hovered ? hover_color : normal_color,
        ...
    }) {
        CLAY_TEXT(label_str, ...);
    }

    // Handle click, return result
    if (is_hovered && pending_click) {
        result.clicked = true;
    }
    return result;
}
```

### 4. IDs Bridge Both Systems

Same hash function, compatible IDs:
- `CC_ID("name")` for components
- `CLAY_ID("name")` for Clay elements
- Components construct `Clay_ElementId` from their ID

## Data Flow

```
Frame N:
  1. cc_frame_begin()           - Reset per-frame state
  2. Clay_BeginLayout()         - Start layout pass
  3. render_ui()                - Build element tree
     ├── CLAY() blocks          - Declare structure
     └── cc_*() calls           - Add interactive elements
  4. Clay_EndLayout()           - Compute layout, get commands
  5. cc_frame_end(dt)           - Update cursor blink, etc.
  6. Renderer draws commands    - WebGL, SDL, etc.

Frame N+1:
  - Clay_PointerOver() returns hover state from Frame N's layout
  - Components can react to clicks that happened in Frame N
```

## Component State

Global state in `cc_common.c`:
- `focused_id` - Currently focused element
- `cursor`, `selection_start` - Text input cursor state
- `active_text`, `active_len` - Focused input's buffer
- `pending_click` - Click event this frame

This is standard immediate mode: minimal retained state, computed each frame.

## Application State Management

OTTO applications use a **single AppState structure** pattern:

```c
/* Logical groupings as sub-structs */
typedef struct {
    double lat, lon;
    int zoom;
    int width, height;
} MapState;

typedef struct {
    bool show_controls;
    int layer_type;
} UIPanels;

typedef struct {
    char search[256];
    int search_len;
} UIText;

/* Root structure contains all state */
typedef struct {
    MapState map;
    UIPanels panels;
    UIText text;
    ClayState clay;
} AppState;

/* Single global, compound literal initialization */
static AppState g_app = {
    .map = { .lat = 47.5, .lon = 19.0, .zoom = 12 },
    .panels = { .show_controls = true },
    .text = { .search = "", .search_len = 0 },
};
```

### Benefits

1. **Clear ownership** - All state in one place
2. **Self-documenting** - Designated initializers show defaults
3. **Type-safe** - Compiler catches field name typos
4. **Debuggable** - Inspect one struct to see everything
5. **Serializable** - Easy to save/load state
6. **Testable** - Pass state explicitly to functions

### Theme as Typed Constants

```c
static const struct {
    Clay_Color bg_dark;
    Clay_Color text_light;
    Clay_Color border;
} THEME = {
    .bg_dark    = {40, 40, 40, 230},
    .text_light = {255, 255, 255, 255},
    .border     = {100, 100, 100, 255},
};
```

This replaces preprocessor macros with typed, named constants.

## Directory Structure

```
shared/ui/
├── clay-components/           # Immediate mode components (C)
│   ├── include/
│   │   ├── cc_common.h        # Core API, focus, input routing
│   │   ├── cc_input.h         # Text input component
│   │   ├── cc_button.h        # Button component
│   │   ├── cc_map.h           # Map component
│   │   └── cc_immediate.h     # Convenience header
│   └── src/
│       ├── cc_common.c        # State, ID hashing, keyboard handling
│       ├── cc_input.c         # Text input implementation
│       ├── cc_button.c        # Button implementation
│       ├── cc_map.c           # Map interaction (pan/zoom)
│       └── cc_immediate.c     # Includes all components
│
├── clay-renderer-webgl/       # Browser renderer (JS)
│   ├── renderer.js            # ClayRenderer class
│   ├── font.js                # MSDF font loading
│   ├── shaders.js             # WebGL shaders
│   ├── map-tiles.js           # Slippy map tile rendering
│   └── fonts/                 # Font assets
│
├── cc-map-demo/               # Example application
│   ├── src/map_ui.c           # WASM source
│   ├── map.js                 # JS entry point
│   └── index.html             # HTML shell
│
vendor/clay/                   # Clay library (single header)
```

## Why This Approach?

### Pros

1. **Separation of concerns** - Layout vs interaction cleanly separated
2. **Familiar patterns** - Immediate mode is intuitive for game/UI devs
3. **Efficient** - Clay's layout is microsecond-fast
4. **Portable** - C code compiles to WASM and native
5. **Composable** - Components nest naturally in Clay blocks
6. **Minimal state** - No complex state management needed

### Cons

1. **Two systems to understand** - Clay + immediate mode concepts
2. **Global state** - Single focused element (fine for most UIs)
3. **Frame delay** - Hover state is from previous frame (standard in imgui)

### Comparison

| Approach | Layout | Interaction | State |
|----------|--------|-------------|-------|
| React | Virtual DOM | Event handlers | Component state |
| Dear ImGui | Immediate | Immediate | Minimal |
| **Clay + cc_*** | **Declarative** | **Immediate** | **Minimal** |
| Qt | Widget tree | Signals/slots | Widget state |

## Future Improvements

### Additional Renderers

The architecture supports multiple backends:

```
clay-renderer-webgl/     # Browser (current)
clay-renderer-sdl/       # SDL2 for desktop/mobile
clay-renderer-raylib/    # raylib for games
clay-renderer-sokol/     # Sokol for minimal deps
clay-renderer-terminal/  # TUI rendering
```

Each renderer implements:
- Rectangle drawing (solid, rounded, borders)
- Text rendering (with font metrics callback)
- Texture/image rendering
- Scissor/clipping

### Additional Components

```c
// Planned components
cc_checkbox(id, &checked, "Label");
cc_radio(id, &selected, options, count);
cc_slider(id, &value, min, max);
cc_dropdown(id, &selected, options, count);
cc_tabs(id, &active, tabs, count);
cc_modal(id, &open, title);
cc_tooltip(id, text);
```

### Theming System

```c
// Define theme
CcTheme dark_theme = {
    .colors = {
        .primary = {59, 130, 246, 255},
        .background = {30, 30, 30, 255},
        .text = {255, 255, 255, 255},
        ...
    },
    .spacing = { .xs = 4, .sm = 8, .md = 16, ... },
    .radii = { .sm = 2, .md = 4, .lg = 8 },
};

// Apply globally
cc_set_theme(&dark_theme);
```

### Accessibility

- Keyboard navigation between components
- Screen reader hints via custom render commands
- High contrast theme support
- Focus indicators

## Usage Example

```c
#include "cc_immediate.h"
#include "clay.h"

static char search_buf[256];
static int search_len = 0;
static int selected_tab = 0;

void render_ui(void) {
    CLAY(CLAY_ID("App"), {
        .layout = { .sizing = CLAY_SIZING_GROW }
    }) {
        // Header with tabs
        CLAY(CLAY_ID("Header"), {...}) {
            if (cc_button(CC_ID("tab_home"), "Home", NULL).clicked)
                selected_tab = 0;
            if (cc_button(CC_ID("tab_settings"), "Settings", NULL).clicked)
                selected_tab = 1;
        }

        // Search bar
        CcInputResult r = cc_input(
            CC_ID("search"),
            search_buf, &search_len, sizeof(search_buf),
            "Search...", NULL
        );
        if (r.submitted) {
            do_search(search_buf);
        }

        // Content based on tab
        if (selected_tab == 0) render_home();
        else render_settings();
    }
}
```

## Building

```bash
# Build clay-components library
cd shared/ui/clay-components
make

# Build demo (requires Emscripten)
cd shared/ui/cc-map-demo
make
make serve  # http://localhost:8000
```

## Related Documentation

- [Clay Library](../../../vendor/clay/CLAUDE.md)
- [clay-components](../clay-components/CLAUDE.md)
- [clay-renderer-webgl](../clay-renderer-webgl/CLAUDE.md)
- [cc-map-demo](../cc-map-demo/CLAUDE.md)
