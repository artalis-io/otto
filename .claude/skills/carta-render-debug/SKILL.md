# Carta Render Debug Skill

Debug and fix rendering issues in the Carta map tile generator.

**Trigger:** Use when asked to fix rendering bugs, compare tiles, debug tile output, or improve render quality in carta.

## Quick Start

```bash
cd /Users/mark/Desktop/work/artalis-io/otto/carta

# 1. Build the comparison tool
make compare

# 2. Get map info and valid tile ranges
./carta-compare info ../data/monaco-latest.osm.pbf

# 3. Generate tiles for comparison
./carta-compare 14/8529/5974 ../data/monaco-latest.osm.pbf

# 4. Test with render presets
./carta-compare 14/8529/5974 ../data/monaco-latest.osm.pbf --preset fast
```

## Render Presets

Carta supports three render presets that control visual quality vs performance:

| Preset | Render Time | Features |
|--------|-------------|----------|
| `default` | ~150ms | Full quality: labels, boundaries, casing, outlines |
| `fast` | ~10-15ms | No boundaries/casing/outlines; labels enabled |
| `quality` | ~200ms | Maximum quality: all effects, low zoom cutoffs |

**Use `--preset fast` for performance testing and tile server deployments.**

```bash
# Compare presets
./carta-compare 14/9058/5729 ../data/hungary-latest.osm.pbf --preset default
./carta-compare 14/9058/5729 ../data/hungary-latest.osm.pbf --preset fast --skip-osm
```

### Preset Details

**Default preset:**
- All layers enabled (water, roads, buildings, landuse, boundaries, labels)
- Road casing, building outlines, label halos enabled
- Boundary dashes enabled

**Fast preset (recommended for tile servers):**
- Boundaries disabled (expensive relation processing)
- Road casing, railway casing, bridge outlines disabled
- Building outlines disabled
- Labels enabled (useful for navigation)
- ~10x faster than default

**Quality preset:**
- All effects enabled at lower zoom cutoffs
- Maximum visual fidelity

## Workflow

### Phase 1: Identify the Issue

1. **Use carta-compare to generate tiles:**
   ```bash
   ./carta-compare <z/x/y> <pbf-file> -o /tmp/carta_debug/
   ```

2. **Read both tiles to compare:**
   - Carta output: `/tmp/carta_debug/carta_<z>_<x>_<y>.png`
   - OSM reference: `/tmp/carta_debug/osm_<z>_<x>_<y>.png`

3. **Check the TILE REPORT for feature counts:**
   ```
   --- FEATURE COUNTS ---
   Water features: N
   Road features: N
   Building features: N
   Landuse features: N
   ```

4. **Document the issue:**
   - What's wrong (missing features, wrong colors, clipping artifacts, etc.)
   - Which zoom levels affected
   - Which feature types affected

### Phase 2: Locate the Bug

**Key source files by render stage:**

| Stage | File | What it does |
|-------|------|--------------|
| PBF parsing | `src/ct_pbf.c` | Extracts features from OSM data |
| Spatial query | `src/ct_rtree.c` | R-tree for tile bbox queries |
| LOD filtering | `src/ct_lod.c` | Zoom-based feature filtering |
| Coord transform | `src/ct_tile.c` | Lat/lon to pixel conversion |
| Geometry clip | `src/ct_tile.c` | Clip to tile bounds |
| Simplification | `src/ct_simplify.c` | Reduce point count |
| Polygon fill | `src/ct_render.c` | Scanline fill algorithm |
| Line drawing | `src/ct_render.c` | Anti-aliased lines |
| PNG encoding | `src/ct_png.c` | RGBA to PNG |
| Styling | `src/ct_style.c` | Colors and widths |

**Common bug patterns:**

| Symptom | Likely cause | Check |
|---------|--------------|-------|
| Missing features | R-tree query or LOD filtering | ct_pbf.c, ct_lod.c |
| Wrong colors | Style configuration | ct_style.c |
| Partial polygons | Clipping bug | ct_tile.c:ct_clip_* |
| Jagged edges | Simplification too aggressive | ct_simplify.c |
| Y-offset | Mercator projection bug | ct_tile.c |
| Tile boundary artifacts | Cross-tile clipping | ct_render.c |

### Phase 3: Fix the Bug

**IMPORTANT: Before writing any code, run /c-audit on the file you're modifying.**

```
/c-audit carta/src/ct_render.c
```

**Code change rules (from c-audit):**

1. **Memory safety:**
   - Use `snprintf` not `sprintf`
   - Use `strncpy` + explicit null terminator
   - Check malloc/calloc return values
   - Free resources on all error paths

2. **Bounds checking:**
   - Validate array indices before access
   - Check pointer validity before dereference

3. **Performance:**
   - Don't add unnecessary allocations in hot paths
   - Avoid O(n²) algorithms in render loops

4. **Testing:**
   - Run `make test` after changes
   - Verify render time doesn't regress significantly

### Phase 4: Verify the Fix

1. **Rebuild carta:**
   ```bash
   make clean && make all
   make compare
   ```

2. **Re-run the comparison:**
   ```bash
   ./carta-compare <same-tile> <same-pbf>
   ```

3. **Read the new tile and compare with OSM reference**

4. **Check performance didn't regress:**
   ```
   --- PERFORMANCE ---
   Render time: X.XX ms  # Should not increase significantly
   ```

5. **Run test suite:**
   ```bash
   make test
   ```

## Known Good Test Tiles

### Monaco (small, fast to load)

| Tile | Content | Size |
|------|---------|------|
| `14/8529/5974` | City center, buildings, roads | ~260 KB |
| `14/8530/5973` | Port area, water, roads | ~140 KB |
| `14/8529/5973` | Northern Monaco | ~50 KB |
| `14/8528/5974` | Western edge | ~6 KB |

```bash
# Monaco: Load time ~1s, good for quick iteration
./carta-compare 14/8529/5974 ../data/monaco-latest.osm.pbf
```

### Hungary (larger, more feature types)

| Tile | Content | Size |
|------|---------|------|
| `14/9058/5729` | Lake Balaton area, water, forests | ~325 KB |
| `14/9057/5730` | Tihany peninsula | ~280 KB |
| `14/9059/5728` | Northern shore | ~200 KB |

```bash
# Hungary: Load time ~30s, use for comprehensive testing
./carta-compare 14/9058/5729 ../data/hungary-latest.osm.pbf
```

## Debugging Specific Issues

### Water not rendering

1. Check if water features are being extracted:
   ```bash
   ./carta-compare <tile> <pbf> 2>&1 | grep "Water features"
   ```

2. If count is 0, check `ct_pbf.c` tag filtering for `natural=water`, `waterway=*`

3. If count > 0 but not visible, check `ct_render.c` polygon fill and `ct_style.c` water color

### Roads missing or wrong width

1. Check road counts in TILE REPORT

2. Check `ct_style.c:ct_default_style()` for road width definitions

3. Check `ct_lod.c` min_zoom settings for road types

4. Check `ct_render.c:ct_render_polyline_cased()` for line drawing

### Polygon clipping artifacts

1. Look for debug messages like:
   ```
   ct_render_polygon: polygon entirely outside tile
   ```

2. Check `ct_tile.c:ct_clip_polygon()` and `ct_clip_multipolygon()`

3. Verify clip buffer size (typically 64 pixels)

### Label placement issues

1. Check `ct_label.c` for collision detection

2. Check `ct_collision.c` for bounding box calculations

3. Check font rendering in `ct_render.c:ct_render_text()`

## LOD (Level of Detail) Debugging

Use zoom-range comparison to debug LOD filtering issues - features appearing/disappearing at wrong zoom levels.

### Quick LOD Check

```bash
# Compare same location across zoom 10-16
./carta-compare 14/9058/5729 ../data/hungary-latest.osm.pbf --zoom-range 10-16 --skip-osm

# With 3x3 neighbors for broader coverage
./carta-compare 14/9058/5729 ../data/hungary-latest.osm.pbf --zoom-range 12-15 -N --skip-osm
```

### Understanding Zoom-Range Output

The tool calculates parent/child tiles at each zoom level from your anchor tile:

```
=== Zoom Range Comparison ===

Anchor tile: 14/9058/5729
Zoom range: 12 to 15

--- Zoom 12 (center: 12/2264/1432) ---
  12/2264/1432: 193.9 KB, 16.6 ms [w:73 r:3314 b:0 l:353]

--- Zoom 13 (center: 13/4529/2864) ---
  13/4529/2864: 328.4 KB, 15.8 ms [w:37 r:2739 b:252 l:280]
```

Key output fields: `[w:water r:roads b:buildings l:landuse]`

### OSM Carto LOD Reference

When debugging LOD, compare against OSM Carto conventions:

| Feature Type | OSM Carto min_zoom | Carta default |
|--------------|-------------------|---------------|
| Motorway | 5 | 5 |
| Trunk | 6 | 6 |
| Primary | 8 | 8 |
| Secondary | 10 | 10 |
| Tertiary | 12 | 12 |
| Residential | 13 | 13 |
| Service | 14 | 14 |
| Buildings | 13 | 13 |
| Industrial landuse | 10 | 12 |
| Residential landuse | 10 | 14 (delayed to reduce clutter) |

### Common LOD Issues

| Symptom | Likely Cause | Check |
|---------|--------------|-------|
| Roads appear too late | min_zoom too high | `ct_lod.c:ct_lod_default()` |
| Buildings appear too early | min_zoom too low | LOD rule for CT_LAYER_BUILDINGS |
| Too cluttered at mid-zoom | Landuse showing early | Delay residential/grass landuse |
| Features disappear at high zoom | max_zoom set incorrectly | Check max_zoom in rules |
| Feature count drops unexpectedly | LOD area/length filter | Check area_sqm, length_m thresholds |

### LOD Debugging Workflow

1. **Identify the zoom transition:**
   ```bash
   ./carta-compare 14/9058/5729 hungary.osm.pbf --zoom-range 10-16 --skip-osm
   ```
   Look for sudden drops in feature counts (e.g., buildings go from 0 to 252 between z12 and z13).

2. **Compare with OSM at problem zooms:**
   ```bash
   ./carta-compare 13/4529/2864 hungary.osm.pbf -o /tmp/z13_check/
   ```
   Read both tiles and compare visually.

3. **Check LOD rules in `ct_lod.c`:**
   ```c
   // Example: buildings visible from z13
   ct_lod_add_rule(config, CT_LAYER_BUILDINGS, 0, 13, -1, 0, 0);
   ```

4. **Verify fix across zoom range:**
   ```bash
   ./carta-compare 14/9058/5729 hungary.osm.pbf --zoom-range 11-15 -N
   ```

### LOD Rule Format

```c
ct_lod_add_rule(config, layer, feature_type, min_zoom, max_zoom, min_area, min_length);
```

| Parameter | Description |
|-----------|-------------|
| `layer` | CT_LAYER_ROADS, CT_LAYER_BUILDINGS, etc. |
| `feature_type` | Sub-type (e.g., CT_ROAD_PRIMARY) or 0 for all |
| `min_zoom` | First zoom level where feature appears |
| `max_zoom` | Last zoom level (-1 = no limit) |
| `min_area` | Minimum area in m² (for polygons) |
| `min_length` | Minimum length in m (for lines) |

### Zoom-Range Summary Report

The tool provides a summary table for quick analysis:

```
Zoom | Tiles | Empty | Avg Time | Features | Water | Roads | Bldgs | Land
-----|-------|-------|----------|----------|-------|-------|-------|-----
  12 |     1 |     0 |   16.6 ms |     3740 |    73 |  3314 |     0 |  353
  13 |     1 |     0 |   15.8 ms |     3308 |    37 |  2739 |   252 |  280
  14 |     1 |     0 |   17.0 ms |     4279 |    16 |  2556 |  1517 |  190
```

Look for:
- **Sudden feature jumps** between zooms (LOD threshold)
- **Decreasing counts** at higher zoom (expected: tiles cover smaller area)
- **Zero counts** where features should exist (LOD too restrictive)

## Tool Reference

### carta-compare commands

```bash
# Show map bounds and suggest tiles
./carta-compare info <pbf-file>

# Compare single tile
./carta-compare <z/x/y> <pbf-file> [options]

# Batch compare multiple tiles
./carta-compare batch <pbf-file> [options]

# Zoom-range comparison (LOD debugging)
./carta-compare <z/x/y> <pbf-file> --zoom-range MIN-MAX [options]

Options:
  --mvt              Generate MVT instead of PNG
  -o, --output DIR   Output directory (default: /tmp/carta_compare)
  -s, --size SIZE    PNG tile size: 256 or 512 (default: 512)
  -z, --zoom LEVEL   Batch mode: specific zoom level
  -n, --max-tiles N  Batch mode: max tiles per zoom (default: 3)
  --skip-osm         Don't fetch OSM reference tiles
  --preset PRESET    Render preset: default, fast, quality

Zoom-range options:
  --zoom-range MIN-MAX   Compare tile across zoom levels MIN to MAX
  -N, --neighbors        Render 3x3 neighbor grid at each zoom level
```

### Reading generated tiles

The tool saves tiles to the output directory. Use the Read tool to view them:

```
Carta output: /tmp/carta_compare/carta_<z>_<x>_<y>.png
OSM reference: /tmp/carta_compare/osm_<z>_<x>_<y>.png
```

## Performance Baseline

### Fast Preset (recommended for production)

| Operation | Target | Notes |
|-----------|--------|-------|
| Monaco tile (z14) | <15 ms | City center with 2000+ features |
| Hungary tile (z14) | <15 ms | Budapest with buildings/roads |
| Empty tile | <3 ms | Background only |

### Default Preset (full quality)

| Operation | Target | Notes |
|-----------|--------|-------|
| Monaco tile (z14) | <50 ms | With boundaries, casing, outlines |
| Hungary tile (z14) | <150 ms | Full quality rendering |
| Empty tile | <5 ms | Background only |

If render time exceeds these by >50%, investigate for performance regression.

**Note:** The `fast` preset is ~10x faster than `default` by disabling:
- Boundary rendering (expensive relation processing)
- Road/railway casing
- Building/bridge outlines

## Checklist Before Committing

- [ ] Ran `/c-audit` on modified files
- [ ] Fixed all Critical and High severity issues
- [ ] `make test` passes (110 tests for carta)
- [ ] Render time not significantly worse
- [ ] Visual comparison shows improvement
- [ ] No new compiler warnings with `-Wall -Wextra`
