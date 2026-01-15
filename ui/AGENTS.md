# Agents Guide for FuelWise UI

## Overview

FuelWise UI is a React application for truck refueling optimization. It provides an interactive map interface for uploading stations, defining routes, and visualizing optimization results.

## Directory Structure

```
ui/
├── src/
│   ├── App.tsx               # Main application component
│   ├── App.css               # Application styles
│   ├── main.tsx              # React entry point
│   ├── components/
│   │   ├── MapView.tsx       # Leaflet map with stations/route
│   │   ├── FileUpload.tsx    # CSV file upload and parsing
│   │   ├── RouteConfig.tsx   # Vehicle configuration form
│   │   └── OptimizationResult.tsx  # Solution display
│   ├── services/
│   │   └── api.ts            # REST API client
│   └── types/
│       └── index.ts          # TypeScript type definitions
├── public/
├── index.html
├── package.json
├── tsconfig.json
├── vite.config.ts
├── AGENTS.md                 # This file
└── CLAUDE.md
```

## Build Commands

```bash
npm install       # Install dependencies
npm run dev       # Start development server (port 5173)
npm run build     # Production build
npm run preview   # Preview production build
```

## Key Technologies

- **React 18** + TypeScript
- **Vite 5** - Build tool
- **Leaflet** + react-leaflet - Interactive maps
- **PapaParse** - CSV parsing
- **OSRM** - Open Source Routing Machine for route geometry

## Component Architecture

```
App.tsx
├── FileUpload.tsx
│   └── CSV parsing for stations and waypoints
├── RouteConfig.tsx
│   └── Vehicle configuration form
│   └── Route segment management
├── MapView.tsx
│   └── Leaflet map
│   └── Station markers
│   └── Route polyline
│   └── Fuel stop highlighting
└── OptimizationResult.tsx
    └── Cost breakdown
    └── Stops table
```

## Data Flow

```
1. User uploads CSV → FileUpload parses → stations/waypoints state
2. User configures vehicle → RouteConfig updates → config state
3. User clicks "Get Route" → api.getRoute() → OSRM → route state
4. User clicks "Optimize" → api.optimize() → REST API → solution state
5. MapView displays route + stations + solution
6. OptimizationResult shows cost breakdown
```

## Type Definitions

```typescript
// Core types (from types/index.ts)
interface Station {
    id: number;
    lat: number;
    lon: number;
    price: number;
    name?: string;
}

interface Coordinate {
    lat: number;
    lon: number;
}

interface RouteSegment {
    startDistance: number;
    weight: number;
    mpg: number;
}

interface VehicleConfig {
    tankCapacity: number;
    currentFuel: number;
    consumptionMpg: number;
    minimumFuel: number;
    maxDistance: number;
    minPurchase: number;
    stopCost: number;
    segments: RouteSegment[];
}
```

## API Integration

The `api.ts` service connects to the FuelWise REST API:

```typescript
// Health check
await api.healthCheck();

// Filter stations to route
const snapped = await api.filterStations(stations, route, maxDistance);

// Full optimization
const solution = await api.optimize({
    stations,
    route,
    tank_capacity: 100,
    current_fuel: 30,
    consumption_mpg: 8,
    minimum_fuel: 10,
    segments: [...]
});

// Get route geometry from OSRM
const osrmRoute = await api.getRoute(waypoints);
```

## Routing Providers

Currently using OSRM public demo server. Abstraction ready for:
- Google Maps Directions API
- Mapbox Directions API
- PTV Developer APIs
- Self-hosted OSRM

## CSV Format

### Stations CSV
```csv
id,lat,lon,price,name
1,34.0522,-118.2437,3.89,Shell LA
2,33.4484,-112.0740,3.45,Pilot Phoenix
```

### Waypoints CSV
```csv
lat,lon
34.0522,-118.2437
33.4484,-112.0740
32.7157,-117.1611
```

## State Management

React useState hooks in App.tsx:
- `stations` - Uploaded station list
- `waypoints` - Route waypoints
- `route` - Polyline from routing API
- `config` - Vehicle configuration
- `solution` - Optimization result
- `loading` - Loading states
- `error` - Error messages

## Styling

CSS in `App.css` with:
- CSS variables for theming
- Flexbox layout
- Responsive sidebar/map split
- Custom Leaflet marker styles

## Common Tasks

### Adding a new configuration field

1. Add to `VehicleConfig` in `types/index.ts`
2. Add input in `RouteConfig.tsx`
3. Include in API request in `App.tsx`

### Supporting a new routing provider

1. Create provider function in `api.ts`:
```typescript
export async function getRouteMapbox(
    waypoints: Coordinate[],
    apiKey: string
): Promise<RouteResult> {
    // ...
}
```

2. Add provider selection in `RouteConfig.tsx`
3. Switch provider in `App.tsx`

### Adding map features

1. Edit `MapView.tsx`
2. Use react-leaflet components
3. Access map instance via `useMap()` hook

## Testing

Currently manual testing. To add automated tests:

```bash
npm install -D vitest @testing-library/react
```

```typescript
// src/components/FileUpload.test.tsx
import { render, fireEvent } from '@testing-library/react';
import { FileUpload } from './FileUpload';

test('parses station CSV', async () => {
    // ...
});
```

## Environment Variables

```env
# .env.local
VITE_API_URL=http://localhost:8080
VITE_OSRM_URL=https://router.project-osrm.org
```

## Performance Considerations

1. **Large station lists**: Consider pagination or clustering
2. **Complex routes**: Simplify polyline for display
3. **Map rendering**: Use marker clustering for 1000+ stations
4. **API calls**: Debounce configuration changes

## Known Limitations

1. **Single route**: No multi-leg trip support
2. **No offline mode**: Requires API/OSRM connectivity
3. **No auth**: Open access to API
4. **OSRM demo limits**: Use self-hosted for production
