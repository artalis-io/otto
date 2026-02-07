# Carta Feature TODOs

This document outlines planned features for Carta with detailed implementation plans.

## Table of Contents

1. [External Tile Server Backends](#1-external-tile-server-backends)
2. [Level of Detail (LOD)](#2-level-of-detail-lod)
3. [Font/Label Rendering](#3-fontlabel-rendering)
4. [Configurable Styling](#4-configurable-styling)
5. [Native Performance Optimization](#5-native-performance-optimization)
6. [Client-Side MVT Rendering (WebGL)](#6-client-side-mvt-rendering-webgl)

---

## 1. External Tile Server Backends

### Motivation

While Carta provides a zero-dependency tile generator suitable for embedded and WASM deployments, production environments may benefit from heavyweight tile serving solutions:

| Server | License | Type | Strengths |
|--------|---------|------|-----------|
| **OpenMapTiles** | BSD-3 | Vector | Full OSM schema, Docker-ready, industry standard |
| **Martin** | MIT/Apache-2 | Vector | Rust-based, very fast, PostGIS integration |
| **TileServer GL** | BSD-2 | Both | MapLibre styles, serves MBTiles, raster from vector |
| **Tegola** | MIT | Vector | Go-based, multiple data sources, simple config |

By providing a common API layer with pluggable backends, users can:
- Use Carta for zero-dependency builds (WASM, embedded, offline)
- Switch to OpenMapTiles/Martin for production-grade vector tile serving
- Use TileServer GL for styled raster output from vector sources
- Benchmark different servers on the same tile requests
- Maintain a single codebase regardless of tile source

### Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Application Code                         │
│                    (uses carta.h API)                        │
├─────────────────────────────────────────────────────────────┤
│                    Carta API Layer                           │
│          ct_get_tile_mvt(), ct_get_tile_png(), etc.          │
├─────────────────────────────────────────────────────────────┤
│                    Backend Dispatcher                        │
│                   (selects active backend)                   │
├─────────┬─────────────────┬─────────────────┬───────────────┤
│  Carta  │  OpenMapTiles   │     Martin      │   Future...   │
│ Native  │    Backend      │    Backend      │   (Tegola)    │
└─────────┴─────────────────┴─────────────────┴───────────────┘
```

### Backend Interface

```c
// ct_backend.h

typedef enum {
    CT_BACKEND_NATIVE,          // Built-in Carta tile generation (default)
    CT_BACKEND_OPENMAPTILES,    // OpenMapTiles via HTTP/MBTiles
    CT_BACKEND_MARTIN,          // Martin tile server via HTTP
    CT_BACKEND_TILESERVER_GL,   // TileServer GL via HTTP
    CT_BACKEND_MBTILES,         // Direct MBTiles file access
} CTBackend;

/**
 * Set the active tile backend
 * Must be called before tile requests
 */
int ct_set_backend(CTBackend backend);

/**
 * Get the currently active backend
 */
CTBackend ct_get_backend(void);

/**
 * Check if a backend is available (compiled in and configured)
 */
int ct_backend_available(CTBackend backend);

/**
 * Get backend version string
 */
const char* ct_backend_version(CTBackend backend);

/**
 * Configure backend connection
 * For HTTP backends: base_url = "http://localhost:8080"
 * For MBTiles: base_url = "/path/to/tiles.mbtiles"
 */
int ct_backend_configure(CTBackend backend, const char *base_url, const char *api_key);
```

### Internal Backend Abstraction

```c
// ct_backend_internal.h

typedef struct CTBackendOps {
    /* Initialization */
    int (*init)(const char *config);
    void (*shutdown)(void);

    /* Tile retrieval */
    int (*get_tile_mvt)(int z, int x, int y, const char *layers,
                        uint8_t **data, size_t *size);
    int (*get_tile_png)(int z, int x, int y, const CTStyle *style,
                        uint8_t **data, size_t *size);
    int (*get_tile_pbf)(int z, int x, int y,
                        uint8_t **data, size_t *size);  // Compressed MVT

    /* Metadata */
    int (*get_metadata)(CTTilesetMetadata *metadata);
    int (*get_tilejson)(char **json, size_t *size);

    /* Capabilities */
    int (*supports_mvt)(void);
    int (*supports_png)(void);
    int (*supports_style)(const char *style_id);
    int (*get_zoom_range)(int *min_zoom, int *max_zoom);
    int (*get_bounds)(double *min_lon, double *min_lat,
                      double *max_lon, double *max_lat);

    /* Cache control */
    int (*set_cache_size)(size_t max_bytes);
    int (*clear_cache)(void);

} CTBackendOps;

/* Backend implementations */
extern const CTBackendOps ct_native_ops;
extern const CTBackendOps ct_openmaptiles_ops;
extern const CTBackendOps ct_martin_ops;
extern const CTBackendOps ct_tileserver_ops;
extern const CTBackendOps ct_mbtiles_ops;
```

### OpenMapTiles Backend Implementation

OpenMapTiles is the industry-standard OSM vector tile schema:

```bash
# Docker deployment
docker run -v $(pwd)/data:/data -p 8080:80 openmaptiles/openmaptiles-tools
# Or use pre-built tiles:
docker run -v $(pwd)/tiles.mbtiles:/data/tiles.mbtiles -p 8080:80 maptiler/tileserver-gl
```

```c
// ct_backend_openmaptiles.c

#include <curl/curl.h>
#include "cJSON.h"

typedef struct {
    char base_url[256];     // e.g., "http://localhost:8080"
    char api_key[128];      // Optional API key
    CURL *curl;
    char *response_buffer;
    size_t response_size;
    size_t response_capacity;
} OpenMapTilesContext;

static OpenMapTilesContext omt_ctx;

static int omt_init(const char *config) {
    // Parse config: "http://localhost:8080"
    strncpy(omt_ctx.base_url, config, sizeof(omt_ctx.base_url) - 1);
    omt_ctx.curl = curl_easy_init();
    omt_ctx.response_capacity = 1024 * 1024;  // 1MB initial
    omt_ctx.response_buffer = malloc(omt_ctx.response_capacity);
    return omt_ctx.curl ? 0 : -1;
}

static void omt_shutdown(void) {
    if (omt_ctx.curl) {
        curl_easy_cleanup(omt_ctx.curl);
        omt_ctx.curl = NULL;
    }
    free(omt_ctx.response_buffer);
    omt_ctx.response_buffer = NULL;
}

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    OpenMapTilesContext *ctx = userp;

    if (ctx->response_size + realsize > ctx->response_capacity) {
        ctx->response_capacity *= 2;
        ctx->response_buffer = realloc(ctx->response_buffer, ctx->response_capacity);
    }

    memcpy(ctx->response_buffer + ctx->response_size, contents, realsize);
    ctx->response_size += realsize;
    return realsize;
}

static int omt_get_tile_mvt(int z, int x, int y, const char *layers,
                            uint8_t **data, size_t *size) {
    char url[512];

    // OpenMapTiles URL format: /tiles/{z}/{x}/{y}.pbf
    snprintf(url, sizeof(url), "%s/tiles/%d/%d/%d.pbf",
             omt_ctx.base_url, z, x, y);

    omt_ctx.response_size = 0;

    curl_easy_setopt(omt_ctx.curl, CURLOPT_URL, url);
    curl_easy_setopt(omt_ctx.curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(omt_ctx.curl, CURLOPT_WRITEDATA, &omt_ctx);

    // Accept gzip encoding (MVT is often gzipped)
    curl_easy_setopt(omt_ctx.curl, CURLOPT_ACCEPT_ENCODING, "gzip");

    CURLcode res = curl_easy_perform(omt_ctx.curl);
    if (res != CURLE_OK) {
        return -1;
    }

    long http_code;
    curl_easy_getinfo(omt_ctx.curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        return -1;
    }

    // Copy response to caller
    *data = malloc(omt_ctx.response_size);
    memcpy(*data, omt_ctx.response_buffer, omt_ctx.response_size);
    *size = omt_ctx.response_size;

    return 0;
}

static int omt_get_tile_png(int z, int x, int y, const CTStyle *style,
                            uint8_t **data, size_t *size) {
    // OpenMapTiles serves vector tiles; PNG requires TileServer GL
    // Option 1: Fetch MVT and render locally with Carta
    // Option 2: Use TileServer GL backend for styled raster

    uint8_t *mvt_data;
    size_t mvt_size;

    int status = omt_get_tile_mvt(z, x, y, NULL, &mvt_data, &mvt_size);
    if (status != 0) return status;

    // Render MVT to PNG using Carta's native renderer
    CTTile tile;
    ct_tile_from_mvt(&tile, mvt_data, mvt_size);
    status = ct_render_tile_png(&tile, style, data, size);

    free(mvt_data);
    ct_tile_clear(&tile);

    return status;
}

static int omt_get_tilejson(char **json, size_t *size) {
    char url[512];
    snprintf(url, sizeof(url), "%s/tiles.json", omt_ctx.base_url);

    omt_ctx.response_size = 0;

    curl_easy_setopt(omt_ctx.curl, CURLOPT_URL, url);
    curl_easy_setopt(omt_ctx.curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(omt_ctx.curl, CURLOPT_WRITEDATA, &omt_ctx);

    CURLcode res = curl_easy_perform(omt_ctx.curl);
    if (res != CURLE_OK) return -1;

    *json = malloc(omt_ctx.response_size + 1);
    memcpy(*json, omt_ctx.response_buffer, omt_ctx.response_size);
    (*json)[omt_ctx.response_size] = '\0';
    *size = omt_ctx.response_size;

    return 0;
}

const CTBackendOps ct_openmaptiles_ops = {
    .init = omt_init,
    .shutdown = omt_shutdown,
    .get_tile_mvt = omt_get_tile_mvt,
    .get_tile_png = omt_get_tile_png,
    .get_tile_pbf = omt_get_tile_mvt,  // Same as MVT
    .get_metadata = omt_get_metadata,
    .get_tilejson = omt_get_tilejson,
    .supports_mvt = omt_supports_mvt,
    .supports_png = omt_supports_png,
    .get_zoom_range = omt_get_zoom_range,
    .get_bounds = omt_get_bounds,
};
```

### Martin Backend Implementation

Martin is a high-performance Rust tile server with PostGIS support:

```bash
# Docker deployment
docker run -p 3000:3000 \
  -e DATABASE_URL=postgresql://user:pass@host/db \
  ghcr.io/maplibre/martin

# Or with MBTiles:
docker run -p 3000:3000 \
  -v $(pwd)/tiles.mbtiles:/tiles.mbtiles \
  ghcr.io/maplibre/martin /tiles.mbtiles
```

```c
// ct_backend_martin.c

typedef struct {
    char base_url[256];     // e.g., "http://localhost:3000"
    CURL *curl;
    char *response_buffer;
    size_t response_size;
    size_t response_capacity;
    char **available_sources;  // List of tile sources
    int num_sources;
} MartinContext;

static MartinContext martin_ctx;

static int martin_init(const char *config) {
    strncpy(martin_ctx.base_url, config, sizeof(martin_ctx.base_url) - 1);
    martin_ctx.curl = curl_easy_init();
    martin_ctx.response_capacity = 1024 * 1024;
    martin_ctx.response_buffer = malloc(martin_ctx.response_capacity);

    // Fetch available sources from catalog
    martin_fetch_catalog();

    return martin_ctx.curl ? 0 : -1;
}

static int martin_get_tile_mvt(int z, int x, int y, const char *layers,
                               uint8_t **data, size_t *size) {
    char url[512];

    // Martin URL format: /{source}/{z}/{x}/{y}
    // If layers specified, use composite endpoint
    const char *source = layers ? layers : "osm";  // Default source

    snprintf(url, sizeof(url), "%s/%s/%d/%d/%d",
             martin_ctx.base_url, source, z, x, y);

    martin_ctx.response_size = 0;

    curl_easy_setopt(martin_ctx.curl, CURLOPT_URL, url);
    curl_easy_setopt(martin_ctx.curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(martin_ctx.curl, CURLOPT_WRITEDATA, &martin_ctx);
    curl_easy_setopt(martin_ctx.curl, CURLOPT_ACCEPT_ENCODING, "gzip");

    CURLcode res = curl_easy_perform(martin_ctx.curl);
    if (res != CURLE_OK) return -1;

    long http_code;
    curl_easy_getinfo(martin_ctx.curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code == 204 || http_code == 404) {
        // Empty tile - return empty but successful
        *data = NULL;
        *size = 0;
        return 0;
    }
    if (http_code != 200) return -1;

    *data = malloc(martin_ctx.response_size);
    memcpy(*data, martin_ctx.response_buffer, martin_ctx.response_size);
    *size = martin_ctx.response_size;

    return 0;
}

static int martin_fetch_catalog(void) {
    char url[512];
    snprintf(url, sizeof(url), "%s/catalog", martin_ctx.base_url);

    martin_ctx.response_size = 0;
    curl_easy_setopt(martin_ctx.curl, CURLOPT_URL, url);
    curl_easy_setopt(martin_ctx.curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(martin_ctx.curl, CURLOPT_WRITEDATA, &martin_ctx);

    CURLcode res = curl_easy_perform(martin_ctx.curl);
    if (res != CURLE_OK) return -1;

    // Parse JSON catalog to get available sources
    cJSON *json = cJSON_Parse(martin_ctx.response_buffer);
    if (!json) return -1;

    // Extract source names...
    cJSON_Delete(json);
    return 0;
}

const CTBackendOps ct_martin_ops = {
    .init = martin_init,
    .shutdown = martin_shutdown,
    .get_tile_mvt = martin_get_tile_mvt,
    .get_tile_png = martin_get_tile_png,  // Render locally
    .get_tile_pbf = martin_get_tile_mvt,
    .get_metadata = martin_get_metadata,
    .get_tilejson = martin_get_tilejson,
    .supports_mvt = martin_supports_mvt,
    .supports_png = martin_supports_png,
    .get_zoom_range = martin_get_zoom_range,
    .get_bounds = martin_get_bounds,
};
```

### MBTiles Backend (Direct File Access)

MBTiles is SQLite-based tile storage, common for offline/embedded use:

```c
// ct_backend_mbtiles.c

#include <sqlite3.h>

typedef struct {
    sqlite3 *db;
    sqlite3_stmt *tile_stmt;    // Prepared statement for tile queries
    int min_zoom, max_zoom;
    double bounds[4];           // minlon, minlat, maxlon, maxlat
    char *format;               // "pbf" or "png"
} MBTilesContext;

static MBTilesContext mbtiles_ctx;

static int mbtiles_init(const char *filepath) {
    int rc = sqlite3_open_v2(filepath, &mbtiles_ctx.db,
                             SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) return -1;

    // Prepare tile query
    const char *sql = "SELECT tile_data FROM tiles "
                      "WHERE zoom_level = ? AND tile_column = ? AND tile_row = ?";
    rc = sqlite3_prepare_v2(mbtiles_ctx.db, sql, -1, &mbtiles_ctx.tile_stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    // Read metadata
    mbtiles_read_metadata();

    return 0;
}

static void mbtiles_shutdown(void) {
    if (mbtiles_ctx.tile_stmt) {
        sqlite3_finalize(mbtiles_ctx.tile_stmt);
        mbtiles_ctx.tile_stmt = NULL;
    }
    if (mbtiles_ctx.db) {
        sqlite3_close(mbtiles_ctx.db);
        mbtiles_ctx.db = NULL;
    }
}

static int mbtiles_get_tile_mvt(int z, int x, int y, const char *layers,
                                uint8_t **data, size_t *size) {
    // MBTiles uses TMS y-coordinate (flip y)
    int tms_y = (1 << z) - 1 - y;

    sqlite3_reset(mbtiles_ctx.tile_stmt);
    sqlite3_bind_int(mbtiles_ctx.tile_stmt, 1, z);
    sqlite3_bind_int(mbtiles_ctx.tile_stmt, 2, x);
    sqlite3_bind_int(mbtiles_ctx.tile_stmt, 3, tms_y);

    int rc = sqlite3_step(mbtiles_ctx.tile_stmt);
    if (rc == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(mbtiles_ctx.tile_stmt, 0);
        int blob_size = sqlite3_column_bytes(mbtiles_ctx.tile_stmt, 0);

        *data = malloc(blob_size);
        memcpy(*data, blob, blob_size);
        *size = blob_size;

        // Check if gzipped and decompress if needed
        if (*size >= 2 && (*data)[0] == 0x1f && (*data)[1] == 0x8b) {
            uint8_t *decompressed;
            size_t decompressed_size;
            if (ct_gunzip(*data, *size, &decompressed, &decompressed_size) == 0) {
                free(*data);
                *data = decompressed;
                *size = decompressed_size;
            }
        }

        return 0;
    } else if (rc == SQLITE_DONE) {
        // No tile at this location
        *data = NULL;
        *size = 0;
        return 0;
    }

    return -1;
}

static int mbtiles_read_metadata(void) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT name, value FROM metadata";

    int rc = sqlite3_prepare_v2(mbtiles_ctx.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *name = (const char*)sqlite3_column_text(stmt, 0);
        const char *value = (const char*)sqlite3_column_text(stmt, 1);

        if (strcmp(name, "minzoom") == 0) {
            mbtiles_ctx.min_zoom = atoi(value);
        } else if (strcmp(name, "maxzoom") == 0) {
            mbtiles_ctx.max_zoom = atoi(value);
        } else if (strcmp(name, "bounds") == 0) {
            sscanf(value, "%lf,%lf,%lf,%lf",
                   &mbtiles_ctx.bounds[0], &mbtiles_ctx.bounds[1],
                   &mbtiles_ctx.bounds[2], &mbtiles_ctx.bounds[3]);
        } else if (strcmp(name, "format") == 0) {
            mbtiles_ctx.format = strdup(value);
        }
    }

    sqlite3_finalize(stmt);
    return 0;
}

const CTBackendOps ct_mbtiles_ops = {
    .init = mbtiles_init,
    .shutdown = mbtiles_shutdown,
    .get_tile_mvt = mbtiles_get_tile_mvt,
    .get_tile_png = mbtiles_get_tile_png,
    .get_tile_pbf = mbtiles_get_tile_mvt,
    .get_metadata = mbtiles_get_metadata,
    .get_tilejson = mbtiles_get_tilejson,
    .supports_mvt = mbtiles_supports_mvt,
    .supports_png = mbtiles_supports_png,
    .get_zoom_range = mbtiles_get_zoom_range,
    .get_bounds = mbtiles_get_bounds,
};
```

### TileServer GL Backend

TileServer GL serves styled raster tiles from vector sources:

```bash
# Docker deployment with style
docker run -p 8080:80 \
  -v $(pwd)/tiles.mbtiles:/data/tiles.mbtiles \
  -v $(pwd)/style.json:/data/styles/style.json \
  maptiler/tileserver-gl
```

```c
// ct_backend_tileserver.c

static int tileserver_get_tile_png(int z, int x, int y, const CTStyle *style,
                                   uint8_t **data, size_t *size) {
    char url[512];

    // TileServer GL raster endpoint
    // Format: /styles/{style_id}/{z}/{x}/{y}.png
    const char *style_id = style ? style->name : "basic";

    snprintf(url, sizeof(url), "%s/styles/%s/%d/%d/%d.png",
             tileserver_ctx.base_url, style_id, z, x, y);

    // Fetch PNG directly from server
    // ... curl request similar to other backends ...

    return 0;
}

static int tileserver_supports_style(const char *style_id) {
    // Query /styles.json to check available styles
    return 1;
}
```

### Docker Deployment Configuration

```yaml
# docker-compose.yml for tile server stack

version: '3.8'

services:
  # Option 1: Martin for high-performance vector tiles
  martin:
    image: ghcr.io/maplibre/martin
    container_name: martin-tiles
    ports:
      - "3000:3000"
    volumes:
      - ./data/tiles.mbtiles:/tiles.mbtiles
    command: /tiles.mbtiles
    restart: unless-stopped

  # Option 2: TileServer GL for styled raster + vector
  tileserver:
    image: maptiler/tileserver-gl
    container_name: tileserver-gl
    ports:
      - "8080:80"
    volumes:
      - ./data/tiles.mbtiles:/data/tiles.mbtiles
      - ./styles:/data/styles
    restart: unless-stopped

  # Option 3: OpenMapTiles stack (full generation)
  openmaptiles:
    image: openmaptiles/openmaptiles-tools
    container_name: openmaptiles
    ports:
      - "8081:80"
    volumes:
      - ./data:/data
    environment:
      - PGHOST=postgres
      - PGDATABASE=openmaptiles
    depends_on:
      - postgres

  postgres:
    image: postgis/postgis:15-3.3
    container_name: openmaptiles-db
    environment:
      - POSTGRES_DB=openmaptiles
      - POSTGRES_USER=openmaptiles
      - POSTGRES_PASSWORD=openmaptiles
    volumes:
      - pgdata:/var/lib/postgresql/data

volumes:
  pgdata:
```

### Build Configuration

```makefile
# Makefile additions

# Optional tile backends
CARTA_WITH_CURL ?= 0
CARTA_WITH_SQLITE ?= 0

ifeq ($(CARTA_WITH_CURL),1)
    CFLAGS += -DCARTA_WITH_CURL
    LDFLAGS += -lcurl
    BACKEND_SRCS += src/ct_backend_openmaptiles.c src/ct_backend_martin.c
    BACKEND_SRCS += src/ct_backend_tileserver.c src/ct_json.c
endif

ifeq ($(CARTA_WITH_SQLITE),1)
    CFLAGS += -DCARTA_WITH_SQLITE
    LDFLAGS += -lsqlite3
    BACKEND_SRCS += src/ct_backend_mbtiles.c
endif
```

### Runtime Backend Selection

```c
// Example usage

#include "carta.h"

int main() {
    // Check available backends
    printf("Native:       %s\n", ct_backend_available(CT_BACKEND_NATIVE) ? "yes" : "no");
    printf("OpenMapTiles: %s\n", ct_backend_available(CT_BACKEND_OPENMAPTILES) ? "yes" : "no");
    printf("Martin:       %s\n", ct_backend_available(CT_BACKEND_MARTIN) ? "yes" : "no");
    printf("MBTiles:      %s\n", ct_backend_available(CT_BACKEND_MBTILES) ? "yes" : "no");

    // Option 1: Use Martin for vector tiles
    if (ct_backend_available(CT_BACKEND_MARTIN)) {
        ct_backend_configure(CT_BACKEND_MARTIN, "http://localhost:3000", NULL);
        ct_set_backend(CT_BACKEND_MARTIN);
        printf("Using Martin backend\n");
    }

    // Option 2: Use local MBTiles file
    if (ct_backend_available(CT_BACKEND_MBTILES)) {
        ct_backend_configure(CT_BACKEND_MBTILES, "/data/planet.mbtiles", NULL);
        ct_set_backend(CT_BACKEND_MBTILES);
        printf("Using MBTiles backend\n");
    }

    // Fetch a tile - API is the same regardless of backend
    uint8_t *tile_data;
    size_t tile_size;

    int status = ct_get_tile_mvt(14, 8529, 5600, NULL, &tile_data, &tile_size);
    if (status == 0) {
        printf("Got MVT tile: %zu bytes\n", tile_size);

        // Render to PNG using Carta's renderer
        CTStyle style;
        ct_style_default(&style);

        uint8_t *png_data;
        size_t png_size;
        ct_render_mvt_to_png(tile_data, tile_size, &style, &png_data, &png_size);

        // Save PNG
        FILE *f = fopen("tile.png", "wb");
        fwrite(png_data, 1, png_size, f);
        fclose(f);

        free(png_data);
        free(tile_data);
    }

    return 0;
}
```

### Tile Caching Layer

```c
// ct_cache.h

typedef enum {
    CT_CACHE_MEMORY,        // In-memory LRU cache
    CT_CACHE_DISK,          // Disk-based cache
    CT_CACHE_MBTILES,       // Cache to MBTiles file
} CTCacheType;

typedef struct {
    CTCacheType type;
    size_t max_size;        // Max cache size (bytes or tiles)
    int max_age_seconds;    // TTL for cached tiles
    char *path;             // For disk/mbtiles cache
} CTCacheConfig;

/**
 * Create tile cache
 */
CTCache* ct_cache_create(const CTCacheConfig *config);

/**
 * Get tile from cache
 * Returns 0 if found, -1 if not in cache
 */
int ct_cache_get(CTCache *cache, int z, int x, int y,
                 uint8_t **data, size_t *size);

/**
 * Put tile in cache
 */
void ct_cache_put(CTCache *cache, int z, int x, int y,
                  const uint8_t *data, size_t size);

/**
 * Clear cache
 */
void ct_cache_clear(CTCache *cache);

/**
 * Free cache
 */
void ct_cache_free(CTCache *cache);
```

### Backend Capabilities Comparison

| Feature | Carta Native | OpenMapTiles | Martin | TileServer GL | MBTiles |
|---------|--------------|--------------|--------|---------------|---------|
| **Deployment** | Embedded/WASM | Docker | Docker | Docker | Embedded |
| **License** | MIT | BSD-3 | MIT | BSD-2 | N/A |
| **Vector Tiles** | Yes | Yes | Yes | Yes | Yes |
| **Raster Tiles** | Yes | Via TileServer | Via render | Yes | If pre-rendered |
| **Live Updates** | From PBF | Via PostGIS | Via PostGIS | No | No |
| **Offline** | Yes | No | No | No | Yes |
| **Styling** | Built-in | Via TileServer | Via render | MapLibre GL | N/A |
| **Performance** | ~50ms | ~5ms | ~2ms | ~10ms | ~1ms |
| **Data Source** | OSM PBF | PostGIS/PBF | PostGIS/MBTiles | MBTiles | SQLite |

### Fallback Strategy

```c
// Automatic fallback when backend fails

int ct_get_tile_with_fallback(int z, int x, int y, const char *layers,
                               uint8_t **data, size_t *size) {
    CTBackend primary = ct_get_backend();

    // Try cache first
    if (ct_cache_get(g_cache, z, x, y, data, size) == 0) {
        return 0;
    }

    // Try primary backend
    int status = ct_get_tile_mvt(z, x, y, layers, data, size);
    if (status == 0) {
        ct_cache_put(g_cache, z, x, y, *data, *size);
        return 0;
    }

    // Fallback to native if primary failed
    if (primary != CT_BACKEND_NATIVE && ct_backend_available(CT_BACKEND_NATIVE)) {
        fprintf(stderr, "Primary backend failed, falling back to native\n");
        ct_set_backend(CT_BACKEND_NATIVE);
        status = ct_get_tile_mvt(z, x, y, layers, data, size);
        ct_set_backend(primary);  // Restore primary

        if (status == 0) {
            ct_cache_put(g_cache, z, x, y, *data, *size);
        }
    }

    return status;
}
```

### TODOs

**Phase 1: Backend Infrastructure**
- [ ] Create `include/ct_backend.h` with backend selection API
- [ ] Create `src/ct_backend.c` with dispatcher implementation
- [ ] Define `CTBackendOps` interface
- [ ] Implement native backend wrapper (`src/ct_backend_native.c`)
- [ ] Add build system support (Makefile)

**Phase 2: HTTP Backends (OpenMapTiles, Martin, TileServer GL)**
- [ ] Create `src/ct_backend_openmaptiles.c`
- [ ] Create `src/ct_backend_martin.c`
- [ ] Create `src/ct_backend_tileserver.c`
- [ ] Add JSON parsing utilities (`src/ct_json.c`)
- [ ] Implement TileJSON metadata parsing
- [ ] Add gzip decompression for compressed tiles
- [ ] Create Docker Compose configuration

**Phase 3: MBTiles Backend**
- [ ] Create `src/ct_backend_mbtiles.c`
- [ ] Implement SQLite-based tile retrieval
- [ ] Handle TMS vs XYZ coordinate systems
- [ ] Implement metadata reading
- [ ] Support both gzipped and uncompressed tiles

**Phase 4: Caching Layer**
- [ ] Create `include/ct_cache.h` with cache API
- [ ] Implement in-memory LRU cache
- [ ] Implement disk-based cache
- [ ] Implement MBTiles cache writer
- [ ] Add cache statistics and management

**Phase 5: Testing and Documentation**
- [ ] Create comparison test suite (same tiles, all backends)
- [ ] Benchmark latency across backends
- [ ] Document backend-specific setup
- [ ] Add examples showing backend selection
- [ ] Create Docker deployment guide

### Files to Create/Modify

| File | Action |
|------|--------|
| `include/ct_backend.h` | **Create** - Backend selection API |
| `include/ct_cache.h` | **Create** - Tile cache API |
| `src/ct_backend.c` | **Create** - Backend dispatcher |
| `src/ct_backend_native.c` | **Create** - Native Carta wrapper |
| `src/ct_backend_openmaptiles.c` | **Create** - OpenMapTiles integration |
| `src/ct_backend_martin.c` | **Create** - Martin integration |
| `src/ct_backend_tileserver.c` | **Create** - TileServer GL integration |
| `src/ct_backend_mbtiles.c` | **Create** - MBTiles direct access |
| `src/ct_cache.c` | **Create** - Tile caching implementation |
| `src/ct_json.c` | **Create** - JSON parsing utilities |
| `include/carta.h` | **Modify** - Add backend selection functions |
| `Makefile` | **Modify** - Add backend build options |
| `docker/docker-compose.yml` | **Create** - Tile server deployment |
| `tests/test_backends.c` | **Create** - Backend comparison tests |

### Notes

**Zero-dependency promise**: The native Carta backend remains the default and requires no external libraries. HTTP backends require libcurl, MBTiles backend requires SQLite - both are strictly optional.

**WASM compatibility**: Only the native backend and MBTiles backend (with Emscripten SQLite) are suitable for WASM builds. HTTP backends require native compilation.

**Performance considerations**:
- Native Carta: ~50ms per tile (from PBF) - suitable for on-demand generation
- MBTiles: ~1ms per tile - excellent for pre-generated tiles
- Martin: ~2ms per tile - best for dynamic PostGIS data
- TileServer GL: ~10ms per tile - includes style rendering

---

## 2. Level of Detail (LOD)

### Problem

Currently, Carta renders all features at all zoom levels. This causes:
- **Visual clutter** at low zoom (z0-z10): residential roads, small buildings, minor waterways overwhelm the map
- **Performance issues**: unnecessary rendering of details invisible at the current scale
- **Large tile sizes**: encoding features that won't be visible increases MVT/PNG size

### Solution Overview

Implement zoom-dependent feature filtering at two stages:
1. **PBF Query Stage**: Filter features during spatial index queries
2. **Render Stage**: Skip features below minimum size threshold

### Implementation Plan

#### Phase 1: Define LOD Rules (`ct_lod.h`, `ct_lod.c`)

Create a new module for LOD configuration:

```c
// ct_lod.h
typedef struct {
    CTLayer layer;
    int feature_type;       // -1 for all types in layer
    int min_zoom;           // Minimum zoom to show this feature
    int max_zoom;           // Maximum zoom (optional, -1 for no limit)
    float min_area_sqm;     // Minimum area for polygons (at this zoom)
    float min_length_m;     // Minimum length for lines (at this zoom)
} CTLODRule;

typedef struct {
    CTLODRule *rules;
    int num_rules;
} CTLODConfig;

// Predefined configs
void ct_lod_default(CTLODConfig *config);
void ct_lod_detailed(CTLODConfig *config);   // More features at lower zoom
void ct_lod_minimal(CTLODConfig *config);    // Fewer features
```

**Default LOD Rules:**

| Layer | Feature Type | Min Zoom | Notes |
|-------|-------------|----------|-------|
| Roads | Motorway | 5 | Always visible at country level |
| Roads | Trunk | 7 | Visible at regional level |
| Roads | Primary | 9 | Visible at city level |
| Roads | Secondary | 11 | Visible at district level |
| Roads | Tertiary | 13 | Visible at neighborhood level |
| Roads | Residential | 14 | Visible at street level |
| Roads | Service | 16 | Only at close zoom |
| Buildings | All | 14 | Only at street level |
| Water | Large lakes | 5 | > 1km area |
| Water | Rivers | 8 | Major rivers |
| Water | Streams | 12 | Minor waterways |
| Railways | Main | 8 | Intercity rail |
| Railways | Other | 12 | Local rail |
| Landuse | Large areas | 10 | > 10km area |
| Landuse | Small areas | 14 | Parks, gardens |

#### Phase 2: Integrate with PBF Queries (`ct_pbf.c`)

Modify `ct_pbf_get_bbox_features()` to accept zoom level:

```c
// Current signature:
CTStatus ct_pbf_get_bbox_features(const CTPBFContext *ctx, CTBBox bbox,
                                   CTFeature **features, size_t *count);

// New signature:
CTStatus ct_pbf_get_tile_features(const CTPBFContext *ctx, CTTileCoord coord,
                                   const CTLODConfig *lod,
                                   CTFeature **features, size_t *count);
```

**Filtering logic in query:**
1. Check `min_zoom <= coord.z <= max_zoom`
2. For polygons: estimate area, skip if below threshold
3. For lines: estimate length, skip if below threshold
4. Return only features passing LOD filter

#### Phase 3: Geometry Simplification (`ct_simplify.c`)

At lower zoom levels, simplify geometry to reduce points:

```c
// Douglas-Peucker simplification
void ct_simplify_linestring(CTTilePoint *points, int *num_points, float tolerance);
void ct_simplify_polygon(CTTilePoint *points, int *num_points, float tolerance);

// Tolerance based on zoom (pixels at that zoom)
float ct_simplify_tolerance(int zoom);  // e.g., z10 = 4px, z14 = 1px, z18 = 0.25px
```

#### Phase 4: Render-time Filtering (`ct_render.c`)

Add minimum size checks in `ct_render_tile()`:

```c
// Skip features smaller than threshold at render time
float min_pixels = 2.0f;  // Minimum visible size

for (size_t i = 0; i < tile->num_features; i++) {
    const CTFeature *f = &tile->features[i];

    // Calculate bounding box in pixels
    float width = (max_x - min_x) * scale;
    float height = (max_y - min_y) * scale;

    if (f->type == CT_GEOM_LINESTRING && line_length < min_pixels) continue;
    if (f->type == CT_GEOM_POLYGON && width < min_pixels && height < min_pixels) continue;

    // Render feature...
}
```

### TODOs

- [x] Create `include/ct_lod.h` with LOD rule structures
- [x] Create `src/ct_lod.c` with default LOD configurations
- [x] Add `min_zoom` field to `CTOSMWay` during PBF parsing
- [x] Modify `ct_pbf_get_bbox_features()` to accept zoom parameter (`ct_pbf_get_tile_features_lod()`)
- [x] Implement area/length estimation for LOD filtering
- [x] Add Douglas-Peucker simplification in `ct_simplify.c`
- [x] Integrate LOD config into `ct_generate_png()` (`ct_generate_png_lod()`)
- [x] Add render-time size threshold filtering
- [ ] Update tests for LOD behavior
- [ ] Document LOD configuration in README
### LOD Quality Improvement Plan

Current LOD uses simple min_zoom rules per road type, which produces poor results compared to OSM. Below is a detailed plan to achieve OSM-quality LOD rendering.

#### Phase 1: Road Importance Scoring

Instead of just using highway type, compute an importance score for each road:

```c
typedef struct {
    float base_score;      /* From highway type: motorway=100, trunk=80, etc. */
    float length_bonus;    /* Longer roads get higher scores */
    float connectivity_bonus; /* Roads connecting important nodes */
    float ref_bonus;       /* Named routes (A1, M1, E50) get bonus */
    float final_score;     /* Combined score for LOD decisions */
} CTRoadImportance;
```

**Scoring formula:**
```
final_score = base_score
            + log10(length_m / 1000) * 10      /* +10 per order of magnitude in km */
            + connectivity_bonus               /* +20 if connects cities */
            + (has_ref ? 15 : 0)               /* +15 for named routes */
```

**Zoom thresholds based on score:**
| Score | Min Zoom |
|-------|----------|
| 100+  | z4       |
| 80+   | z6       |
| 60+   | z8       |
| 40+   | z10      |
| 20+   | z12      |
| 10+   | z14      |
| <10   | z16      |

#### Phase 2: Connectivity Analysis

Build a simplified road network graph during PBF parsing:

1. **Identify junction nodes** - nodes where 3+ ways meet
2. **Identify important nodes** - cities, towns (place=city/town tags)
3. **Score roads by what they connect:**
   - Connects two cities: +30
   - Connects city to town: +20
   - Connects two towns: +15
   - Dead-end or local loop: +0

```c
typedef struct {
    int64_t node_id;
    int connection_count;    /* Number of roads meeting here */
    int place_importance;    /* 0=none, 1=village, 2=town, 3=city */
} CTJunctionNode;
```

#### Phase 3: Length-Based Filtering

Roads should appear based on their length relative to the tile size:

```c
/* Meters per pixel at each zoom level (at equator) */
float meters_per_pixel[23] = {
    156543, 78271, 39135, 19567, 9783,   /* z0-z4 */
    4891, 2445, 1222, 611, 305,          /* z5-z9 */
    152, 76, 38, 19, 9.5,                /* z10-z14 */
    4.7, 2.3, 1.1, 0.5, 0.25,           /* z15-z19 */
    0.12, 0.06, 0.03                     /* z20-z22 */
};

/* Road should appear when it would be at least N pixels long */
int min_pixels_to_show = 50;  /* ~50 pixels minimum */

/* Check: road_length_m / meters_per_pixel[zoom] >= min_pixels_to_show */
```

#### Phase 4: Progressive Detail Levels

Instead of binary show/hide, use multiple detail levels:

| Zoom | Roads Shown | Simplification | Buildings | Water |
|------|-------------|----------------|-----------|-------|
| z0-z4 | score≥100 | Heavy (500m) | None | >100km² |
| z5-z7 | score≥60 | Medium (100m) | None | >10km² |
| z8-z10 | score≥30 | Light (25m) | None | >1km² |
| z11-z13 | score≥10 | Minimal (5m) | Large only | >0.01km² |
| z14-z16 | All classified | None | All | All |
| z17+ | All | None | All | All |

#### Phase 5: Area Feature Scaling

For polygons (buildings, water, landuse), use area-based thresholds:

```c
/* Minimum area in square meters to show at each zoom */
float min_area_sqm[23] = {
    1e12, 1e11, 1e10, 1e9, 1e8,         /* z0-z4: country-scale */
    1e7, 1e6, 5e5, 2e5, 1e5,            /* z5-z9: region-scale */
    5e4, 2e4, 1e4, 5000, 2000,          /* z10-z14: city-scale */
    500, 200, 100, 50, 20,              /* z15-z19: street-scale */
    10, 5, 1                             /* z20-z22: detail */
};
```

#### Phase 6: Improved Simplification

Current Douglas-Peucker tolerance is too aggressive. Use OSM-style tolerances:

| Zoom | Tolerance (meters) | Purpose |
|------|-------------------|---------|
| z0-z5 | 500m | Continental overview |
| z6-z8 | 100m | Country level |
| z9-z11 | 25m | Regional |
| z12-z13 | 5m | City level |
| z14+ | 1m or less | Street detail |

#### Phase 7: Reference Tag Parsing

Parse `ref=*` tags to identify important named routes:

```c
/* During PBF parsing, extract ref tag */
if (strcmp(key, "ref") == 0) {
    way->ref = strdup(val);
    /* Boost importance for numbered routes */
    if (val[0] == 'A' || val[0] == 'M' || val[0] == 'E') {
        way->importance_boost += 20;
    }
}
```

#### Implementation Order

1. **Phase 1** (Road Importance Scoring) - Most impact, moderate complexity
2. **Phase 3** (Length-Based Filtering) - Quick win, low complexity
3. **Phase 5** (Area Feature Scaling) - Already partially done, needs tuning
4. **Phase 6** (Improved Simplification) - Already done, needs parameter tuning
5. **Phase 4** (Progressive Detail) - Integration of above
6. **Phase 2** (Connectivity Analysis) - Complex, highest quality improvement
7. **Phase 7** (Reference Tags) - Polish, requires PBF parser changes

#### Files to Modify

| File | Changes |
|------|---------|
| `ct_types.h` | Add `CTRoadImportance` struct, expand `CTOSMWay` |
| `ct_lod.h` | Add importance-based LOD functions |
| `ct_lod.c` | Implement scoring and threshold logic |
| `ct_pbf.c` | Parse `ref` tags, compute importance during load |
| `ct_simplify.c` | Tune tolerance tables |

#### Success Criteria

- At z6: Only motorways and major trunk roads visible
- At z10: Primary roads and long secondary roads visible
- At z12: Most classified roads visible, residential starting to appear
- At z14: All roads visible, buildings appearing
- Smooth transitions between zoom levels (no jarring appearance/disappearance)
- Visual comparison with OSM standard style should be comparable

### Files to Modify/Create

| File | Action |
|------|--------|
| `include/ct_lod.h` | **Created** - LOD rule structures |
| `src/ct_lod.c` | **Created** - LOD configuration functions |
| `src/ct_simplify.c` | **Created** - Geometry simplification |
| `include/ct_simplify.h` | **Created** - Simplification header |
| `src/ct_pbf.c` | **Modified** - Add zoom-aware feature queries |
| `src/ct_render.c` | **Modified** - Add render-time LOD filtering |
| `include/ct_types.h` | **Modified** - Add min_zoom to CTOSMWay |

---

## 3. Font/Label Rendering

### Problem

Map tiles need text labels for:
- Road names
- Place names (cities, towns, villages)
- Points of interest
- Water body names

Currently, `CT_LAYER_LABELS` exists in the enum but has no implementation.

### Solution Options

#### Option A: Embedded Bitmap Font (Zero Dependencies)

**Approach**: Embed a simple bitmap font directly in the code.

**Pros:**
- Zero external dependencies
- Small footprint (~10-20KB for basic ASCII)
- Fast rendering (direct pixel copy)
- WASM-friendly

**Cons:**
- Limited to ASCII or small character sets
- Fixed sizes (need multiple bitmaps for different sizes)
- No anti-aliasing without multiple alpha levels
- Not suitable for international text

**Implementation:**

```c
// ct_font.h
typedef struct {
    uint8_t *bitmap;        // 1-bit or 8-bit alpha bitmap
    int char_width;         // Fixed width (or 0 for variable)
    int char_height;
    int *char_widths;       // For variable-width fonts
    int first_char;         // Usually 32 (space)
    int num_chars;          // Usually 95 (ASCII printable)
} CTBitmapFont;

// Built-in fonts
extern const CTBitmapFont ct_font_small;   // 8px height
extern const CTBitmapFont ct_font_medium;  // 12px height
extern const CTBitmapFont ct_font_large;   // 16px height

void ct_render_text(CTRenderContext *ctx, const char *text,
                    int x, int y, const CTBitmapFont *font, CTColor color);
```

**Data source**: Convert a public domain bitmap font (e.g., IBM PC fonts, GNU Unifont subset) to C arrays.

#### Option B: Vendored Font Library (stb_truetype)

**Approach**: Vendor `stb_truetype.h` (single-header, public domain).

**Pros:**
- TrueType/OpenType font support
- Scalable to any size
- Anti-aliased rendering
- Full Unicode support
- Still zero external dependencies (vendored)

**Cons:**
- Larger codebase (~5000 lines)
- Requires bundled font file
- More complex integration
- CPU-intensive glyph rasterization

**Implementation:**

```c
// ct_font.h (with stb_truetype)
typedef struct {
    stbtt_fontinfo info;
    uint8_t *data;          // Font file data
    float scale;            // Current scale factor
} CTFont;

CTFont *ct_font_load(const uint8_t *ttf_data, size_t size);
void ct_font_free(CTFont *font);

void ct_render_text(CTRenderContext *ctx, const char *text,
                    int x, int y, CTFont *font, float size, CTColor color);
```

**Font bundling**: Embed a permissively-licensed font (e.g., DejaVu Sans, Noto Sans) as a C array.

#### Option C: Hybrid Approach (Recommended)

Use **Option A (bitmap fonts) as default** with **Option B as optional**.

1. Ship with embedded bitmap fonts for basic ASCII labels
2. Allow users to optionally load TTF fonts via `stb_truetype`
3. Configure at compile time with `-DCT_USE_STB_TRUETYPE`

### Label Placement Algorithm

Regardless of font choice, need intelligent label placement:

```c
// ct_label.h
typedef struct {
    char *text;
    double lat, lon;        // Anchor point
    CTLayer layer;          // For priority sorting
    int feature_type;       // Road type, place type, etc.
    int priority;           // Higher = more important
} CTLabel;

typedef struct {
    CTLabel *labels;
    size_t count;
    // Collision detection grid
    uint8_t *occupied;      // Bitmap of occupied regions
    int grid_width;
    int grid_height;
} CTLabelContext;

// Extract labels from PBF data
void ct_extract_labels(const CTPBFContext *pbf, CTTileCoord coord,
                       CTLabel **labels, size_t *count);

// Place labels avoiding collisions
void ct_place_labels(CTLabelContext *ctx, CTLabel *labels, size_t count);

// Render placed labels
void ct_render_labels(CTRenderContext *render_ctx, const CTLabelContext *label_ctx);
```

**Placement rules:**
1. Sort labels by priority (major roads > minor roads > POIs)
2. For each label:
   - Calculate bounding box
   - Check collision grid
   - If no collision, mark grid cells as occupied
   - If collision, try alternate positions (for point labels)
3. Render non-colliding labels

### Label Styling

```c
// Add to CTStyle
typedef struct {
    CTColor text_color;
    CTColor halo_color;     // Outline/glow around text
    float halo_width;
    int font_size;          // In pixels
    int font_weight;        // 0=normal, 1=bold
} CTLabelStyle;

// In CTStyle:
CTLabelStyle road_label_styles[CT_ROAD_TYPE_COUNT];
CTLabelStyle place_label_style;
CTLabelStyle water_label_style;
```

### TODOs

- [x] Create font structures (in `shared/include/sh_font.h`)
- [x] Create embedded bitmap font (`shared/src/sh_font.c`)
- [x] Implement basic text rendering (`ct_render_text()`)
- [x] Add text halo rendering (outline around text for readability)
- [x] Create `include/ct_label.h` with label structures
- [x] Create `src/ct_label.c` with label extraction and placement
- [x] Implement collision detection grid (`src/ct_collision.c`)
- [x] Add `name` field extraction in PBF parsing
- [x] Integrate label rendering into tile generation
- [ ] (Optional) Vendor `stb_truetype.h` for TTF support
- [ ] (Optional) Embed DejaVu Sans font as C array
- [x] Add label styling to CTStyle
- [ ] Update tests for label rendering

### Files to Modify/Create

| File | Action |
|------|--------|
| `include/ct_font.h` | **Create** - Font structures and API |
| `src/ct_font_bitmap.c` | **Create** - Bitmap font rendering |
| `src/ct_font_data.c` | **Create** - Embedded font bitmap data |
| `include/ct_label.h` | **Create** - Label structures |
| `src/ct_label.c` | **Create** - Label placement algorithm |
| `vendor/stb_truetype.h` | **Optional** - TTF rendering library |
| `src/ct_font_ttf.c` | **Optional** - TTF font support |
| `tools/gen_font.py` | **Create** - Tool to generate font data |
| `src/ct_pbf.c` | **Modify** - Extract name tags |
| `src/ct_render.c` | **Modify** - Add label rendering pass |
| `include/ct_types.h` | **Modify** - Add label styles to CTStyle |

---

## 4. Configurable Styling

### Problem

Currently, styles are hardcoded in `ct_style.c`:
- Colors are fixed (OSM Carto-like palette)
- No way to change styles without recompiling
- No support for different map themes (dark mode, print, etc.)

### Solution Overview

Implement a flexible styling system with:
1. **Runtime-configurable styles** via C API
2. **Style presets** (light, dark, print, satellite overlay)
3. **Optional style file loading** (simple key-value format)

### Implementation Plan

#### Phase 1: Extended Style API (`ct_style.h`, `ct_style.c`)

Expand the existing `CTStyle` structure:

```c
// ct_style.h (extended)

// Layer visibility flags
typedef struct {
    int show_roads;
    int show_buildings;
    int show_water;
    int show_landuse;
    int show_railways;
    int show_labels;
    int show_boundaries;
} CTLayerVisibility;

// Complete style configuration
typedef struct {
    // Existing fields...
    CTColor road_colors[CT_ROAD_TYPE_COUNT];
    CTColor road_outline_colors[CT_ROAD_TYPE_COUNT];
    float road_widths[CT_ROAD_TYPE_COUNT];
    // ...

    // New: Layer visibility
    CTLayerVisibility visibility;

    // New: Opacity per layer (0.0 - 1.0)
    float road_opacity;
    float building_opacity;
    float water_opacity;
    float landuse_opacity;

    // New: Additional colors
    CTColor boundary_color;
    CTColor coastline_color;
    CTColor bridge_color;
    CTColor tunnel_color;

    // New: Line styles
    typedef enum { CT_LINE_SOLID, CT_LINE_DASHED, CT_LINE_DOTTED } CTLineStyle;
    CTLineStyle railway_line_style;
    CTLineStyle boundary_line_style;

    // New: Label styling (if implemented)
    CTLabelStyle label_styles[CT_LAYER_COUNT];

} CTStyle;

// Style presets
void ct_style_default(CTStyle *style);          // Current light theme
void ct_style_dark(CTStyle *style);             // Dark mode
void ct_style_print(CTStyle *style);            // High contrast for printing
void ct_style_satellite_overlay(CTStyle *style); // Transparent for satellite
void ct_style_minimal(CTStyle *style);          // Roads + water only

// Individual setters for common operations
void ct_style_set_road_color(CTStyle *style, CTRoadType type, CTColor color);
void ct_style_set_road_width(CTStyle *style, CTRoadType type, float width);
void ct_style_set_layer_visible(CTStyle *style, CTLayer layer, int visible);
void ct_style_set_layer_opacity(CTStyle *style, CTLayer layer, float opacity);
```

#### Phase 2: Style Presets

**Dark Mode (`ct_style_dark`):**
```c
void ct_style_dark(CTStyle *style)
{
    style->background_color = CT_RGB(30, 30, 30);
    style->land_color = CT_RGB(40, 40, 40);
    style->water_color = CT_RGB(20, 50, 70);

    style->road_colors[CT_ROAD_MOTORWAY] = CT_RGB(180, 100, 120);
    style->road_colors[CT_ROAD_TRUNK] = CT_RGB(180, 130, 100);
    style->road_colors[CT_ROAD_PRIMARY] = CT_RGB(150, 140, 90);
    style->road_colors[CT_ROAD_SECONDARY] = CT_RGB(100, 100, 80);
    style->road_colors[CT_ROAD_TERTIARY] = CT_RGB(80, 80, 80);
    style->road_colors[CT_ROAD_RESIDENTIAL] = CT_RGB(70, 70, 70);

    style->building_color = CT_RGB(50, 50, 50);
    style->building_outline_color = CT_RGB(60, 60, 60);
    // ...
}
```

**Satellite Overlay (`ct_style_satellite_overlay`):**
```c
void ct_style_satellite_overlay(CTStyle *style)
{
    // Transparent background
    style->background_color = CT_RGBA(0, 0, 0, 0);
    style->land_color = CT_RGBA(0, 0, 0, 0);

    // Semi-transparent roads with bright colors
    style->road_colors[CT_ROAD_MOTORWAY] = CT_RGBA(255, 200, 0, 200);
    style->road_colors[CT_ROAD_PRIMARY] = CT_RGBA(255, 255, 100, 180);

    // Hide buildings and landuse
    style->visibility.show_buildings = 0;
    style->visibility.show_landuse = 0;

    // Bright labels with dark halo
    // ...
}
```

#### Phase 3: Style File Loading (Optional)

Simple key-value format for external configuration:

```ini
# carta-style.conf

# General
background_color = #1e1e1e
land_color = #282828

# Roads
road_motorway_color = #e490a1
road_motorway_width = 8.0
road_trunk_color = #fbb289
road_trunk_width = 7.0

# Visibility
show_buildings = true
show_railways = false

# Opacity
water_opacity = 0.8
```

**Parser:**
```c
// ct_style.h
CTStatus ct_style_load(CTStyle *style, const char *filename);
CTStatus ct_style_load_string(CTStyle *style, const char *config_str);
CTStatus ct_style_save(const CTStyle *style, const char *filename);
```

#### Phase 4: Dashed/Dotted Line Support (`ct_render.c`)

For railways, boundaries, and tunnels:

```c
// Line style parameters
typedef struct {
    float dash_length;      // Length of dash in pixels
    float gap_length;       // Length of gap in pixels
    float offset;           // Starting offset (for animation)
} CTDashPattern;

void ct_render_polyline_dashed(CTRenderContext *ctx,
                               const CTTilePoint *points, int num_points,
                               CTColor color, float width,
                               const CTDashPattern *pattern);
```

### TODOs

- [x] Extend `CTStyle` with visibility flags (`CTRenderOptions`)
- [x] Add render options structure with layer toggles
- [ ] Implement `ct_style_dark()` preset
- [ ] Implement `ct_style_print()` preset
- [ ] Implement `ct_style_satellite_overlay()` preset
- [ ] Implement `ct_style_minimal()` preset
- [ ] Add individual style setter functions
- [x] Implement dashed line rendering (`ct_render_polyline_dashed()`)
- [x] Add dash pattern support for boundaries
- [ ] (Optional) Implement style file parser
- [ ] (Optional) Add style file saving
- [x] Update tile rendering to respect render options
- [x] Add alpha blending for layer opacity
- [ ] Document style API and presets
- [ ] Add style preview examples in tests

### Files to Modify/Create

| File | Action |
|------|--------|
| `include/ct_style.h` | **Create** - Extended style API (split from ct_types.h) |
| `src/ct_style.c` | **Modify** - Add presets and setters |
| `src/ct_style_dark.c` | **Create** - Dark mode preset |
| `src/ct_style_presets.c` | **Create** - All preset implementations |
| `src/ct_style_parser.c` | **Optional** - Config file parser |
| `src/ct_render.c` | **Modify** - Visibility checks, opacity, dashed lines |
| `include/ct_types.h` | **Modify** - Add visibility, line style enums |

---

## 5. Native Performance Optimization

### Performance Gap Analysis

Current performance comparison:

| Backend | Latency | Throughput | Notes |
|---------|---------|------------|-------|
| **Martin** | ~2ms | ~500 tiles/s | Pre-generated MBTiles, Rust, zero-copy |
| **Carta Native** | ~50ms | ~20 tiles/s | On-demand from PBF, C |
| **Target** | ~5ms | ~200 tiles/s | Competitive with external backends |

**Gap: 25x slower than Martin**

### Root Cause Analysis

The performance gap comes from fundamental architectural differences:

| Operation | Martin | Carta Native | Impact |
|-----------|--------|--------------|--------|
| **Data Source** | Pre-computed tiles in SQLite | Raw PBF, process on-demand | 10-20x |
| **Spatial Query** | None (tile already exists) | R-tree query + filtering | 2-5x |
| **Geometry Processing** | Pre-clipped, pre-simplified | Clip + simplify per request | 2-3x |
| **MVT Encoding** | Pre-encoded, just decompress | Encode from scratch | 2-3x |
| **Memory** | mmap + zero-copy | malloc per tile | 1.5-2x |

### Optimization Strategy

To close the gap, we need a **hybrid approach**:
1. **Tile caching** - Don't regenerate tiles that haven't changed
2. **Pre-computation** - Move work from request-time to load-time
3. **Algorithmic improvements** - Faster spatial queries, clipping, encoding
4. **Memory optimization** - Reduce allocations, use pools
5. **Parallelization** - Multi-threaded tile generation

---

### Phase 1: Tile Caching (Expected: 50ms → 2ms for cached tiles)

The single biggest win. Most tiles are requested repeatedly.

#### In-Memory LRU Cache

```c
// ct_tile_cache.h

typedef struct {
    uint64_t key;           // z << 40 | x << 20 | y
    uint8_t *data;
    size_t size;
    uint32_t last_access;   // For LRU eviction
    uint32_t generation;    // For invalidation
} CTCacheEntry;

typedef struct {
    CTCacheEntry *entries;
    int capacity;
    int count;
    size_t max_bytes;
    size_t current_bytes;

    // Hash table for O(1) lookup
    int *hash_table;
    int hash_size;

    // LRU tracking
    int *lru_prev;
    int *lru_next;
    int lru_head, lru_tail;

    // Statistics
    uint64_t hits, misses;
} CTTileCache;

CTTileCache* ct_cache_create(size_t max_bytes, int max_tiles);
void ct_cache_free(CTTileCache *cache);

// Returns cached tile or NULL
const uint8_t* ct_cache_get(CTTileCache *cache, int z, int x, int y, size_t *size);

// Store tile in cache (takes ownership of data)
void ct_cache_put(CTTileCache *cache, int z, int x, int y, uint8_t *data, size_t size);

// Invalidate tiles in bbox (for updates)
void ct_cache_invalidate_bbox(CTTileCache *cache, CTBBox bbox, int min_z, int max_z);
```

**Cache sizing:**
- 1GB cache ≈ 50,000 tiles at ~20KB average
- Hit rate of 90%+ expected for typical usage

#### Disk-Based Cache (SQLite)

For persistence across restarts:

```c
// ct_disk_cache.c

typedef struct {
    sqlite3 *db;
    sqlite3_stmt *get_stmt;
    sqlite3_stmt *put_stmt;
    size_t max_bytes;
} CTDiskCache;

// Schema:
// CREATE TABLE tiles (
//     z INTEGER, x INTEGER, y INTEGER,
//     data BLOB,
//     size INTEGER,
//     created INTEGER,
//     PRIMARY KEY (z, x, y)
// );
// CREATE INDEX idx_created ON tiles(created);

int ct_disk_cache_get(CTDiskCache *cache, int z, int x, int y,
                      uint8_t **data, size_t *size);
void ct_disk_cache_put(CTDiskCache *cache, int z, int x, int y,
                       const uint8_t *data, size_t size);
void ct_disk_cache_evict_oldest(CTDiskCache *cache, size_t bytes_to_free);
```

#### Two-Level Cache

Combine memory and disk for best of both:

```c
typedef struct {
    CTTileCache *l1;        // Fast in-memory cache
    CTDiskCache *l2;        // Persistent disk cache
} CTTwoLevelCache;

const uint8_t* ct_cache_get_2l(CTTwoLevelCache *cache, int z, int x, int y, size_t *size) {
    // Try L1 first
    const uint8_t *data = ct_cache_get(cache->l1, z, x, y, size);
    if (data) return data;

    // Try L2
    uint8_t *disk_data;
    if (ct_disk_cache_get(cache->l2, z, x, y, &disk_data, size) == 0) {
        // Promote to L1
        ct_cache_put(cache->l1, z, x, y, disk_data, *size);
        return disk_data;
    }

    return NULL;  // Cache miss
}
```

---

### Phase 2: Spatial Index Optimization (Expected: 5-10x query speedup)

#### Current: R-tree on Features

Current approach queries R-tree for every tile, returning features that intersect the tile bbox.

**Problems:**
- R-tree query returns features, not pre-organized data
- Large features (country borders) appear in every tile
- No zoom-level awareness

#### Optimization 2a: Tile-Based Spatial Index

Pre-organize features by tile at load time:

```c
// ct_tile_index.h

typedef struct {
    int z;                  // Zoom level for this index
    int num_tiles_x;        // 2^z
    int num_tiles_y;        // 2^z

    // Per-tile feature lists
    struct {
        uint32_t *feature_ids;
        int count;
        int capacity;
    } *tiles;               // [num_tiles_x * num_tiles_y]

} CTTileIndex;

typedef struct {
    CTTileIndex *indices;   // One per zoom level (e.g., z0-z14)
    int min_zoom;
    int max_zoom;
} CTTileIndexSet;

// Build indices at PBF load time
CTTileIndexSet* ct_build_tile_indices(const CTPBFContext *pbf, int min_z, int max_z);

// Get features for tile - O(1) lookup instead of R-tree query
const uint32_t* ct_get_tile_features(const CTTileIndexSet *idx, int z, int x, int y, int *count);
```

**Memory cost:** ~100MB for z0-z14 on Hungary-sized region

**Build time:** ~30 seconds at PBF load (one-time)

#### Optimization 2b: Hierarchical Feature Assignment

For large features spanning many tiles, store at coarser zoom and inherit:

```c
// Feature stored at lowest zoom where it fits in ≤4 tiles
// Child tiles inherit parent's features

typedef struct {
    int stored_zoom;        // Zoom level where feature is stored
    int stored_x, stored_y; // Tile coordinates at stored_zoom
} CTFeatureLocation;

// Query: collect features from this tile + all ancestors
void ct_collect_tile_features(const CTTileIndexSet *idx, int z, int x, int y,
                              uint32_t **features, int *count) {
    // Start from z, walk up to z0, collecting features
    for (int zoom = z; zoom >= 0; zoom--) {
        int tile_x = x >> (z - zoom);
        int tile_y = y >> (z - zoom);
        // Append features stored at this ancestor tile
    }
}
```

---

### Phase 3: Geometry Processing Optimization (Expected: 2-3x speedup)

#### Optimization 3a: Pre-Clipped Geometry Cache

Store clipped geometry per tile instead of re-clipping:

```c
// At index build time, pre-clip features to tile boundaries
typedef struct {
    uint32_t feature_id;
    CTGeometry clipped;     // Geometry clipped to this tile
} CTClippedFeature;

typedef struct {
    CTClippedFeature *features;
    int count;
} CTPreclippedTile;

// Build during indexing
void ct_preclip_tile(const CTPBFContext *pbf, int z, int x, int y,
                     const uint32_t *feature_ids, int num_features,
                     CTPreclippedTile *out);
```

**Trade-off:** Higher memory/disk usage, but eliminates clipping at request time.

#### Optimization 3b: Sutherland-Hodgman with Early Exit

Current clipping may process entire polygon even when it's clearly inside/outside:

```c
// Fast bbox check before full clip
typedef enum {
    CT_CLIP_INSIDE,     // Entirely inside tile - no clipping needed
    CT_CLIP_OUTSIDE,    // Entirely outside tile - skip feature
    CT_CLIP_PARTIAL,    // Needs clipping
} CTClipResult;

CTClipResult ct_classify_bbox(const CTBBox *feature_bbox, const CTBBox *tile_bbox) {
    if (feature_bbox->min_x >= tile_bbox->min_x &&
        feature_bbox->max_x <= tile_bbox->max_x &&
        feature_bbox->min_y >= tile_bbox->min_y &&
        feature_bbox->max_y <= tile_bbox->max_y) {
        return CT_CLIP_INSIDE;
    }
    if (feature_bbox->max_x < tile_bbox->min_x ||
        feature_bbox->min_x > tile_bbox->max_x ||
        feature_bbox->max_y < tile_bbox->min_y ||
        feature_bbox->min_y > tile_bbox->max_y) {
        return CT_CLIP_OUTSIDE;
    }
    return CT_CLIP_PARTIAL;
}
```

#### Optimization 3c: SIMD Coordinate Transform

Batch coordinate transforms with SIMD:

```c
#include <immintrin.h>

// Transform 4 coordinates at once (AVX)
void ct_transform_coords_simd(const double *lons, const double *lats,
                               int *tile_x, int *tile_y, int count,
                               int zoom, const CTBBox *tile_bbox) {
    __m256d scale_x = _mm256_set1_pd(4096.0 / (tile_bbox->max_x - tile_bbox->min_x));
    __m256d scale_y = _mm256_set1_pd(4096.0 / (tile_bbox->max_y - tile_bbox->min_y));
    __m256d offset_x = _mm256_set1_pd(tile_bbox->min_x);
    __m256d offset_y = _mm256_set1_pd(tile_bbox->min_y);

    for (int i = 0; i < count; i += 4) {
        __m256d lon = _mm256_loadu_pd(&lons[i]);
        __m256d lat = _mm256_loadu_pd(&lats[i]);

        __m256d x = _mm256_mul_pd(_mm256_sub_pd(lon, offset_x), scale_x);
        __m256d y = _mm256_mul_pd(_mm256_sub_pd(lat, offset_y), scale_y);

        // Convert to int and store
        __m128i xi = _mm256_cvtpd_epi32(x);
        __m128i yi = _mm256_cvtpd_epi32(y);

        _mm_storeu_si128((__m128i*)&tile_x[i], xi);
        _mm_storeu_si128((__m128i*)&tile_y[i], yi);
    }
}
```

#### Optimization 3d: Douglas-Peucker with Stack (No Recursion)

Avoid recursion overhead in simplification:

```c
void ct_simplify_dp_iterative(CTPoint *points, int *num_points, double epsilon) {
    int *stack = malloc(*num_points * 2 * sizeof(int));
    int stack_top = 0;
    uint8_t *keep = calloc(*num_points, 1);

    keep[0] = keep[*num_points - 1] = 1;
    stack[stack_top++] = 0;
    stack[stack_top++] = *num_points - 1;

    while (stack_top > 0) {
        int end = stack[--stack_top];
        int start = stack[--stack_top];

        double max_dist = 0;
        int max_idx = start;

        for (int i = start + 1; i < end; i++) {
            double dist = point_line_distance(points[i], points[start], points[end]);
            if (dist > max_dist) {
                max_dist = dist;
                max_idx = i;
            }
        }

        if (max_dist > epsilon) {
            keep[max_idx] = 1;
            stack[stack_top++] = start;
            stack[stack_top++] = max_idx;
            stack[stack_top++] = max_idx;
            stack[stack_top++] = end;
        }
    }

    // Compact points array
    int j = 0;
    for (int i = 0; i < *num_points; i++) {
        if (keep[i]) points[j++] = points[i];
    }
    *num_points = j;

    free(stack);
    free(keep);
}
```

---

### Phase 4: MVT Encoding Optimization (Expected: 2x speedup)

#### Optimization 4a: Pre-Allocated Buffers

Avoid repeated malloc during encoding:

```c
typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t size;

    // Scratch space for encoding
    int32_t *command_buffer;
    int command_capacity;

    // String interning for keys/values
    char **key_table;
    int num_keys;
    char **value_table;
    int num_values;
} CTMVTEncoder;

CTMVTEncoder* ct_mvt_encoder_create(size_t initial_capacity);
void ct_mvt_encoder_reset(CTMVTEncoder *enc);  // Reuse for next tile
void ct_mvt_encoder_free(CTMVTEncoder *enc);
```

#### Optimization 4b: Batched Varint Encoding

Encode multiple varints at once:

```c
// Encode 4 varints at once using table lookup
static const uint8_t varint_table[128][2] = {
    {0x00, 1}, {0x01, 1}, ..., {0x7f, 1},  // 1-byte varints
};

void ct_encode_varints_batch(uint8_t *out, const uint32_t *values, int count, int *bytes_written) {
    int offset = 0;
    for (int i = 0; i < count; i++) {
        uint32_t v = values[i];
        if (v < 128) {
            out[offset++] = varint_table[v][0];
        } else {
            // Multi-byte encoding
            while (v >= 128) {
                out[offset++] = (v & 0x7f) | 0x80;
                v >>= 7;
            }
            out[offset++] = v;
        }
    }
    *bytes_written = offset;
}
```

#### Optimization 4c: Delta Encoding with SIMD

```c
// Compute deltas for coordinate arrays using SIMD
void ct_compute_deltas_simd(const int32_t *coords, int32_t *deltas, int count) {
    __m128i prev = _mm_setzero_si128();

    for (int i = 0; i < count; i += 4) {
        __m128i curr = _mm_loadu_si128((__m128i*)&coords[i]);
        __m128i shifted = _mm_alignr_epi8(curr, prev, 12);
        __m128i delta = _mm_sub_epi32(curr, shifted);

        // Zigzag encode: (n << 1) ^ (n >> 31)
        __m128i shifted_delta = _mm_slli_epi32(delta, 1);
        __m128i sign = _mm_srai_epi32(delta, 31);
        __m128i zigzag = _mm_xor_si128(shifted_delta, sign);

        _mm_storeu_si128((__m128i*)&deltas[i], zigzag);
        prev = curr;
    }
}
```

---

### Phase 5: Memory Optimization (Expected: 1.5-2x speedup)

#### Optimization 5a: Memory Pool for Tile Generation

```c
// ct_pool.h

typedef struct {
    uint8_t *base;
    size_t capacity;
    size_t used;
} CTMemoryPool;

CTMemoryPool* ct_pool_create(size_t size);
void* ct_pool_alloc(CTMemoryPool *pool, size_t size);
void ct_pool_reset(CTMemoryPool *pool);  // Reset for next tile (O(1))
void ct_pool_free(CTMemoryPool *pool);

// Thread-local pool for concurrent tile generation
typedef struct {
    CTMemoryPool *pool;
    CTMVTEncoder *encoder;
    // Other per-thread scratch space
} CTTileContext;

CTTileContext* ct_tile_context_create(void);
void ct_tile_context_reset(CTTileContext *ctx);
```

#### Optimization 5b: Compact Feature Storage

```c
// Instead of storing full geometry per feature, store offsets into shared arrays
typedef struct {
    uint32_t type : 2;          // Point, Line, Polygon
    uint32_t coord_offset : 30; // Offset into coordinate array
    uint16_t coord_count;
    uint16_t tag_offset;        // Offset into tag array
    uint8_t tag_count;
} CTCompactFeature;

// Coordinates stored as fixed-point (microdegrees)
typedef struct {
    int32_t x;  // lon * 1e6
    int32_t y;  // lat * 1e6
} CTCompactCoord;
```

---

### Phase 6: Parallelization (Expected: 4-8x throughput)

#### Optimization 6a: Thread Pool for Tile Generation

```c
// ct_threadpool.h

typedef struct {
    pthread_t *threads;
    int num_threads;

    // Work queue
    struct {
        int z, x, y;
        void (*callback)(int z, int x, int y, uint8_t *data, size_t size, void *user);
        void *user_data;
    } *queue;
    int queue_size;
    int queue_head, queue_tail;

    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int shutdown;

    // Per-thread contexts
    CTTileContext **contexts;
} CTThreadPool;

CTThreadPool* ct_threadpool_create(int num_threads);
void ct_threadpool_submit(CTThreadPool *pool, int z, int x, int y,
                          void (*callback)(int, int, int, uint8_t*, size_t, void*),
                          void *user_data);
void ct_threadpool_shutdown(CTThreadPool *pool);
```

#### Optimization 6b: Batch Tile Generation

Generate multiple tiles in parallel:

```c
// Generate all tiles for a bbox at a zoom level
void ct_generate_tiles_batch(CTPBFContext *pbf, int z,
                             int min_x, int max_x, int min_y, int max_y,
                             CTTileCallback callback, void *user_data) {
    CTThreadPool *pool = ct_threadpool_create(ct_get_num_cores());

    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            ct_threadpool_submit(pool, z, x, y, callback, user_data);
        }
    }

    ct_threadpool_wait(pool);
    ct_threadpool_shutdown(pool);
}
```

---

### Phase 7: Pre-Generation Pipeline (Expected: Match Martin's 2ms)

For truly matching Martin's performance, pre-generate tiles:

```c
// ct_pregenerate.h

typedef struct {
    const CTPBFContext *pbf;
    const CTStyle *style;
    const CTLODConfig *lod;

    int min_zoom, max_zoom;
    CTBBox bounds;

    // Output
    const char *output_path;  // MBTiles file
    CTTileFormat format;      // MVT or PNG

    // Progress
    void (*progress)(int tiles_done, int tiles_total, void *user);
    void *user_data;
} CTPregenConfig;

/**
 * Pre-generate all tiles and write to MBTiles
 * Can then serve via MBTiles backend at ~1ms/tile
 */
int ct_pregenerate_tiles(const CTPregenConfig *config);
```

**Workflow:**
1. Load PBF once
2. Build tile indices
3. Generate all tiles for z0-z14 (parallelized)
4. Write to MBTiles
5. Serve via ct_mbtiles_ops backend

---

### Performance Roadmap

| Phase | Optimization | Latency Impact | Effort |
|-------|--------------|----------------|--------|
| 1 | Tile caching | 50ms → 2ms (cached) | Low |
| 2 | Tile-based spatial index | 50ms → 20ms | Medium |
| 3 | Geometry optimization | 20ms → 10ms | Medium |
| 4 | MVT encoding optimization | 10ms → 7ms | Low |
| 5 | Memory pooling | 7ms → 5ms | Low |
| 6 | Parallelization | 5ms → 5ms (but 4x throughput) | Medium |
| 7 | Pre-generation | 5ms → 2ms | Medium |
| 8 | Boundary optimization | Reduce boundary overhead to <5ms | Medium |

**Target achieved:** 50ms → 5ms for uncached, 2ms for cached/pre-generated

---

### TODOs

**Phase 1: Tile Caching**
- [ ] Implement in-memory LRU cache (`ct_tile_cache.c`)
- [ ] Implement disk cache with SQLite (`ct_disk_cache.c`)
- [ ] Implement two-level cache
- [ ] Add cache statistics and monitoring
- [ ] Add cache invalidation for bbox updates
- [ ] Benchmark cache hit rates

**Phase 2: Spatial Index Optimization**
- [ ] Implement tile-based spatial index
- [ ] Build indices at PBF load time
- [ ] Implement hierarchical feature assignment
- [ ] Benchmark query time improvement

**Phase 3: Geometry Processing**
- [ ] Add early-exit bbox classification
- [ ] Implement iterative Douglas-Peucker
- [x] Add SIMD coordinate transforms (with fallback)
- [ ] Pre-clip geometry at index time (optional)

**Phase 4: MVT Encoding**
- [ ] Implement encoder with pre-allocated buffers
- [ ] Add batched varint encoding
- [ ] Add SIMD delta encoding (with fallback)
- [ ] Profile and optimize hot paths

**Phase 5: Memory Optimization**
- [ ] Implement memory pool for tile generation
- [x] Add thread-local tile contexts (in tile server)
- [ ] Implement compact feature storage
- [ ] Reduce allocations per tile to near-zero

**Phase 6: Parallelization**
- [ ] Implement thread pool
- [ ] Add batch tile generation API
- [ ] Ensure thread safety in all components
- [ ] Benchmark scaling with core count

**Phase 7: Pre-Generation**
- [ ] Implement pre-generation pipeline
- [ ] Write tiles to MBTiles format
- [ ] Add progress reporting
- [ ] Integrate with MBTiles backend

**Phase 8: Boundary Rendering Optimization**
- [ ] Pre-stitch boundary segments at index build time (currently done per-render)
- [ ] Pre-clip boundaries to tile grid during indexing
- [ ] Cache assembled boundary linestrings in binary index
- [ ] Avoid per-boundary malloc in render loop (use arena allocator)
- [ ] Use fixed-point tile coordinates instead of float transforms per-point
- [ ] Add boundary-specific LOD with pre-simplified geometries per zoom
- [ ] Consider separate boundary R-tree with coarser granularity
- [ ] Benchmark: target <5ms overhead for boundaries at z14

### Files to Create/Modify

| File | Action |
|------|--------|
| `include/ct_cache.h` | **Create** - Tile cache API |
| `src/ct_tile_cache.c` | **Create** - In-memory LRU cache |
| `src/ct_disk_cache.c` | **Create** - SQLite-based disk cache |
| `include/ct_tile_index.h` | **Create** - Tile-based spatial index |
| `src/ct_tile_index.c` | **Create** - Index building and lookup |
| `src/ct_clip_fast.c` | **Create** - Optimized clipping |
| `src/ct_simd.c` | **Create** - SIMD utilities |
| `src/ct_mvt_fast.c` | **Create** - Optimized MVT encoder |
| `include/ct_pool.h` | **Create** - Memory pool API |
| `src/ct_pool.c` | **Create** - Memory pool implementation |
| `include/ct_threadpool.h` | **Create** - Thread pool API |
| `src/ct_threadpool.c` | **Create** - Thread pool implementation |
| `src/ct_pregenerate.c` | **Create** - Pre-generation pipeline |
| `benchmarks/bench_native.c` | **Create** - Performance benchmarks |

### Known Performance Issues (Feb 2026)

#### PNG Tile Rendering at z14+

**Problem**: z14 PNG tiles take ~280ms vs target ~75ms (3.7x slower)

| Zoom | Buildings | Render Time | Status |
|------|-----------|-------------|--------|
| z10 | 0 | 72ms | ✅ On target |
| z13 | 3631 | 154ms | ⚠️ 2x target |
| z14 | 1501 | 280ms | ❌ 3.7x target |

**Investigation (Feb 2026)**: Zoom-adaptive building outlines implemented
(z13: no outline, z14: 1px, z15+: 1.5px) but only ~1% improvement.

**Actual root cause**: The main bottleneck is **NOT** building outlines. It's likely:

1. **Road/railway casing** - Each road at z14+ draws 2 passes (outline + fill)
   - 2556 roads × 2 passes = 5112 polyline renders
   - Each uses `ct_render_polyline_cased()` with Xiaolin Wu AA

2. **AA line drawing overhead** - Xiaolin Wu is expensive per-pixel
   - Every line segment requires floating-point blend calculations
   - Thick lines (width > 1) draw multiple parallel AA lines

3. **Feature vertex count** - z14 features may have more vertices
   - Higher zoom = more detailed geometry = more AA line segments

**Potential fixes** (updated priority):

1. **Bresenham for thin road casing** (Medium effort, high impact)
   - Use non-AA lines for casing width < 1px
   - Keep AA only for the road fill line

2. **Skip road casing at z13** (Easy, medium impact)
   - Roads at z13 are thin enough that casing adds little value
   - Currently casing=0.3 at z12-13, could be 0

3. **Batch polyline rendering by color** (Medium effort, medium impact)
   - Group roads by type, render all same-color roads together
   - Better CPU cache locality

4. **SIMD-accelerated AA line drawing** (Hard, high impact)
   - Vectorize the Xiaolin Wu inner loop
   - 4-8x speedup potential on blend operations

5. **Pre-simplified geometries per zoom** (Hard, very high impact)
   - Store simplified coordinates in index file
   - Trade disk space for CPU time

---

## 6. Client-Side MVT Rendering (WebGL)

### Motivation

Move tile rendering from server to client for:
- **Horizontal scaling** - Server becomes stateless file serving
- **CDN distribution** - Pre-generated MVT tiles cache globally
- **Reduced server costs** - No CPU-intensive rendering
- **Zero egress costs** - CloudFlare R2 or similar
- **Dynamic styling** - Theme changes without re-rendering server-side
- **Interactivity** - Hover states, click handling on features

### Architecture

```
One-time generation (batch job):
  hungary.osm.pbf → carta-mvt-batch → tiles/{z}/{x}/{y}.mvt → CloudFlare R2

Runtime (per request):
  Browser → CloudFlare CDN → R2 bucket → MVT bytes
         ← WebGL renderer ← parsed geometry ←
```

### Cost Model (CloudFlare R2)

| Component | Cost |
|-----------|------|
| R2 storage | ~$0.015/GB/month |
| R2 egress | **$0** |
| R2 operations | $0.36/million Class B reads |

For Hungary (~93k km²):
- z0-z14: ~500K tiles × ~10KB avg = **~5 GB storage = ~$0.08/month**
- 1M tile requests = **$0.36**

The entire planet's tiles could be served for under $50/month in storage.

### What Already Exists (ClayShards)

| Component | Status | Location |
|-----------|--------|----------|
| Tile loading/caching | ✅ Works for PNG | `clay-shards-webgl/map-tiles.js` |
| WebGL shaders | ✅ Rect, text, texture | `clay-shards-webgl/shaders.js` |
| MSDF font rendering | ✅ Crisp at any size | `clay-shards-webgl/font.js` |
| Map pan/zoom | ✅ Full support | `clay-shards/src/cs_map.c` |
| MVT encoding (C) | ✅ Encoder only | `carta/src/ct_mvt.c` |

### Components to Build

#### Phase 1: MVT Parser (JavaScript) - ~400 lines

Parse Mapbox Vector Tile protobuf format:

```javascript
// mvt-parser.js

export class MVTParser {
    /**
     * Parse MVT tile from ArrayBuffer
     * @returns {Object} Parsed layers with features
     */
    parse(buffer) {
        const pbf = new Pbf(buffer);
        const tile = { layers: {} };

        while (pbf.pos < pbf.len) {
            const tag = pbf.readTag();
            if (tag.field === 3) {  // Layer
                const layer = this.readLayer(pbf);
                tile.layers[layer.name] = layer;
            } else {
                pbf.skip(tag.type);
            }
        }
        return tile;
    }

    readLayer(pbf) { /* ... */ }
    readFeature(pbf, keys, values) { /* ... */ }

    /**
     * Decode geometry commands to coordinate arrays
     * Commands: MoveTo(1), LineTo(2), ClosePath(7)
     */
    decodeGeometry(geometry, type) {
        const result = [];
        let x = 0, y = 0;
        let ring = [];

        for (let i = 0; i < geometry.length; ) {
            const cmdInt = geometry[i++];
            const cmd = cmdInt & 0x7;
            const count = cmdInt >> 3;

            if (cmd === 1) {  // MoveTo
                if (ring.length) result.push(ring);
                ring = [];
                for (let j = 0; j < count; j++) {
                    x += this.zigzag(geometry[i++]);
                    y += this.zigzag(geometry[i++]);
                    ring.push([x, y]);
                }
            } else if (cmd === 2) {  // LineTo
                for (let j = 0; j < count; j++) {
                    x += this.zigzag(geometry[i++]);
                    y += this.zigzag(geometry[i++]);
                    ring.push([x, y]);
                }
            } else if (cmd === 7) {  // ClosePath
                if (ring.length) ring.push(ring[0]);
            }
        }
        if (ring.length) result.push(ring);
        return result;
    }

    zigzag(n) {
        return (n >> 1) ^ -(n & 1);
    }
}
```

#### Phase 2: Geometry Rendering - ~1500 lines

**Polygon Fill (~400 lines):**
```javascript
// Use earcut for triangulation
import earcut from 'earcut';

export class PolygonRenderer {
    constructor(gl) {
        this.gl = gl;
        this.program = this.createProgram(POLYGON_VS, POLYGON_FS);
        this.buffer = gl.createBuffer();
    }

    render(polygons, color, projMatrix) {
        const vertices = [];

        for (const polygon of polygons) {
            // Flatten coordinates for earcut
            const coords = polygon.flat();
            const indices = earcut(coords);

            // Build triangle vertices
            for (const idx of indices) {
                vertices.push(coords[idx * 2], coords[idx * 2 + 1]);
            }
        }

        // Upload and draw
        this.gl.bufferData(this.gl.ARRAY_BUFFER,
            new Float32Array(vertices), this.gl.DYNAMIC_DRAW);
        this.gl.drawArrays(this.gl.TRIANGLES, 0, vertices.length / 2);
    }
}
```

**Line Rendering (~800 lines):**

This is the hardest part. Thick lines with proper joins require generating quads:

```javascript
export class LineRenderer {
    constructor(gl) {
        this.gl = gl;
        this.program = this.createProgram(LINE_VS, LINE_FS);
    }

    /**
     * Generate thick line geometry with miter joins
     */
    buildLineGeometry(points, width) {
        const vertices = [];
        const halfWidth = width / 2;

        for (let i = 0; i < points.length - 1; i++) {
            const p0 = points[i];
            const p1 = points[i + 1];

            // Direction and normal
            const dx = p1[0] - p0[0];
            const dy = p1[1] - p0[1];
            const len = Math.sqrt(dx * dx + dy * dy);
            const nx = -dy / len * halfWidth;
            const ny = dx / len * halfWidth;

            // Quad vertices (2 triangles)
            vertices.push(
                p0[0] + nx, p0[1] + ny,
                p0[0] - nx, p0[1] - ny,
                p1[0] + nx, p1[1] + ny,

                p0[0] - nx, p0[1] - ny,
                p1[0] - nx, p1[1] - ny,
                p1[0] + nx, p1[1] + ny
            );

            // TODO: Miter/bevel joins at corners
            // TODO: Round caps at endpoints
        }

        return new Float32Array(vertices);
    }

    render(lines, color, width, projMatrix) {
        for (const line of lines) {
            const vertices = this.buildLineGeometry(line, width);
            // Upload and draw...
        }
    }
}
```

**Point Labels (~200 lines):**

Already have MSDF font support, just need placement:

```javascript
export class LabelRenderer {
    constructor(gl, font) {
        this.gl = gl;
        this.font = font;
    }

    render(labels, projMatrix) {
        for (const label of labels) {
            const width = this.font.measureText(label.text, label.fontSize);
            const x = label.x - width / 2;  // Center horizontally
            const y = label.y;

            this.font.renderText(label.text, x, y, label.fontSize,
                                 label.color, projMatrix);
        }
    }
}
```

#### Phase 3: Tile Manager - ~300 lines

```javascript
export class MVTTileCache {
    constructor(gl, maxTiles = 200) {
        this.gl = gl;
        this.cache = new Map();
        this.maxTiles = maxTiles;
        this.parser = new MVTParser();
    }

    async getTile(z, x, y, baseUrl) {
        const key = `${z}/${x}/${y}`;

        if (this.cache.has(key)) {
            return this.cache.get(key);
        }

        const url = `${baseUrl}/${z}/${x}/${y}.mvt`;
        const response = await fetch(url);
        const buffer = await response.arrayBuffer();

        const tile = {
            data: this.parser.parse(buffer),
            geometry: this.buildGeometry(this.parser.parse(buffer)),
            loaded: true
        };

        this.cache.set(key, tile);
        this.evictOldTiles();

        return tile;
    }

    /**
     * Pre-build WebGL buffers for tile geometry
     */
    buildGeometry(tileData) {
        const geometry = {};

        for (const [name, layer] of Object.entries(tileData.layers)) {
            geometry[name] = {
                polygons: [],
                lines: [],
                points: []
            };

            for (const feature of layer.features) {
                const coords = this.parser.decodeGeometry(
                    feature.geometry, feature.type);

                if (feature.type === 3) {  // Polygon
                    geometry[name].polygons.push({
                        coords,
                        properties: feature.properties
                    });
                } else if (feature.type === 2) {  // Line
                    geometry[name].lines.push({
                        coords,
                        properties: feature.properties
                    });
                } else if (feature.type === 1) {  // Point
                    geometry[name].points.push({
                        coords: coords[0][0],
                        properties: feature.properties
                    });
                }
            }
        }

        return geometry;
    }
}
```

#### Phase 4: Styling System - ~400 lines

```javascript
export const DEFAULT_STYLE = {
    layers: {
        water: {
            type: 'fill',
            paint: {
                'fill-color': '#aad3df'
            }
        },
        landuse: {
            type: 'fill',
            paint: {
                'fill-color': ['match', ['get', 'class'],
                    'park', '#c8facc',
                    'forest', '#add19e',
                    '#f2efe9'
                ]
            }
        },
        roads: {
            type: 'line',
            paint: {
                'line-color': ['match', ['get', 'class'],
                    'motorway', '#e990a0',
                    'trunk', '#f9b29c',
                    'primary', '#fcd6a4',
                    '#ffffff'
                ],
                'line-width': ['match', ['get', 'class'],
                    'motorway', 4,
                    'trunk', 3,
                    'primary', 2,
                    1
                ]
            }
        },
        buildings: {
            type: 'fill',
            minzoom: 14,
            paint: {
                'fill-color': '#d9d0c9',
                'fill-outline-color': '#b9a9a0'
            }
        },
        labels: {
            type: 'symbol',
            layout: {
                'text-field': ['get', 'name'],
                'text-size': 12
            },
            paint: {
                'text-color': '#333333',
                'text-halo-color': '#ffffff',
                'text-halo-width': 1.5
            }
        }
    }
};
```

### Batch MVT Generation Tool

```c
// carta-mvt-batch.c

typedef struct {
    const CTPBFContext *pbf;
    const CTLODConfig *lod;
    int min_zoom, max_zoom;
    CTBBox bounds;
    const char *output_dir;
    int num_threads;
} CTMVTBatchConfig;

/**
 * Generate all MVT tiles for a region
 * Output: {output_dir}/{z}/{x}/{y}.mvt
 */
int ct_mvt_batch_generate(const CTMVTBatchConfig *config) {
    // Calculate total tiles
    int total_tiles = 0;
    for (int z = config->min_zoom; z <= config->max_zoom; z++) {
        int tiles_at_zoom = count_tiles_in_bbox(config->bounds, z);
        total_tiles += tiles_at_zoom;
    }

    printf("Generating %d tiles (z%d-z%d)\n",
           total_tiles, config->min_zoom, config->max_zoom);

    // Generate tiles in parallel
    #pragma omp parallel for schedule(dynamic) num_threads(config->num_threads)
    for (int z = config->min_zoom; z <= config->max_zoom; z++) {
        for_each_tile_in_bbox(config->bounds, z, [&](int x, int y) {
            uint8_t buffer[256 * 1024];  // 256KB max
            size_t size = ct_generate_mvt(config->pbf,
                                          (CTTileCoord){z, x, y},
                                          NULL, config->lod,
                                          buffer, sizeof(buffer));

            if (size > 0) {
                char path[256];
                snprintf(path, sizeof(path), "%s/%d/%d/%d.mvt",
                         config->output_dir, z, x, y);
                mkdir_p(dirname(path));
                write_file(path, buffer, size);
            }
        });
    }

    return 0;
}
```

### Upload to CloudFlare R2

```bash
# Generate tiles
./carta-mvt-batch data/hungary.osm.pbf \
    --output tiles/ \
    --min-zoom 0 --max-zoom 14 \
    --threads 8

# Upload to R2 (using rclone)
rclone sync tiles/ r2:otto-tiles/hungary/ \
    --transfers 32 \
    --checkers 16

# Or using AWS CLI (R2 is S3-compatible)
aws s3 sync tiles/ s3://otto-tiles/hungary/ \
    --endpoint-url https://<account>.r2.cloudflarestorage.com
```

### Effort Estimate

| Component | Lines | Time | Difficulty |
|-----------|-------|------|------------|
| MVT Parser | ~400 | 1 day | Easy |
| Polygon Renderer | ~400 | 1 day | Medium |
| Line Renderer (basic) | ~300 | 1 day | Medium |
| Line Renderer (joins/caps) | ~500 | 2 days | Hard |
| Point Labels | ~200 | 0.5 days | Easy |
| Tile Manager | ~300 | 1 day | Easy |
| Styling System | ~400 | 1 day | Medium |
| Batch Generator | ~300 | 1 day | Easy |
| **Total** | **~2800** | **~9 days** | |

### Alternatives Considered

| Approach | Pros | Cons |
|----------|------|------|
| **Build custom WebGL** | Full control, lightweight | Line rendering is hard |
| **Use MapLibre GL JS** | Complete solution | Large dependency (500KB+) |
| **Use deck.gl MVTLayer** | Good for overlays | React-oriented, complex |
| **WASM carta renderer** | Reuse C code | Canvas 2D only, no WebGL |

### TODOs

**Phase 1: MVT Parser**
- [ ] Create `clayshards/clay-shards-webgl/mvt-parser.js`
- [ ] Implement protobuf decoding (or vendor `pbf` library)
- [ ] Implement geometry command decoding
- [ ] Add layer/feature/property parsing
- [ ] Test with tiles from carta server

**Phase 2: Basic Rendering**
- [ ] Create polygon fill renderer with earcut triangulation
- [ ] Create basic line renderer (1px and simple thick lines)
- [ ] Create point renderer
- [ ] Integrate with existing map-tiles.js

**Phase 3: Advanced Line Rendering**
- [ ] Implement miter joins
- [ ] Implement bevel joins
- [ ] Implement round joins
- [ ] Implement line caps (butt, round, square)
- [ ] Add dashed line support

**Phase 4: Labels**
- [ ] Integrate with existing MSDF font renderer
- [ ] Add label collision detection
- [ ] Add text-along-path for road labels (stretch goal)

**Phase 5: Styling**
- [ ] Create style specification
- [ ] Implement property-based styling
- [ ] Add zoom-based visibility
- [ ] Support multiple style presets

**Phase 6: Batch Generation**
- [ ] Create `carta-mvt-batch` tool
- [ ] Add parallel tile generation
- [ ] Add progress reporting
- [ ] Document R2 upload workflow

### Files to Create

| File | Purpose |
|------|---------|
| `clayshards/clay-shards-webgl/mvt-parser.js` | MVT protobuf parsing |
| `clayshards/clay-shards-webgl/mvt-renderer.js` | Main MVT rendering class |
| `clayshards/clay-shards-webgl/polygon-renderer.js` | Polygon fill with triangulation |
| `clayshards/clay-shards-webgl/line-renderer.js` | Thick lines with joins |
| `clayshards/clay-shards-webgl/mvt-style.js` | Styling system |
| `clayshards/clay-shards-webgl/mvt-shaders.js` | WebGL shaders for MVT |
| `carta/tools/carta-mvt-batch.c` | Batch tile generator |
| `scripts/upload-tiles-r2.sh` | R2 upload script |

---

## Implementation Priority

Recommended order of implementation:

1. **Tile Caching (Phase 1 of Native Perf)** - Highest priority
   - Immediate 25x speedup for repeated requests
   - Low implementation complexity
   - Foundation for all other optimizations
   - Two-level cache (memory + disk) recommended

2. **External Tile Server Backends** - High priority
   - Enables production-grade performance immediately
   - Low risk (wraps proven servers)
   - MBTiles especially valuable for offline/embedded use
   - Martin/OpenMapTiles excellent for live PostGIS data

3. **Level of Detail (LOD)** - High priority
   - Immediate visual improvement at low zoom
   - Performance benefits
   - Foundation for other features

4. **Client-Side MVT Rendering** - High priority (if scaling is a concern)
   - Offloads rendering from server to client
   - Zero egress with CloudFlare R2
   - Pre-generate tiles once, serve forever
   - ~9 days effort for basic implementation

5. **Spatial Index Optimization (Phase 2 of Native Perf)** - High priority
   - 2.5x speedup for uncached tiles
   - Reduces load on R-tree queries
   - Tile-based index is straightforward

6. **Geometry & Encoding Optimization (Phases 3-5)** - Medium priority
   - Additional 2-4x speedup combined
   - SIMD optional (provide scalar fallback)
   - Memory pooling is low-hanging fruit

7. **Configurable Styling** - Medium priority
   - Quick wins with presets
   - Visibility toggles are simple
   - Dashed lines add polish

8. **Parallelization (Phase 6)** - Medium priority
   - Throughput scaling, not latency reduction
   - Important for batch pre-generation
   - Thread pool is reusable infrastructure

9. **Pre-Generation Pipeline (Phase 7)** - Medium priority
   - Enables true parity with Martin (~2ms)
   - Outputs MBTiles for production serving
   - Useful for offline/WASM deployment

10. **Font/Label Rendering** - Lower priority (more complex)
    - Start with bitmap fonts
    - Label placement is algorithmically complex
    - Can be deferred until performance work is stable

---

## Backlog Items

### Code Refactoring

- [ ] **Move StringPool to shared/** - The `StringPool` implementation in `ct_serialize.c` is generic and would be useful for other modules (locus string deduplication, velo place names). Move to `shared/include/sh_stringpool.h`.

### Label Improvements

Missing label types compared to OSM Carto (identified via /carta-render-debug on Budapest tiles):

| Label Type | OSM Example | Status | Difficulty |
|------------|-------------|--------|------------|
| City/town names | "Budapest" | ✅ Works | - |
| District names | "Lipótváros" | ✅ Works | - |
| Bridge labels | "Széchenyi lánchíd" | ❌ Missing | Medium |
| River labels | "Duna" | ❌ Missing | Medium |
| Street names | "Hegyalja út" | ❌ Missing | Hard |
| Road shields | "5101", "M1" | ❌ Missing | Hard |
| POI icons | Landmarks, churches | ❌ Missing | Hard |

**Priority:** Bridge and river labels are relatively easy (extract name from way/relation). Street names and road shields require line label placement along curved paths (complex).

### Boundary Rendering Optimization

Current boundary rendering is expensive due to:
1. Relation processing to assemble segments
2. Segment stitching into continuous lines
3. Dashed line rendering

Options:
- [ ] Pre-compute boundary geometry in binary index
- [ ] Cache assembled boundaries per zoom level
- [ ] Simplify boundary geometry aggressively at low zoom

---

## Testing Strategy

For each feature:
1. **Unit tests**: Individual functions (LOD rules, style parsing, font rendering)
2. **Visual regression tests**: Generate reference tiles, compare after changes
3. **Performance benchmarks**: Measure tile generation time before/after

## Resources

- [Mapbox Vector Tile Spec](https://github.com/mapbox/vector-tile-spec)
- [OSM Carto Style](https://github.com/gravitystorm/openstreetmap-carto)
- [stb_truetype](https://github.com/nothings/stb)
- [Douglas-Peucker Algorithm](https://en.wikipedia.org/wiki/Ramer%E2%80%93Douglas%E2%80%93Peucker_algorithm)
- [Map Label Placement](https://www.cs.ubc.ca/~tmm/courses/cpsc533c-04-spr/readings/text-labeling.html)
