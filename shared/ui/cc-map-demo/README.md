# cc_map Demo

A demonstration map viewer using the `cc_map` immediate mode component with [clay-renderer-webgl](../clay-renderer-webgl/).

## Features

- Pan and zoom like Leaflet.js
- Multiple tile sources (OSM, Carto, Stamen Terrain)
- Clay-based UI overlays (info panel, layer selector, zoom controls)
- Uses `cc_map`, `cc_input`, `cc_button` components from clay-components
- WebGL rendering via clay-renderer-webgl

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

## Controls

- **Drag**: Pan the map
- **Scroll/Wheel**: Zoom in/out
- **+/-**: Zoom buttons or keyboard
- **Layer buttons**: Switch tile source

## Architecture

```
┌─────────────────────────────────────┐
│             Browser                 │
│  ┌───────────────────────────────┐  │
│  │           map.js              │  │
│  │  imports clay-renderer-webgl  │  │
│  └───────────────┬───────────────┘  │
│                  │                  │
│  ┌───────────────▼───────────────┐  │
│  │     clay-renderer-webgl       │  │
│  │  ClayRenderer, TileCache,     │  │
│  │  MapTileRenderer, MSDFFont    │  │
│  └───────────────┬───────────────┘  │
│                  │                  │
│  ┌───────────────▼───────────────┐  │
│  │        map_ui.wasm            │  │
│  │  Clay + cc_map + cc_input     │  │
│  └───────────────────────────────┘  │
└─────────────────────────────────────┘
```

## Files

| File | Description |
|------|-------------|
| `src/map_ui.c` | C source with Clay UI and cc_map component |
| `map.js` | JavaScript entry point (imports from clay-renderer-webgl) |
| `index.html` | HTML shell with canvas |
| `Makefile` | Emscripten build configuration |

## Dependencies

- [clay-components](../clay-components/) - `cc_map`, `cc_input`, `cc_button`
- [clay-renderer-webgl](../clay-renderer-webgl/) - WebGL renderer, tile loading, fonts
- [Clay](../../../vendor/clay/) - UI layout library

## License

MIT - Part of the OTTO platform
