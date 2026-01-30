# Clay Map Viewer

A demonstration map viewer using the [Clay UI layout library](https://github.com/nicbarker/clay) with WebAssembly and HTML5 Canvas rendering.

## Features

- Pan and zoom like Leaflet.js
- Multiple tile sources (OSM, Carto, Stamen Terrain)
- Clay-based UI overlays (info panel, layer selector, zoom controls)
- Compiled to ~15KB WASM

## Quick Start

```bash
# Requires Emscripten SDK
# Install: https://emscripten.org/docs/getting_started/downloads.html

# Build WASM module
make

# Start development server
make serve
# Open http://localhost:8000
```

Or from the project root:

```bash
make clay-map-serve
```

## Controls

- **Drag**: Pan the map
- **Scroll/Wheel**: Zoom in/out
- **+/-**: Zoom buttons
- **Layer buttons**: Switch tile source

## Architecture

```
┌─────────────────────────────┐
│       Browser               │
│  ┌───────────┬───────────┐  │
│  │ Map Tiles │ Clay UI   │  │
│  │ (Canvas)  │ (Canvas)  │  │
│  └─────┬─────┴─────┬─────┘  │
│        │           │        │
│  ┌─────▼───────────▼─────┐  │
│  │      map.js           │  │
│  │  - Tile loading       │  │
│  │  - Event handling     │  │
│  │  - Render loop        │  │
│  └─────────┬─────────────┘  │
│            │                │
│  ┌─────────▼─────────────┐  │
│  │    map_ui.wasm        │  │
│  │  - UI layout (Clay)   │  │
│  │  - Map state          │  │
│  └───────────────────────┘  │
└─────────────────────────────┘
```

## Files

| File | Description |
|------|-------------|
| `src/map_ui.c` | C source with Clay UI definitions |
| `map.js` | JavaScript runtime (tile loading, rendering) |
| `index.html` | HTML shell with dual canvas |
| `Makefile` | Emscripten build configuration |

## Why Clay?

Clay provides:
- Declarative, React-like UI in pure C
- Single 4KB header file
- Microsecond layout performance
- 15KB WASM output
- Renderer-agnostic (Canvas 2D, WebGL, SDL, etc.)

This makes it ideal for performance-critical applications like map viewers where every millisecond counts.

## Related

- [Clay Library](../../vendor/clay/)
- [Carta Tile Generator](../../carta/)
- [Velo Routing Engine](../../velo/)

## License

MIT - Part of the OTTO platform
