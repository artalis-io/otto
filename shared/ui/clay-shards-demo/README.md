# cs_map Demo

A demonstration map viewer using the `cs_map` immediate mode component with [clay-shards-webgl](../clay-shards-webgl/).

## Features

- Pan and zoom like Leaflet.js
- Multiple tile sources (OSM, Carto, Stamen Terrain)
- Clay-based UI overlays (info panel, layer selector, zoom controls)
- Uses `cs_map`, `cs_input`, `cs_button` components from clay-shards
- WebGL rendering via clay-shards-webgl

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
│  │  imports clay-shards-webgl  │  │
│  └───────────────┬───────────────┘  │
│                  │                  │
│  ┌───────────────▼───────────────┐  │
│  │     clay-shards-webgl       │  │
│  │  ClayRenderer, TileCache,     │  │
│  │  MapTileRenderer, MSDFFont    │  │
│  └───────────────┬───────────────┘  │
│                  │                  │
│  ┌───────────────▼───────────────┐  │
│  │        map_ui.wasm            │  │
│  │  Clay + cs_map + cs_input     │  │
│  └───────────────────────────────┘  │
└─────────────────────────────────────┘
```

## Files

| File | Description |
|------|-------------|
| `src/map_ui.c` | C source with Clay UI and cs_map component |
| `map.js` | JavaScript entry point (imports from clay-shards-webgl) |
| `index.html` | HTML shell with canvas |
| `Makefile` | Emscripten build configuration |

## Dependencies

- [clay-shards](../clay-shards/) - `cs_map`, `cs_input`, `cs_button`
- [clay-shards-webgl](../clay-shards-webgl/) - WebGL renderer, tile loading, fonts
- [Clay](../../../vendor/clay/) - UI layout library

## License

MIT - Part of the OTTO platform
