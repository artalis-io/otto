# Velo Continental Scale Plan

**Created:** 2026-02-06
**Status:** Draft for Review

## Summary

Scale Velo routing engine to handle continental-scale maps (contiguous US ~36GB, EU+UK ~72GB) while maintaining existing algorithm support and preparing data structures for Phase 2 CCH.

## Requirements (Confirmed)

| Requirement | Value |
|-------------|-------|
| Query latency | ~2 seconds (95th percentile optimal acceptable) |
| Memory budget | ~120GB total, separate US/EU deployments |
| Algorithms | ALT bidirectional, A*, Dijkstra (Phase 1); CCH (Phase 2) |
| Truck constraints | Axle weight, count, weight limit, height limit, length limit, turn restrictions |
| Edge storage | Sparse with index (only store non-default restrictions) |
| mmap support | Compile flag or runtime option |
| Parallelism | 16-64 cores, producer-consumer queue |
| Preprocessing | Progress reporting, failure recovery |

## Phase 1: Continental Foundation

### 1.1 Memory-Mapped Graph Storage

**Goal:** Load 36-72GB graphs without 2x memory spike during parsing.

**Files to create:**
- `velo/include/vl_mmap.h` - mmap abstraction
- `velo/src/vl_mmap.c` - platform-specific implementation

**Design:**

```c
/* Compile-time option */
#ifdef VL_USE_MMAP
#define VL_GRAPH_STORAGE_MMAP 1
#else
#define VL_GRAPH_STORAGE_MMAP 0
#endif

/* Runtime option (when both compiled in) */
typedef enum {
    VL_STORAGE_MALLOC,  /* Traditional: parse PBF → malloc graph */
    VL_STORAGE_MMAP     /* Memory-mapped: load pre-built .vlg file */
} VLStorageMode;

/* Graph file format (.vlg) */
typedef struct {
    uint32_t magic;           /* 'VLG1' */
    uint32_t version;         /* Format version */
    uint64_t num_nodes;
    uint64_t num_edges;
    uint64_t edge_offset;     /* Offset to edge array */
    uint64_t node_offset;     /* Offset to node array */
    uint64_t restrict_offset; /* Offset to restriction index */
    uint64_t landmark_offset; /* Offset to landmark data */
    uint32_t flags;           /* VL_FLAG_HAS_RESTRICTIONS, etc. */
    uint32_t reserved[7];
} VLGraphHeader;

/* API */
VLGraph *vl_graph_mmap(const char *path);      /* mmap existing .vlg */
int vl_graph_save(VLGraph *g, const char *path); /* Save to .vlg */
void vl_graph_unmap(VLGraph *g);               /* munmap */
```

**Platform abstraction:**

```c
/* vl_mmap.h */
typedef struct {
    void *addr;
    size_t size;
    int fd;
} VLMappedFile;

VLMappedFile *vl_mmap_open(const char *path);
void vl_mmap_close(VLMappedFile *mf);

/* vl_mmap.c - platform-specific */
#ifdef _WIN32
/* CreateFileMapping / MapViewOfFile */
#else
/* mmap / munmap */
#endif
```

### 1.2 Sparse Truck Restriction Storage

**Goal:** Store truck constraints only for edges that have restrictions (estimated 1-5% of edges).

**Files to modify:**
- `velo/include/vl_types.h` - add restriction types
- `velo/src/vl_restrict.c` - restriction index (new file)

**Design:**

```c
/* Restriction types - bitfield for common combinations */
typedef struct {
    uint16_t flags;           /* VL_RESTRICT_* flags */
    uint16_t max_weight_100kg;/* Max weight in 100kg units (0 = no limit) */
    uint8_t max_height_dm;    /* Max height in decimeters (0 = no limit) */
    uint8_t max_length_dm;    /* Max length in decimeters (0 = no limit) */
    uint8_t max_width_dm;     /* Max width in decimeters (0 = no limit) */
    uint8_t max_axle_weight_100kg; /* Per-axle weight limit */
    uint8_t max_axle_count;   /* Max axles (0 = no limit) */
    uint8_t hazmat_flags;     /* Hazmat categories prohibited */
} VLTruckRestriction;

#define VL_RESTRICT_NO_TRUCKS     (1 << 0)  /* hgv=no */
#define VL_RESTRICT_WEIGHT        (1 << 1)  /* maxweight set */
#define VL_RESTRICT_HEIGHT        (1 << 2)  /* maxheight set */
#define VL_RESTRICT_LENGTH        (1 << 3)  /* maxlength set */
#define VL_RESTRICT_WIDTH         (1 << 4)  /* maxwidth set */
#define VL_RESTRICT_AXLE_WEIGHT   (1 << 5)  /* maxaxleload set */
#define VL_RESTRICT_AXLE_COUNT    (1 << 6)  /* maxaxles set */
#define VL_RESTRICT_HAZMAT        (1 << 7)  /* hazmat restrictions */
#define VL_RESTRICT_TURN          (1 << 8)  /* Turn restriction exists */

/* Sparse index: sorted array of (edge_id, restriction_offset) */
typedef struct {
    uint64_t *edge_ids;       /* Sorted edge IDs with restrictions */
    uint32_t *offsets;        /* Offset into restrictions array */
    VLTruckRestriction *data; /* Packed restriction data */
    size_t count;             /* Number of restricted edges */
} VLRestrictionIndex;

/* Lookup: O(log n) binary search */
const VLTruckRestriction *vl_restrict_lookup(
    const VLRestrictionIndex *idx,
    uint64_t edge_id
);

/* Check if edge passable for vehicle */
bool vl_restrict_passable(
    const VLRestrictionIndex *idx,
    uint64_t edge_id,
    const VLVehicleProfile *vehicle
);
```

**Memory estimate:**
- 12 bytes per restriction entry
- 12 bytes index overhead (8 byte edge_id + 4 byte offset)
- US roads: ~250M edges, ~5% restricted = 12.5M entries
- Storage: 12.5M × 24 bytes = 300MB (acceptable)

### 1.3 Turn Restriction Support

**Goal:** Model OSM turn restrictions for truck routing.

**Design:**

```c
/* Turn restriction: (from_edge, via_node, to_edge) → restriction */
typedef struct {
    uint64_t from_edge;
    uint32_t via_node;
    uint64_t to_edge;
    uint16_t restriction_type;  /* VL_TURN_* */
    uint16_t vehicle_flags;     /* Which vehicles affected */
} VLTurnRestriction;

#define VL_TURN_NO_LEFT       (1 << 0)
#define VL_TURN_NO_RIGHT      (1 << 1)
#define VL_TURN_NO_UTURN      (1 << 2)
#define VL_TURN_NO_STRAIGHT   (1 << 3)
#define VL_TURN_ONLY_LEFT     (1 << 4)
#define VL_TURN_ONLY_RIGHT    (1 << 5)
#define VL_TURN_ONLY_STRAIGHT (1 << 6)

/* Vehicle-specific flags */
#define VL_VEHICLE_CAR        (1 << 0)
#define VL_VEHICLE_TRUCK      (1 << 1)
#define VL_VEHICLE_BUS        (1 << 2)
#define VL_VEHICLE_HAZMAT     (1 << 3)

/* Index by via_node for efficient lookup during routing */
typedef struct {
    uint32_t *node_offsets;     /* Per-node offset into restrictions */
    VLTurnRestriction *data;    /* Packed restrictions */
    size_t count;
} VLTurnIndex;

/* During edge relaxation, check if turn is allowed */
bool vl_turn_allowed(
    const VLTurnIndex *idx,
    uint64_t from_edge,
    uint32_t via_node,
    uint64_t to_edge,
    uint16_t vehicle_type
);
```

### 1.4 ALT Algorithm Optimization

**Goal:** Reduce landmark count while maintaining quality for continental scale.

**Current:** 16 landmarks
**Target:** 8-12 landmarks with strategic placement

**Strategy:**

1. **Farthest-first selection** on road network (not Euclidean)
2. **Corner placement** - landmarks at network extremities
3. **Partition-aware** - at least one landmark per major region

**Memory per landmark:**
- Forward distances: 4 bytes × num_nodes
- Backward distances: 4 bytes × num_nodes
- US (175M nodes): 175M × 8 bytes × 12 landmarks = 16.8GB
- EU (350M nodes): 350M × 8 bytes × 12 landmarks = 33.6GB

**Optimization: Distance compression**

```c
/* Compress distances to 2 bytes with variable precision */
typedef struct {
    uint16_t *forward;   /* Compressed forward distances */
    uint16_t *backward;  /* Compressed backward distances */
    double scale;        /* Scaling factor for decompression */
} VLCompressedLandmark;

/* Compression: log-scale for large distances */
uint16_t vl_compress_distance(double dist_meters) {
    if (dist_meters <= 0) return 0;
    if (dist_meters >= 5000000) return UINT16_MAX;  /* 5000km cap */
    /* Log scale: ~1% precision */
    return (uint16_t)(log(dist_meters + 1) * 4000);
}

double vl_decompress_distance(uint16_t compressed) {
    return exp(compressed / 4000.0) - 1;
}
```

**Memory with compression:**
- US: 175M × 4 bytes × 12 = 8.4GB (vs 16.8GB uncompressed)
- EU: 350M × 4 bytes × 12 = 16.8GB (vs 33.6GB uncompressed)

### 1.5 Preprocessing Pipeline

**Goal:** Build continental graph from PBF with progress reporting and recovery.

**Files to create:**
- `velo/tools/vl_build.c` - Graph builder CLI

**Design:**

```
Usage: vl-build [options] <input.osm.pbf> <output.vlg>

Options:
  -p, --progress        Show progress bar
  -c, --checkpoint DIR  Enable checkpoint/recovery
  -m, --memory LIMIT    Memory limit (e.g., 64G)
  -j, --jobs N          Parallel jobs (default: auto)
  -l, --landmarks N     Number of ALT landmarks (default: 12)
  --profile PROFILE     Vehicle profile for preprocessing
  --no-restrictions     Skip truck restriction parsing
  --no-turns            Skip turn restriction parsing
```

**Checkpoint/Recovery:**

```c
typedef enum {
    VL_BUILD_STAGE_NODES,       /* Parsing OSM nodes */
    VL_BUILD_STAGE_WAYS,        /* Parsing OSM ways */
    VL_BUILD_STAGE_RELATIONS,   /* Parsing turn restrictions */
    VL_BUILD_STAGE_GRAPH,       /* Building CSR graph */
    VL_BUILD_STAGE_LANDMARKS,   /* Computing landmark distances */
    VL_BUILD_STAGE_INDEX,       /* Building restriction index */
    VL_BUILD_STAGE_WRITE,       /* Writing .vlg file */
    VL_BUILD_STAGE_DONE
} VLBuildStage;

typedef struct {
    VLBuildStage stage;
    uint64_t progress;          /* Bytes/items processed */
    uint64_t total;             /* Total bytes/items */
    char checkpoint_path[256];
} VLBuildProgress;

/* Checkpoint file contains completed stages + partial state */
int vl_build_save_checkpoint(const VLBuildProgress *prog, const char *dir);
int vl_build_load_checkpoint(VLBuildProgress *prog, const char *dir);
```

**Progress callback:**

```c
typedef void (*VLProgressCallback)(
    VLBuildStage stage,
    uint64_t progress,
    uint64_t total,
    void *user_data
);

int vl_build_graph(
    const char *pbf_path,
    const char *output_path,
    const VLBuildConfig *config,
    VLProgressCallback progress_cb,
    void *user_data
);
```

### 1.6 Data Structures for Phase 2 CCH

**Goal:** Design graph structure that supports later CCH overlay.

**Key insight:** CCH adds shortcut edges and node ordering. Design for this now.

```c
/* Node structure with CCH-ready fields */
typedef struct {
    double lat, lon;
    uint32_t first_edge;      /* Index into edge array */
    uint32_t edge_count;      /* Number of outgoing edges */
    uint32_t cch_level;       /* 0 = unordered, >0 = CCH level (Phase 2) */
    uint32_t cch_first_up;    /* First upward edge in CCH (Phase 2) */
    uint32_t cch_first_down;  /* First downward edge in CCH (Phase 2) */
} VLNode;

/* Edge structure with shortcut support */
typedef struct {
    uint32_t target;          /* Target node ID */
    uint32_t cost;            /* Edge cost (time or distance) */
    uint64_t osm_way_id;      /* Original OSM way (0 for shortcuts) */
    uint32_t middle_node;     /* For shortcuts: node this edge skips */
                              /* 0 = not a shortcut */
} VLEdge;

/* Graph with reserved space for CCH */
typedef struct {
    VLNode *nodes;
    VLEdge *edges;
    size_t num_nodes;
    size_t num_edges;
    size_t edge_capacity;     /* Reserved for Phase 2 shortcuts */

    /* Restriction indices */
    VLRestrictionIndex restrictions;
    VLTurnIndex turns;

    /* ALT landmarks */
    VLCompressedLandmark *landmarks;
    size_t num_landmarks;

    /* CCH data (Phase 2, NULL in Phase 1) */
    uint32_t *cch_order;      /* Node ordering */
    void *cch_overlay;        /* CCH-specific data */

    /* Storage mode */
    VLStorageMode storage;
    VLMappedFile *mmap_file;  /* NULL if malloc'd */
} VLGraph;
```

## Phase 2: Contraction Hierarchies (Future)

### 2.1 CCH Overview

**Goal:** Sub-100ms queries via preprocessed hierarchy.

CCH (Customizable Contraction Hierarchies) allows:
- Pre-compute node ordering and shortcuts once (expensive, ~hours)
- Customize edge weights quickly (~minutes for new truck profile)
- Query in milliseconds

**Not in Phase 1 scope, but data structures prepared.**

### 2.2 CCH Integration Points

```c
/* Phase 2 API (stubbed in Phase 1) */

/* Build CCH overlay (expensive, do once per region) */
int vl_cch_build(VLGraph *g, const VLCCHConfig *config);

/* Customize for specific vehicle profile (fast) */
int vl_cch_customize(VLGraph *g, const VLVehicleProfile *profile);

/* Query using CCH (fast) */
int vl_route_cch(
    VLGraph *g,
    uint32_t from,
    uint32_t to,
    const VLVehicleProfile *profile,
    VLRoute *out
);
```

## Implementation Roadmap

### Milestone 1: Core Infrastructure (Week 1-2)

| Task | Files | Priority |
|------|-------|----------|
| mmap abstraction | vl_mmap.h/c | P0 |
| Graph file format | vl_graph.c updates | P0 |
| Platform testing | macOS, Linux | P0 |

**Deliverable:** Can save/load graph via mmap.

### Milestone 2: Truck Restrictions (Week 3-4)

| Task | Files | Priority |
|------|-------|----------|
| Restriction types | vl_types.h | P0 |
| Sparse index | vl_restrict.c | P0 |
| Turn restrictions | vl_turn.c | P1 |
| PBF parsing updates | vl_osm.c | P0 |

**Deliverable:** Truck constraints parsed and queryable.

### Milestone 3: Preprocessing (Week 5-6)

| Task | Files | Priority |
|------|-------|----------|
| Build CLI tool | tools/vl_build.c | P0 |
| Progress reporting | vl_build.c | P1 |
| Checkpoint/recovery | vl_build.c | P2 |
| Parallel landmark computation | vl_landmarks.c | P1 |

**Deliverable:** Can build continental graph from PBF.

### Milestone 4: Query Optimization (Week 7-8)

| Task | Files | Priority |
|------|-------|----------|
| Distance compression | vl_landmarks.c | P1 |
| ALT tuning | vl_route.c | P1 |
| Constraint checking integration | vl_route.c | P0 |
| Benchmark suite | benchmarks/ | P1 |

**Deliverable:** 2-second continental queries with truck constraints.

### Milestone 5: Integration (Week 9-10)

| Task | Files | Priority |
|------|-------|----------|
| API server updates | api/src/main.c | P0 |
| Documentation | CLAUDE.md, API docs | P1 |
| US region testing | - | P0 |
| EU region testing | - | P0 |

**Deliverable:** Production-ready continental routing.

## Memory Budget Breakdown

### US Deployment (~36GB)

| Component | Size | Notes |
|-----------|------|-------|
| Nodes (175M) | 4.2GB | 24 bytes/node |
| Edges (250M) | 4.0GB | 16 bytes/edge |
| Restriction index | 0.3GB | 5% of edges |
| Turn restrictions | 0.1GB | Estimated |
| Landmarks (12) | 8.4GB | Compressed |
| Working memory | 2-4GB | Per-query scratch |
| **Total** | **~20GB** | Leaves headroom |

### EU+UK Deployment (~72GB)

| Component | Size | Notes |
|-----------|------|-------|
| Nodes (350M) | 8.4GB | 24 bytes/node |
| Edges (500M) | 8.0GB | 16 bytes/edge |
| Restriction index | 0.6GB | 5% of edges |
| Turn restrictions | 0.2GB | Estimated |
| Landmarks (12) | 16.8GB | Compressed |
| Working memory | 2-4GB | Per-query scratch |
| **Total** | **~37GB** | Leaves headroom |

## Testing Strategy

### Unit Tests
- Restriction lookup correctness
- Turn restriction evaluation
- Distance compression round-trip
- mmap open/close/access

### Integration Tests
- End-to-end routing with restrictions
- Graph build from small PBF
- Checkpoint/recovery

### Benchmark Tests
- Continental query latency (P50, P95, P99)
- Landmark computation time
- Memory usage tracking

### Validation
- Compare routes against OSRM for sample queries
- Verify restriction application matches OSM semantics

## Risk Mitigation

| Risk | Mitigation |
|------|------------|
| Landmark computation too slow | Parallelize, use distance lower bounds |
| 2-second target not achievable | Accept 95% optimal, reduce landmark count |
| mmap portability issues | Abstract behind VL_STORAGE_MODE, test early |
| Restriction parsing incomplete | Start with major constraint types, iterate |
| Memory overrun | Monitor with metrics, mmap helps |

## Open Questions

1. **Landmark selection algorithm** - Farthest-first on road network or partition-based?
2. **Distance compression scheme** - Log-scale or fixed-point?
3. **Turn restriction scope** - All OSM restrictions or truck-only subset?
4. **Graph file versioning** - How to handle format evolution?

## Success Criteria

- [ ] US continental query in <2 seconds (P95)
- [ ] EU continental query in <2 seconds (P95)
- [ ] Truck restrictions correctly applied
- [ ] Memory under 40GB for US, 80GB for EU
- [ ] Graph build completes without OOM
- [ ] Existing tests pass (47 velo tests)
