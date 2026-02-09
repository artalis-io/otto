# Commercial Truck Routing Analysis for Velo

**Author:** Systems + Routing Engineering Analysis
**Date:** February 2026
**Status:** Draft for Review

---

## Executive Summary

This document provides a comprehensive analysis of what is required to upgrade the velo routing engine to support commercial-grade truck routing using commercial datasets (TomTom GDF-AS or PTV Digital Data Streets). The analysis covers the current architecture, identifies gaps, proposes a canonical internal schema, and provides a phased implementation plan.

**Key Finding:** Velo has a solid foundation (CSR graph, bidirectional A*, ALT landmarks, mmap support) but lacks critical truck routing features: turn restrictions, dimensional constraints, and commercial data format ingestion.

**Recommendation:** Start with PTV DDS due to simpler format, lower licensing cost, and strong European trucking industry adoption.

---

## A. "As-Is" Velo Architecture Map

### 1. Module Structure

```
┌─────────────────────────────────────────────────────────────────────┐
│                        API Layer (main.c)                          │
│  HTTP REST via Mongoose │ Rate Limiting │ Work Queue │ CORS        │
├─────────────────────────────────────────────────────────────────────┤
│                     Core Routing (vl_route.c)                       │
│  Dijkstra │ Bidir Dijkstra │ A* │ Bidir A* │ ALT (Landmarks)       │
├─────────────────────────────────────────────────────────────────────┤
│                    Graph Structure (vl_graph.c)                     │
│  CSR Format │ Grid Index │ Reverse Index │ Hilbert Reorder         │
│  Degree-2 Contraction │ Binary Save/Load │ mmap Support            │
├─────────────────────────────────────────────────────────────────────┤
│                    Data Ingestion (vl_pbf.c)                        │
│  OSM PBF Parsing │ DenseNodes │ Ways │ Access Tags                 │
├─────────────────────────────────────────────────────────────────────┤
│                    Preprocessing (vl_landmarks.c)                   │
│  Farthest Landmark Selection │ Dual-Mode Distances │ SIMD Heuristic│
├─────────────────────────────────────────────────────────────────────┤
│                    Utilities (vl_geo.c, vl_heap.c)                  │
│  Haversine │ Binary Heap │ Bucket Heap                             │
└─────────────────────────────────────────────────────────────────────┘
```

### 2. Data Structures

#### VLNode (vl_types.h:~50)
```c
typedef struct {
    int64_t osm_id;          // OSM node ID
    VLCoordFixed coord;      // lat/lon as fixed-point (1e-7 precision)
    uint32_t edge_start;     // Index into edges array
    uint32_t edge_count;     // Number of outgoing edges
} VLNode;
```

#### VLEdge (vl_types.h:~60)
```c
typedef struct {
    uint32_t target;         // Target node index
    uint32_t distance;       // Distance in millimeters
    uint16_t duration;       // Travel time in deciseconds
    uint16_t flags;          // Road type (4 bits) + access flags (12 bits)
} VLEdge;
```

**Current flags encoding:**
- Bits 0-3: Road type (MOTORWAY, TRUNK, PRIMARY, etc.)
- Bit 8: VL_EDGE_ONEWAY
- Bits 9-12: Access restrictions (NO_CAR, NO_TRUCK, NO_BIKE, NO_FOOT)

#### VLProfile (vl_types.h:~30)
```c
typedef enum {
    VL_PROFILE_CAR = 0,
    VL_PROFILE_TRUCK = 1,
    VL_PROFILE_BIKE = 2,
    VL_PROFILE_FOOT = 3,
    VL_PROFILE_ANY = 4
} VLProfile;
```

### 3. Current Routing Flow

```
Request: from=(lat,lon), to=(lat,lon), profile=truck
                    │
                    ▼
        ┌───────────────────────┐
        │ vl_graph_nearest_node │  Grid spatial index lookup
        └───────────────────────┘
                    │
                    ▼
        ┌───────────────────────┐
        │ vl_route_astar_*()    │  A* or ALT algorithm
        │                       │
        │ For each edge:        │
        │   if (flags & mask)   │  ◄── Only access flag check
        │     skip              │      No turn restrictions!
        │   else                │      No dimensional check!
        │     relax edge        │
        └───────────────────────┘
                    │
                    ▼
        ┌───────────────────────┐
        │ reconstruct_path()    │  Build VLRoute with geometry
        └───────────────────────┘
```

### 4. Profile Filtering Logic (vl_route.c:92-123)

```c
static inline uint16_t profile_access_mask(VLProfile profile) {
    switch (profile) {
    case VL_PROFILE_TRUCK: return VL_ACCESS_NO_TRUCK;
    // ...
    }
}

static inline int edge_accessible(uint16_t flags, VLProfile profile, uint16_t access_mask) {
    if (flags & access_mask) return 0;  // Only checks access flag
    // Bike/Foot: avoid motorways/trunk
    return 1;
}
```

**Critical Gap:** This is the entire truck routing logic - just a single bit flag!

### 5. OSM Data Ingestion (vl_pbf.c)

Currently parses:
- `highway=*` → road type
- `oneway=yes/-1` → direction
- `maxspeed=*` → speed limit
- `access=no`, `motor_vehicle=no` → general access
- `hgv=no` → truck access (only restriction captured)

Does NOT parse:
- `hgv:conditional=*`
- `maxweight=*`, `maxheight=*`, `maxwidth=*`, `maxlength=*`
- `maxaxleload=*`
- Turn restriction relations
- Hazmat restrictions

---

## B. Gap Analysis

### 1. Turn Restrictions (Critical)

| Aspect | Current State | Required for Commercial |
|--------|---------------|------------------------|
| Turn modeling | **None** | Edge-to-edge restrictions |
| Data structure | N/A | Turn restriction table |
| Query overhead | N/A | O(1) lookup per turn |
| Storage | N/A | ~5-10% of edge count |

**Impact:** Trucks cannot make signed turns, U-turns, or time-restricted maneuvers. Routes will be illegal.

### 2. Dimensional Constraints (Critical)

| Constraint | Current State | Required |
|------------|---------------|----------|
| Max height | **None** | Bridge/tunnel clearance |
| Max weight | **None** | Bridge weight limits |
| Max width | **None** | Road width restrictions |
| Max length | **None** | Turning radius limits |
| Max axle load | **None** | Road surface limits |

**Impact:** Routes may direct trucks to roads they physically cannot traverse.

### 3. Commercial Data Formats (Critical)

| Format | Current State | Required |
|--------|---------------|----------|
| OSM PBF | ✅ Supported | Keep for consumer routing |
| TomTom GDF-AS | ❌ Not supported | Add parser |
| PTV DDS | ❌ Not supported | Add parser (recommended first) |
| HERE HDLM | ❌ Not supported | Future consideration |

### 4. Time-Dependent Routing (Important)

| Feature | Current State | Required |
|---------|---------------|----------|
| Time windows | **None** | Delivery time constraints |
| Rush hour traffic | **None** | Time-varying speeds |
| Conditional restrictions | **None** | Time-based turn bans |

### 5. Hazmat Routing (Important)

| Category | Current State | Required |
|----------|---------------|----------|
| Tunnel codes | **None** | ADR tunnel categories (A-E) |
| Hazmat classes | **None** | UN hazmat classification |
| Bridge restrictions | **None** | Weight + hazmat combined |

### 6. Speed Profiles (Moderate)

| Aspect | Current State | Required |
|--------|---------------|----------|
| Truck speeds | Uses car speeds | Truck-specific per road class |
| Speed by weight | **None** | Loaded vs unloaded |
| Gradient effects | **None** | Slower uphill |

### 7. Cost Modeling (Future)

| Factor | Current State | Required |
|--------|---------------|----------|
| Tolls | **None** | Toll road costs |
| Ferry costs | **None** | Ferry pricing |
| Fuel cost | **None** | Route fuel estimation |

---

## C. Proposed Canonical Internal Schema

### Design Principles

1. **Format-agnostic** - Single internal representation for OSM, GDF-AS, PTV DDS
2. **Compact** - Minimize memory for continental-scale graphs
3. **Query-efficient** - O(1) constraint checks during routing
4. **Extensible** - Easy to add new constraint types

### 1. Extended Edge Structure

```c
typedef struct {
    uint32_t target;              // Target node index
    uint32_t distance;            // Distance in millimeters
    uint16_t duration;            // Base travel time in deciseconds
    uint16_t flags;               // Road type + basic access

    // NEW: Truck-specific attributes (optional, stored separately)
    uint32_t restriction_idx;     // Index into restriction table (0 = none)
} VLEdge;

// Separate restriction record (sparse - only for edges with restrictions)
typedef struct {
    uint16_t max_height_dm;       // Decimeters (0 = no limit, 40 = 4.0m)
    uint16_t max_width_dm;        // Decimeters
    uint32_t max_weight_kg;       // Kilograms (0 = no limit)
    uint16_t max_length_dm;       // Decimeters
    uint16_t max_axle_kg;         // Axle load in 100kg units
    uint8_t hazmat_flags;         // Bit flags for hazmat categories
    uint8_t tunnel_code;          // ADR tunnel category (0-5)
    uint16_t reserved;            // Padding/future use
} VLEdgeRestriction;
```

**Memory estimate:**
- Base edge: 14 bytes (current: 14 bytes, no change)
- Restriction record: 16 bytes × ~10% of edges
- Continental US: ~400M edges × 14B + 40M × 16B = 6.2GB (vs 5.6GB now)

### 2. Turn Restriction Structure

```c
typedef struct {
    uint32_t from_edge;           // Edge index we're coming from
    uint32_t to_edge;             // Edge index we're going to
    uint8_t restriction_type;     // PROHIBITED, REQUIRED, CONDITIONAL
    uint8_t vehicle_mask;         // Which vehicles (CAR, TRUCK, etc.)
    uint16_t time_condition;      // Index into time condition table (0 = always)
} VLTurnRestriction;

// Turn restriction index for O(1) lookup
typedef struct {
    uint32_t *turn_restriction_start;  // Per-node: start index in restrictions
    uint16_t *turn_restriction_count;  // Per-node: count of restrictions
    VLTurnRestriction *restrictions;   // All turn restrictions
    uint32_t num_restrictions;
} VLTurnIndex;
```

**Memory estimate:**
- Restriction record: 12 bytes
- Continental US: ~20M turn restrictions × 12B = 240MB
- Index arrays: 2 × num_nodes × 4B = 3.2GB (for 400M nodes)

### 3. Vehicle Profile Structure

```c
typedef struct {
    VLProfile base_profile;       // CAR, TRUCK, etc.

    // Physical dimensions (decimeters)
    uint16_t height_dm;           // Vehicle height
    uint16_t width_dm;            // Vehicle width
    uint16_t length_dm;           // Vehicle length

    // Weights (kilograms)
    uint32_t gross_weight_kg;     // Total weight
    uint16_t axle_weight_100kg;   // Max axle load in 100kg units

    // Hazmat
    uint8_t hazmat_class;         // UN hazmat class (0 = none)
    uint8_t tunnel_category;      // Required tunnel category

    // Speed factors (percentage of base speed)
    uint8_t speed_factor_motorway;
    uint8_t speed_factor_primary;
    uint8_t speed_factor_secondary;
    uint8_t speed_factor_residential;
} VLTruckProfile;
```

### 4. Constraint Check Flow

```c
static inline int edge_accessible_truck(
    const VLGraph *graph,
    const VLEdge *edge,
    const VLTruckProfile *truck,
    uint32_t from_node
) {
    // 1. Basic access check
    if (edge->flags & VL_ACCESS_NO_TRUCK) return 0;

    // 2. Dimensional restrictions (if present)
    if (edge->restriction_idx != 0) {
        const VLEdgeRestriction *r = &graph->restrictions[edge->restriction_idx - 1];

        if (r->max_height_dm && truck->height_dm > r->max_height_dm) return 0;
        if (r->max_width_dm && truck->width_dm > r->max_width_dm) return 0;
        if (r->max_weight_kg && truck->gross_weight_kg > r->max_weight_kg) return 0;
        if (r->max_length_dm && truck->length_dm > r->max_length_dm) return 0;
        if (r->max_axle_kg && truck->axle_weight_100kg > r->max_axle_kg) return 0;

        // Hazmat check
        if (r->hazmat_flags && (truck->hazmat_class & r->hazmat_flags)) return 0;
        if (r->tunnel_code && truck->tunnel_category > r->tunnel_code) return 0;
    }

    // 3. Turn restriction check (at routing time)
    // See turn_allowed() below

    return 1;
}

static inline int turn_allowed(
    const VLGraph *graph,
    uint32_t from_edge,
    uint32_t via_node,
    uint32_t to_edge,
    const VLTruckProfile *truck
) {
    if (!graph->turn_index) return 1;  // No restrictions

    uint32_t start = graph->turn_index->turn_restriction_start[via_node];
    uint16_t count = graph->turn_index->turn_restriction_count[via_node];

    for (uint16_t i = 0; i < count; i++) {
        const VLTurnRestriction *tr = &graph->turn_index->restrictions[start + i];

        if (tr->from_edge == from_edge && tr->to_edge == to_edge) {
            if (tr->vehicle_mask & (1 << truck->base_profile)) {
                if (tr->restriction_type == VL_TURN_PROHIBITED) {
                    return 0;
                }
            }
        }
    }

    return 1;
}
```

---

## D. Commercial Data Format Details

### 1. TomTom GDF-AS (Geographic Data Files - Automotive Standard)

**Format Structure:**
```
Dataset/
├── Network/
│   ├── RoadElement.csv       # Road segments
│   ├── Junction.csv          # Intersections
│   ├── RoadElementRelation.csv
│   └── Manoeuvre.csv         # Turn restrictions
├── VehicleRestrictions/
│   ├── PhysicalRestriction.csv  # Height, width, weight
│   ├── TimeDependent.csv
│   └── HazmatRestriction.csv
├── Speed/
│   ├── SpeedProfile.csv
│   └── SpeedPatterns.csv
└── Meta/
    ├── Coverage.csv
    └── Version.csv
```

**Key Tables for Truck Routing:**

| Table | Purpose | Key Fields |
|-------|---------|------------|
| RoadElement | Network links | ID, geometry, road class |
| PhysicalRestriction | Dimensional limits | max_height, max_weight, max_width |
| Manoeuvre | Turn restrictions | from_link, via_node, to_link, type |
| HazmatRestriction | Hazmat categories | link_id, hazmat_class, tunnel_code |

**Parsing Complexity:** High - multiple related files, complex relationships

### 2. PTV DDS (Digital Data Streets)

**Available Formats:**
- MapInfo TAB
- MIF/MID
- ESRI Shapefile

**Note:** Sample data can likely be obtained from PTV for development purposes.

**Format Structure (Shapefile-based):**
```
dds_country/
├── net/
│   ├── links.shp             # Road segments
│   ├── nodes.shp             # Intersections
│   ├── turns.dbf             # Turn restrictions
│   └── geometry/             # Polylines
├── restrictions/
│   ├── vehicle.dbf           # Physical restrictions
│   ├── hazmat.dbf            # Hazmat restrictions
│   └── time.dbf              # Time-based restrictions
├── attributes/
│   ├── speed.dbf             # Speed limits
│   └── toll.dbf              # Toll information
└── meta/
    └── version.xml
```

**Key Tables for Truck Routing:**

| Table | Purpose | Key Fields |
|-------|---------|------------|
| links | Network segments | ID, geometry, road class |
| turns | Turn restrictions | from_link, to_link, flags |
| vehicle | Physical limits | link_id, height, weight, width |
| hazmat | Hazmat restrictions | link_id, category, tunnel |

**Parsing Complexity:** Medium - Shapefile/DBF is well-documented, existing libraries available

### 3. Format Comparison

| Aspect | TomTom GDF-AS | PTV DDS |
|--------|---------------|---------|
| File format | CSV/TSV | Shapefile/MIF/TAB |
| Structure complexity | High | Medium |
| Documentation | Extensive | Good |
| Licensing cost | High ($$$) | Medium ($$) |
| Update frequency | Quarterly | Monthly |
| European coverage | Excellent | Excellent |
| US coverage | Excellent | Good |
| Truck attributes | Very complete | Complete |
| Parser effort | 3-4 weeks | 2-3 weeks |
| Sample availability | Paid only | Dev samples available |

---

## E. Implementation Plan

### Phase 0: Schema + Infrastructure (1 week)

**Goal:** Define internal structures and extend graph format

**Tasks:**
1. Define `VLEdgeRestriction` structure in `vl_types.h`
2. Define `VLTurnRestriction` and `VLTurnIndex` structures
3. Define `VLTruckProfile` structure
4. Extend `VLGraph` to include restriction arrays
5. Update binary format version and save/load functions
6. Add unit tests for new structures

**Files Modified:**
- `velo/include/vl_types.h`
- `velo/src/vl_graph.c` (save/load)
- `velo/tests/test_velo.c`

**Deliverable:** Extended data structures, backward-compatible binary format

### Phase 1: Turn Restriction Support (2 weeks)

**Goal:** Enable turn restriction modeling and enforcement

**Tasks:**
1. Implement turn restriction index construction
2. Modify routing algorithms to check turn restrictions
3. Handle edge-to-edge transitions in path reconstruction
4. Add OSM turn restriction relation parsing (for testing)
5. Benchmark routing with turn restrictions enabled

**Files Modified:**
- `velo/src/vl_graph.c` (turn index)
- `velo/src/vl_route.c` (all algorithms)
- `velo/src/vl_pbf.c` (OSM relations)
- `velo/tests/test_velo.c`

**Algorithm Changes:**
```c
// Before: only check node accessibility
// After: track incoming edge, check turn at each step

// In dijkstra/astar:
for (each edge from current_node) {
    if (!edge_accessible(edge)) continue;
    if (!turn_allowed(prev_edge, current_node, edge)) continue;  // NEW
    // ... relax edge
}
```

**Deliverable:** Working turn restriction enforcement, OSM relation parsing

### Phase 2: Dimensional Constraints (1 week)

**Goal:** Enable physical restriction checks

**Tasks:**
1. Implement restriction index in graph
2. Extend `edge_accessible()` for dimensional checks
3. Add `VLTruckProfile` to routing options
4. Update API to accept truck dimensions
5. Test with synthetic restriction data

**Files Modified:**
- `velo/src/vl_route.c`
- `velo/api/src/main.c`
- `velo/include/vl_types.h`

**API Change:**
```
GET /api/v1/route?from=...&to=...&profile=truck
    &height=4.0&width=2.55&weight=40000&length=16.5
```

**Deliverable:** Dimensional constraint enforcement

### Phase 3: PTV DDS Parser (2-3 weeks)

**Goal:** Ingest PTV Digital Data Streets format

**Tasks:**
1. Implement binary file readers for DDS format
2. Parse links.dat → VLNode/VLEdge
3. Parse turns.dat → VLTurnRestriction
4. Parse vehicle.dat → VLEdgeRestriction
5. Parse hazmat.dat → hazmat flags
6. Build coordinate index for snapping
7. Test with sample PTV dataset

**New Files:**
- `velo/src/vl_dds.c`
- `velo/include/vl_dds.h`
- `velo/tests/test_dds.c`

**Deliverable:** Working PTV DDS ingestion

### Phase 4: Integration + Testing (2 weeks)

**Goal:** End-to-end commercial truck routing

**Tasks:**
1. Create test dataset with known restrictions
2. Validate routes against ground truth
3. Performance benchmarking
4. API documentation
5. Update WASM demo with truck profile
6. Write operator documentation

**Test Cases:**
- Route avoiding low bridge
- Route around weight-limited road
- Route respecting no-left-turn
- Hazmat route avoiding tunnel
- Multi-constraint combination

**Deliverable:** Production-ready commercial truck routing

### Phase 5: TomTom GDF-AS Parser (Optional, 3-4 weeks)

**Goal:** Support alternative commercial data source

**Tasks:**
1. Implement CSV/TSV parsers for GDF tables
2. Map GDF feature codes to internal types
3. Handle GDF relationship tables
4. Test with sample TomTom dataset

**New Files:**
- `velo/src/vl_gdf.c`
- `velo/include/vl_gdf.h`

**Deliverable:** GDF-AS support for customers with TomTom licenses

---

## F. Recommendation: Start with PTV DDS

### Rationale

| Factor | PTV DDS | TomTom GDF-AS |
|--------|---------|---------------|
| Parser complexity | Simpler (binary) | Complex (relational CSV) |
| Time to implement | 2-3 weeks | 3-4 weeks |
| Licensing cost | Medium | High |
| European logistics adoption | Very high | High |
| Truck attribute coverage | Complete | Very complete |
| Update frequency | Monthly | Quarterly |

### Strategic Considerations

1. **Customer base:** Girteka, Waberer's (European fleets) likely have PTV data access
2. **Trimble angle:** PTV is independent of Trimble, reduces lock-in narrative
3. **Lower barrier:** Can ship faster, iterate based on feedback
4. **Fallback:** GDF-AS parser can be added later if customers require TomTom

### Recommended Timeline

| Week | Deliverable |
|------|-------------|
| 1 | Schema + infrastructure |
| 2-3 | Turn restrictions (OSM for testing) |
| 4 | Dimensional constraints |
| 5-7 | PTV DDS parser |
| 8-9 | Integration + testing |
| **Total** | **~9 weeks to production** |

---

## G. Appendix: OSM Truck Tags (For Reference)

Even with commercial data, OSM parsing improvements are valuable:

```
# Physical restrictions (parse these)
maxheight=4.0
maxweight=7.5
maxwidth=2.5
maxlength=12
maxaxleload=10

# HGV access (already parsed)
hgv=no
hgv=designated
hgv:conditional=no @ (Mo-Fr 07:00-09:00,17:00-19:00)

# Hazmat (parse these)
hazmat=no
hazmat:water=no
tunnel=E  # ADR tunnel category
```

---

## H. Appendix: Test Dataset Strategy

For development and CI:

1. **Synthetic mini-graph** - 100 nodes, all restriction types
2. **OSM extract** - Small city with turn restrictions (Liechtenstein)
3. **PTV sample** - Request from PTV (free for development)
4. **Known routes** - Ground truth from actual truck GPS traces

---

*End of Analysis*
