# Clay Components

Immediate mode UI components for the [Clay](https://github.com/nicbarker/clay) layout library.

## Features

- **Immediate mode API** - Simple call-and-check pattern
- **Built on Clay** - Leverages Clay's fast layout engine
- **Zero dependencies** - Pure C, compiles to WASM
- **Components**: Button, Text Input, Map (pan/zoom)

## Quick Start

```c
#include "cc_immediate.h"

static char name[64];
static int name_len = 0;

void render(void) {
    CLAY(CLAY_ID("Form"), {...}) {
        // Text input
        cc_input(CC_ID("name"), name, &name_len, sizeof(name), "Name...", NULL);

        // Button
        if (cc_button(CC_ID("submit"), "Submit", NULL).clicked) {
            printf("Hello, %s!\n", name);
        }
    }
}
```

## How It Works

Components are called inside Clay layout blocks:

1. Component internally builds Clay elements (becomes children)
2. Uses Clay's hover detection from previous frame
3. Returns result struct with interaction flags
4. You check flags and react immediately

```c
CcButtonResult r = cc_button(CC_ID("btn"), "Click", NULL);
if (r.clicked) {
    // Handle click
}
if (r.hovered) {
    // Show tooltip
}
```

## Building

```bash
make          # Build static library
make test     # Run tests
```

For WASM, include `src/cc_immediate.c` in your build.

## Components

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

## License

MIT - Part of the OTTO platform
