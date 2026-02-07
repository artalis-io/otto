# Continental-Scale Tile Serving with Carta

## Problem Statement

| Scale | PBF Size | RAM Required | Single Instance? |
|-------|----------|--------------|------------------|
| Country (Hungary) | 300MB | ~4GB | ✅ Yes |
| Large Country (Germany) | 3.5GB | ~50GB | ⚠️ Expensive |
| Continental (Europe) | 25GB | ~300GB | ❌ Impractical |
| Planet | 70GB | ~1TB | ❌ No |

Continental and planet-scale data cannot fit in a single Carta instance. We need a sharding strategy.

## Border Challenges

When dividing by country/region:

1. **Cross-border features**: Roads, rivers, railways don't stop at borders
2. **Tile boundary problem**: A single tile at z=14 might span France + Germany
3. **Seamless panning**: User scrolling across border shouldn't see seams
4. **Feature duplication**: The same highway appears in both country extracts

## Solution: Buffer Zone Sharding

Each region extract includes a buffer zone (50-100km) around its borders:

```
┌─────────────────────────────────────────────────┐
│                    France                        │
│                                                  │
│    ┌──────────────────────────────────────┐     │
│    │         Buffer Zone (~50km)          │     │
├────┼──────────────────────────────────────┼─────┤
│    │         Buffer Zone (~50km)          │     │
│    └──────────────────────────────────────┘     │
│                    Germany                       │
│                                                  │
└─────────────────────────────────────────────────┘
```

**Key insight**: With 50km buffers, most border tiles are fully contained in at least one shard's buffer. Only tiles exactly on the buffer boundary need multi-shard queries (rare).

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                     Continental Tile Service                     │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Layer 1: CDN (Cloudflare/Fastly)                               │
│  ├── Caches rendered tiles                                       │
│  └── 90%+ hit rate for popular tiles                            │
│                                                                  │
│  Layer 2: Tile Router                                           │
│  ├── Routes by tile bbox → shard                                │
│  ├── Handles border tiles (multi-shard merge)                   │
│  └── Redis cache for recent tiles                               │
│                                                                  │
│  Layer 3: Regional Carta Instances                              │
│  ├── carta-iberia (ES, PT + 50km buffer)                        │
│  ├── carta-france (FR + 50km buffer)                            │
│  ├── carta-benelux (BE, NL, LU + 50km buffer)                   │
│  ├── carta-dach (DE, AT, CH + 50km buffer)                      │
│  ├── carta-nordics (NO, SE, FI, DK + 50km buffer)               │
│  ├── carta-british-isles (UK, IE + 50km buffer)                 │
│  ├── carta-italy (IT + 50km buffer)                             │
│  ├── carta-balkans (HR, SI, RS, etc. + 50km buffer)             │
│  ├── carta-poland (PL + 50km buffer)                            │
│  └── carta-eastern (UA, BY, RU-west + 50km buffer)              │
│                                                                  │
│  Layer 4: Low-Zoom Server                                        │
│  └── carta-overview (simplified planet, z0-10 only)             │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

## Hierarchical Zoom Levels

Different data sources for different zoom levels:

| Zoom | Data Source | Why |
|------|-------------|-----|
| 0-6 | Simplified planet extract | Coastlines, country borders, major cities |
| 7-10 | Continental extract (simplified) | Major roads, large water bodies |
| 11-18 | Regional shards with buffers | Full detail |

## Components

### 1. Shard Configuration

```c
typedef struct {
    char *region_id;           // "DE", "FR", etc.
    CTBBox core_bbox;          // Actual country bounds
    CTBBox buffered_bbox;      // Core + buffer zone
    double buffer_km;          // Buffer size (e.g., 50km)
    char *pbf_path;            // Path to regional PBF with buffer
} ShardConfig;

typedef struct {
    ShardConfig *shards;
    int num_shards;
    void *shard_index;         // R-tree for fast bbox lookup
} ShardRouter;
```

### 2. Tile Classification

```c
typedef enum {
    TILE_SINGLE_SHARD,      // Fully within one shard (common case)
    TILE_IN_BUFFER,         // In buffer zone, one shard can handle
    TILE_MULTI_SHARD        // Spans buffer boundary, needs merge
} TileShardType;

TileShardType classify_tile(ShardRouter *router, CTTileCoord coord) {
    CTBBox tile_bbox = ct_tile_bounds(coord);

    int overlapping_cores = 0;
    int overlapping_buffers = 0;

    for (int i = 0; i < router->num_shards; i++) {
        if (bbox_intersects(tile_bbox, router->shards[i].core_bbox)) {
            overlapping_cores++;
        }
        if (bbox_intersects(tile_bbox, router->shards[i].buffered_bbox)) {
            overlapping_buffers++;
        }
    }

    if (overlapping_cores == 1) {
        return TILE_SINGLE_SHARD;  // Easy case - 95%+ of requests
    } else if (overlapping_buffers == 1) {
        return TILE_IN_BUFFER;     // Buffer handles it
    } else {
        return TILE_MULTI_SHARD;   // Need to merge (rare)
    }
}
```

### 3. Feature Merger (for multi-shard tiles)

```c
// For tiles that span multiple shards:
// 1. Query each contributing shard
// 2. Merge features (deduplicate by OSM ID)
// 3. Render merged tile

CTStatus generate_border_tile(ShardRouter *router, CTTileCoord coord,
                               uint8_t *buffer, size_t *size) {
    ShardConfig *shards[MAX_OVERLAPPING_SHARDS];
    int num_shards;

    shard_route_tile(router, coord, shards, &num_shards);

    CTFeature *merged_features = NULL;
    size_t merged_count = 0;

    for (int i = 0; i < num_shards; i++) {
        CTFeature *shard_features;
        size_t shard_count;

        // Get features from this shard
        ct_pbf_get_tile_features(shards[i]->ctx, coord,
                                  &shard_features, &shard_count);

        // Merge, deduplicating by OSM ID
        merge_features_by_osm_id(&merged_features, &merged_count,
                                  shard_features, shard_count);
    }

    // Render merged tile
    return ct_render_tile(merged_features, merged_count, buffer, size);
}
```

### 4. Generating Buffered Extracts

Use osmium-tool to create buffered regional extracts:

```bash
#!/bin/bash
# generate-buffered-extracts.sh

BUFFER_KM=50
EUROPE_PBF="europe-latest.osm.pbf"

# Germany: core bbox [5.87, 47.27, 15.04, 55.06]
# With 50km buffer: [5.37, 46.82, 15.54, 55.51]
osmium extract -b 5.37,46.82,15.54,55.51 \
    $EUROPE_PBF -o germany-buffered.osm.pbf

# France: core bbox [-5.14, 41.33, 9.56, 51.09]
# With 50km buffer: [-5.64, 40.88, 10.06, 51.54]
osmium extract -b -5.64,40.88,10.06,51.54 \
    $EUROPE_PBF -o france-buffered.osm.pbf

# ... repeat for other regions
```

## Pre-computed Tile Pyramid (Apex Integration)

For high-traffic deployments, pre-render tiles:

| Zoom | Tiles | Storage (est.) |
|------|-------|----------------|
| 0-10 | ~1.4M | ~50GB |
| 0-12 | ~22M | ~750GB |
| 0-14 | ~350M | ~12TB |

**Strategy**:
- Pre-compute z0-12 (manageable storage)
- On-demand generation for z13+ with aggressive caching
- CDN handles most traffic

## Memory Budget per Shard

| Region | PBF Size | RAM Needed | Instance Type |
|--------|----------|------------|---------------|
| Germany + buffer | ~4GB | ~50GB | r6i.4xlarge |
| France + buffer | ~4GB | ~50GB | r6i.4xlarge |
| UK + Ireland | ~1.5GB | ~20GB | r6i.2xlarge |
| Nordics | ~2GB | ~25GB | r6i.2xlarge |
| Benelux | ~2GB | ~25GB | r6i.2xlarge |
| Italy + buffer | ~1.5GB | ~20GB | r6i.2xlarge |
| Iberia | ~1.5GB | ~20GB | r6i.2xlarge |
| Poland + buffer | ~1.5GB | ~20GB | r6i.2xlarge |
| Balkans | ~1.5GB | ~20GB | r6i.2xlarge |
| Eastern Europe | ~2GB | ~25GB | r6i.2xlarge |
| Overview (z0-10) | ~500MB | ~8GB | r6i.xlarge |

**Total for Europe: ~10-12 instances, ~280GB total RAM**

## Implementation Phases

### Phase 1: Buffered Shards (2-3 weeks)
- [ ] Script to generate buffered extracts with osmium
- [ ] Shard configuration file format (JSON/YAML)
- [ ] Simple nginx/HAProxy routing by geographic region
- [ ] Each Carta instance handles its buffered region
- [ ] Manual failover between shards

### Phase 2: Smart Router (2-3 weeks)
- [ ] Tile Router service (new component)
- [ ] R-tree index of shard boundaries
- [ ] Tile classification (single/buffer/multi-shard)
- [ ] Redis caching layer for rendered tiles
- [ ] Health checks and automatic failover

### Phase 3: Feature Merger (1-2 weeks)
- [ ] OSM ID tracking in CTFeature
- [ ] Feature deduplication by OSM ID
- [ ] Multi-shard tile generation
- [ ] Caching of merged border tiles

### Phase 4: Hierarchical Zoom (1-2 weeks)
- [ ] Simplified planet extract for z0-10
- [ ] Zoom-based routing in Tile Router
- [ ] Separate overview server instance

### Phase 5: Apex Pre-computation (4-6 weeks)
- [ ] Offline tile generation pipeline
- [ ] MBTiles or S3 storage backend
- [ ] Tile serving from pre-computed store
- [ ] Incremental updates (OSM diffs)

## API Changes

### Shard Router API

```
GET /tiles/{z}/{x}/{y}.png
    → Routes to appropriate shard(s)
    → Returns merged tile if multi-shard
    → X-Carta-Shard header indicates source

GET /api/v1/shards
    → List all configured shards with status

GET /api/v1/shard/{id}/stats
    → Stats for specific shard
```

### Configuration

```yaml
# shards.yaml
shards:
  - id: "dach"
    name: "Germany, Austria, Switzerland"
    core_bbox: [5.87, 45.82, 17.16, 55.06]
    buffer_km: 50
    pbf_path: "/data/dach-buffered.osm.pbf"
    endpoint: "http://carta-dach:8081"

  - id: "france"
    name: "France"
    core_bbox: [-5.14, 41.33, 9.56, 51.09]
    buffer_km: 50
    pbf_path: "/data/france-buffered.osm.pbf"
    endpoint: "http://carta-france:8081"

overview:
  max_zoom: 10
  pbf_path: "/data/planet-simplified.osm.pbf"
  endpoint: "http://carta-overview:8081"

cache:
  redis_url: "redis://cache:6379"
  ttl_seconds: 86400
```

## Deployment (Kubernetes)

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: carta-dach
spec:
  replicas: 2
  template:
    spec:
      containers:
      - name: carta
        image: carta:latest
        args: ["/data/dach-buffered.idx"]
        resources:
          requests:
            memory: "50Gi"
          limits:
            memory: "55Gi"
        volumeMounts:
        - name: data
          mountPath: /data
      volumes:
      - name: data
        persistentVolumeClaim:
          claimName: carta-dach-data
---
apiVersion: v1
kind: Service
metadata:
  name: carta-dach
spec:
  selector:
    app: carta-dach
  ports:
  - port: 8081
```

## Cost Estimate (AWS, Europe)

| Component | Instance | Count | Monthly Cost |
|-----------|----------|-------|--------------|
| Regional shards | r6i.2xlarge | 8 | ~$2,400 |
| Large shards (DE, FR) | r6i.4xlarge | 2 | ~$1,200 |
| Overview server | r6i.xlarge | 1 | ~$150 |
| Tile Router | c6i.xlarge | 2 | ~$250 |
| Redis cache | r6i.large | 1 | ~$100 |
| Load balancer | ALB | 1 | ~$50 |
| **Total** | | | **~$4,150/month** |

Add CDN costs (~$0.02-0.08/GB egress) based on traffic.

## Alternatives Considered

### 1. Single Giant Instance
- ❌ 300GB+ RAM impractical
- ❌ Single point of failure
- ❌ No horizontal scaling

### 2. No Buffer (Query Multiple Shards Always)
- ❌ Every border tile needs 2+ queries
- ❌ Higher latency
- ❌ Complex merging for every request

### 3. Full Pre-computation (All Zoom Levels)
- ❌ z0-18 = trillions of tiles
- ❌ Petabytes of storage
- ❌ Update latency (regenerate everything)

### 4. Vector Tiles Only (Client-side Rendering)
- ✅ Smaller tiles, less server compute
- ⚠️ Requires MapLibre/Mapbox GL client
- ⚠️ Not compatible with Leaflet raster layers
- Consider as future optimization
