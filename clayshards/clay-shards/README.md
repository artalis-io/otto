# ClayShards

**Immediate-mode UI components in C11. Built on Clay layout. Render anywhere.**

Write your UI once in C, run it on WASM/WebGL, embedded displays, or any target that consumes render commands. The same code runs across targets.

```c
#include "cs_immediate.h"

static char search_text[256] = "";
static int search_len = 0;

void render_ui(float dt) {
    cs_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("App"), CLAY_LAYOUT(.padding = {16, 16, 16, 16})) {
        // Text input - you own the buffer, ClayShards owns cursor/selection
        CsInputResult input = cs_input(
            CS_ID("search"),
            search_text, &search_len, sizeof(search_text),
            "Search...", NULL
        );

        if (cs_button(CS_ID("go"), "Search", NULL).clicked || input.submitted) {
            do_search(search_text);
        }
    }

    Clay_RenderCommandArray commands = Clay_EndLayout();
    cs_frame_end(dt);

    render_commands(commands);  // Your renderer: WebGL, TUI, SDL, raylib, etc.
}
```

**Key ideas:**
- **`CS_ID("name")`** → stable identity via string hash (no ID stacks)
- **You own business state** (buffers, values, domain data)
- **ClayShards owns UI mechanics** (cursor, focus, blink, selection)
- **Same code, any target** — web and embedded from one codebase

See [MANIFESTO.md](MANIFESTO.md) for design principles.
See [DESIGN.md](DESIGN.md) for architecture.
See [CONTRIBUTING.md](CONTRIBUTING.md) for adding widgets.

---

## Components

| Component | Description |
|-----------|-------------|
| `cs_button` | Clickable button with hover/focus/keyboard support |
| `cs_input` | Text input with cursor, selection, clipboard (Ctrl+C/V/X) |
| `cs_map` | Slippy map pan/zoom interaction |

### cs_button

```c
if (cs_button(CS_ID("save"), "Save", NULL).clicked) {
    save_file();
}
```

### cs_input

```c
static char buf[256];
static int len = 0;

CsInputResult r = cs_input(CS_ID("search"), buf, &len, 256, "Search...", NULL);
if (r.submitted) {
    search(buf);
}
```

### cs_map

```c
static double lat = 47.5, lon = 19.0;
static int zoom = 12;

CsMapResult r = cs_map(CS_ID("map"), &lat, &lon, &zoom, 800, 600, NULL);
if (r.clicked) {
    add_marker(r.click_lat, r.click_lon);
}
```

---

## Building

```bash
make          # Build static library
make test     # Run 37 tests
```

For WASM, include `src/cs_immediate.c` in your Emscripten build.

---

## License

MIT
