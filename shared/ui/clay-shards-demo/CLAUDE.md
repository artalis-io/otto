# cs_map Demo

A demonstration of the `cs_map` immediate mode map component with the `clay-shards-webgl` library.

## Overview

This demo shows how to:
1. Use the `cs_map` component from `clay-shards` for map interaction
2. Render Clay UI using `clay-shards-webgl`
3. Load and display slippy map tiles
4. Handle user input (pan, zoom, click)

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                      Browser                            │
├─────────────────────────────────────────────────────────┤
│  ┌─────────────────────────────────────────────────────┐│
│  │                    map.js                           ││
│  │  - Uses loadWasm, createRenderLoop                  ││
│  │  - Event handling                                   ││
│  │  - Tile rendering callback                          ││
│  └──────────────────────┬──────────────────────────────┘│
│                         │                               │
│  ┌──────────────────────▼──────────────────────────────┐│
│  │            clay-shards-webgl                      ││
│  │  - ClayRenderer (rects, text, textures)             ││
│  │  - createRenderLoop (generic render loop)           ││
│  │  - loadWasm (WASM loading with validation)          ││
│  │  - TileCache, MapTileRenderer (slippy tiles)        ││
│  └──────────────────────┬──────────────────────────────┘│
│                         │                               │
│  ┌──────────────────────▼──────────────────────────────┐│
│  │               map_ui.wasm                           ││
│  │  - Clay UI layout                                   ││
│  │  - cs_map component (pan/zoom/click handling)       ││
│  │  - cs_input, cs_button components                   ││
│  └─────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────┘
```

## Files

| File | Purpose |
|------|---------|
| `src/map_ui.c` | WASM source - Clay UI layout with cs_map component |
| `map.js` | JavaScript entry point - uses clay-shards-webgl |
| `index.html` | HTML shell with canvas |
| `Makefile` | Emscripten build configuration |

## Building

```bash
# Requires Emscripten SDK
make

# Start development server
make serve
# Open http://localhost:8000
```

## Dependencies

- **clay-shards**: `cs_map`, `cs_input`, `cs_button` components
- **clay-shards-webgl**: WebGL renderer, MSDF fonts, tile rendering
- **Clay**: UI layout library (vendor)

## How It Works

### State Management Pattern

All application state is contained in a single `AppState` structure, using C11 compound literals:

```c
/* Logical groupings as sub-structs */
typedef struct {
    double lat, lon;
    int zoom, width, height;
    uint32_t component_id;
} MapState;

typedef struct {
    bool show_controls, show_tile_info;
    int layer_type;
} UIPanels;

/* Root application state */
typedef struct {
    MapState map;
    UIPanels panels;
    UIText text;
    TextBuffers scratch;
    ClayState clay;
} AppState;

/* Single global with compound literal initialization */
static AppState g_app = {
    .map = { .lat = 47.4979, .lon = 19.0402, .zoom = 12 },
    .panels = { .show_tile_info = true },
};
```

### Theme Constants

Colors use typed compound literal constants instead of macros:

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

### Map Component

The `cs_map` component modifies state pointers based on user interaction:

```c
cs_map(g_app.map.component_id,
       &g_app.map.lat, &g_app.map.lon, &g_app.map.zoom,
       (float)g_app.map.width, (float)g_app.map.height, NULL);
```

### Render Loop

The app uses `createRenderLoop` from clay-shards-webgl with tile rendering as the `onRender` callback:

```javascript
import { createRenderLoop, loadWasm, ... } from '../clay-shards-webgl/index.js';

// Load WASM with required exports
const [wasm] = await Promise.all([
    loadWasm('build/map_ui.wasm', REQUIRED_EXPORTS),
    font.load(...)
]);

// Create render loop with tile rendering callback
const render = createRenderLoop(renderer, wasm, font, {
    frameFunction: 'map_frame',
    onRender: (projMatrix) => {
        tileRenderer.render(
            wasm.map_get_lat(),
            wasm.map_get_lon(),
            wasm.map_get_zoom(),
            wasm.map_get_layer(),
            renderer.width, renderer.height,
            projMatrix
        );
    }
});
requestAnimationFrame(render);
```

The render loop handles:
- Delta time calculation
- Clearing and projection matrix
- Calling the domain-specific `onRender` callback (tiles)
- Rendering Clay UI commands
- Rendering text cursor for focused inputs

## UI Components

- **Info Panel** (top-left): Coordinates, zoom, search input
- **Layer Selector** (top-right): OSM, Carto, Terrain
- **Zoom Controls** (right): + and - buttons
- **Tile Info** (bottom-left): Current tile z/x/y
- **Attribution** (bottom-right): OSM credit

## Controls

- **Drag**: Pan the map
- **Scroll/Wheel**: Zoom in/out
- **+/-**: Zoom buttons or keyboard
- **Layer buttons**: Switch tile source

## Related

- [clay-shards](../clay-shards/) - Immediate mode UI components
- [clay-shards-webgl](../clay-shards-webgl/) - WebGL renderer library
- [Clay](../../../vendor/clay/) - UI layout library
