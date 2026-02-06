# Boundary Relation Extraction Plan

## Overview

Extract `boundary=administrative` relations from OSM PBF to render international/state/county borders.

## Current State

Carta already has infrastructure for relation processing:
- `ct_pbf.c:parse_relation()` - Extracts relation ID, member IDs/types/roles, tags
- `ct_multipolygon.c` - Stitches way segments into closed rings
- `CTOSMRelation` struct stores relation data with member references

Currently, only `type=multipolygon` relations are kept (line 1453). Boundary relations have:
- `type=boundary` (most common) or `boundary=administrative`
- `admin_level=N` tag (2=country, 4=state, 6=county, 8=city, 10=suburb)

## Key Differences from Multipolygons

| Aspect | Multipolygons | Boundaries |
|--------|---------------|------------|
| Geometry | Closed rings (polygons) | Open linestrings |
| Output | CTAssembledMultipolygon | Array of linestrings |
| Roles | "outer", "inner" | "outer", "inner", or empty |
| Rendering | Polygon fill | Dashed/solid line |
| Continuity | Must close | Can be open segments |

## Questions for Reviewer

### 1. Admin Level Priority
Which admin levels should we support?

**Options:**
- A) All levels (2-10): Most complete, but adds clutter at low zoom
- B) Country + State only (2-4): Minimal, trucking-relevant borders
- C) Country through County (2-6): Good balance

**Recommendation:** Option C - Country through County. Trucking needs customs borders (country), regional regulations (state), and local restrictions (county).

### 2. Memory Strategy
How should we store assembled boundaries?

**Options:**
- A) Separate `ctx->boundaries` array: Clean separation, easy to query by admin_level
- B) Store as CTFeature with CT_LAYER_BOUNDARIES: Unified with existing features
- C) Both: Store raw segments + pre-assembled linestrings

**Recommendation:** Option A for storage, then convert to CTFeature during tile generation. This matches the multipolygon pattern.

### 3. Partial Boundaries
What about boundaries that extend beyond our data (e.g., Hungary-Romania border)?

**Options:**
- A) Render available segments only (may look broken)
- B) Mark boundaries as "incomplete" and style differently
- C) Extend to tile edges (cosmetic completion)

**Recommendation:** Option A for now. Partial rendering is better than nothing.

### 4. Disputed/Special Boundaries
Should we handle:
- `disputed=yes` boundaries
- `maritime=yes` boundaries
- `boundary=protected_area` (national parks)

**Recommendation:** Start with just `boundary=administrative`. Add protected areas in Phase 2 since they'd also help with national park rendering.

## Implementation Plan

### Phase 1: Parse Boundary Relations (ct_pbf.c)

**Estimated effort:** 1-2 hours

```c
// In parse_relation(), after checking for multipolygon:

/* Check if this is a boundary relation */
int is_boundary = 0;
int admin_level = -1;

for (int i = 0; i < num_tags; i++) {
    const char *key = sh_string_table_get(st, keys[i]);
    const char *val = sh_string_table_get(st, vals[i]);

    if (strcmp(key, "type") == 0 && strcmp(val, "boundary") == 0) {
        is_boundary = 1;
    }
    if (strcmp(key, "boundary") == 0 && strcmp(val, "administrative") == 0) {
        is_boundary = 1;
    }
    if (strcmp(key, "admin_level") == 0) {
        admin_level = atoi(val);
    }
}

if (is_boundary && admin_level >= 2 && admin_level <= 6) {
    // Store boundary relation
}
```

**Changes:**
1. Add `CTOSMBoundary` struct to `ct_types.h`
2. Add `boundaries`, `num_boundaries`, `boundaries_capacity` to `CTPBFContext`
3. Extend `parse_relation()` to detect and store boundary relations
4. Store only way members (ignore node/relation members)

### Phase 2: Stitch Boundary Segments (ct_boundary.c - new file)

**Estimated effort:** 2-3 hours

Create `ct_boundary.c` with functions:

```c
// Stitch way segments into continuous boundary lines
CTStatus ct_assemble_boundaries(CTPBFContext *ctx);

// Get boundaries intersecting a bounding box
CTStatus ct_boundary_query(const CTPBFContext *ctx, CTBBox bbox,
                           CTAssembledBoundary **out, size_t *count);
```

**Algorithm:**
```
1. For each boundary relation:
   a. Collect all way member coordinates from way_map
   b. Build adjacency: endpoint -> [ways with that endpoint]
   c. Greedy stitch: start at any unused way, extend both directions
   d. Output: array of linestrings (may be multiple disconnected segments)
2. Store assembled boundaries with:
   - admin_level
   - bbox (for R-tree indexing)
   - coordinate array
   - name (optional)
```

**Key difference from multipolygon ring building:**
- Don't require closure (boundaries can be open)
- Stitch in both directions from starting segment
- Keep disconnected segments as separate linestrings

### Phase 3: R-tree Index for Boundaries (ct_pbf.c)

**Estimated effort:** 1 hour

Add `boundary_rtree` to `CTPBFContext`, built after assembly:

```c
// In ct_pbf_load() after ct_assemble_multipolygons():
if (ct_assemble_boundaries(ctx) == CT_OK) {
    ct_build_boundary_rtree(ctx);
}
```

Reuse existing R-tree infrastructure from `ct_rtree.c`.

### Phase 4: Render Boundaries (ct_render.c)

**Estimated effort:** 1-2 hours

Update `ct_render_from_pbf()`:

```c
// After rendering other features:
ct_render_boundaries(ctx, pbf, coord);
```

Rendering function:
```c
static void ct_render_boundaries(CTRenderContext *ctx,
                                  const CTPBFContext *pbf,
                                  CTTileCoord coord)
{
    CTBBox bbox = ct_tile_bounds(coord);
    CTAssembledBoundary *boundaries;
    size_t count;

    if (ct_boundary_query(pbf, bbox, &boundaries, &count) != CT_OK) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        CTAssembledBoundary *b = &boundaries[i];

        // Check LOD visibility
        if (!ct_lod_is_visible(&ctx->lod, CT_LAYER_BOUNDARIES,
                               b->admin_level, coord.z, 0, b->length_m)) {
            continue;
        }

        // Transform to tile coordinates
        // Render as dashed line (admin_level determines style)
    }
}
```

### Phase 5: Dashed Line Rendering (ct_render.c)

**Estimated effort:** 1-2 hours

Add `ct_render_polyline_dashed()`:

```c
void ct_render_polyline_dashed(CTRenderContext *ctx,
                                const CTTilePoint *points, int num_points,
                                CTColor color, float width,
                                float dash_length, float gap_length);
```

Style by admin_level:
- Level 2 (country): Purple, 2px, dash 10-5
- Level 4 (state): Purple, 1.5px, dash 6-4
- Level 6 (county): Gray, 1px, dash 4-3

### Phase 6: carta-compare Update (tools/carta_compare.c)

**Estimated effort:** 30 mins

Add boundary count to tile statistics:
```c
typedef struct {
    // ...existing fields...
    int boundary_count;
} TileStats;
```

## File Changes Summary

| File | Changes |
|------|---------|
| `ct_types.h` | Add `CTOSMBoundary`, `CTAssembledBoundary` structs |
| `ct_pbf.c` | Parse boundary relations, add `boundaries` storage |
| `ct_boundary.c` | **New file** - Boundary assembly |
| `ct_boundary.h` | **New file** - Boundary API |
| `ct_rtree.c` | Minor: ensure compatible with boundary indexing |
| `ct_render.c` | Add `ct_render_boundaries()`, dashed line rendering |
| `ct_style.c` | Add boundary colors/widths by admin_level |
| `carta_compare.c` | Add boundary stats |
| `Makefile` | Add `ct_boundary.o` |

## Data Structures

```c
// ct_types.h

/* Assembled boundary linestring */
typedef struct {
    int64_t relation_id;
    int admin_level;          /* 2=country, 4=state, 6=county */
    CTCoord *coords;
    int num_coords;
    CTBBox bbox;
    float length_m;           /* For LOD filtering */
    char *name;               /* Optional: boundary name */
} CTAssembledBoundary;

/* Add to CTPBFContext */
typedef struct CTPBFContext {
    // ...existing fields...

    /* Boundary relations (raw) */
    CTOSMRelation *boundary_relations;
    size_t num_boundary_relations;
    size_t boundary_relations_capacity;

    /* Assembled boundaries (stitched linestrings) */
    CTAssembledBoundary *boundaries;
    size_t num_boundaries;
    size_t boundaries_capacity;

    /* R-tree for boundary queries */
    CTRTree *boundary_rtree;
} CTPBFContext;
```

## Testing Plan

1. **Unit tests** (`tests/test_carta.c`):
   - Boundary relation parsing
   - Way stitching (open linestrings)
   - Dashed line rendering

2. **Visual tests** (carta-compare):
   - Hungary z7-z10: Should show Hungary-Romania, Hungary-Serbia borders
   - Monaco: Should show France border

3. **Performance** (benchmark):
   - Hungary: Measure boundary assembly time
   - Ensure no regression in tile generation

## Memory Estimates

| Region | Boundary Relations | Assembled Lines | Memory |
|--------|-------------------|-----------------|--------|
| Monaco | ~2 | ~2 | <1 KB |
| Hungary | ~50-100 | ~20-50 | ~50-100 KB |
| Germany | ~500 | ~200 | ~500 KB |
| Europe | ~5000 | ~2000 | ~5 MB |

Boundaries are sparse compared to ways/multipolygons, so memory impact is minimal.

## Estimated Total Effort

| Phase | Effort |
|-------|--------|
| Phase 1: Parse | 1-2 hours |
| Phase 2: Stitch | 2-3 hours |
| Phase 3: R-tree | 1 hour |
| Phase 4: Render | 1-2 hours |
| Phase 5: Dashed lines | 1-2 hours |
| Phase 6: carta-compare | 30 mins |
| Testing | 1-2 hours |
| **Total** | **8-12 hours** |

## Future Enhancements (Not in Scope)

1. **Protected areas** (`boundary=protected_area`): National parks, nature reserves
2. **Maritime boundaries**: Territorial waters, EEZ
3. **Disputed boundaries**: Special styling for disputed areas
4. **Boundary labels**: "HUNGARY / ROMANIA" text along border
5. **Index file support**: Serialize assembled boundaries to .idx file
