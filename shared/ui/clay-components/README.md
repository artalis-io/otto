# ClayShards

**Immediate-mode UI components in C11. Built on Clay layout. Render anywhere.**

Write your UI once in C, run it on WASM/WebGL, embedded displays, or any target that consumes render commands. The same code runs across targets.

```c
#include "cc_immediate.h"

static char search_text[256] = "";
static int search_len = 0;

void render_ui(float dt) {
    cc_frame_begin();
    Clay_BeginLayout();

    CLAY(CLAY_ID("App"), CLAY_LAYOUT(.padding = {16, 16, 16, 16})) {
        // Text input - you own the buffer, ClayShards owns cursor/selection
        CcInputResult input = cc_input(
            CC_ID("search"),
            search_text, &search_len, sizeof(search_text),
            "Search...", NULL
        );

        if (cc_button(CC_ID("go"), "Search", NULL).clicked || input.submitted) {
            do_search(search_text);
        }
    }

    Clay_RenderCommandArray commands = Clay_EndLayout();
    cc_frame_end(dt);

    render_commands(commands);  // Your renderer: WebGL, SDL, raylib, etc.
}
```

**Key ideas:**
- **`CC_ID("name")`** → stable identity via string hash (no ID stacks)
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
| `cc_button` | Clickable button with hover/focus/keyboard support |
| `cc_input` | Text input with cursor, selection, clipboard (Ctrl+C/V/X) |
| `cc_map` | Slippy map pan/zoom interaction |

### cc_button

```c
if (cc_button(CC_ID("save"), "Save", NULL).clicked) {
    save_file();
}
```

### cc_input

```c
static char buf[256];
static int len = 0;

CcInputResult r = cc_input(CC_ID("search"), buf, &len, 256, "Search...", NULL);
if (r.submitted) {
    search(buf);
}
```

### cc_map

```c
static double lat = 47.5, lon = 19.0;
static int zoom = 12;

CcMapResult r = cc_map(CC_ID("map"), &lat, &lon, &zoom, 800, 600, NULL);
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

For WASM, include `src/cc_immediate.c` in your Emscripten build.

---

## License

MIT
