# Shared Library

Common utilities for the OTTO platform, used by velo and carta modules.

## Features

- **Geographic calculations**: Haversine distance, bearing, destination
- **Coordinate types**: Floating-point and fixed-point (OSM compatible)
- **Bounding box operations**: Intersection, union, containment
- **Web Mercator projection**: Lat/lon to tile coordinates
- **Protobuf encoding/decoding**: Varints, signed varints, tags, packed arrays
- **Zlib compression**: Inflate/deflate via miniz
- **PBF parsing**: OSM PBF blob parsing, string tables

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

### Protobuf Functions

| Function | Description |
|----------|-------------|
| `sh_pb_read_varint(buf, len, &value)` | Read unsigned varint |
| `sh_pb_read_svarint(buf, len, &value)` | Read signed varint (zigzag) |
| `sh_pb_read_tag(buf, len, &field, &wire)` | Read protobuf tag |
| `sh_pb_skip_field(buf, len, wire)` | Skip unknown field |
| `sh_pb_write_varint(buf, cap, value)` | Write unsigned varint |
| `sh_pb_write_svarint(buf, cap, value)` | Write signed varint |
| `sh_pb_delta_decode_i64(arr, count)` | Delta decode array in-place |

### Inflate/Deflate Functions

| Function | Description |
|----------|-------------|
| `sh_inflate(src, src_len, dst, dst_len, &actual)` | Decompress zlib data |
| `sh_inflate_raw(src, src_len, dst, dst_len, &actual)` | Decompress raw DEFLATE |
| `sh_inflate_alloc(src, src_len, expected, &actual)` | Decompress with allocation |
| `sh_deflate(src, src_len, dst, cap, &actual, level)` | Compress to zlib format |

### PBF Parsing Functions

| Function | Description |
|----------|-------------|
| `sh_string_table_init(st)` | Initialize string table |
| `sh_string_table_add(st, data, len)` | Add string to table |
| `sh_string_table_get(st, idx)` | Get string by index |
| `sh_string_table_free(st)` | Free string table |
| `sh_pbf_parse_blob_header(data, len, ...)` | Parse PBF blob header |
| `sh_pbf_decompress_blob(data, len, &blob)` | Decompress PBF blob |

## Dependencies

- **Vendored**: ../vendor/miniz (for zlib compression)
