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
```

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

## Tool Reference

### carta-compare commands

```bash
# Show map bounds and suggest tiles
./carta-compare info <pbf-file>

# Compare single tile
./carta-compare <z/x/y> <pbf-file> [options]

# Batch compare multiple tiles
./carta-compare batch <pbf-file> [options]

Options:
  --mvt              Generate MVT instead of PNG
  -o, --output DIR   Output directory (default: /tmp/carta_compare)
  -s, --size SIZE    PNG tile size: 256 or 512 (default: 512)
  -z, --zoom LEVEL   Batch mode: specific zoom level
  -n, --max-tiles N  Batch mode: max tiles per zoom (default: 3)
  --skip-osm         Don't fetch OSM reference tiles
```

### Reading generated tiles

The tool saves tiles to the output directory. Use the Read tool to view them:

```
Carta output: /tmp/carta_compare/carta_<z>_<x>_<y>.png
OSM reference: /tmp/carta_compare/osm_<z>_<x>_<y>.png
```

## Performance Baseline

| Operation | Target | Notes |
|-----------|--------|-------|
| Monaco tile (z14) | <20 ms | City center with 2000+ features |
| Hungary tile (z14) | <25 ms | Lake Balaton with water/forests |
| Empty tile | <3 ms | Background only |

If render time exceeds these by >50%, investigate for performance regression.

## Checklist Before Committing

- [ ] Ran `/c-audit` on modified files
- [ ] Fixed all Critical and High severity issues
- [ ] `make test` passes (110 tests for carta)
- [ ] Render time not significantly worse
- [ ] Visual comparison shows improvement
- [ ] No new compiler warnings with `-Wall -Wextra`
