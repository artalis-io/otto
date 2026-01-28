# Velo Routing Engine - Feature TODOs

This document outlines planned features for the Velo routing engine with detailed implementation plans.

## Table of Contents

1. [External Routing Backends (OSRM, PTV)](#1-external-routing-backends-osrm-ptv)
2. [Arc Flags](#2-arc-flags)
3. [Reach Pruning](#3-reach-pruning)
4. [Transit Node Routing](#4-transit-node-routing)
5. [Contraction Hierarchies](#5-contraction-hierarchies)
6. [Hub Labeling](#6-hub-labeling)

---

## 1. External Routing Backends (OSRM, PTV)

### Motivation

While Velo provides a zero-dependency routing engine suitable for embedded and WASM deployments, production environments may benefit from heavyweight routing solutions:

| Engine | License/Model | Strengths |
|--------|---------------|-----------|
| **OSRM** | BSD-2 (open source) | Fast CH-based routing, self-hosted via Docker, active community |
| **PTV xRoute/xServer** | Commercial | Truck routing, time-dependent routing, rich attributes, industry standard |

By providing a common API layer with pluggable backends, users can:
- Use Velo for zero-dependency builds (WASM, embedded, offline)
- Switch to OSRM for production-grade performance with Docker deployment
- Switch to PTV for commercial truck routing with regulatory compliance
- Benchmark different engines on the same routes
- Maintain a single codebase regardless of routing engine

### Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Application Code                         │
│                    (uses velo.h API)                         │
├─────────────────────────────────────────────────────────────┤
│                    Velo API Layer                            │
│          vl_route_coords(), vl_route_astar(), etc.           │
├─────────────────────────────────────────────────────────────┤
│                    Backend Dispatcher                        │
│                   (selects active backend)                   │
├─────────┬─────────────────┬─────────────────┬───────────────┤
│  Velo   │      OSRM       │   PTV xRoute    │   Future...   │
│ Native  │    Backend      │     Backend     │   (Valhalla)  │
└─────────┴─────────────────┴─────────────────┴───────────────┘
```

### Backend Interface

```c
// vl_backend.h

typedef enum {
    VL_BACKEND_NATIVE,      // Built-in Velo routing (default)
    VL_BACKEND_OSRM,        // OSRM via HTTP API
    VL_BACKEND_PTV,         // PTV xRoute via HTTP API
} VLBackend;

/**
 * Set the active routing backend
 * Must be called before routing operations
 */
int vl_set_backend(VLBackend backend);

/**
 * Get the currently active backend
 */
VLBackend vl_get_backend(void);

/**
 * Check if a backend is available (compiled in and configured)
 */
int vl_backend_available(VLBackend backend);

/**
 * Get backend version string
 */
const char* vl_backend_version(VLBackend backend);

/**
 * Configure backend connection (for HTTP-based backends)
 */
int vl_backend_configure(VLBackend backend, const char *base_url, const char *api_key);
```

### Internal Backend Abstraction

```c
// vl_backend_internal.h

typedef struct VLBackendOps {
    /* Initialization */
    int (*init)(const char *config);
    void (*shutdown)(void);

    /* Core routing */
    int (*route)(const VLCoord *origin, const VLCoord *dest,
                 const VLRouteOptions *opts, VLRoute *route);

    /* Multi-waypoint routing */
    int (*route_waypoints)(const VLCoord *waypoints, int num_waypoints,
                           const VLRouteOptions *opts, VLRoute *route);

    /* Distance/duration matrix */
    int (*matrix)(const VLCoord *origins, int num_origins,
                  const VLCoord *dests, int num_dests,
                  const VLRouteOptions *opts, VLMatrix *matrix);

    /* Nearest node/snap to road */
    int (*nearest)(const VLCoord *coord, const VLRouteOptions *opts,
                   VLCoord *snapped, double *distance);

    /* Isochrone/isodistance */
    int (*isochrone)(const VLCoord *origin, double limit,
                     const VLRouteOptions *opts, VLPolygon *polygon);

    /* Capabilities */
    int (*supports_profile)(VLProfile profile);
    int (*supports_weight)(VLWeight weight);
    int (*supports_traffic)(void);
    int (*supports_time_dependent)(void);

} VLBackendOps;

/* Backend implementations */
extern const VLBackendOps vl_native_ops;
extern const VLBackendOps vl_osrm_ops;
extern const VLBackendOps vl_ptv_ops;
```

### OSRM Backend Implementation

OSRM is deployed via Docker and exposes an HTTP API:

```bash
# Docker deployment
docker run -t -v "${PWD}:/data" osrm/osrm-backend osrm-extract -p /opt/car.lua /data/map.osm.pbf
docker run -t -v "${PWD}:/data" osrm/osrm-backend osrm-partition /data/map.osrm
docker run -t -v "${PWD}:/data" osrm/osrm-backend osrm-customize /data/map.osrm
docker run -t -p 5000:5000 -v "${PWD}:/data" osrm/osrm-backend osrm-routed --algorithm mld /data/map.osrm
```

```c
// vl_backend_osrm.c

#include <curl/curl.h>
#include "cJSON.h"  // or similar JSON parser

typedef struct {
    char base_url[256];     // e.g., "http://localhost:5000"
    CURL *curl;
    char *response_buffer;
    size_t response_size;
} OSRMContext;

static OSRMContext osrm_ctx;

static int osrm_init(const char *config) {
    // Parse config: "http://localhost:5000"
    strncpy(osrm_ctx.base_url, config, sizeof(osrm_ctx.base_url) - 1);
    osrm_ctx.curl = curl_easy_init();
    return osrm_ctx.curl ? 0 : -1;
}

static void osrm_shutdown(void) {
    if (osrm_ctx.curl) {
        curl_easy_cleanup(osrm_ctx.curl);
        osrm_ctx.curl = NULL;
    }
    free(osrm_ctx.response_buffer);
    osrm_ctx.response_buffer = NULL;
}

static int osrm_route(const VLCoord *origin, const VLCoord *dest,
                      const VLRouteOptions *opts, VLRoute *route) {
    char url[512];

    // Build OSRM route URL
    // Format: /route/v1/{profile}/{lon1},{lat1};{lon2},{lat2}?overview=full&geometries=polyline
    const char *profile = osrm_profile_string(opts ? opts->profile : VL_PROFILE_CAR);

    snprintf(url, sizeof(url),
             "%s/route/v1/%s/%.6f,%.6f;%.6f,%.6f"
             "?overview=full&geometries=polyline&steps=true",
             osrm_ctx.base_url, profile,
             origin->lon, origin->lat,
             dest->lon, dest->lat);

    // Execute HTTP request
    curl_easy_setopt(osrm_ctx.curl, CURLOPT_URL, url);
    curl_easy_setopt(osrm_ctx.curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(osrm_ctx.curl, CURLOPT_WRITEDATA, &osrm_ctx);

    CURLcode res = curl_easy_perform(osrm_ctx.curl);
    if (res != CURLE_OK) {
        return -1;
    }

    // Parse JSON response
    cJSON *json = cJSON_Parse(osrm_ctx.response_buffer);
    if (!json) return -1;

    cJSON *routes = cJSON_GetObjectItem(json, "routes");
    if (!routes || !cJSON_IsArray(routes) || cJSON_GetArraySize(routes) == 0) {
        cJSON_Delete(json);
        return -1;
    }

    cJSON *first_route = cJSON_GetArrayItem(routes, 0);

    // Extract distance and duration
    route->distance_m = cJSON_GetObjectItem(first_route, "distance")->valuedouble;
    route->duration_s = cJSON_GetObjectItem(first_route, "duration")->valuedouble;

    // Decode polyline geometry
    const char *geometry = cJSON_GetObjectItem(first_route, "geometry")->valuestring;
    decode_polyline(geometry, &route->coords, &route->num_coords);

    cJSON_Delete(json);
    return 0;
}

static int osrm_matrix(const VLCoord *origins, int num_origins,
                       const VLCoord *dests, int num_dests,
                       const VLRouteOptions *opts, VLMatrix *matrix) {
    char url[4096];
    char coords[2048];
    int offset = 0;

    // Build coordinate string: lon1,lat1;lon2,lat2;...
    for (int i = 0; i < num_origins; i++) {
        offset += snprintf(coords + offset, sizeof(coords) - offset,
                           "%.6f,%.6f;", origins[i].lon, origins[i].lat);
    }
    for (int i = 0; i < num_dests; i++) {
        offset += snprintf(coords + offset, sizeof(coords) - offset,
                           "%.6f,%.6f%s", dests[i].lon, dests[i].lat,
                           (i < num_dests - 1) ? ";" : "");
    }

    // Build sources and destinations indices
    char sources[256], destinations[256];
    int s_off = 0, d_off = 0;
    for (int i = 0; i < num_origins; i++) {
        s_off += snprintf(sources + s_off, sizeof(sources) - s_off,
                          "%d%s", i, (i < num_origins - 1) ? ";" : "");
    }
    for (int i = 0; i < num_dests; i++) {
        d_off += snprintf(destinations + d_off, sizeof(destinations) - d_off,
                          "%d%s", num_origins + i, (i < num_dests - 1) ? ";" : "");
    }

    const char *profile = osrm_profile_string(opts ? opts->profile : VL_PROFILE_CAR);

    snprintf(url, sizeof(url),
             "%s/table/v1/%s/%s?sources=%s&destinations=%s",
             osrm_ctx.base_url, profile, coords, sources, destinations);

    // Execute and parse response
    // ... similar to osrm_route ...

    return 0;
}

static const char* osrm_profile_string(VLProfile profile) {
    switch (profile) {
        case VL_PROFILE_CAR:   return "car";
        case VL_PROFILE_TRUCK: return "car";  // OSRM needs custom profile for truck
        case VL_PROFILE_BIKE:  return "bike";
        case VL_PROFILE_FOOT:  return "foot";
        default:               return "car";
    }
}

const VLBackendOps vl_osrm_ops = {
    .init = osrm_init,
    .shutdown = osrm_shutdown,
    .route = osrm_route,
    .route_waypoints = osrm_route_waypoints,
    .matrix = osrm_matrix,
    .nearest = osrm_nearest,
    .isochrone = NULL,  // OSRM doesn't support isochrones natively
    .supports_profile = osrm_supports_profile,
    .supports_weight = osrm_supports_weight,
    .supports_traffic = osrm_supports_traffic,
    .supports_time_dependent = osrm_supports_time_dependent,
};
```

### PTV xRoute Backend Implementation

PTV xRoute/xServer is a commercial routing engine with comprehensive truck routing:

```c
// vl_backend_ptv.c

#include <curl/curl.h>
#include "cJSON.h"

typedef struct {
    char base_url[256];     // e.g., "https://xserver2-europe-eu-test.cloud.ptvgroup.com"
    char api_key[128];      // PTV API key
    CURL *curl;
    char *response_buffer;
    size_t response_size;
} PTVContext;

static PTVContext ptv_ctx;

static int ptv_init(const char *config) {
    // Parse config: "url|api_key"
    char *sep = strchr(config, '|');
    if (sep) {
        strncpy(ptv_ctx.base_url, config, sep - config);
        strncpy(ptv_ctx.api_key, sep + 1, sizeof(ptv_ctx.api_key) - 1);
    } else {
        strncpy(ptv_ctx.base_url, config, sizeof(ptv_ctx.base_url) - 1);
    }
    ptv_ctx.curl = curl_easy_init();
    return ptv_ctx.curl ? 0 : -1;
}

static int ptv_route(const VLCoord *origin, const VLCoord *dest,
                     const VLRouteOptions *opts, VLRoute *route) {
    char url[512];
    char post_data[4096];

    // PTV xRoute uses POST with JSON body
    snprintf(url, sizeof(url), "%s/services/rs/XRoute/calculateRoute", ptv_ctx.base_url);

    // Build request JSON
    cJSON *request = cJSON_CreateObject();

    // Waypoints
    cJSON *waypoints = cJSON_CreateArray();
    cJSON *wp1 = cJSON_CreateObject();
    cJSON *wp1_coord = cJSON_CreateObject();
    cJSON_AddNumberToObject(wp1_coord, "x", origin->lon);
    cJSON_AddNumberToObject(wp1_coord, "y", origin->lat);
    cJSON_AddItemToObject(wp1, "offRoadCoordinate", wp1_coord);
    cJSON_AddItemToArray(waypoints, wp1);

    cJSON *wp2 = cJSON_CreateObject();
    cJSON *wp2_coord = cJSON_CreateObject();
    cJSON_AddNumberToObject(wp2_coord, "x", dest->lon);
    cJSON_AddNumberToObject(wp2_coord, "y", dest->lat);
    cJSON_AddItemToObject(wp2, "offRoadCoordinate", wp2_coord);
    cJSON_AddItemToArray(waypoints, wp2);

    cJSON_AddItemToObject(request, "waypoints", waypoints);

    // Vehicle profile
    cJSON *vehicle = cJSON_CreateObject();
    ptv_set_vehicle_profile(vehicle, opts ? opts->profile : VL_PROFILE_CAR);
    cJSON_AddItemToObject(request, "vehicle", vehicle);

    // Request options
    cJSON *result_fields = cJSON_CreateObject();
    cJSON_AddBoolToObject(result_fields, "polyline", 1);
    cJSON_AddItemToObject(request, "resultFields", result_fields);

    char *json_str = cJSON_PrintUnformatted(request);

    // Set up curl request
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Authorization: %s", ptv_ctx.api_key);
    headers = curl_slist_append(headers, auth_header);

    curl_easy_setopt(ptv_ctx.curl, CURLOPT_URL, url);
    curl_easy_setopt(ptv_ctx.curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(ptv_ctx.curl, CURLOPT_POSTFIELDS, json_str);
    curl_easy_setopt(ptv_ctx.curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(ptv_ctx.curl, CURLOPT_WRITEDATA, &ptv_ctx);

    CURLcode res = curl_easy_perform(ptv_ctx.curl);
    curl_slist_free_all(headers);
    cJSON_Delete(request);
    free(json_str);

    if (res != CURLE_OK) {
        return -1;
    }

    // Parse response
    cJSON *response = cJSON_Parse(ptv_ctx.response_buffer);
    if (!response) return -1;

    // Extract distance (meters) and travel time (seconds)
    route->distance_m = cJSON_GetObjectItem(response, "distance")->valuedouble;
    route->duration_s = cJSON_GetObjectItem(response, "travelTime")->valuedouble;

    // Extract polyline
    cJSON *polyline = cJSON_GetObjectItem(response, "polyline");
    if (polyline && cJSON_IsObject(polyline)) {
        cJSON *plain = cJSON_GetObjectItem(polyline, "plain");
        if (plain) {
            ptv_decode_polyline(plain, &route->coords, &route->num_coords);
        }
    }

    cJSON_Delete(response);
    return 0;
}

static void ptv_set_vehicle_profile(cJSON *vehicle, VLProfile profile) {
    switch (profile) {
        case VL_PROFILE_CAR:
            cJSON_AddStringToObject(vehicle, "type", "CAR");
            break;

        case VL_PROFILE_TRUCK:
            cJSON_AddStringToObject(vehicle, "type", "TRUCK");
            // Truck-specific attributes
            cJSON *dimensions = cJSON_CreateObject();
            cJSON_AddNumberToObject(dimensions, "height", 400);    // cm
            cJSON_AddNumberToObject(dimensions, "width", 255);     // cm
            cJSON_AddNumberToObject(dimensions, "length", 1650);   // cm
            cJSON_AddItemToObject(vehicle, "dimensions", dimensions);

            cJSON *weight = cJSON_CreateObject();
            cJSON_AddNumberToObject(weight, "total", 40000);       // kg
            cJSON_AddNumberToObject(weight, "axle", 11500);        // kg
            cJSON_AddItemToObject(vehicle, "weight", weight);
            break;

        case VL_PROFILE_BIKE:
            cJSON_AddStringToObject(vehicle, "type", "BICYCLE");
            break;

        case VL_PROFILE_FOOT:
            cJSON_AddStringToObject(vehicle, "type", "PEDESTRIAN");
            break;

        default:
            cJSON_AddStringToObject(vehicle, "type", "CAR");
    }
}

/* PTV supports time-dependent routing */
static int ptv_route_time_dependent(const VLCoord *origin, const VLCoord *dest,
                                     time_t departure_time,
                                     const VLRouteOptions *opts, VLRoute *route) {
    // Similar to ptv_route but with startTime parameter
    // PTV will account for traffic patterns and time-of-day restrictions
    return 0;
}

const VLBackendOps vl_ptv_ops = {
    .init = ptv_init,
    .shutdown = ptv_shutdown,
    .route = ptv_route,
    .route_waypoints = ptv_route_waypoints,
    .matrix = ptv_matrix,
    .nearest = ptv_nearest,
    .isochrone = ptv_isochrone,  // PTV supports isochrones
    .supports_profile = ptv_supports_profile,
    .supports_weight = ptv_supports_weight,
    .supports_traffic = ptv_supports_traffic,
    .supports_time_dependent = ptv_supports_time_dependent,
};
```

### Docker Deployment Configuration

```yaml
# docker-compose.yml for OSRM deployment

version: '3.8'

services:
  osrm:
    image: osrm/osrm-backend
    container_name: osrm-routing
    ports:
      - "5000:5000"
    volumes:
      - ./data:/data
    command: osrm-routed --algorithm mld /data/map.osrm
    restart: unless-stopped
    healthcheck:
      test: ["CMD", "curl", "-f", "http://localhost:5000/health"]
      interval: 30s
      timeout: 10s
      retries: 3

  # Optional: OSRM preprocessing container
  osrm-preprocess:
    image: osrm/osrm-backend
    volumes:
      - ./data:/data
    profiles:
      - preprocess
    command: >
      sh -c "osrm-extract -p /opt/car.lua /data/map.osm.pbf &&
             osrm-partition /data/map.osrm &&
             osrm-customize /data/map.osrm"
```

### Build Configuration

```makefile
# Makefile additions

# Optional routing backends
VELO_WITH_OSRM ?= 0
VELO_WITH_PTV ?= 0

ifeq ($(VELO_WITH_OSRM),1)
    CFLAGS += -DVELO_WITH_OSRM
    LDFLAGS += -lcurl
    BACKEND_SRCS += src/vl_backend_osrm.c src/vl_json.c
endif

ifeq ($(VELO_WITH_PTV),1)
    CFLAGS += -DVELO_WITH_PTV
    LDFLAGS += -lcurl
    BACKEND_SRCS += src/vl_backend_ptv.c src/vl_json.c
endif
```

### Runtime Backend Selection

```c
// Example usage

#include "velo.h"

int main() {
    // Check available backends
    printf("Native: %s\n", vl_backend_available(VL_BACKEND_NATIVE) ? "yes" : "no");
    printf("OSRM:   %s\n", vl_backend_available(VL_BACKEND_OSRM) ? "yes" : "no");
    printf("PTV:    %s\n", vl_backend_available(VL_BACKEND_PTV) ? "yes" : "no");

    // Configure and select OSRM backend
    if (vl_backend_available(VL_BACKEND_OSRM)) {
        vl_backend_configure(VL_BACKEND_OSRM, "http://localhost:5000", NULL);
        vl_set_backend(VL_BACKEND_OSRM);
        printf("Using OSRM backend\n");
    }

    // Use Velo API as normal - backend is transparent
    VLCoord origin = {47.4979, 19.0402};  // Budapest
    VLCoord dest = {46.2530, 20.1414};    // Szeged

    VLRoute route;
    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.profile = VL_PROFILE_TRUCK;

    int status = vl_route_coords(NULL, origin, dest, &opts, &route);
    if (status == 0) {
        printf("Route: %.1f km in %.1f min\n",
               route.distance_m / 1000.0, route.duration_s / 60.0);
    }

    vl_free_route(&route);
    return 0;
}
```

### Matrix API (Distance/Duration Tables)

```c
// vl_matrix.h

typedef struct {
    int num_origins;
    int num_destinations;
    double *distances;      // [num_origins × num_destinations], row-major
    double *durations;      // [num_origins × num_destinations], row-major
} VLMatrix;

/**
 * Compute distance/duration matrix between origins and destinations
 * Works with any backend (native uses repeated Dijkstra, OSRM/PTV use table API)
 */
int vl_matrix_compute(VLGraph *graph, const VLCoord *origins, int num_origins,
                      const VLCoord *dests, int num_dests,
                      const VLRouteOptions *opts, VLMatrix *matrix);

/**
 * Free matrix resources
 */
void vl_matrix_free(VLMatrix *matrix);

/**
 * Get distance from origin i to destination j
 */
static inline double vl_matrix_distance(const VLMatrix *m, int i, int j) {
    return m->distances[i * m->num_destinations + j];
}

/**
 * Get duration from origin i to destination j
 */
static inline double vl_matrix_duration(const VLMatrix *m, int i, int j) {
    return m->durations[i * m->num_destinations + j];
}
```

### Backend Capabilities Comparison

| Feature | Velo Native | OSRM | PTV xRoute |
|---------|-------------|------|------------|
| **Deployment** | Embedded/WASM | Docker | Docker/Cloud |
| **License** | MIT | BSD-2 | Commercial |
| **Basic Routing** | Yes | Yes | Yes |
| **Truck Routing** | Basic | Custom profile | Full (height, weight, hazmat) |
| **Time-Dependent** | No | No | Yes |
| **Live Traffic** | No | No | Yes (addon) |
| **Matrix API** | Via Dijkstra | Native | Native |
| **Isochrones** | No | Via plugin | Yes |
| **Turn Restrictions** | Basic | Yes | Yes |
| **Toll Calculation** | No | No | Yes |
| **Offline** | Yes | Yes | No |
| **Latency** | ~30ms | ~5ms | ~50-100ms |

### Caching Layer

For HTTP backends, implement response caching:

```c
// vl_cache.h

typedef struct {
    char *key;
    char *response;
    size_t response_len;
    time_t expires;
    struct VLCacheEntry *next;
} VLCacheEntry;

typedef struct {
    VLCacheEntry *entries;
    int num_entries;
    int max_entries;
    int ttl_seconds;
    pthread_mutex_t lock;
} VLCache;

/**
 * Initialize cache with max entries and TTL
 */
VLCache* vl_cache_create(int max_entries, int ttl_seconds);

/**
 * Look up cached response
 */
const char* vl_cache_get(VLCache *cache, const char *key);

/**
 * Store response in cache
 */
void vl_cache_put(VLCache *cache, const char *key, const char *response, size_t len);

/**
 * Clear expired entries
 */
void vl_cache_cleanup(VLCache *cache);
```

### Fallback Strategy

```c
// Automatic fallback when backend fails

int vl_route_with_fallback(VLGraph *graph, const VLCoord *origin, const VLCoord *dest,
                           const VLRouteOptions *opts, VLRoute *route) {
    VLBackend primary = vl_get_backend();
    VLBackend fallback = VL_BACKEND_NATIVE;

    // Try primary backend
    int status = vl_route_coords(graph, *origin, *dest, opts, route);
    if (status == 0) {
        return 0;
    }

    // Fallback to native if primary failed
    if (primary != VL_BACKEND_NATIVE && vl_backend_available(VL_BACKEND_NATIVE)) {
        fprintf(stderr, "Primary backend failed, falling back to native\n");
        vl_set_backend(VL_BACKEND_NATIVE);
        status = vl_route_coords(graph, *origin, *dest, opts, route);
        vl_set_backend(primary);  // Restore primary
    }

    return status;
}
```

### TODOs

**Phase 1: Backend Infrastructure**
- [ ] Create `include/vl_backend.h` with backend selection API
- [ ] Create `src/vl_backend.c` with dispatcher implementation
- [ ] Define `VLBackendOps` interface
- [ ] Implement native backend wrapper (`src/vl_backend_native.c`)
- [ ] Add build system support (Makefile)

**Phase 2: OSRM Backend**
- [ ] Create `src/vl_backend_osrm.c`
- [ ] Implement route API (single origin-destination)
- [ ] Implement table/matrix API
- [ ] Implement nearest/snap API
- [ ] Add polyline decoding (Google polyline format)
- [ ] Add Docker Compose configuration
- [ ] Test against native backend results

**Phase 3: PTV Backend**
- [ ] Create `src/vl_backend_ptv.c`
- [ ] Implement route API with JSON request/response
- [ ] Implement vehicle profile mapping (truck dimensions, weight)
- [ ] Implement matrix API
- [ ] Implement time-dependent routing
- [ ] Add authentication handling
- [ ] Test against native backend results

**Phase 4: Common Infrastructure**
- [ ] Add JSON parsing utilities (`src/vl_json.c`)
- [ ] Implement response caching
- [ ] Implement fallback strategy
- [ ] Add connection pooling for HTTP backends
- [ ] Add timeout and retry handling

**Phase 5: Testing and Documentation**
- [ ] Create comparison test suite (same routes, all backends)
- [ ] Benchmark latency across backends
- [ ] Document backend-specific limitations
- [ ] Add examples showing backend selection
- [ ] Create Docker deployment guide

### Files to Create/Modify

| File | Action |
|------|--------|
| `include/vl_backend.h` | **Create** - Backend selection API |
| `include/vl_matrix.h` | **Create** - Matrix API |
| `src/vl_backend.c` | **Create** - Backend dispatcher |
| `src/vl_backend_native.c` | **Create** - Native Velo wrapper |
| `src/vl_backend_osrm.c` | **Create** - OSRM integration |
| `src/vl_backend_ptv.c` | **Create** - PTV integration |
| `src/vl_json.c` | **Create** - JSON parsing utilities |
| `src/vl_cache.c` | **Create** - Response caching |
| `include/velo.h` | **Modify** - Add backend selection functions |
| `Makefile` | **Modify** - Add backend build options |
| `docker/docker-compose.yml` | **Create** - OSRM deployment |
| `tests/test_backends.c` | **Create** - Backend comparison tests |

### Notes

**Zero-dependency promise**: The native Velo backend remains the default and requires no external libraries. OSRM and PTV backends require libcurl and are strictly optional.

**WASM compatibility**: Only the native backend is suitable for WASM builds. HTTP backends require native compilation.

**Latency considerations**:
- Native Velo: ~30ms (ALT) - suitable for real-time applications
- OSRM: ~5ms (CH) - excellent for high-throughput servers
- PTV: ~50-100ms (network latency) - best for batch processing

**Truck routing**: For serious truck routing (height/weight restrictions, hazmat, toll optimization), PTV is the industry standard. OSRM requires custom Lua profiles. Native Velo provides basic filtering only.

---

## 2. Arc Flags

### Overview

Arc flags partition the graph into regions and precompute reachability bitmasks per edge, enabling aggressive pruning during search.

### Algorithm

**Preprocessing:**
1. Partition graph into k regions (grid-based or METIS)
2. For each edge (u,v), compute which regions are reachable from v
3. Store as k-bit flag per edge

**Query:**
1. Determine target region r
2. During relaxation: skip edge if `!(edge->arc_flags & (1 << r))`
3. Prunes 70-90% of edges

### Expected Performance

- **Preprocessing**: ~30 minutes for Hungary (parallelizable)
- **Memory**: +8 bytes/edge (64 regions) = ~44MB for Hungary
- **Speedup**: 3-5x over ALT alone

### TODOs

- [ ] Implement grid-based graph partitioning
- [ ] Implement arc flag computation (backward Dijkstra per region)
- [ ] Add arc flag checking in A* edge relaxation
- [ ] Parallelize preprocessing
- [ ] Benchmark combined with ALT

---

## 3. Reach Pruning

### Overview

Reach pruning eliminates nodes that can't appear on shortest paths based on their "reach" value.

### Algorithm

**Preprocessing:**
- reach(v) = max over all shortest paths P containing v of:
  min(dist(start(P), v), dist(v, end(P)))

**Query:**
- Prune node if: reach(v) < min(g(v), h(v))

### Approximation

Full reach computation is expensive. Use bounded Dijkstra:
- Run Dijkstra from each node with limited radius (~50km)
- Compute "local reach" = max shortest path distance within radius
- Still effective for interior nodes

### Expected Performance

- **Speedup**: 2-3x additional (combined with Arc Flags: 6-15x)

### TODOs

- [ ] Implement bounded Dijkstra for local reach
- [ ] Add reach values to VLGraph structure
- [ ] Implement reach pruning check in search loop
- [ ] Test combined with Arc Flags and ALT

---

## 4. Transit Node Routing

### Overview

For long-distance queries, identify a small set of "transit nodes" (highway junctions) that all long paths must pass through.

### Algorithm

**Preprocessing:**
1. Identify transit nodes (high-betweenness nodes)
2. Compute all-pairs shortest paths between transit nodes
3. For each node, identify its "access nodes" (nearest transit nodes)

**Query:**
- If query is "global": dist(s,t) = min over access nodes a,b of:
  dist(s,a) + dist(a,b) + dist(b,t)
- Table lookup instead of graph search

### Expected Performance

- **Query time**: Microseconds for long routes
- **Memory**: High (all-pairs table between transit nodes)
- **Best for**: Long-distance queries (>50km)

### TODOs

- [ ] Implement transit node identification
- [ ] Precompute all-pairs distances between transit nodes
- [ ] Compute access node sets
- [ ] Implement local/global query classification
- [ ] Benchmark on long-distance queries

---

## 5. Contraction Hierarchies

### Overview

Industry-standard speedup technique. Contract nodes in order of importance, creating shortcuts.

### Algorithm

**Preprocessing:**
1. Order nodes by "importance" (edge difference heuristic)
2. Contract nodes in order: remove node, add shortcuts to preserve distances
3. Build hierarchical graph

**Query:**
1. Bidirectional Dijkstra
2. Only relax edges going "up" in hierarchy
3. Meet in the middle at highest-importance node

### Expected Performance

- **Query time**: <1ms
- **Preprocessing**: Minutes to hours depending on graph size
- **Memory**: 2-3x edge count (shortcuts)

### Status

Deferred - significant implementation effort. External backends (OSRM) provide CH-based routing.

### TODOs (Deferred)

- [ ] Implement node ordering heuristic
- [ ] Implement contraction with shortcut creation
- [ ] Implement bidirectional upward search
- [ ] Implement shortcut unpacking for path geometry

---

## 6. Hub Labeling

### Overview

Alternative to CH with simpler query algorithm. Precompute label sets for each node.

### Algorithm

**Preprocessing:**
- L(v) = {(h, dist(v,h))} for hub nodes h reachable from v
- Requires careful hub selection for label size

**Query:**
- dist(s,t) = min over h in L(s) ∩ L(t) of: L(s)[h] + L(t)[h]
- Pure table lookup, no graph traversal

### Expected Performance

- **Query time**: Microseconds
- **Memory**: 4-8GB for Hungary
- **Simplicity**: Simpler than CH queries

### TODOs (Low Priority)

- [ ] Implement hierarchical hub labeling
- [ ] Optimize label compression
- [ ] Benchmark memory/speed tradeoff

---

## Implementation Priority

1. **External Backends (OSRM, PTV)** - High priority
   - Enables production-grade performance immediately
   - Low risk (wraps proven engines)
   - OSRM especially valuable (open source, Docker-ready)

2. **Arc Flags** - High priority
   - Best effort/reward ratio for native engine
   - Expected: ~15ms queries

3. **Reach Pruning** - Medium priority
   - Combines well with Arc Flags
   - Expected: ~8-12ms queries

4. **Transit Node Routing** - Medium priority
   - Excellent for long routes
   - Higher memory cost

5. **Contraction Hierarchies** - Low priority (deferred)
   - Use OSRM backend instead
   - Only implement if native sub-ms queries required

6. **Hub Labeling** - Low priority
   - Alternative to CH
   - Very high memory cost
