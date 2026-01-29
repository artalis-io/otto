# FuelWise UI - Usage Guide

## Overview

FuelWise UI is a React application with Leaflet map integration for visualizing and interacting with the refueling optimization API. It allows users to input routes, view fuel stations, and see optimized refueling plans.

## Quick Start

```bash
cd fuelwise/ui

# Install dependencies
npm install

# Start development server
npm run dev

# Open http://localhost:5173
```

## Building the UI

```bash
cd fuelwise/ui

# Development
npm install           # Install dependencies
npm run dev           # Start dev server (port 5173)

# Production
npm run build         # Build for production
npm run preview       # Preview production build

# Linting
npm run lint          # Run ESLint
```

## Directory Structure

```
fuelwise/ui/
├── src/
│   ├── App.tsx           # Main application component
│   ├── App.css           # Application styles
│   ├── main.tsx          # Entry point
│   ├── components/
│   │   ├── MapView.tsx   # Leaflet map component
│   │   ├── RouteInput.tsx# Route input form
│   │   ├── StationList.tsx# Station display
│   │   └── Results.tsx   # Optimization results
│   ├── hooks/
│   │   └── useOptimize.ts# API integration hook
│   └── types/
│       └── index.ts      # TypeScript definitions
├── public/
├── index.html
├── package.json
├── tsconfig.json
└── vite.config.ts
```

## Key Components

### App.tsx

Main application component that orchestrates:
- Route input and validation
- API calls to FuelWise backend
- Results display and map visualization

### MapView.tsx

Leaflet-based map component:
- Route polyline display
- Station markers with price info
- Interactive map controls
- Optimized stops highlighting

### RouteInput.tsx

Route configuration form:
- Origin/destination coordinates
- Tank capacity and current fuel
- Fuel consumption rate
- Station search radius

## Configuration

### Environment Variables

```bash
# .env
VITE_API_URL=http://localhost:8080/api/v1
VITE_TILE_SERVER=http://localhost:8081
```

### API URL Configuration

The UI connects to the FuelWise API by default at `http://localhost:8080`. Configure this in `vite.config.ts` or via environment variable.

## Features

### Route Input

- Enter origin and destination coordinates
- Configure vehicle parameters (tank size, current fuel, mpg)
- Set maximum station search radius
- Support for waypoints

### Map Visualization

- Display route polyline on map
- Show all stations near route
- Highlight recommended stops
- Click stations for details

### Results Display

- Total fuel cost
- Individual stop recommendations
- Gallons to purchase at each stop
- Distance to each stop

## Development

### Adding a New Component

1. Create component in `src/components/`
2. Add TypeScript types in `src/types/`
3. Import and use in `App.tsx`

### API Integration

```typescript
// src/hooks/useOptimize.ts
import { useState } from 'react';

interface OptimizeParams {
  route: [number, number][];
  stations: Station[];
  tankCapacity: number;
  currentFuel: number;
  consumptionMpg: number;
}

export function useOptimize() {
  const [loading, setLoading] = useState(false);
  const [result, setResult] = useState(null);

  const optimize = async (params: OptimizeParams) => {
    setLoading(true);
    const response = await fetch('/api/v1/optimize', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(params),
    });
    const data = await response.json();
    setResult(data);
    setLoading(false);
  };

  return { optimize, loading, result };
}
```

### Leaflet Setup

```typescript
// src/components/MapView.tsx
import { MapContainer, TileLayer, Polyline, Marker } from 'react-leaflet';

function MapView({ route, stations, selectedStops }) {
  return (
    <MapContainer center={[39.8283, -98.5795]} zoom={5}>
      <TileLayer
        url="https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png"
        attribution='&copy; OpenStreetMap'
      />
      <Polyline positions={route} color="blue" />
      {stations.map(s => (
        <Marker key={s.id} position={[s.lat, s.lon]} />
      ))}
    </MapContainer>
  );
}
```

## TypeScript Types

```typescript
// src/types/index.ts

export interface Station {
  id: number;
  lat: number;
  lon: number;
  price: number;
  name?: string;
}

export interface Purchase {
  stationId: number;
  gallons: number;
  cost: number;
  distance: number;
}

export interface OptimizeResult {
  status: 'optimal' | 'infeasible' | 'error';
  totalCost: number;
  purchases: Purchase[];
}

export interface RouteParams {
  route: [number, number][];
  tankCapacity: number;
  currentFuel: number;
  consumptionMpg: number;
  minimumFuel: number;
  maxDistance: number;
}
```

## Build for Production

```bash
# Build
npm run build

# Output in dist/ directory
# Deploy to any static hosting (nginx, S3, etc.)
```

### Nginx Configuration

```nginx
server {
    listen 80;
    server_name example.com;

    root /var/www/fuelwise-ui/dist;
    index index.html;

    location / {
        try_files $uri $uri/ /index.html;
    }

    location /api/ {
        proxy_pass http://localhost:8080;
    }
}
```

## Dependencies

- **React**: UI framework
- **react-leaflet**: Map component
- **leaflet**: Mapping library
- **TypeScript**: Type safety
- **Vite**: Build tool
