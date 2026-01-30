# Clay Map Viewer

A demonstration of using the Clay UI layout library to build a Leaflet-like map viewer with WebAssembly and Canvas rendering.

## Overview

This example shows how to:
1. Use Clay for declarative UI layout in C
2. Compile to WebAssembly for browser deployment
3. Render Clay's output commands using HTML5 Canvas
4. Integrate with OSM tile servers for map rendering

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Browser                              │
├─────────────────────────────────────────────────────────┤
│  ┌─────────────┐      ┌─────────────────────────────┐  │
│  │   map.js    │      │       index.html            │  │
│  │ (Runtime)   │      │   ┌─────────┐ ┌─────────┐   │  │
│  │             │      │   │map-canvas│ │ui-canvas│   │  │
│  │ - Tile load │─────▶│   │(tiles)  │ │ (Clay)  │   │  │
│  │ - Events    │      │   └─────────┘ └─────────┘   │  │
│  │ - Rendering │      └─────────────────────────────┘  │
│  └──────┬──────┘                                        │
│         │                                               │
│  ┌──────▼──────┐                                        │
│  │ map_ui.wasm │  ◀── Compiled from C + Clay            │
│  │             │                                        │
│  │ - UI layout │                                        │
│  │ - Map state │                                        │
│  │ - Input     │                                        │
│  └─────────────┘                                        │
└─────────────────────────────────────────────────────────┘
```

## Files

| File | Purpose |
|------|---------|
| `src/map_ui.c` | Main C source - Clay UI definitions and map state |
| `map.js` | JavaScript runtime - tile loading, rendering, events |
| `index.html` | HTML shell with dual canvas setup |
| `Makefile` | Build configuration for Emscripten |

## Building

Requires Emscripten SDK:

```bash
# Install Emscripten (if not already)
# See: https://emscripten.org/docs/getting_started/downloads.html

# Build WASM module
make

# Build and start development server
make serve
```

## How It Works

### Clay UI Layout (C/WASM)

The C code uses Clay macros to define a declarative UI:

```c
CLAY(CLAY_ID("InfoPanel"), {
    .floating = {
        .attachTo = CLAY_ATTACH_TO_ROOT,
        .attachPoints = {
            .element = CLAY_ATTACH_POINT_LEFT_TOP,
            .parent = CLAY_ATTACH_POINT_LEFT_TOP
        },
        .offset = {16, 16}
    },
    .layout = {
        .layoutDirection = CLAY_TOP_TO_BOTTOM,
        .padding = CLAY_PADDING_ALL(12),
        .childGap = 8
    },
    .backgroundColor = COLOR_BG_DARK,
    .cornerRadius = CLAY_CORNER_RADIUS(8)
}) {
    CLAY_TEXT(CLAY_STRING("Clay Map Viewer"), CLAY_TEXT_CONFIG({...}));
}
```

Clay computes the layout and produces render commands that JavaScript interprets.

### Tile Rendering (JavaScript)

The JavaScript runtime:
1. Fetches map state from WASM (lat, lon, zoom)
2. Calculates visible tile coordinates
3. Loads tiles from OSM servers
4. Renders tiles to the map canvas

### UI Rendering (JavaScript)

After each frame:
1. Call `map_render()` in WASM to get Clay render commands
2. Read commands from WASM memory
3. Draw rectangles, text, borders using Canvas 2D API

### Input Handling

1. JavaScript captures mouse/touch/keyboard events
2. Events are forwarded to WASM (`map_pointer_down`, `map_scroll`, etc.)
3. WASM updates map state and checks for UI element interactions
4. Changes are reflected in the next render frame

## UI Components

- **Info Panel** (top-left): Shows coordinates and zoom level
- **Layer Selector** (top-right): Switch between tile sources
- **Zoom Controls** (right): + and - buttons
- **Tile Info** (bottom-left): Current tile coordinates
- **Attribution** (bottom-right): OSM credit

## Tile Sources

| Layer | Provider |
|-------|----------|
| OSM | OpenStreetMap standard tiles |
| Carto | CartoDB light basemap |
| Terrain | Stadia Maps (Stamen Terrain) |

## Performance Notes

- Tiles are cached in JavaScript (LRU, max 200 tiles)
- Canvas 2D is used for rendering (simpler than WebGL for this demo)
- Clay layout runs in WASM (microsecond performance)
- Frame rate is typically 60fps with smooth panning

## Extending

### Adding UI Elements

1. Add Clay element definition in `render_ui()` (map_ui.c)
2. Handle interactions in `map_handle_click()` if needed
3. Rebuild WASM

### Adding Map Features

1. Add state to `MapState` struct (map_ui.c)
2. Export getter/setter functions
3. Handle in JavaScript (map.js)

### Using WebGL

To switch to WebGL rendering:
1. Create WebGL context instead of 2D
2. Implement shaders for rectangles, text, and tiles
3. Batch draw calls for better performance

## Dependencies

- **Clay**: `vendor/clay/clay.h` (single header, ~4K LOC)
- **Emscripten**: For WASM compilation
- **Python**: For development server (optional)

## Related

- [Clay Documentation](../../../vendor/clay/CLAUDE.md)
- [Carta Tile Generator](../../../carta/CLAUDE.md)
- [Velo Routing Engine](../../../velo/CLAUDE.md)
