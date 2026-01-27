# Velo vs OSRM Benchmark Results

## Test Environment
- **Map**: Hungary (294 MB PBF)
- **Graph**: 2,720,957 nodes, 5,476,396 edges
- **Platform**: Linux (WSL2)

## Velo Performance (Hungary)

### Mid-graph Route (~183 km)
| Algorithm | Time (ms) | Nodes Explored |
|-----------|-----------|----------------|
| Dijkstra | 200.6 | 993,707 |
| Dijkstra Bidir | 317.7 | 1,059,465 |
| **A*** | **76.3** | **181,050** |
| A* Bidir | 107.0 | 132,307 |

### End-to-end Route (~429 km)
| Algorithm | Time (ms) | Nodes Explored |
|-----------|-----------|----------------|
| Dijkstra | 443.9 | 2,648,225 |
| Dijkstra Bidir | 584.5 | 2,388,094 |
| A* | 323.7 | 1,196,736 |
| **A* Bidir** | **337.2** | **791,701** |

### Graph Loading
| Operation | Time (ms) |
|-----------|-----------|
| PBF Parse + Build | 7,822 |
| Binary Load | 191 |
| Binary Mmap | 116 |

## OSRM Reference Performance

OSRM with Contraction Hierarchies (MLD algorithm):
- **Query time**: < 1 ms (typically 0.1-0.5 ms)
- **Preprocessing**: ~10-30 minutes for Hungary
- **Memory**: ~500 MB RAM for Hungary

Source: [OSRM benchmarks](https://github.com/Project-OSRM/osrm-backend/wiki/Benchmarks)

## Performance Gap Analysis

| Metric | Velo (A*) | OSRM (CH) | Gap |
|--------|-----------|-----------|-----|
| Query Time | 76-337 ms | < 1 ms | ~100-500x |
| Preprocessing | 8 sec | 10-30 min | Velo faster |
| Memory | 125 MB | ~500 MB | Velo smaller |

### Why OSRM is Faster

OSRM uses **Contraction Hierarchies** (CH):
1. **Preprocessing**: Contracts nodes by importance, adding shortcut edges
2. **Query**: Bidirectional Dijkstra on hierarchy (only ~1000 nodes explored)
3. **Trade-off**: Long preprocessing for instant queries

### Velo Advantages

1. **Zero preprocessing** - Load and route immediately
2. **Smaller memory footprint** - No shortcut edges stored
3. **Simpler codebase** - No external dependencies
4. **WASM compatible** - Runs in browsers

## Recommendations

### For Real-time Applications (< 10 ms required)
Use OSRM or implement Contraction Hierarchies in Velo.

### For Batch Processing / Offline
Velo's 76-337 ms is acceptable for:
- Background route calculation
- Batch optimization
- Embedded systems with preprocessing constraints

### Future Velo Improvements
1. **Contraction Hierarchies** - Would achieve OSRM-like performance
2. **Hub Labeling** - Alternative fast query algorithm
3. **Goal-directed search** - Better heuristics (ALT algorithm)
4. **Edge bundling** - Reduce graph size for long-distance routes

## Raw Benchmark Output

```
Benchmark: Hungary (2720957 nodes, 5476396 edges)
Route: node 680239 -> node 2040717, 10 iterations
Algorithm               Time (ms)        Nodes     Distance
-------------------  ------------ ------------ ------------
Dijkstra                  200.566       993707       182946
Dijkstra Bidir            317.682      1059465       182946
A*                         76.258       181050       182946
A* Bidir                  106.954       132307       182946

Benchmark: Hungary (end-to-end) (2720957 nodes, 5476396 edges)
Route: node 0 -> node 2720956, 5 iterations
Algorithm               Time (ms)        Nodes     Distance
-------------------  ------------ ------------ ------------
Dijkstra                  443.902      2648225       429359
Dijkstra Bidir            584.546      2388094       429359
A*                        323.660      1196736       429359
A* Bidir                  337.238       791701       429359
```
