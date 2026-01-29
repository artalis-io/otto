# Shared Utilities Library - Usage Guide

## Overview

The shared library provides common geographic utilities used across Velo (routing) and Carta (tiles). It includes coordinate types, distance calculations, bounding box operations, and Web Mercator projection functions.

## Quick Start

```bash
# Build and test
cd shared
make all && make test
```

## Building Shared

```bash
cd shared
make all       # Build library + tests
make lib       # Build libshared.a only
make test      # Run test suite (23 tests)
make clean     # Remove build artifacts
```

## Directory Structure

```
shared/
├── include/
│   ├── shared.h      # Main header (includes sh_geo.h)
│   └── sh_geo.h      # Geographic utilities
├── src/
│   └── sh_geo.c      # Implementation
└── tests/
    └── test_shared.c # Test suite
```

## Using Shared in Your Code

### Coordinate Types

```c
#include "shared.h"

// Floating-point coordinate (degrees)
SHCoord budapest = {.lat = 47.497, .lon = 19.040};

// Fixed-point coordinate (1e7 scale, matches OSM PBF)
SHCoordFixed fixed = {.lat = 474970000, .lon = 190400000};

// Convert between formats
SHCoord from_fixed = SH_FIXED_TO_COORD(fixed);
SHCoordFixed to_fixed = SH_COORD_TO_FIXED(budapest);

// Bounding box
SHBBox bbox = {
    .min_lat = 47.0, .min_lon = 19.0,
    .max_lat = 48.0, .max_lon = 20.0
};
```

### Distance Calculations

```c
#include "shared.h"

SHCoord budapest = {47.497, 19.040};
SHCoord vienna = {48.208, 16.373};

// Haversine distance (accurate)
double dist = sh_haversine(budapest, vienna);
printf("Distance: %.1f km\n", dist / 1000.0);

// Fast approximation (accurate within ~0.5% for <500km)
double fast_dist = sh_distance_fast(budapest, vienna);

// For fixed-point coordinates
SHCoordFixed a = SH_COORD_TO_FIXED(budapest);
SHCoordFixed b = SH_COORD_TO_FIXED(vienna);
double dist_fixed = sh_haversine_fixed(a, b);
```

### Coordinate Utilities

```c
#include "shared.h"

SHCoord a = {47.497, 19.040};
SHCoord b = {48.208, 16.373};

// Check validity
if (sh_coord_valid(a)) {
    printf("Valid coordinate\n");
}

// Midpoint between two coordinates
SHCoord mid = sh_coord_midpoint(a, b);

// Bearing from a to b (degrees, 0-360)
double bearing = sh_bearing(a, b);

// Destination point given start, bearing, distance
SHCoord dest = sh_destination(a, 270.0, 50000);  // 50km west
```

### Bounding Box Operations

```c
#include "shared.h"

// Initialize empty bounding box
SHBBox bbox;
sh_bbox_init(&bbox);

// Expand to include coordinates
SHCoord p1 = {47.0, 19.0};
SHCoord p2 = {48.0, 20.0};
sh_bbox_expand(&bbox, p1);
sh_bbox_expand(&bbox, p2);

// Check if coordinate is in bbox
SHCoord test = {47.5, 19.5};
if (sh_coord_in_bbox(test, bbox)) {
    printf("Point is inside bbox\n");
}

// Check bbox intersection
SHBBox other = {47.5, 19.5, 48.5, 20.5};
if (sh_bbox_intersects(bbox, other)) {
    printf("Bounding boxes overlap\n");
}

// Union of two bboxes
SHBBox combined = sh_bbox_union(bbox, other);
```

### Web Mercator Projection

```c
#include "shared.h"

double lat = 47.497, lon = 19.040;

// Convert lat/lon to Web Mercator (EPSG:3857)
double x, y;
sh_latlon_to_mercator(lat, lon, &x, &y);
printf("Mercator: %.2f, %.2f\n", x, y);

// Convert back
double lat2, lon2;
sh_mercator_to_latlon(x, y, &lat2, &lon2);

// Convert to tile coordinates at zoom level
int tile_x, tile_y;
sh_latlon_to_tile(lat, lon, 14, &tile_x, &tile_y);
printf("Tile: %d/%d/%d\n", 14, tile_x, tile_y);

// Get bounding box for a tile
SHBBox tile_bbox = sh_tile_bounds(14, tile_x, tile_y);
```

### Compiling Your Program

```bash
gcc -O3 -I./include myprogram.c -L. -lshared -lm -o myprogram
```

## API Reference

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `SH_EARTH_RADIUS_M` | 6371000.0 | Earth's mean radius (WGS-84) |
| `SH_DEG_TO_RAD` | π/180 | Degrees to radians |
| `SH_RAD_TO_DEG` | 180/π | Radians to degrees |
| `SH_COORD_SCALE` | 1e7 | Fixed-point scale factor |

### Types

| Type | Description |
|------|-------------|
| `SHCoord` | Floating-point coordinate (lat/lon in degrees) |
| `SHCoordFixed` | Fixed-point coordinate (lat/lon × 1e7) |
| `SHBBox` | Bounding box (min/max lat/lon) |

### Distance Functions

| Function | Description |
|----------|-------------|
| `sh_haversine(a, b)` | Great-circle distance (meters) |
| `sh_distance_fast(a, b)` | Fast approximation (meters) |
| `sh_haversine_fixed(a, b)` | Haversine for fixed-point |
| `sh_distance_fast_fixed(a, b)` | Fast distance for fixed-point |

### Coordinate Functions

| Function | Description |
|----------|-------------|
| `sh_coord_valid(c)` | Check if coordinate is valid |
| `sh_coord_in_bbox(c, bbox)` | Check if in bounding box |
| `sh_coord_midpoint(a, b)` | Midpoint between two coords |
| `sh_bearing(a, b)` | Initial bearing (degrees) |
| `sh_destination(start, bearing, dist)` | Destination point |

### Bounding Box Functions

| Function | Description |
|----------|-------------|
| `sh_bbox_init(bbox)` | Initialize to empty |
| `sh_bbox_valid(bbox)` | Check if valid (non-empty) |
| `sh_bbox_expand(bbox, c)` | Expand to include coord |
| `sh_bbox_intersects(a, b)` | Check intersection |
| `sh_bbox_union(a, b)` | Compute union |

### Projection Functions

| Function | Description |
|----------|-------------|
| `sh_latlon_to_mercator(lat, lon, x, y)` | To Web Mercator |
| `sh_mercator_to_latlon(x, y, lat, lon)` | From Web Mercator |
| `sh_latlon_to_tile(lat, lon, zoom, x, y)` | To tile coordinates |
| `sh_tile_bounds(zoom, x, y)` | Tile bounding box |

## Performance Notes

| Function | Relative Speed | Accuracy |
|----------|---------------|----------|
| `sh_haversine` | 1x | Exact |
| `sh_distance_fast` | 3-5x faster | ~0.5% error (<500km) |
| `sh_haversine_fixed` | 1.2x | Exact |

## Dependencies

- **External**: None
- **Standard Library**: math.h (sin, cos, atan2, sqrt)
