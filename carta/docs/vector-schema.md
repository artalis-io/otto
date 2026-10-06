# Carta vector tile schema

Carta emits Mapbox Vector Tiles (MVT 2.1) at `/tiles/{z}/{x}/{y}.mvt`. This
document defines the layers and feature properties. **This is Carta's own schema,
not OpenMapTiles**: layer and property names are chosen for Carta's data and must
not be assumed compatible with OMT styles. Only attributes Carta actually retains
are exported; nothing is synthesized.

All strings are UTF-8 and are copied verbatim into the tile (accented names such
as Hungarian `ő`/`ű` round-trip byte-for-byte). Tile extent is 4096 unless
overridden. Property absence is meaningful: a property is simply omitted when the
source value is missing (see "missing-value behavior" per layer).

## Source layers

| Layer | Geometry | Source |
|-------|----------|--------|
| `roads` | LineString (areas as Polygon) | highway ways |
| `water` | Polygon / LineString | water bodies, riverbanks, waterways |
| `landuse` | Polygon | landuse / natural / leisure areas |
| `railways` | LineString | railway ways |
| `buildings` | Polygon | building ways |
| `boundaries` | LineString | administrative boundaries |
| `labels` | Point | place labels (cities/towns/villages/peaks/...) |

Geometry layers are emitted only when the tile contains features of that layer;
`labels` is emitted only when the tile (by zoom + bbox) has labeled points.

## Feature properties

### `roads`
| Key | Type | Values / notes | Missing-value behavior |
|-----|------|----------------|------------------------|
| `class` | string | `motorway`, `trunk`, `primary`, `secondary`, `tertiary`, `residential`, `service`, `other` | always present (unknown subtypes -> `other`) |
| `name` | string | road name (UTF-8) | omitted if the way is unnamed |
| `bridge` | bool | `true` when the segment is on a bridge | omitted (i.e. not a bridge) when false |
| `tunnel` | bool | `true` when the segment is in a tunnel | omitted when false |

### `water`
| Key | Type | Values / notes | Missing-value behavior |
|-----|------|----------------|------------------------|
| `class` | string | `river`, `canal`, `stream`, `drain`, `ditch`, `water` (lake/reservoir/pond), `riverbank`, `other` | always present |
| `name` | string | water body / waterway name | omitted if unnamed |

### `landuse`
| Key | Type | Values / notes | Missing-value behavior |
|-----|------|----------------|------------------------|
| `class` | string | `forest`, `park`, `residential`, `commercial`, `industrial`, `farmland`, `grass`, `cemetery`, `military`, `other` | always present |
| `name` | string | area name | omitted if unnamed |

### `railways`
| Key | Type | Values / notes | Missing-value behavior |
|-----|------|----------------|------------------------|
| `class` | string | `rail`, `subway`, `tram`, `narrow_gauge`, `preserved`, `disused`, `other` | always present |
| `name` | string | line name | omitted if unnamed |

### `buildings`, `boundaries`
`buildings` carries `name` when present (no `class` yet). `boundaries` carries
`name` when present. Additional attributes may be added later; consumers must
tolerate missing keys.

### `labels` (place points)
| Key | Type | Values / notes | Missing-value behavior |
|-----|------|----------------|------------------------|
| `name` | string | place name (UTF-8) | omitted if unnamed (rare) |
| `place_type` | string | `country`, `state`, `city`, `town`, `village`, `hamlet`, `suburb`, `neighbourhood`, `locality`, `island`, `peak`, `other` | always present |
| `rank` | int | label priority (higher = more important); from Carta's label ranking | always present |
| `population` | int | population | omitted when 0 / unknown |
| `min_zoom` | int | minimum zoom Carta assigns for display | omitted when 0 |

## Encoding notes (for implementers)

- Properties use the standard MVT out-of-line representation: per-layer `keys`
  (field 3, strings) and `values` (field 4, typed `Value` messages); each feature
  carries packed `[key_index, value_index]` tags (field 2). Keys and values are
  deduplicated per layer.
- Value types used: `string_value` (1), `int_value` (4, plain varint int64 for
  `rank`/`population`/`min_zoom`), `bool_value` (7). No float/double are emitted.
- `name` is a borrowed pointer through the pipeline (owned by the PBF/index
  context); the encoder copies the bytes into the tile, so emitted tiles hold no
  reference to the context.

## Compatibility

- The raster (PNG) path and `ct_encode_mvt()` geometry output are unchanged;
  adding properties is additive and backward compatible for existing MVT
  consumers that ignore tags.
- Do not rely on OpenMapTiles layer/field names. Style against the names above.
