# Relation Parsing Implementation Plan

## Overview

Implement OSM relation parsing in Carta to support multipolygon features like large rivers (Danube), lakes (Balaton), and complex land areas.

## Current State

- **Parsed**: Nodes, Ways
- **Skipped**: Relations (field 4 in PrimitiveGroup)
- **Impact**: Large water bodies mapped as multipolygon relations don't render

## Implementation Phases

### Phase 1: PBF Field Definitions
**File**: `shared/include/sh_pbf.h`

Add relation field numbers from OSM PBF spec:
```c
/* Relation */
#define SH_PBF_RELATION_ID         1
#define SH_PBF_RELATION_KEYS       2
#define SH_PBF_RELATION_VALS       3
#define SH_PBF_RELATION_INFO       4
#define SH_PBF_RELATION_ROLES_SID  8   // Role string indices
#define SH_PBF_RELATION_MEMIDS     9   // Delta-encoded member IDs
#define SH_PBF_RELATION_TYPES     10   // Member types (0=node, 1=way, 2=relation)
```

### Phase 2: Data Structures
**File**: `carta/include/ct_types.h`

Add structures for relations:
```c
/* Relation member types */
typedef enum {
    CT_MEMBER_NODE = 0,
    CT_MEMBER_WAY = 1,
    CT_MEMBER_RELATION = 2
} CTMemberType;

/* Single relation member */
typedef struct {
    int64_t ref;           // Member ID (node/way/relation)
    CTMemberType type;     // Member type
    char *role;            // Role string (e.g., "outer", "inner")
} CTRelationMember;

/* Parsed OSM relation */
typedef struct {
    int64_t id;
    CTRelationMember *members;
    int num_members;

    CTOSMFeatureClass feature_class;
    int feature_type;
    char *name;

    /* For multipolygons: assembled geometry */
    int is_multipolygon;
} CTOSMRelation;

/* Assembled multipolygon ring */
typedef struct {
    CTCoord *coords;
    int num_coords;
    int is_outer;          // 1=outer ring, 0=inner (hole)
} CTMultipolygonRing;

/* Assembled multipolygon */
typedef struct {
    CTMultipolygonRing *rings;
    int num_rings;
    CTBBox bbox;
    float area_sqm;

    CTOSMFeatureClass feature_class;
    int feature_type;
} CTAssembledMultipolygon;
```

Update `CTPBFContext`:
```c
/* Add to CTPBFContext */
CTOSMRelation *relations;
size_t num_relations;
size_t relations_capacity;

/* Way ID lookup for relation member resolution */
struct {
    int64_t *keys;      // Way IDs
    uint32_t *values;   // Indices into ways[]
    size_t capacity;
    size_t count;
} way_map;

/* Assembled multipolygons (after relation processing) */
CTAssembledMultipolygon *multipolygons;
size_t num_multipolygons;
size_t multipolygons_capacity;
```

### Phase 3: Relation Parsing
**File**: `carta/src/ct_pbf.c`

3.1. Add `way_map_insert()` and `way_map_lookup()` (similar to node_map)

3.2. Add `parse_relation()` function:
```c
static CTStatus parse_relation(CTPBFContext *ctx, const uint8_t *data,
                               size_t len, const SHStringTable *st)
{
    // Parse relation fields:
    // - ID (field 1)
    // - Keys/Vals (fields 2, 3)
    // - Roles (field 8) - string indices
    // - Member IDs (field 9) - delta-encoded
    // - Member types (field 10) - packed enum

    // Classify: only keep type=multipolygon with natural=water, landuse, etc.
    // Store in ctx->relations[]
}
```

3.3. Update `parse_primitive_group()` to call `parse_relation()` for field 4

### Phase 4: Multipolygon Assembly
**File**: `carta/src/ct_multipolygon.c` (new file)

4.1. `ct_assemble_multipolygons()` - Main assembly function:
```c
CTStatus ct_assemble_multipolygons(CTPBFContext *ctx)
{
    // For each relation where is_multipolygon == 1:
    //   1. Collect member ways
    //   2. Separate by role (outer vs inner)
    //   3. Assemble outer rings (connect ways by shared endpoints)
    //   4. Assemble inner rings (holes)
    //   5. Compute bbox and area
    //   6. Store in ctx->multipolygons[]
}
```

4.2. Ring assembly algorithm:
```c
// Input: Array of ways with same role
// Output: Closed ring (connected way coordinates)

static CTMultipolygonRing *assemble_ring(CTOSMWay **ways, int num_ways)
{
    // 1. Start with first way
    // 2. Find next way that shares endpoint
    // 3. Append coordinates (possibly reversed)
    // 4. Repeat until ring closes
    // 5. Handle cases:
    //    - Single closed way (trivial)
    //    - Multiple ways forming ring
    //    - Disconnected ways (error case)
}
```

4.3. Endpoint matching:
```c
// Ways share endpoint if:
// way1.coords[last] == way2.coords[0] → append way2 forward
// way1.coords[last] == way2.coords[last] → append way2 reversed
```

### Phase 5: R-Tree Integration
**File**: `carta/src/ct_rtree.c`

5.1. Update R-Tree to index multipolygons:
```c
// Option A: Index multipolygons separately
CTRTree *ct_rtree_build_multipolygons(CTAssembledMultipolygon *polys, size_t count);

// Option B: Convert multipolygons to CTOSMWay-compatible format
// and use existing R-Tree (simpler)
```

### Phase 6: Feature Extraction
**File**: `carta/src/ct_pbf.c`

6.1. Update `ct_pbf_get_tile_features_lod()`:
```c
// After querying ways from R-Tree:
// 1. Also query multipolygons
// 2. Convert multipolygons to CTFeature with ring_ends[] populated
// 3. Return combined feature list
```

6.2. CTFeature ring support:
```c
// For multipolygon with holes:
// points[] = [outer_ring..., inner_ring_1..., inner_ring_2...]
// ring_ends[] = [outer_end, inner_1_end, inner_2_end]
// num_rings = 3
```

### Phase 7: Rendering Updates
**File**: `carta/src/ct_render.c`

7.1. Update polygon rendering to handle holes:
```c
void ct_render_polygon_with_holes(CTRenderContext *ctx,
                                  const CTTilePoint *points,
                                  const int *ring_ends, int num_rings,
                                  CTColor color)
{
    // Scanline fill that respects inner rings as holes
    // Even-odd or winding number rule
}
```

## Testing Strategy

### Unit Tests
1. **PBF Field Parsing**: Test relation field extraction
2. **Ring Assembly**: Test way connection logic
3. **Multipolygon Assembly**: Test outer/inner ring handling
4. **Feature Conversion**: Test CTFeature ring_ends population

### Integration Tests
1. **Hungary PBF**: Verify Danube river renders as polygon
2. **Lake Balaton**: Verify large lake with islands
3. **Monaco PBF**: Small test file with multipolygons

### Test Data
Create minimal PBF test file with:
- Simple multipolygon (single outer ring)
- Multipolygon with hole (outer + inner)
- Multipolygon from multiple ways

## Risk Areas

1. **Memory**: Relations reference ways by ID; need way_map for O(1) lookup
2. **Topology errors**: OSM data may have gaps/overlaps in rings
3. **Performance**: Hungary has ~10k relations; assembly must be efficient
4. **Coordinate precision**: Double vs fixed-point during assembly

## File Changes Summary

| File | Changes |
|------|---------|
| `shared/include/sh_pbf.h` | Add relation field definitions |
| `carta/include/ct_types.h` | Add CTOSMRelation, CTMultipolygonRing, CTAssembledMultipolygon |
| `carta/src/ct_pbf.c` | Add parse_relation(), way_map, update context |
| `carta/src/ct_multipolygon.c` | **NEW** - Ring assembly logic |
| `carta/include/ct_multipolygon.h` | **NEW** - Assembly API |
| `carta/src/ct_rtree.c` | Index multipolygons (optional: reuse way indexing) |
| `carta/src/ct_render.c` | Polygon with holes rendering |
| `carta/tests/test_carta.c` | Add relation/multipolygon tests |

## Estimated Complexity

- Phase 1-2: Low (definitions only)
- Phase 3: Medium (parsing mirrors way parsing)
- Phase 4: High (topology, ring assembly algorithm)
- Phase 5-6: Medium (extend existing patterns)
- Phase 7: Medium (scanline with holes)

## Questions Before Implementation

1. Should we store relations even if not multipolygons (for future use)?
2. How to handle relation members that are other relations (recursive)?
3. Should broken/incomplete multipolygons be discarded or partially rendered?
4. Memory strategy: Assemble all at once or on-demand during tile generation?
