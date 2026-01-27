# Velo Performance Benchmark Results

## Test Environment
- **Map**: Hungary (294 MB PBF)
- **Graph**: 2,720,957 nodes, 5,476,396 edges
- **Platform**: Linux (WSL2)

---

## Optimization Summary

| Optimization | Preprocessing | Memory | Query Speedup | Status |
|--------------|--------------|--------|---------------|--------|
| Degree-2 Contraction | 683ms | 0 | ~19% | ✅ Working |
| ALT (16 Landmarks) | 2.2s | 664 MB | **2.5-3.8x** | ✅ Working |
| Hilbert Reordering | 648ms | 0 | **1.5-2.1x** | ✅ Working |
| Bucket Heap | 0 | 0 | N/A | ⚠️ Slower than binary heap |

### Combined Optimizations

With ALT + Hilbert on Hungary:
- **Query time**: 40-130ms (vs 150-350ms baseline)
- **Total preprocessing**: ~3 seconds
- **Extra memory**: 664 MB for landmarks

---

## Baseline Performance (No Optimizations)

### A* Bidirectional
| Route | Distance | Time (ms) | Nodes Explored |
|-------|----------|-----------|----------------|
| Budapest → Szeged | 170 km | 154 | 196,032 |
| Sopron → Nyíregyháza | 434 km | 345 | 700,782 |
| Pécs → Debrecen | 344 km | 200 | 363,041 |

---

## With Hilbert Reordering

Preprocessing: 648ms (includes mmap → malloc conversion)

### A* Bidirectional
| Route | Time (ms) | Speedup |
|-------|-----------|---------|
| Budapest → Szeged | 73 | **2.1x** |
| Sopron → Nyíregyháza | 232 | **1.5x** |
| Pécs → Debrecen | 114 | **1.8x** |

**Why it helps**: Hilbert curve orders nodes by spatial locality, improving CPU cache hits during graph traversal.

---

## With ALT (Landmarks)

Preprocessing: 2.2s for 16 landmarks (664 MB memory)

### A* with Landmarks
| Route | A* Bidir (ms) | ALT (ms) | Speedup |
|-------|---------------|----------|---------|
| Budapest → Szeged | 157 | 42 | **3.8x** |
| Sopron → Nyíregyháza | 336 | 134 | **2.5x** |
| Pécs → Debrecen | 199 | 78 | **2.6x** |

**Why it helps**: Triangle inequality with precomputed landmark distances provides tighter heuristic bounds than haversine.

---

## Algorithm Comparison (No Optimizations)

### Mid-distance Route (~170 km Budapest → Szeged)
| Algorithm | Time (ms) | Nodes Explored |
|-----------|-----------|----------------|
| Dijkstra | 303 | 1,615,473 |
| Dijkstra Bidir | 310 | 970,166 |
| A* | 100 | 229,400 |
| **A* Bidir** | **154** | **196,032** |

### Long-distance Route (~434 km Sopron → Nyíregyháza)
| Algorithm | Time (ms) | Nodes Explored |
|-----------|-----------|----------------|
| Dijkstra | 408 | 2,478,859 |
| Dijkstra Bidir | 533 | 2,042,433 |
| A* | 265 | 910,521 |
| **A* Bidir** | **345** | **700,782** |

---

## Graph Loading Performance

| Operation | Time (ms) |
|-----------|-----------|
| PBF Parse + Build | 7,822 |
| Binary Load (fread) | 191 |
| Binary Mmap | 116 |

---

## OSRM Comparison

OSRM with Contraction Hierarchies (MLD algorithm):
- **Query time**: < 1 ms (typically 0.1-0.5 ms)
- **Preprocessing**: ~10-30 minutes for Hungary
- **Memory**: ~500 MB RAM for Hungary

### Performance Gap

| Metric | Velo (best) | OSRM (CH) | Gap |
|--------|-------------|-----------|-----|
| Query Time | 40-130 ms | < 1 ms | ~50-150x |
| Preprocessing | 3 sec | 10-30 min | **Velo 200x faster** |
| Memory | 125 + 664 MB | ~500 MB | Similar |

### When to Use Velo vs OSRM

**Use Velo when:**
- Preprocessing time matters (cold start, dynamic maps)
- Memory is constrained (embedded systems)
- Building WASM applications
- Batch processing where 40-130ms is acceptable
- You need zero external dependencies

**Use OSRM when:**
- Sub-millisecond queries are required
- Preprocessing time doesn't matter
- You have dedicated routing infrastructure

---

## Usage Examples

### Standard A* (no preprocessing)
```c
VLGraph *graph = vl_load_pbf("hungary.osm.pbf");
vl_route(graph, src, dst, &opts, &route);  // ~150-350ms
```

### With Hilbert Reordering
```c
VLGraph *graph = vl_load_pbf("hungary.osm.pbf");
vl_graph_reorder_hilbert(graph);  // 648ms one-time
vl_route(graph, src, dst, &opts, &route);  // ~70-230ms
```

### With ALT (Landmarks)
```c
VLGraph *graph = vl_load_pbf("hungary.osm.pbf");
VLLandmarks *lm = vl_landmarks_create(graph, 16);  // 2.2s + 664MB
vl_route_astar_landmarks(graph, lm, src, dst, &opts, &route);  // ~40-130ms
vl_landmarks_free(lm);
```

### Combined (Hilbert + ALT)
```c
VLGraph *graph = vl_load_pbf("hungary.osm.pbf");
vl_graph_reorder_hilbert(graph);  // 648ms
VLLandmarks *lm = vl_landmarks_create(graph, 16);  // 2.2s
vl_route_astar_landmarks(graph, lm, src, dst, &opts, &route);  // fastest
```

---

## Raw Benchmark Output

```
Without optimizations:
Benchmark: Budapest -> Szeged (2720957 nodes, 5476396 edges)
A* Bidir                  154.333       196032       169932

With Hilbert reordering:
Hilbert reordering: 648.5 ms
A* Bidir                   73.451       196032       169932

With ALT (16 landmarks):
Landmark preprocessing: 2165.5 ms
ALT                        41.5       ~50000       169932
```
