# Infrastructure Improvements

API server and observability enhancements.

## 11. API Server Infrastructure Improvements

### Overview

Shared infrastructure improvements for all OTTO API servers (Carta, Velo, Locus, FuelWise) to improve reliability under load.

### Current State

| Server | HTTP Threads | Compute Workers | Slow Client Risk |
|--------|-------------|-----------------|------------------|
| Carta | Multiple (auto-detect CPU) | Dedicated render pool | Medium |
| Velo | Single event loop | Dedicated route pool | High |
| Locus | Single event loop | None (inline, <20µs) | Low |
| FuelWise | Single event loop | Dedicated solve pool | High |

### Problems Identified

1. **Slow client vulnerability**: `mg_send()` blocks event loop on slow TCP ACKs. A client reading at 1KB/s can lock an HTTP thread for ~2000s on a 2MB tile.

2. **Single event loop**: Velo/FuelWise use single HTTP event loops. All requests serialize through one thread.

3. **Work cancellation race**: HTTP handler times out → returns 504 → worker still processes item → result discarded (CPU wasted).

4. **No chunked streaming**: All responses fully buffered in memory before send.

### Planned Components

#### 16.1 sh_httpserver.h - Multi-threaded HTTP Server

> **Superseded.** This was an early planned design that was never built. The
> actual HTTP layer is the Keel-based helper set in `sh_httpserver.{c,h}` +
> `sh_httpasync.{c,h}` (public helpers `sh_http_*`) — see
> "HTTP Server: Keel v3 — Complete" below. The `ShHttpServer`/`sh_httpserver_*`
> API sketched here never existed.

Shared abstraction wrapping the Keel HTTP server with:
- Multiple event loops (one per CPU core, uses SO_REUSEPORT)
- Socket write timeout (prevents slow client DoS)
- Request context lifecycle management
- Integration with sh_workqueue for compute offload

```c
typedef struct {
    int num_threads;           /* 0 = auto-detect CPU count */
    int port;
    const char *host;
    int socket_timeout_ms;     /* Write timeout per connection */
    ShWorkQueue *work_queue;   /* Optional: for compute offload */
    ShRateLimiter *rate_limiter;
} ShHttpServerConfig;

typedef void (*ShHttpHandler)(ShHttpRequest *req, ShHttpResponse *res);

ShHttpServer *sh_httpserver_create(const ShHttpServerConfig *cfg);
void sh_httpserver_route(ShHttpServer *srv, const char *pattern, ShHttpHandler handler);
void sh_httpserver_run(ShHttpServer *srv);  /* Blocking */
void sh_httpserver_stop(ShHttpServer *srv);
void sh_httpserver_free(ShHttpServer *srv);
```

#### 16.2 sh_workqueue.h Extensions - Cancellation Support

Add cancellation flag that workers can check during long computations:

```c
/* Mark a work item as cancelled (e.g., when HTTP handler times out) */
void sh_workqueue_item_cancel(ShWorkItem *item);

/* Check if item was cancelled (workers call periodically) */
int sh_workqueue_item_cancelled(const ShWorkItem *item);

/* Stats extension */
typedef struct {
    /* ... existing fields ... */
    uint64_t total_cancelled;  /* Items cancelled before completion */
} ShWorkQueueStats;
```

#### 16.3 sh_chunked.h - Chunked Transfer Encoding

Helpers for streaming large responses without buffering:

```c
/* Start chunked response */
void sh_chunked_begin(struct mg_connection *c, int status, const char *content_type);

/* Send a chunk (can be called multiple times) */
void sh_chunked_send(struct mg_connection *c, const void *data, size_t len);

/* End chunked response */
void sh_chunked_end(struct mg_connection *c);
```

### Migration Path

1. **Phase 1**: Add socket timeout to HTTP connections (immediate fix)
2. **Phase 2**: Add cancellation support to sh_workqueue
3. **Phase 3**: Migrate Velo/FuelWise to multi-threaded HTTP (use Carta pattern)
4. **Phase 4**: Add chunked streaming for large tiles (optional optimization)

### TODOs

- [ ] Add `sh_workqueue_item_cancel()` and `sh_workqueue_item_cancelled()`
- [ ] Add `total_cancelled` to ShWorkQueueStats
- [ ] Create `sh_httpserver.h` with multi-threaded Keel wrapper
- [ ] Add socket write timeout to all API servers
- [ ] Migrate Velo API to multi-threaded HTTP pattern
- [ ] Migrate FuelWise API to multi-threaded HTTP pattern
- [ ] Create `sh_chunked.h` for chunked transfer encoding
- [ ] Add chunked streaming option for large PNG tiles

---

## 12. Observability Infrastructure (Logging, Tracing, Metrics)

### Overview

Production-grade observability for OTTO API servers with structured logging, distributed tracing, and metrics export to Grafana/Datadog.

### Components Implemented

| Component | Header | Purpose |
|-----------|--------|---------|
| Logging | `sh_log.h` | Structured logging (JSON/text), log levels, thread-safe |
| Tracing | `sh_trace.h` | UUID v4 trace IDs, HTTP header propagation, spans |
| Metrics | `sh_metrics.h` | Counters, gauges, histograms with StatsD + Prometheus |

### Log Levels

| Level | Usage |
|-------|-------|
| TRACE | Detailed debugging (high volume) |
| DEBUG | Development debugging |
| INFO | Normal operations |
| WARN | Recoverable issues |
| ERROR | Failures requiring attention |
| FATAL | Critical failures |

### Configuration

```bash
# Logging
SH_LOG_LEVEL=INFO           # Minimum level (TRACE, DEBUG, INFO, WARN, ERROR, FATAL)
SH_LOG_FORMAT=json          # Output format (json, text)
SH_LOG_COLOR=1              # ANSI colors in text mode (auto-detected for TTY)

# Metrics
SH_METRICS_STATSD_HOST=localhost  # StatsD host (empty to disable)
SH_METRICS_STATSD_PORT=8125       # StatsD port
```

### Usage Example

```c
#include "sh_log.h"
#include "sh_trace.h"
#include "sh_metrics.h"

void handle_request(struct mg_connection *c, struct mg_http_message *hm) {
    /* Start trace context */
    sh_trace_from_headers(my_header_getter, hm);

    /* Log with trace correlation */
    SH_LOG_INFO("Request received",
                "method", "GET",
                "path", "/tiles/10/567/357.png");

    /* Start timer */
    ShMetricsTimer timer = sh_metrics_timer_start();

    /* ... process request ... */

    /* Record metrics */
    sh_metrics_timer_observe(timer, "http_request_duration_ms",
                             "method:GET", "endpoint:/tiles", NULL);
    sh_metrics_counter_inc("http_requests_total", 1,
                           "method:GET", "status:200", NULL);

    /* Clear trace context */
    sh_trace_clear();
}
```

### Log Output Formats

**Text format** (human-readable):
```
INFO  12:34:56.789 [a1b2c3d4] Request received | method=GET path=/tiles
WARN  12:34:57.001 [a1b2c3d4] Slow response | duration_ms=523 (main.c:142)
```

**JSON format** (machine-parseable):
```json
{"timestamp":"2024-01-15T12:34:56.789Z","level":"info","service":"carta","trace_id":"a1b2c3d4-...","message":"Request received","method":"GET","path":"/tiles"}
```

### Metrics for Grafana/Datadog

#### HTTP Metrics

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `http_requests_total` | Counter | method, status | Total requests |
| `http_request_duration_ms` | Histogram | method, endpoint | Request latency |
| `http_request_size_bytes` | Histogram | - | Request body size |
| `http_response_size_bytes` | Histogram | - | Response body size |
| `http_active_connections` | Gauge | - | Current open connections |

#### Work Queue Metrics

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `workqueue_depth` | Gauge | - | Current queue depth |
| `workqueue_capacity` | Gauge | - | Maximum queue size |
| `workqueue_pushed_total` | Counter | - | Items added |
| `workqueue_popped_total` | Counter | - | Items processed |
| `workqueue_dropped_total` | Counter | - | Items rejected (full) |
| `workqueue_expired_total` | Counter | - | Items expired in queue |
| `workqueue_cancelled_total` | Counter | - | Items cancelled |

#### Rate Limiter Metrics

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `ratelimit_allowed_total` | Counter | - | Requests allowed |
| `ratelimit_denied_total` | Counter | - | Requests denied (429) |
| `ratelimit_active_entries` | Gauge | - | Tracked IP addresses |

#### Application-Specific Metrics

**Carta (Tile Server):**
| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `carta_tiles_rendered_total` | Counter | format (png/mvt), zoom | Tiles generated |
| `carta_tile_render_duration_ms` | Histogram | format, zoom | Render time |
| `carta_pbf_nodes_total` | Gauge | - | Loaded OSM nodes |
| `carta_pbf_ways_total` | Gauge | - | Loaded OSM ways |

**Velo (Routing):**
| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `velo_routes_calculated_total` | Counter | profile, mode | Routes calculated |
| `velo_route_duration_ms` | Histogram | profile | Route calculation time |
| `velo_nodes_explored` | Histogram | algorithm | A* nodes explored |

**FuelWise (Refueling):**
| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `fuelwise_optimizations_total` | Counter | status | Optimization runs |
| `fuelwise_optimization_duration_ms` | Histogram | - | Solve time |
| `fuelwise_stations_filtered` | Histogram | - | Stations per request |

### Prometheus Endpoint

Add to each API server:
```c
if (mg_match(hm->uri, mg_str("/metrics"), NULL)) {
    char *prom = sh_metrics_prometheus_output();
    if (prom) {
        mg_http_reply(c, 200, "Content-Type: text/plain\r\n", "%s", prom);
        free(prom);
    }
}
```

### Grafana Dashboard Recommendations

**Overview Dashboard:**
- Request rate (RPS) by service
- Error rate (5xx) by service
- P50/P95/P99 latency by service
- Active connections per service

**Work Queue Dashboard:**
- Queue depth over time
- Queue utilization (depth/capacity)
- Drop rate (dropped/pushed)
- Expiration rate

**Rate Limiter Dashboard:**
- Allow/deny ratio
- Denial rate by service
- Active tracked IPs

### StatsD Integration (Datadog)

Metrics are sent as UDP packets to StatsD:
```
carta.http_requests_total:1|c|#method:GET,status:200
carta.http_request_duration_ms:45.5|h|#method:GET
carta.http_active_connections:12|g
```

Datadog tags format (`#tag:value`) is used for dimensional metrics.

### Distributed Tracing Flow

```
Client Request
    │
    ├─▶ [API Gateway] X-Trace-Id: abc123...
    │       │
    │       ├─▶ [Carta] sh_trace_from_headers() extracts trace
    │       │       │
    │       │       └─▶ [Worker Thread] sh_log_set_trace_id()
    │       │               Log: {"trace_id":"abc123...","message":"Rendering tile"}
    │       │
    │       └─▶ [Response] X-Trace-Id: abc123...
    │
    └─▶ Client receives trace ID for support queries
```

### TODOs

- [x] Implement `sh_log.h` - structured logging
- [x] Implement `sh_trace.h` - trace ID generation
- [x] Implement `sh_metrics.h` - metrics collection
- [x] Add tests for logging, tracing, metrics (162 tests pass)
- [ ] Integrate logging into carta/api
- [ ] Integrate logging into velo/api
- [ ] Integrate logging into fuelwise/api
- [ ] Add `/metrics` endpoint to all API servers
- [ ] Add trace ID propagation to work queue
- [ ] Create Grafana dashboard templates
- [ ] Document alerting thresholds

## HTTP Server: Keel v3 — Complete

The previous GPL-licensed HTTP server has been removed. All six API servers now
run on Keel (`vendor/keel`, MIT, git submodule pinned to v3.0.0-rc.3), through a
thin shared helper layer in `shared/src/sh_httpserver.c` + `sh_httpasync.c`.

### Why this mattered

The previous server was licensed `GPL-2.0-only or commercial`. GPL-2.0-**only**
(not "or later") cannot combine with OTTO's AGPL-3.0 — GPLv2-or-later can upgrade
into AGPLv3, `-only` cannot — and the commercial tier in
`docs/business/STRATEGY.md` could not sublicense it either. Porting the servers
onto Keel made the code independent of it and resolved the conflict, since the
GPL-2.0-only source is no longer in the tree.

That server's header had also carried a clean transport-agnostic API
(`ShHttpServer`, `ShHttpRequest`, `ShHttpResponse`) that no server ever adopted —
every one of them called its API directly. It went with the rest.

### Naming

The migration introduced the Keel helper layer as `sh_keelserver.{c,h}` (sync
reply/CORS/health helpers) and `sh_keelasync.{c,h}` (the async/SSE dispatch
protocol). With the migration complete and Keel now the permanent HTTP layer,
these were renamed to the neutral `sh_httpserver.{c,h}` and `sh_httpasync.{c,h}`,
reclaiming the name the removed server used to hold. The public helpers are
`sh_http_*`; the async config type is `ShHttpAsync`. The file-local Keel
adapters inside `sh_httpasync.c` (`KeelCall`, `KeelStream`) keep their names —
they name the Keel boundary they wrap.

### Kept deliberately

Comments in the ported servers that describe how the previous HTTP server *used
to* behave are kept in past tense. They explain why several decisions look the
way they do — the 400 for malformed `/tiles/` paths, the CORS-preflight ordering,
the `Method not allowed` shape, and why Carta no longer needs N event loops.

### Verification

All six `main.c` files plus `sh_httpserver.c` and `sh_httpasync.c` compile clean
under `-Wall -Wextra`, and every server has a gating CI suite (Surge 11, Ralph 14,
FuelWise 20, Velo 26, Carta 19, Locus 19).
