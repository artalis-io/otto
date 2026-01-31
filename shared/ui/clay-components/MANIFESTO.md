# ClayShards Manifesto

## 0. What ClayShards Is

**ClayShards is an immediate-mode UI component library in C11**, built on **Clay** for layout, with rendering via Clay's backend-agnostic render command array that can target **WebGL/WASM today**, and other targets (OpenGL ES, Linux framebuffer, etc.) by implementing a renderer.

ClayShards exists for builders who want:

* **embedded-first constraints**
* **portable UI**
* **business logic in C**
* **web deployment without rewriting the app**

---

## 1. First Principles

### 1) Immediate mode is the interface

UI is authored as code that is executed every frame.

There is no retained widget tree, no DOM, and no reconciliation/diffing.

> The UI is rebuilt each frame because it is simpler, more predictable, and more portable.

### 2) Layout is declarative; components are imperative

ClayShards combines:

* **Declarative layout composition** (Clay)
* **Imperative immediate-mode components** (widgets)

Layout answers *"where and how big?"*
Components answer *"what is it, how does it behave, what does it do?"*

### 3) Stable identity via string hash

Every component has a stable ID derived from a **string hash** (FNV-1a).

```c
#define CC_ID(name) cc_hash_id(name)

cc_button(CC_ID("submit"), "Submit", NULL);
cc_input(CC_ID("search"), text, &len, max, "Search...", NULL);
```

**Design decision:** ClayShards uses explicit string-hash IDs rather than implicit ID stacks. This prioritizes:

* **Debuggability**: IDs are visible in code, not computed implicitly
* **Deterministic identity**: same string → same ID, always
* **No implicit state**: no stack bugs, no push/pop mismatches
* **Predictable memory**: fixed hash table, no per-frame allocations

For dynamic content, construct unique string names:
```c
char id[32];
for (int i = 0; i < 3; i++) {
    snprintf(id, sizeof(id), "item_%d", i);
    cc_button(CC_ID(id), labels[i], NULL);
}
```

**Constraint:** IDs must be unique per component. Duplicate IDs will share widget state (cursor, scroll, etc.), which is usually incorrect. In debug builds, ClayShards may detect duplicate IDs and emit diagnostics.

Stable identity enables:

* persistent widget state (cursor position, scroll offset)
* deterministic input routing
* consistent interaction semantics across targets

### 4) State ownership follows Dear ImGui model

ClayShards recognizes two categories of state:

* **Business state**: owned by the application (C structs, passed by pointer)
* **UI internal state**: owned by ClayShards (keyed by widget ID in internal store)

Widgets may mutate business state inline when interacted with. UI internal mechanics (cursor position, scroll offset, open/closed) are managed by ClayShards keyed by stable widget ID.

**The application owns what the widget represents. ClayShards owns how the widget behaves.**

### 5) Full redraw is the default

ClayShards assumes full-frame redraw is acceptable and desirable for simplicity.

Partial redraw is not a requirement for correctness. It may exist later as an optimization (e.g., e-ink).

### 6) Rendering uses Clay's render commands

ClayShards participates in Clay's layout tree and produces Clay render commands.

The render command array is the contract:

* UI and layout produce render commands
* Renderer backends consume render commands
* ClayShards does **not** manage rendering state (no global GL state assumptions)
* Backends are responsible for translating render commands to GPU/display calls

This separation is what makes "render into anything" real.

### 7) Determinism

Given identical inputs and state, ClayShards produces identical layout and render commands. This is essential for embedded/web parity and testability.

---

## 2. Architecture Commitments

### Frame model

Each frame:

1. **Input is gathered** (mouse/keyboard/touch)
2. **UI code runs** (immediate mode)
3. **Clay layout tree is rebuilt**
4. **Layout is solved**
5. **Render commands are emitted**
6. **Renderer backend executes commands**

This happens the same way on embedded and on web.

### Clay integration

Clay is the layout engine.

* Layout tree is rebuilt every frame
* Components participate by producing layout nodes and rendering inside their computed rects

### Widget state store

Widgets maintain internal state in a **fixed-size hash table keyed by stable ID**. Capacity is configurable at compile time (`CC_WIDGET_STORE_SIZE`) to preserve deterministic memory usage.

This includes:

* Text input: cursor position, selection range, blink state
* Scroll containers: scroll offset (x, y)
* Collapsibles: open/closed state
* Any per-widget persistent UI state

State persists across frames and focus changes. Components access state internally via `cc_widget_state(id)`.

```c
typedef struct {
    uint32_t id;            /* Widget ID (0 = empty slot) */
    int cursor;             /* Text cursor position */
    int selection_start;    /* Selection anchor (-1 = none) */
    float scroll_x, scroll_y;
    bool open;
    /* ... */
} CcWidgetState;
```

### Focus model: linear navigation

ClayShards uses a **linear focus model**:

* Tab cycles forward through focusable elements
* Shift+Tab cycles backward
* Focus wraps at boundaries
* Focusable elements register each frame during render
* **Focus order is defined by widget registration order** (call order during frame)

This model is:

* Stable under string-hash IDs
* Compatible with immediate mode
* Consistent across mouse/touch/keyboard
* Deterministic under full redraw

---

## 3. Target Environments

ClayShards is designed for:

### Embedded

* Deterministic performance
* No dynamic platform dependencies
* Small surface area, predictable runtime

### Web

* WASM-first
* WebGL rendering backend
* The browser is a deployment target, not an architectural constraint

### WASI

ClayShards supports WASI and browser WASM as first-class compilation targets.

---

## 4. Non-Goals (Explicit)

ClayShards does **not** aim to be:

* a DOM
* a retained-mode UI framework
* a React clone
* a CSS engine
* a web-first framework that "also runs on embedded"

ClayShards does not require:

* diffing
* virtual DOM
* runtime reflection
* garbage collection

---

## 5. The ClayShards Promise

If you build your UI with ClayShards:

* Your UI will run in **WASM/WebGL** and **embedded** without rewriting.
* Your business logic remains in **C**.
* Your UI remains **simple to reason about** because it is immediate mode.
* Your layout remains **composable and structured** because it is Clay.
* Your renderer remains **replaceable** because it consumes render commands.
