# cc_map Demo

A demonstration of the `cc_map` immediate mode map component with the `clay-renderer-webgl` library.

## Overview

This demo shows how to:
1. Use the `cc_map` component from `clay-components` for map interaction
2. Render Clay UI using `clay-renderer-webgl`
3. Load and display slippy map tiles
4. Handle user input (pan, zoom, click)

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                      Browser                            │
├─────────────────────────────────────────────────────────┤
│  ┌─────────────────────────────────────────────────────┐│
│  │                    map.js                           ││
│  │  - Imports from clay-renderer-webgl                 ││
│  │  - WASM loading and event handling                  ││
│  │  - Orchestrates tile + UI rendering                 ││
│  └──────────────────────┬──────────────────────────────┘│
│                         │                               │
│  ┌──────────────────────▼──────────────────────────────┐│
│  │            clay-renderer-webgl                      ││
│  │  - ClayRenderer (rects, text, textures)             ││
│  │  - MSDFFont (crisp text at any size)                ││
│  │  - TileCache, MapTileRenderer (slippy tiles)        ││
│  └──────────────────────┬──────────────────────────────┘│
│                         │                               │
│  ┌──────────────────────▼──────────────────────────────┐│
│  │               map_ui.wasm                           ││
│  │  - Clay UI layout                                   ││
│  │  - cc_map component (pan/zoom/click handling)       ││
│  │  - cc_input, cc_button components                   ││
│  └─────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────┘
```

## Files

| File | Purpose |
|------|---------|
| `src/map_ui.c` | WASM source - Clay UI layout with cc_map component |
| `map.js` | JavaScript entry point - uses clay-renderer-webgl |
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

- **clay-components**: `cc_map`, `cc_input`, `cc_button` components
- **clay-renderer-webgl**: WebGL renderer, MSDF fonts, tile rendering
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

The `cc_map` component modifies state pointers based on user interaction:

```c
cc_map(g_app.map.component_id,
       &g_app.map.lat, &g_app.map.lon, &g_app.map.zoom,
       (float)g_app.map.width, (float)g_app.map.height, NULL);
```

### Tile Rendering

JavaScript fetches map state from WASM and renders tiles:

```javascript
const lat = wasm.map_get_lat();
const lon = wasm.map_get_lon();
const zoom = wasm.map_get_zoom();

tileRenderer.render(lat, lon, zoom, layerType, width, height, projMatrix);
```

### UI Rendering

Clay computes layout, produces render commands, which are drawn by the renderer:

```javascript
const count = wasm.map_frame(dt);
renderer.renderClayCommands(wasm, count, projMatrix);
```

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

- [clay-components](../clay-components/) - Immediate mode UI components
- [clay-renderer-webgl](../clay-renderer-webgl/) - WebGL renderer library
- [Clay](../../../vendor/clay/) - UI layout library
