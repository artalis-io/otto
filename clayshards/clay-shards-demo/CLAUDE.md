# ClayShards Demo

A demonstration of ClayShards immediate-mode UI components with the clay-shards-webgl renderer.

## Overview

This demo shows how to:
1. Use ClayShards components (`cs_map`, `cs_input`, `cs_button`, `cs_checkbox`, `cs_toggle`, `cs_slider`, `cs_dropdown`) for UI
2. Render Clay UI using clay-shards-webgl
3. Load and display slippy map tiles
4. Handle user input (pan, zoom, click, keyboard)

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                      Browser                            │
├─────────────────────────────────────────────────────────┤
│  ┌─────────────────────────────────────────────────────┐│
│  │                    demo.js                          ││
│  │  - Uses loadWasm, createRenderLoop                  ││
│  │  - Event handling (mouse, touch, keyboard)          ││
│  │  - Tile rendering callback                          ││
│  └──────────────────────┬──────────────────────────────┘│
│                         │                               │
│  ┌──────────────────────▼──────────────────────────────┐│
│  │              clay-shards-webgl                      ││
│  │  - ClayRenderer (rects, text, textures)             ││
│  │  - createRenderLoop (generic render loop)           ││
│  │  - setupKeyboardHandler (Tab, clipboard)            ││
│  │  - TileCache, MapTileRenderer (slippy tiles)        ││
│  └──────────────────────┬──────────────────────────────┘│
│                         │                               │
│  ┌──────────────────────▼──────────────────────────────┐│
│  │               demo.wasm                             ││
│  │  - Clay UI layout                                   ││
│  │  - cs_map component (pan/zoom/click)                ││
│  │  - cs_input, cs_button, cs_checkbox                 ││
│  │  - cs_toggle, cs_slider, cs_dropdown                ││
│  └─────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────┘
```

## Files

| File | Purpose |
|------|---------|
| `src/demo.c` | WASM source - Clay UI layout with ClayShards components |
| `demo.js` | JavaScript entry point - uses clay-shards-webgl |
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

- **clay-shards**: `cs_map`, `cs_input`, `cs_button`, `cs_checkbox`, `cs_toggle`, `cs_slider`, `cs_dropdown` components
- **clay-shards-webgl**: WebGL renderer, MSDF fonts, tile rendering
- **Clay**: UI layout library (vendor)

## How It Works

### State Management Pattern

All application state is contained in a single `AppState` structure:

```c
typedef struct {
    double lat, lon;
    int zoom, width, height;
    uint32_t component_id;
} MapState;

typedef struct {
    bool show_tile_info;     // Checkbox: display tile z/x/y
    bool smooth_zoom;        // Toggle: smooth zoom animation
    int layer_type;          // Layer buttons: 0=Carta, 1=OSM
    float route_line_width;  // Slider: route polyline width
    int route_profile_idx;   // Dropdown: 0=Car, 1=Truck
} UIPanels;

typedef struct {
    MapState map;
    UIPanels panels;
    // ...
} AppState;

static AppState g_app = {
    .map = { .lat = 47.4979, .lon = 19.0402, .zoom = 12 },
    .panels = { .show_tile_info = true, .smooth_zoom = true, .route_line_width = 5.0f },
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

The app uses `createRenderLoop` from clay-shards-webgl:

```javascript
import { createRenderLoop, loadWasm, ... } from '../clay-shards-webgl/index.js';

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
- Domain-specific rendering (tiles)
- Clay UI command rendering
- Text cursor for focused inputs

## UI Components

- **Info Panel** (top-left): Coordinates, zoom, search input
- **Layer Panel** (top-right): Layer buttons (Carta/OSM), tile info checkbox, smooth zoom toggle
- **Zoom Controls** (right): + and - buttons
- **Route Panel** (bottom-left): Profile dropdown (Car/Truck), mode buttons, line width slider, route info
- **Tile Info** (bottom-left, toggleable): Current tile z/x/y
- **Attribution** (bottom-right): OSM credit

### Widget Examples

| Widget | Location | Function |
|--------|----------|----------|
| `cs_checkbox` | Layer Panel | Toggle tile info display |
| `cs_toggle` | Layer Panel | Enable/disable smooth zoom animation |
| `cs_slider` | Route Panel | Adjust route line width (1-10px) |
| `cs_dropdown` | Route Panel | Select route profile (Car/Truck) |

## Controls

- **Drag**: Pan the map
- **Scroll/Wheel**: Zoom in/out
- **Tab**: Navigate between focusable elements
- **+/-**: Zoom buttons or keyboard
- **Ctrl+C/V/X**: Clipboard in text inputs
- **Layer buttons**: Switch tile source

## Related

- [clay-shards](../clay-shards/) - Immediate mode UI components
- [clay-shards-webgl](../clay-shards-webgl/) - WebGL renderer library
- [Clay](../../vendor/clay/) - UI layout library
