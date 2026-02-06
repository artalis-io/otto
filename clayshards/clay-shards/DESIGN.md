# ClayShards Design

Technical architecture for ClayShards immediate-mode components.

---

## Frame Lifecycle

Each frame follows this sequence:

```
1. cs_frame_begin()          Reset per-frame state
2. Clay_BeginLayout()        Start Clay layout pass
3. [Your UI code]            Components register and render
4. Clay_EndLayout()          Solve layout, emit render commands
5. cs_frame_end(dt)          Update timers, consume pending input
6. [Renderer]                Execute render commands
```

### cs_frame_begin()

Resets:
- `clicked_id`, `hovered_id` → 0
- `active_text`, `active_len` → NULL (no text buffer bound yet)
- `focused_x/y/w/h` → 0 (cleared until a text input sets them)
- `focusable_count` → 0 (re-register focusables this frame)

Does NOT reset:
- `focused_id` (persists across frames)
- `pending_click`, `pending_enter` (consumed during render)
- Widget state store (persists always)

### cs_frame_end(dt)

- Clears `pending_click`, `pending_enter`
- Updates cursor blink timer for focused widget

---

## Widget State Store

A fixed-size hash table stores per-widget persistent state.

```c
#define CS_WIDGET_STATE_CAPACITY 256  // Compile-time configurable

typedef struct {
    uint32_t id;            // Widget ID (0 = empty slot)
    int cursor;             // Text cursor position
    int selection_start;    // Selection anchor (-1 = none)
    float cursor_blink;     // Blink timer
    bool cursor_visible;    // Current blink state
    float scroll_x, scroll_y;
    bool open;              // For collapsibles, dropdowns
} CsWidgetState;
```

### Lookup: `cs_widget_state(id)`

1. Hash `id` to slot: `slot = id & (CAPACITY - 1)`
2. Linear probe until:
   - Found slot with matching `id` → return it
   - Found empty slot (`id == 0`) → initialize and return it
3. Table full → return fallback slot (should never happen with reasonable capacity)

### Lifecycle

- State is created on first access (lazy initialization)
- State persists until explicitly cleared or program exits
- No garbage collection — slots remain allocated

### Why Not Per-Frame Allocation?

Embedded constraint: no heap allocations during frame render. Fixed table = predictable memory.

---

## Focus Model

### Focus State

```c
uint32_t focused_id;  // Currently focused widget (0 = none)
```

### Focusable Registration

Each frame, focusable widgets call `cs_register_focusable(id)`:

```c
uint32_t focusables[CS_MAX_FOCUSABLES];  // 64 slots
int focusable_count;
```

**Tab order = registration order = call order in your code.**

### Tab Navigation

```c
cs_focus_next()  // Tab: move to next registered focusable
cs_focus_prev()  // Shift+Tab: move to previous
```

Wraps at boundaries. If nothing focused, Tab focuses first element.

### Focus Transfer

When focus changes (`cs_focus(new_id)`):
- `focused_id` updates
- Widget state's blink timer resets
- Cursor position is NOT reset (preserved in widget state)

### Click-to-Focus

Components handle their own click-to-focus:

```c
if (is_hovered && g->pending_click) {
    cs_focus(id);
    // Calculate cursor position from click X
}
```

---

## Input Routing

### Pending Input Flags

Set by platform before frame render:

```c
bool pending_click;   // cs_set_pending_click() on mousedown
bool pending_enter;   // Set internally on Enter key for buttons
```

Components check and consume these during render.

### Keyboard Flow

```
Platform keydown
    ↓
cs_key_down(keyCode, shift, ctrl)
    ↓
Tab? → cs_focus_next/prev
    ↓
Has focused widget?
    ↓
Route to focused widget's key handler
    ↓
Return true if consumed (platform should preventDefault)
```

### Character Input

```c
cs_key_char(charCode)  // ASCII 32-126
```

Inserts character at cursor, deletes selection if any.

### Clipboard (JS only)

Handled in JavaScript before reaching WASM:
- Ctrl+C: Read selection from WASM, write to clipboard
- Ctrl+V: Read clipboard, call `cs_key_char()` per character
- Ctrl+X: Copy + delete selection

---

## Text Input Internals

### State Ownership

| State | Owner | Storage |
|-------|-------|---------|
| Text buffer | Application | Your `char[]` |
| Buffer length | Application | Your `int*` |
| Cursor position | ClayShards | Widget state store |
| Selection | ClayShards | Widget state store |
| Blink timer | ClayShards | Widget state store |

### Active Text Buffer

During render, the focused text input binds its buffer:

```c
g->active_text = text;      // Pointer to app's buffer
g->active_len = len;        // Pointer to app's length
g->active_max_len = max;    // Capacity
```

Keyboard handlers use these pointers to modify text.

### Cursor Positioning on Click

```c
float click_x = cs_pointer_x();
float text_start_x = box.x + padding;
float x_offset = click_x - text_start_x;

cursor = cs_clay_x_to_cursor(text, len, x_offset, font_size);
```

Uses glyph advances for accurate positioning.

---

## Rendering Contract

ClayShards produces Clay render commands. It does NOT:
- Manage GL/GPU state
- Assume any rendering backend
- Call any drawing functions directly

Backends consume `Clay_RenderCommandArray` and translate to:
- WebGL draw calls
- SDL2 rendering
- OpenGL ES
- Software rasterization
- etc.

### Cursor Rendering (JS Example)

The cursor is rendered by the JS layer, not ClayShards:

```javascript
// Only render cursor for focused text inputs (not buttons)
if (wasm.cs_focused_id() !== 0 && wasm.cs_focused_w() > 0) {
    // Selection (always visible)
    if (selStart >= 0 && selStart !== cursor) {
        renderer.renderRect(selStartX, y, selEndX - selStartX, h, selectionColor);
    }
    // Cursor (blinks)
    if (wasm.cs_cursor_visible()) {
        renderer.renderRect(cursorX, y, 2, h, cursorColor);
    }
}
```

---

## Memory Model

### Static Allocations

| Structure | Size | Configurable |
|-----------|------|--------------|
| Widget state table | `CS_WIDGET_STATE_CAPACITY * sizeof(CsWidgetState)` | Yes |
| Focusables array | `CS_MAX_FOCUSABLES * sizeof(uint32_t)` | Yes |
| Global state | `sizeof(CsState)` | No |

### Per-Frame Allocations

**Zero.** All state lives in static structures.

### Clay Memory

Clay has its own arena (typically 8MB). ClayShards does not allocate from it directly — Clay layout nodes are created via `CLAY()` macro.

---

## Determinism

Given identical:
- Input events (pointer position, clicks, keys)
- Application state (text buffers, values)
- Backend behavior (font metrics)

ClayShards produces identical:
- Layout tree
- Render commands
- Widget state mutations

This enables:
- Reproducible testing
- Web/embedded parity
- Snapshot-based debugging

---

## Renderer Targets

ClayShards produces Clay render commands that can be consumed by different backends. Each backend has unique constraints.

### WebGL Renderer

The reference implementation (`clay-shards-webgl/`):
- Pixel-based coordinates from Clay layout
- Floating-point positioning and sizing
- GPU-accelerated rendering
- Borders rendered as separate draw calls (no layout impact)

### TUI Renderer

Terminal text renderer (`clay-shards-tui/`):
- **Character cell coordinates**: All positions/sizes are in character cells, not pixels
- **Integer grid**: 1 unit = 1 character cell (e.g., height=1 means 1 row)
- **Borders consume cells**: A border adds 1 character width/height to each side
- **Double-buffered**: Front/back buffers enable differential updates

#### TUI Design Constraints

These learnings apply to all text-based renderers:

**1. Input-Before-Render Pattern**

Process all input (keyboard, mouse) BEFORE rendering so state changes are reflected immediately in the same frame:

```c
/* WRONG: Input after rendering = 1-frame lag */
void cs_slider(...) {
    /* Render with current_value */
    CLAY(...) { render_track_and_thumb(current_value); }
    /* Process input - too late! */
    if (is_focused && g->pending_arrow_right) {
        *value = current_value + step;  /* Won't show until next frame */
    }
}

/* CORRECT: Input before rendering = immediate response */
void cs_slider(...) {
    /* Process input first */
    if (is_focused && g->pending_arrow_right) {
        *value = current_value + step;
        current_value = *value;  /* Update local for rendering */
    }
    /* Render with updated value */
    CLAY(...) { render_track_and_thumb(current_value); }
}
```

**2. Focus Indication via Background Color**

In TUI mode, borders add characters and shift layout. Use background color for focus indication instead:

```c
/* WRONG: Border-based focus shifts element position */
if (is_focused) {
    config.border.width = {2, 2, 2, 2, 0};  /* Adds 2 chars each side */
}

/* CORRECT: Background color change, no layout impact */
Clay_Color bg = is_focused ? (Clay_Color){CS_COLOR_BTN_BLUE_FOCUS}
                          : (Clay_Color){CS_COLOR_BTN_BLUE};
```

**3. Differential Update Gap Detection**

When updating only changed cells, track cursor position to handle non-consecutive updates:

```c
/* WRONG: Simple flag misses gaps */
bool moved = false;
for (int x = 0; x < width; x++) {
    if (!cell_changed) continue;
    if (!moved) { position_cursor(y, x); moved = true; }
    output_char();  /* Gap = wrong position! */
}

/* CORRECT: Track last position, reposition on gap */
int last_x = -1;
for (int x = 0; x < width; x++) {
    if (!cell_changed) continue;
    if (last_x < 0 || x != last_x + 1) {
        position_cursor(y, x);  /* Reposition on gap */
    }
    output_char();
    last_x = x;
}
```

**4. Z-Index for Layered Content**

TUI rendering uses z-index to determine which content wins when overlapping:

- Higher z-index content overwrites lower z-index
- Floating elements (dropdowns, tooltips) need elevated z-index
- Buffer cleared to `z_index = INT16_MIN` each frame

**5. Rectangle Fill Must Clear Characters**

When rendering rectangles, set both background color AND clear the codepoint to space:

```c
/* Fill rectangle area */
cell->bg_r = bg.r;
cell->bg_g = bg.g;
cell->bg_b = bg.b;
cell->codepoint = ' ';  /* Clear old text! */
```

**6. Cursor Visibility Management**

Hide cursor when focused widget is not a text input:

```c
/* Show cursor only for text inputs */
if (cs_focused_bounds(&x, &y, &w, &h) && w > 0 && h > 0) {
    cs_tui_set_cursor(r, cursor_x, cursor_y, true);
} else {
    cs_tui_set_cursor(r, 0, 0, false);  /* Hide */
}
```
