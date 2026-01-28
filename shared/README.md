# Shared Library

Common utilities for the FuelWise platform, used by velo and carta modules.

## Features

- **Geographic calculations**: Haversine distance, bearing, destination
- **Coordinate types**: Floating-point and fixed-point (OSM compatible)
- **Bounding box operations**: Intersection, union, containment
- **Web Mercator projection**: Lat/lon to tile coordinates

## Building

```bash
make          # Build library and tests
make test     # Run test suite
make clean    # Remove build artifacts
```

## Usage

```c
#include "shared.h"

// Distance calculation
SHCoord budapest = {47.4979, 19.0402};
SHCoord vienna = {48.2082, 16.3738};
double dist = sh_haversine(budapest, vienna);  // ~214 km

// Tile coordinates
int tx, ty;
sh_latlon_to_tile(47.4979, 19.0402, 14, &tx, &ty);  // 9058, 5729

// Bounding box
SHBBox bbox = sh_tile_bounds(14, tx, ty);
```

## API Reference

### Distance Functions

| Function | Description |
|----------|-------------|
| `sh_haversine(a, b)` | Great-circle distance in meters |
| `sh_distance_fast(a, b)` | Fast equirectangular approximation |
| `sh_haversine_fixed(a, b)` | Haversine for fixed-point coords |

### Coordinate Functions

| Function | Description |
|----------|-------------|
| `sh_coord_valid(c)` | Check if coordinate is valid |
| `sh_coord_in_bbox(c, bbox)` | Check if coordinate is in bbox |
| `sh_coord_midpoint(a, b)` | Calculate midpoint |
| `sh_bearing(a, b)` | Initial bearing (0-360 degrees) |
| `sh_destination(start, bearing, dist)` | Destination from start |

### Bounding Box Functions

| Function | Description |
|----------|-------------|
| `sh_bbox_init(bbox)` | Initialize empty bbox |
| `sh_bbox_valid(bbox)` | Check if bbox is non-empty |
| `sh_bbox_expand(bbox, c)` | Expand bbox to include coord |
| `sh_bbox_intersects(a, b)` | Check if two bboxes intersect |
| `sh_bbox_union(a, b)` | Compute union of two bboxes |

### Web Mercator Functions

| Function | Description |
|----------|-------------|
| `sh_latlon_to_mercator(lat, lon, &x, &y)` | Convert to Mercator meters |
| `sh_mercator_to_latlon(x, y, &lat, &lon)` | Convert from Mercator |
| `sh_latlon_to_tile(lat, lon, z, &x, &y)` | Get tile coordinates |
| `sh_tile_bounds(z, x, y)` | Get tile bounding box |

## Dependencies

None - this is a zero-dependency library.
