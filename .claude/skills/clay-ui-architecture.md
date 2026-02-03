# ClayShards UI Architecture

## Overview

The OTTO platform uses a hybrid UI architecture combining **Clay** (declarative layout) with **ClayShards** immediate mode components (interaction handling). This provides the best of both worlds: efficient layout computation with intuitive interaction code.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Application Code                        │
│  ┌───────────────────────────────────────────────────────┐  │
│  │  CLAY(CLAY_ID("Panel"), {...}) {                      │  │
│  │      if (cs_button(CS_ID("btn"), "Click").clicked) {  │  │
│  │          do_something();                              │  │
│  │      }                                                │  │
│  │      cs_input(CS_ID("search"), buf, &len, ...);       │  │
│  │  }                                                    │  │
│  └───────────────────────────────────────────────────────┘  │
├─────────────────────────────────────────────────────────────┤
│                   ClayShards (cs_*)                         │
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
│  • clay-shards-webgl (browser)                              │
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

### 2. cs_* = Interaction Wrappers (Immediate Mode)

ClayShards components handle user interaction:
- Focus management
- Keyboard input routing
- Click/hover detection
- Return results for caller to act on

```c
// Immediate mode pattern: call, check result, react
CsButtonResult r = cs_button(CS_ID("submit"), "Submit", NULL);
if (r.clicked) {
    submit_form();
}
```

### 3. Components Build Clay Elements Internally

Inside `cs_button()`:

```c
CsButtonResult cs_button(uint32_t id, const char *label, ...) {
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
- `CS_ID("name")` for ClayShards components
- `CLAY_ID("name")` for Clay elements
- Components construct `Clay_ElementId` from their ID

## Data Flow

```
Frame N:
  1. cs_frame_begin()           - Reset per-frame state
  2. Clay_BeginLayout()         - Start layout pass
  3. render_ui()                - Build element tree
     ├── CLAY() blocks          - Declare structure
     └── cs_*() calls           - Add interactive elements
  4. Clay_EndLayout()           - Compute layout, get commands
  5. cs_frame_end(dt)           - Update cursor blink, etc.
  6. Renderer draws commands    - WebGL, SDL, etc.

Frame N+1:
  - Clay_PointerOver() returns hover state from Frame N's layout
  - Components can react to clicks that happened in Frame N
```

## Component State

Global state in `cs_common.c`:
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
clayshards/
├── clay-shards/               # Immediate mode components (C)
│   ├── include/
│   │   ├── cs_common.h        # Core API, focus, input routing
│   │   ├── cs_input.h         # Text input component
│   │   ├── cs_button.h        # Button component
│   │   ├── cs_map.h           # Map component
│   │   └── cs_immediate.h     # Convenience header
│   └── src/
│       ├── cs_common.c        # State, ID hashing, keyboard handling
│       ├── cs_input.c         # Text input implementation
│       ├── cs_button.c        # Button implementation
│       ├── cs_map.c           # Map interaction (pan/zoom)
│       └── cs_immediate.c     # Includes all components
│
├── clay-shards-webgl/         # Browser renderer (JS)
│   ├── renderer.js            # ClayRenderer class
│   ├── font.js                # MSDF font loading
│   ├── shaders.js             # WebGL shaders
│   └── map-tiles.js           # Slippy map tile rendering
│
├── clay-shards-demo/          # Example application
│   ├── src/demo.c             # WASM source
│   ├── demo.js                # JS entry point
│   └── index.html             # HTML shell
│
└── fonts/                     # MSDF font assets
    ├── ui-font.json           # Font metrics
    └── ui-font.png            # Font atlas

vendor/clay/                   # Clay library (single header)
```

## Assessment

### Strengths

| Aspect | Assessment |
|--------|------------|
| **Separation of concerns** | Excellent. Layout → Components → Rendering are cleanly decoupled. |
| **WASM-first** | Solid. ~140KB WASM for a full map viewer with UI is impressive. |
| **Renderer-agnostic** | Good foundation. Same C code could target SDL, raylib, framebuffer. |
| **Zero dependencies** | Pure C with no allocations in hot paths. Embedded-friendly. |
| **Immediate mode API** | Clean. `if (cs_button(...).clicked)` is intuitive. |
| **Memory model** | Predictable. Fixed arena, no dynamic allocation during render. |
| **Layered architecture** | Clean separation allows swapping renderers or adding components independently. |

### Known Limitations

**1. Text Measurement is Approximate**
```c
float char_width = config->fontSize * g_clay.char_width_ratio;  // 0.6
```
Works for monospace fonts. Proportional fonts will have layout inaccuracies.

**Solutions:**
- Provide real font metrics from JS/native back to WASM
- Use monospace fonts only
- Build a font metrics table into WASM at compile time

**2. Single Map Instance**
```c
static CsMapDragState g_map_drag = {0};  // Only one map supported
```
The drag state is global. Multiple maps would interfere.

**Solution:** Use a hash table keyed by component ID for multi-instance support.

**3. WASM Boundary Overhead**
```javascript
for (let i = 0; i < commandCount; i++) {
    cmd.type(i);  // WASM call
    cmd.x(i);     // WASM call
    cmd.y(i);     // WASM call
    // ~10 calls per command
}
```
Many small WASM calls add overhead.

**Solution:** Bulk accessor returning typed array view into WASM memory.

**4. No Animation System**
Transitions, easing, and animated state changes require manual implementation.

**5. Limited Component Set**
Currently: button, input, map. Missing common widgets.

**6. No Accessibility**
No keyboard navigation between elements, no screen reader support.

### Platform Suitability

**Web Applications:**
- Excellent for specialized widgets (map viewer, data visualization)
- Small WASM footprint, fast rendering
- Not a replacement for React/Vue for full dashboards
- CSS would be more familiar to web developers

**Recommended:** Embed ClayShards components where you need pixel-perfect control, use React/Vue for forms, tables, settings.

**Embedded Systems:**
- Small footprint, pure C, predictable memory
- Same codebase as web version
- Efficient layout algorithm
- Needs renderer implementation (SDL, framebuffer, OpenGL ES)
- Font handling needs adaptation

**Recommended:** Ideal for in-cab displays, industrial HMIs, or any system needing cross-platform C UI.

### Comparison

| Approach | Layout | Interaction | State | Portability |
|----------|--------|-------------|-------|-------------|
| React | Virtual DOM | Event handlers | Component state | Web only |
| Dear ImGui | Immediate | Immediate | Minimal | C++, many backends |
| **Clay + cs_*** | **Declarative** | **Immediate** | **Minimal** | **C, any backend** |
| Qt | Widget tree | Signals/slots | Widget state | C++, heavy runtime |
| LVGL | Object tree | Callbacks | Widget state | C, embedded focus |

### Verdict

**Is this a solid foundation?** Yes.

**Is it production-ready?** Not yet. Needs:
1. Real font metrics (critical for non-monospace)
2. More components (checkbox, dropdown, slider)
3. Keyboard navigation between focusables
4. At least one native renderer to prove cross-platform

**Best use:** Specialized rendering (maps, charts, custom visualizations) where you need the same C code on web and embedded.

---

## Roadmap

### Phase 1: Core Stability (Current)
- [x] Clay integration with immediate mode wrapper
- [x] Button, input, map components
- [x] WebGL renderer with MSDF text
- [x] Generic render loop and WASM utilities
- [ ] Real font metrics (measure text accurately)
- [ ] Fix single-instance limitations

### Phase 2: Component Library
- [ ] Checkbox component
- [ ] Radio button component
- [ ] Dropdown/select component
- [ ] Slider component
- [ ] Tabs component
- [ ] Modal/dialog component
- [ ] Tooltip component

### Phase 3: Cross-Platform
- [ ] SDL2 renderer (desktop/embedded Linux)
- [ ] Native font loading
- [ ] Touch gesture support
- [ ] High-DPI handling

### Phase 4: Polish
- [ ] Keyboard navigation (Tab/Shift+Tab)
- [ ] Focus indicators
- [ ] Animation/transition system
- [ ] Theming system
- [ ] Accessibility hints

---

## Implementation Notes

### Adding a New Component

1. Create header `include/cs_<name>.h` with types and API
2. Create source `src/cs_<name>.c` with implementation
3. Add include to `cs_immediate.h`
4. Add source include to `cs_immediate.c`
5. Add tests to `tests/test_clay_shards.c`

Component pattern:
```c
typedef struct {
    bool changed;
    bool clicked;
    // ... result fields
} CsFooResult;

typedef struct {
    float width, height;
    CsMargin margin;
    CsAlign align;
    // ... style fields
} CsFooStyle;

CsFooResult cs_foo(uint32_t id, /* state pointers */, const CsFooStyle *style);
```

### Adding a New Renderer

Implement these operations:
1. **Rectangle** - Solid color, rounded corners, optional border
2. **Text** - String at position with font size and color
3. **Texture** - Image/tile quad
4. **Scissor** - Push/pop clipping regions

Process Clay render commands:
```c
for (int i = 0; i < commands.length; i++) {
    Clay_RenderCommand *cmd = &commands.internalArray[i];
    switch (cmd->commandType) {
        case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: /* ... */ break;
        case CLAY_RENDER_COMMAND_TYPE_TEXT: /* ... */ break;
        case CLAY_RENDER_COMMAND_TYPE_BORDER: /* ... */ break;
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START: /* ... */ break;
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END: /* ... */ break;
    }
}
```

### Font Metrics Integration

To support proportional fonts accurately:

**Option A: JS-side measurement (web)**
```javascript
// Measure in JS, call back to WASM
function measureText(text, fontSize) {
    ctx.font = `${fontSize}px ${fontFamily}`;
    return ctx.measureText(text).width;
}
// Register callback with Clay
```

**Option B: Embedded font metrics (native/WASM)**
```c
// Build glyph width table into binary
static const float GLYPH_WIDTHS[128] = { /* generated */ };

Clay_Dimensions measure_text(Clay_StringSlice text, ...) {
    float width = 0;
    for (int i = 0; i < text.length; i++) {
        width += GLYPH_WIDTHS[(int)text.chars[i]] * scale;
    }
    return (Clay_Dimensions){width, fontSize};
}
```

## Usage Example

```c
#include "cs_immediate.h"
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
            if (cs_button(CS_ID("tab_home"), "Home", NULL).clicked)
                selected_tab = 0;
            if (cs_button(CS_ID("tab_settings"), "Settings", NULL).clicked)
                selected_tab = 1;
        }

        // Search bar
        CsInputResult r = cs_input(
            CS_ID("search"),
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
# Build clay-shards library
cd clayshards/clay-shards
make

# Build demo (requires Emscripten)
cd clayshards/clay-shards-demo
make
make serve  # http://localhost:8000
```

## Related Documentation

- [Clay Library](../../vendor/clay/CLAUDE.md)
- [clay-shards](../../clayshards/clay-shards/CLAUDE.md)
- [clay-shards-webgl](../../clayshards/clay-shards-webgl/CLAUDE.md)
- [clay-shards-demo](../../clayshards/clay-shards-demo/CLAUDE.md)
