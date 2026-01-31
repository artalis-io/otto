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
// cc_slider.h
typedef struct {
    bool changed;       // Value was modified
    bool dragging;      // Currently being dragged
} CcSliderResult;
```

### 2. Define the Style Struct (Optional)

```c
typedef struct {
    float width;
    float height;
    float track_height;
    float thumb_radius;
} CcSliderStyle;

extern const CcSliderStyle CC_SLIDER_STYLE_DEFAULT;
```

### 3. Implement the Widget

```c
// cc_slider.c
#include "cc_slider.h"
#include "cc_internal.h"
#include "clay.h"

const CcSliderStyle CC_SLIDER_STYLE_DEFAULT = {
    .width = 200.0f,
    .height = 24.0f,
    .track_height = 4.0f,
    .thumb_radius = 8.0f,
};

CcSliderResult cc_slider(
    uint32_t id,
    float *value,           // App's value (modified in place)
    float min,
    float max,
    const CcSliderStyle *style
) {
    CcSliderResult result = {0};
    CcState *g = cc_get_state();

    if (!value) return result;
    if (!style) style = &CC_SLIDER_STYLE_DEFAULT;

    // 1. Register for tab navigation (if focusable)
    cc_register_focusable(id);

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
        cc_focus(id);
        // Calculate value from click position
        // *value = ... (modify app's value)
        result.changed = true;
    }

    // 5. Use widget state for drag state
    CcWidgetState *w = cc_widget_state(id);
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
cc_register_focusable(id);
```

This enables Tab navigation. Order = call order.

#### Use Widget State Store for UI State

```c
CcWidgetState *w = cc_widget_state(id);
if (w) {
    // Use w->cursor, w->scroll_x, w->open, etc.
    // Add new fields to CcWidgetState if needed
}
```

Never store UI state in globals or statics outside the widget state store.

#### Mutate App State via Pointers

```c
CcSliderResult cc_slider(uint32_t id, float *value, ...) {
    // Modify *value directly when user interacts
    *value = new_value;
    result.changed = true;
}
```

Never copy app state into ClayShards.

#### Handle Click-to-Focus

```c
if (is_hovered && g->pending_click) {
    cc_focus(id);
    g->clicked_id = id;
    // ... handle the click
}
```

#### Return Results, Don't Callback

```c
// GOOD: Return flags
if (cc_slider(CC_ID("vol"), &volume, 0, 100, NULL).changed) {
    update_audio(volume);
}

// BAD: Callback-based (not immediate mode)
cc_slider(CC_ID("vol"), &volume, 0, 100, on_change_callback);
```

---

## File Structure

```
include/
├── cc_common.h      # Core API (don't modify without good reason)
├── cc_slider.h      # Your new widget header
└── cc_immediate.h   # Add #include "cc_slider.h"

src/
├── cc_common.c      # Core implementation
├── cc_slider.c      # Your new widget implementation
├── cc_immediate.c   # Add #include "cc_slider.c"
└── cc_internal.h    # Internal state (extend CcWidgetState if needed)
```

---

## Testing

Add tests in `tests/test_immediate.c`:

```c
void test_slider_result_init(void) {
    CcSliderResult r = {0};
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
WASM_FLAGS += -s EXPORTED_FUNCTIONS='[..., "_cc_slider_get_thumb_x", ...]'
```

Keep exports minimal. Prefer doing logic in C.

---

## Checklist Before PR

- [ ] Widget takes `uint32_t id` as first parameter
- [ ] Uses `CC_ID("name")` pattern in examples
- [ ] Calls `cc_register_focusable(id)` if keyboard-navigable
- [ ] Uses `cc_widget_state(id)` for UI state
- [ ] Mutates app state via pointers (not copies)
- [ ] Returns result struct with interaction flags
- [ ] Has default style constant
- [ ] Handles `pending_click` for focus
- [ ] No heap allocations
- [ ] Added tests
- [ ] Updated `cc_immediate.h` includes

---

## Anti-Patterns

### Don't Store State in Globals

```c
// BAD
static float last_value;  // Where does this belong?

// GOOD
CcWidgetState *w = cc_widget_state(id);
w->scroll_x = drag_offset;  // Keyed by widget ID
```

### Don't Use Callbacks

```c
// BAD
typedef void (*CcSliderCallback)(float value);
cc_slider(id, &value, 0, 100, on_change);

// GOOD
if (cc_slider(id, &value, 0, 100, NULL).changed) {
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
