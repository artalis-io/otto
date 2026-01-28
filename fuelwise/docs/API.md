# FuelWise REST API Reference

## Base URL

```
http://localhost:8080/api/v1
```

## Endpoints

### Health Check

```
GET /health
```

**Response:**
```json
{
  "status": "healthy",
  "version": "1.0.0",
  "service": "fuelwise-api"
}
```

---

### Filter Stations

Filter and snap stations to a route polyline.

```
POST /filter
```

**Request Body:**
```json
{
  "stations": [
    {"id": 1, "lat": 34.05, "lon": -118.24, "price": 3.89},
    {"id": 2, "lat": 33.94, "lon": -117.93, "price": 3.79}
  ],
  "route": [
    [34.05, -118.24],
    [33.83, -116.55],
    [33.45, -112.07]
  ],
  "max_distance": 5
}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| stations | array | Yes | Fuel stations with location and price |
| stations[].id | number | No | Station identifier (auto-assigned if omitted) |
| stations[].lat | number | Yes | Latitude in degrees |
| stations[].lon | number | Yes | Longitude in degrees |
| stations[].price | number | Yes | Price per gallon |
| route | array | Yes | Polyline as [[lat, lon], ...] |
| max_distance | number | No | Max perpendicular distance in miles (default: 5) |

**Response:**
```json
{
  "count": 2,
  "stations": [
    {
      "station_id": 1,
      "distance_from_start": 0.00,
      "perpendicular_distance": 0.123,
      "price_per_gallon": 3.89,
      "snap_point": [34.050000, -118.240000]
    },
    {
      "station_id": 2,
      "distance_from_start": 45.67,
      "perpendicular_distance": 2.345,
      "price_per_gallon": 3.79,
      "snap_point": [33.940000, -117.930000]
    }
  ]
}
```

---

### Solve (Pre-snapped Stations)

Solve the refueling optimization with stations already snapped to route.

```
POST /solve
```

**Request Body:**
```json
{
  "total_distance": 500,
  "tank_capacity": 150,
  "current_fuel": 50,
  "consumption_mpg": 6.5,
  "minimum_fuel": 20,
  "minimum_fuel_at_end": 25,
  "min_purchase": 20,
  "stop_cost": 10,
  "stations": [
    {"id": 1, "distance": 100, "price": 3.50},
    {"id": 2, "distance": 250, "price": 3.25},
    {"id": 3, "distance": 400, "price": 3.75}
  ],
  "segments": [
    {"start": 0, "weight": 80000, "mpg": 5.0},
    {"start": 200, "weight": 50000, "mpg": 6.5}
  ]
}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| total_distance | number | Yes | Total route distance in miles |
| tank_capacity | number | Yes | Tank capacity in gallons |
| current_fuel | number | Yes | Current fuel level in gallons |
| consumption_mpg | number | Yes* | Base miles per gallon (*required if no segments) |
| minimum_fuel | number | Yes | Minimum fuel level to maintain |
| minimum_fuel_at_end | number | No | Minimum fuel at destination (default: minimum_fuel) |
| min_purchase | number | No | Minimum gallons per stop (enables MILP) |
| stop_cost | number | No | Fixed cost per stop in $ (enables MILP) |
| stations | array | Yes | Stations sorted by distance |
| stations[].id | number | No | Station identifier |
| stations[].distance | number | Yes | Distance from route start in miles |
| stations[].price | number | Yes | Price per gallon |
| segments | array | No | Variable consumption segments |
| segments[].start | number | Yes | Segment start distance |
| segments[].weight | number | No | Cargo weight in lbs (informational) |
| segments[].mpg | number | Yes | Fuel efficiency for segment |

**Response:**
```json
{
  "status": "Optimal solution found",
  "num_stops": 2,
  "total_cost": 175.50,
  "gross_cost": 155.50,
  "remaining_fuel": 25.00,
  "stops": [
    {"station_id": 1, "gallons": 20.00, "cost": 70.00},
    {"station_id": 2, "gallons": 26.31, "cost": 85.50}
  ]
}
```

---

### Optimize (Full Pipeline)

Complete optimization: filter stations to route + solve.

```
POST /optimize
```

**Request Body:**
```json
{
  "stations": [
    {"id": 1, "lat": 34.05, "lon": -118.24, "price": 3.89},
    {"id": 2, "lat": 33.83, "lon": -116.55, "price": 3.69},
    {"id": 3, "lat": 33.45, "lon": -112.07, "price": 3.59}
  ],
  "route": [
    [34.05, -118.24],
    [33.83, -116.55],
    [33.45, -112.07]
  ],
  "tank_capacity": 150,
  "current_fuel": 50,
  "consumption_mpg": 6.5,
  "minimum_fuel": 20,
  "max_distance": 10,
  "min_purchase": 0,
  "stop_cost": 0,
  "segments": [
    {"start": 0, "mpg": 5.5},
    {"start": 150, "mpg": 7.0}
  ]
}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| stations | array | Yes | Fuel stations with lat/lon |
| route | array | Yes | Route polyline [[lat, lon], ...] |
| tank_capacity | number | No | Tank capacity (default: 100) |
| current_fuel | number | No | Current fuel (default: 50) |
| consumption_mpg | number | No | Base MPG (default: 6.5) |
| minimum_fuel | number | No | Min fuel level (default: 25) |
| max_distance | number | No | Max filter distance (default: 5) |
| min_purchase | number | No | Min gallons per stop |
| stop_cost | number | No | Fixed cost per stop |
| segments | array | No | Variable consumption segments |

**Response:**
```json
{
  "status": "Optimal solution found",
  "route_distance": 365.42,
  "stations_filtered": 3,
  "num_stops": 2,
  "total_cost": 125.75,
  "gross_cost": 125.75,
  "remaining_fuel": 20.00,
  "stops": [
    {
      "station_id": 2,
      "distance_from_start": 125.30,
      "gallons": 15.50,
      "cost": 57.20
    },
    {
      "station_id": 3,
      "distance_from_start": 365.42,
      "gallons": 19.10,
      "cost": 68.55
    }
  ]
}
```

---

## Error Responses

All errors return JSON with an `error` field:

```json
{
  "error": "Error description"
}
```

### HTTP Status Codes

| Code | Meaning |
|------|---------|
| 200 | Success |
| 400 | Bad request (invalid input) |
| 404 | Endpoint not found |
| 405 | Method not allowed |
| 422 | Unprocessable (infeasible problem) |
| 500 | Server error |

### Common Errors

| Error Message | Cause |
|---------------|-------|
| "Invalid request format" | Malformed JSON |
| "Missing 'stations' array" | Required field missing |
| "Invalid tank capacity" | Validation failure |
| "Cannot reach station X" | Infeasible: gap too large |
| "No feasible solution" | Problem constraints impossible |

---

## CORS

The API includes CORS headers for browser access:

```
Access-Control-Allow-Origin: *
Access-Control-Allow-Methods: GET, POST, OPTIONS
Access-Control-Allow-Headers: Content-Type
```

Preflight requests (OPTIONS) return 204 No Content.

---

## Examples

### cURL

```bash
# Health check
curl http://localhost:8080/api/v1/health

# Simple optimization
curl -X POST http://localhost:8080/api/v1/optimize \
  -H "Content-Type: application/json" \
  -d '{
    "stations": [
      {"id": 1, "lat": 34.05, "lon": -118.24, "price": 3.50},
      {"id": 2, "lat": 33.45, "lon": -112.07, "price": 3.25}
    ],
    "route": [[34.05, -118.24], [33.45, -112.07]],
    "tank_capacity": 100,
    "current_fuel": 30,
    "consumption_mpg": 8,
    "minimum_fuel": 10
  }'
```

### JavaScript

```javascript
const response = await fetch('http://localhost:8080/api/v1/optimize', {
  method: 'POST',
  headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify({
    stations: [
      { id: 1, lat: 34.05, lon: -118.24, price: 3.50 },
      { id: 2, lat: 33.45, lon: -112.07, price: 3.25 }
    ],
    route: [[34.05, -118.24], [33.45, -112.07]],
    tank_capacity: 100,
    current_fuel: 30,
    consumption_mpg: 8,
    minimum_fuel: 10
  })
});

const result = await response.json();
console.log(`Total cost: $${result.total_cost}`);
```

### Python

```python
import requests

response = requests.post('http://localhost:8080/api/v1/optimize', json={
    'stations': [
        {'id': 1, 'lat': 34.05, 'lon': -118.24, 'price': 3.50},
        {'id': 2, 'lat': 33.45, 'lon': -112.07, 'price': 3.25}
    ],
    'route': [[34.05, -118.24], [33.45, -112.07]],
    'tank_capacity': 100,
    'current_fuel': 30,
    'consumption_mpg': 8,
    'minimum_fuel': 10
})

result = response.json()
print(f"Total cost: ${result['total_cost']:.2f}")
```
