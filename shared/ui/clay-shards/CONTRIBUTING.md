# Contributing to ClayShards

Guidelines for adding widgets that follow the ClayShards manifesto.

---

## The Rules

Before writing code, internalize these constraints:

1. **Explicit string-hash IDs** — no ID stacks, no implicit identity
2. **App owns business state** — widget takes pointers, mutates in place
3. **ClayShards owns UI state** — cursor, scroll, selection via widget state store
4. **No heap allocations per frame** — use static structures only
5. **Deterministic** — same inputs → same outputs
6. **Focus must be keyboard-navigable** — register focusables, handle Tab

---

## Adding a New Widget

### 1. Define the Result Struct

Return interaction flags, not callbacks:

```c
// cs_slider.h
typedef struct {
    bool changed;       // Value was modified
    bool dragging;      // Currently being dragged
} CsSliderResult;
```

### 2. Define the Style Struct (Optional)

```c
typedef struct {
    float width;
    float height;
    float track_height;
    float thumb_radius;
} CsSliderStyle;

extern const CsSliderStyle CC_SLIDER_STYLE_DEFAULT;
```

### 3. Implement the Widget

```c
// cs_slider.c
#include "cs_slider.h"
#include "cs_internal.h"
#include "clay.h"

const CsSliderStyle CC_SLIDER_STYLE_DEFAULT = {
    .width = 200.0f,
    .height = 24.0f,
    .track_height = 4.0f,
    .thumb_radius = 8.0f,
};

CsSliderResult cs_slider(
    uint32_t id,
    float *value,           // App's value (modified in place)
    float min,
    float max,
    const CsSliderStyle *style
) {
    CsSliderResult result = {0};
    CsState *g = cs_get_state();

    if (!value) return result;
    if (!style) style = &CC_SLIDER_STYLE_DEFAULT;

    // 1. Register for tab navigation (if focusable)
    cs_register_focusable(id);

    bool is_focused = (g->focused_id == id);

    // 2. Build Clay layout
    Clay_ElementId clay_id = (Clay_ElementId){.id = id, .stringId = {0}};

    CLAY(clay_id, {
        .layout = {
            .sizing = {
                .width = CLAY_SIZING_FIXED(style->width),
                .height = CLAY_SIZING_FIXED(style->height)
            }
        },
        // ... styling
    }) {
        // Track and thumb rendering via Clay children
    }

    // 3. Check interaction
    bool is_hovered = Clay_PointerOver(clay_id);

    if (is_hovered) {
        g->hovered_id = id;
    }

    // 4. Handle click/drag
    if (is_hovered && g->pending_click) {
        cs_focus(id);
        // Calculate value from click position
        // *value = ... (modify app's value)
        result.changed = true;
    }

    // 5. Use widget state for drag state
    CsWidgetState *w = cs_widget_state(id);
    if (w) {
        // w->scroll_x could store drag offset
        // w->open could store "is dragging" state
    }

    return result;
}
```

### 4. Key Implementation Points

#### Always Register Focusables

If your widget can receive focus:

```c
cs_register_focusable(id);
```

This enables Tab navigation. Order = call order.

#### Use Widget State Store for UI State

```c
CsWidgetState *w = cs_widget_state(id);
if (w) {
    // Use w->cursor, w->scroll_x, w->open, etc.
    // Add new fields to CsWidgetState if needed
}
```

Never store UI state in globals or statics outside the widget state store.

#### Mutate App State via Pointers

```c
CsSliderResult cs_slider(uint32_t id, float *value, ...) {
    // Modify *value directly when user interacts
    *value = new_value;
    result.changed = true;
}
```

Never copy app state into ClayShards.

#### Handle Click-to-Focus

```c
if (is_hovered && g->pending_click) {
    cs_focus(id);
    g->clicked_id = id;
    // ... handle the click
}
```

#### Return Results, Don't Callback

```c
// GOOD: Return flags
if (cs_slider(CS_ID("vol"), &volume, 0, 100, NULL).changed) {
    update_audio(volume);
}

// BAD: Callback-based (not immediate mode)
cs_slider(CS_ID("vol"), &volume, 0, 100, on_change_callback);
```

---

## File Structure

```
include/
├── cs_common.h      # Core API (don't modify without good reason)
├── cs_slider.h      # Your new widget header
└── cs_immediate.h   # Add #include "cs_slider.h"

src/
├── cs_common.c      # Core implementation
├── cs_slider.c      # Your new widget implementation
├── cs_immediate.c   # Add #include "cs_slider.c"
└── cs_internal.h    # Internal state (extend CsWidgetState if needed)
```

---

## Testing

Add tests in `tests/test_immediate.c`:

```c
void test_slider_result_init(void) {
    CsSliderResult r = {0};
    ASSERT(!r.changed);
    ASSERT(!r.dragging);
}

void test_slider_value_clamp(void) {
    // ...
}
```

Run with `make test`.

---

## WASM Exports

If your widget needs JS interaction, add exports to the app's Makefile:

```makefile
WASM_FLAGS += -s EXPORTED_FUNCTIONS='[..., "_cs_slider_get_thumb_x", ...]'
```

Keep exports minimal. Prefer doing logic in C.

---

## Checklist Before PR

- [ ] Widget takes `uint32_t id` as first parameter
- [ ] Uses `CS_ID("name")` pattern in examples
- [ ] Calls `cs_register_focusable(id)` if keyboard-navigable
- [ ] Uses `cs_widget_state(id)` for UI state
- [ ] Mutates app state via pointers (not copies)
- [ ] Returns result struct with interaction flags
- [ ] Has default style constant
- [ ] Handles `pending_click` for focus
- [ ] No heap allocations
- [ ] Added tests
- [ ] Updated `cs_immediate.h` includes

---

## Anti-Patterns

### Don't Store State in Globals

```c
// BAD
static float last_value;  // Where does this belong?

// GOOD
CsWidgetState *w = cs_widget_state(id);
w->scroll_x = drag_offset;  // Keyed by widget ID
```

### Don't Use Callbacks

```c
// BAD
typedef void (*CsSliderCallback)(float value);
cs_slider(id, &value, 0, 100, on_change);

// GOOD
if (cs_slider(id, &value, 0, 100, NULL).changed) {
    on_change(value);
}
```

### Don't Allocate Per Frame

```c
// BAD
char *label = malloc(strlen(text) + 1);

// GOOD
// Use stack or static buffers
char label[64];
snprintf(label, sizeof(label), "%s", text);
```

### Don't Assume Rendering Backend

```c
// BAD
glDrawArrays(...);  // ClayShards doesn't render

// GOOD
// Build Clay elements, let renderer handle it
CLAY(clay_id, config) { ... }
```
