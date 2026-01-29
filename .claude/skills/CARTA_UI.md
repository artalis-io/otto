# Carta UI - Tile Viewer

## Overview

Carta UI is a React application for viewing and testing tiles from the Carta tile server. It provides a Leaflet-based map interface for browsing raster and vector tiles.

## Quick Start

```bash
cd carta/ui

# Install dependencies
npm install

# Start development server
VITE_TILE_SERVER=http://localhost:8081 npm run dev

# Open http://localhost:5173
```

## Building the UI

```bash
cd carta/ui

# Development
npm install           # Install dependencies
npm run dev           # Start dev server (port 5173)

# Production
npm run build         # Build for production
npm run preview       # Preview production build
```

## Directory Structure

```
carta/ui/
├── src/
│   ├── App.tsx       # Main application component
│   ├── App.css       # Application styles
│   └── main.tsx      # Entry point
├── public/
├── index.html
├── package.json
├── tsconfig.json
└── vite.config.ts
```

## Configuration

### Environment Variables

```bash
# .env
VITE_TILE_SERVER=http://localhost:8081
```

### Tile Server URL

Configure the tile server URL in `vite.config.ts` or via the `VITE_TILE_SERVER` environment variable.

## Features

### Map View

- Pan and zoom across the map
- Automatic tile loading from Carta server
- Zoom level indicator
- Tile boundary overlay (debug mode)

### TileJSON Integration

The UI fetches `tiles.json` from the server to auto-configure:
- Map bounds
- Center position
- Min/max zoom levels
- Attribution

## Component Overview

### App.tsx

```typescript
import { useEffect, useState } from 'react';
import { MapContainer, TileLayer, useMap } from 'react-leaflet';

function App() {
  const [tileJson, setTileJson] = useState(null);
  const tileServer = import.meta.env.VITE_TILE_SERVER || 'http://localhost:8081';

  useEffect(() => {
    fetch(`${tileServer}/tiles.json`)
      .then(res => res.json())
      .then(setTileJson);
  }, []);

  if (!tileJson) return <div>Loading...</div>;

  return (
    <MapContainer
      center={[tileJson.center[1], tileJson.center[0]]}
      zoom={tileJson.center[2]}
      style={{ height: '100vh', width: '100%' }}
    >
      <TileLayer
        url={`${tileServer}/tiles/{z}/{x}/{y}.png`}
        minZoom={tileJson.minzoom}
        maxZoom={tileJson.maxzoom}
        attribution="&copy; OpenStreetMap | Carta"
      />
    </MapContainer>
  );
}
```

### App.css

```css
/* Full-screen map */
#root {
  height: 100vh;
  width: 100vw;
  margin: 0;
  padding: 0;
}

.leaflet-container {
  height: 100%;
  width: 100%;
}

/* Zoom indicator */
.zoom-indicator {
  position: absolute;
  top: 10px;
  right: 10px;
  background: white;
  padding: 5px 10px;
  border-radius: 4px;
  box-shadow: 0 2px 4px rgba(0,0,0,0.2);
  z-index: 1000;
}
```

## Development

### Adding Debug Features

```typescript
// Tile boundary overlay
function TileBoundary({ tileCoord }) {
  const map = useMap();
  const bounds = tileToBounds(tileCoord);

  return (
    <Rectangle
      bounds={bounds}
      pathOptions={{ color: 'red', weight: 1, fillOpacity: 0 }}
    />
  );
}
```

### Vector Tile Support

For vector tiles, use MapLibre GL:

```typescript
import maplibregl from 'maplibre-gl';

const map = new maplibregl.Map({
  container: 'map',
  style: {
    version: 8,
    sources: {
      carta: {
        type: 'vector',
        tiles: [`${tileServer}/tiles/{z}/{x}/{y}.mvt`],
        maxzoom: 18
      }
    },
    layers: [
      {
        id: 'roads',
        type: 'line',
        source: 'carta',
        'source-layer': 'roads',
        paint: { 'line-color': '#888' }
      }
    ]
  }
});
```

## Build for Production

```bash
# Build
npm run build

# Output in dist/ directory
```

### Docker Integration

The production build can be served by the Carta tile server itself by placing files in the `static/` directory.

## Dependencies

- **React**: UI framework
- **react-leaflet**: Map component
- **leaflet**: Mapping library
- **TypeScript**: Type safety
- **Vite**: Build tool
