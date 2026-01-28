# Claude Code Instructions for FuelWise UI

## Overview

FuelWise UI is a React + TypeScript application for truck refueling optimization. It connects to the FuelWise REST API and displays results on an interactive map.

## Quick Start

```bash
npm install       # Install dependencies
npm run dev       # Start dev server on port 5173
```

Requires the API server running on port 8080:
```bash
cd ../api && make run
```

## Key Files

| File | Purpose |
|------|---------|
| `src/App.tsx` | Main component, state management |
| `src/components/MapView.tsx` | Leaflet map integration |
| `src/components/FileUpload.tsx` | CSV upload/parsing |
| `src/components/RouteConfig.tsx` | Vehicle config form |
| `src/services/api.ts` | REST API client |
| `src/types/index.ts` | TypeScript definitions |

## Architecture

```
User → FileUpload (CSV) → stations state
User → RouteConfig (form) → config state
User → "Get Route" → OSRM API → route state
User → "Optimize" → FuelWise API → solution state
MapView renders: route + stations + solution
OptimizationResult shows: cost breakdown + stops
```

## Common Tasks

### Adding a new input field

1. Add type to `types/index.ts`:
```typescript
interface VehicleConfig {
    // ...existing fields
    newField: number;
}
```

2. Add input in `RouteConfig.tsx`:
```tsx
<div className="form-group">
    <label>New Field</label>
    <input
        type="number"
        value={config.newField}
        onChange={(e) => setConfig({
            ...config,
            newField: parseFloat(e.target.value)
        })}
    />
</div>
```

3. Include in API request in `App.tsx`:
```typescript
const request = {
    // ...existing fields
    new_field: config.newField
};
```

### Modifying map display

MapView uses react-leaflet. Key patterns:

```tsx
// Access map instance
function MapController() {
    const map = useMap();
    // Use map methods
    return null;
}

// Add marker with popup
<Marker position={[lat, lon]} icon={customIcon}>
    <Popup>Station info</Popup>
</Marker>

// Add polyline
<Polyline positions={coords} color="blue" />
```

### Adding a routing provider

1. Add function in `api.ts`:
```typescript
export async function getRouteGoogle(
    waypoints: Coordinate[],
    apiKey: string
): Promise<Coordinate[]> {
    const response = await fetch(
        `https://maps.googleapis.com/maps/api/directions/json?...`
    );
    // Parse and return polyline
}
```

2. Add provider selection UI in RouteConfig
3. Call appropriate function in App.tsx

### Handling errors

Error pattern in App.tsx:
```typescript
try {
    setLoading(true);
    setError(null);
    const result = await api.optimize(request);
    setSolution(result);
} catch (err) {
    setError(err instanceof Error ? err.message : 'Unknown error');
} finally {
    setLoading(false);
}
```

Display errors:
```tsx
{error && <div className="error-message">{error}</div>}
```

## CSV Parsing

FileUpload uses PapaParse:

```typescript
Papa.parse(file, {
    header: true,
    dynamicTyping: true,
    complete: (results) => {
        const stations = results.data.map(row => ({
            id: row.id,
            lat: row.lat,
            lon: row.lon,
            price: row.price,
            name: row.name
        }));
        onStationsLoaded(stations);
    }
});
```

Expected CSV format:
```csv
id,lat,lon,price,name
1,34.05,-118.24,3.89,Shell
```

## Styling

CSS in App.css:
- `.app` - Main container (flexbox row)
- `.sidebar` - Left panel (controls)
- `.map-container` - Right panel (map)
- `.form-group` - Form field wrapper
- `.btn` - Button styles
- `.results` - Solution display

## API Client

The `api.ts` service:

```typescript
const API_BASE = 'http://localhost:8080/api/v1';

export async function optimize(request: OptimizeRequest) {
    const response = await fetch(`${API_BASE}/optimize`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(request)
    });

    if (!response.ok) {
        const error = await response.json();
        throw new Error(error.error || 'Optimization failed');
    }

    return response.json();
}
```

## Type Safety

All API requests/responses typed:
- `OptimizeRequest` - Request body
- `OptimizeResponse` - Success response
- `FuelStop` - Individual stop info

Use strict TypeScript:
```json
// tsconfig.json
{
    "compilerOptions": {
        "strict": true,
        "noImplicitAny": true
    }
}
```

## Testing

Manual testing workflow:
1. Start API: `cd ../api && make run`
2. Start UI: `npm run dev`
3. Upload sample-stations.csv
4. Add waypoints
5. Get route
6. Optimize
7. Verify results on map

## Environment Configuration

```typescript
// vite.config.ts
export default defineConfig({
    plugins: [react()],
    server: {
        proxy: {
            '/api': 'http://localhost:8080'
        }
    }
});
```

## Code Style

- Functional components with hooks
- TypeScript strict mode
- Named exports
- Descriptive prop names
- CSS in single App.css file
