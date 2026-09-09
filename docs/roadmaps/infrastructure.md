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

## MSVC support: shared and ralph (reference implementation)

`sh_pal` removed the pthread dependency above the PAL, which is what made MSVC
reachable. This pass makes it real for `shared` and `ralph`, and establishes the
pattern the remaining six libraries adopt without redesign.

    make            GCC or Clang, unchanged
    make CC=cl      MSVC, from a Visual Studio Developer shell

### Why a toolchain layer and not CMake

The existing Make build is the build graph; a second one would drift from it.
`mk/toolchain.mk` is the only file that knows a compiler exists. Module Makefiles
include it and compose normalized variables (`CC_WARN`, `CC_OPT`, `CC_FP`,
`CC_OMP`, `OBJ_OUT`, `AR_CMD`, `link_lib`, ...). There is no `ifeq ($(CC),cl)`
anywhere else in the tree.

The GNU branch reproduces what the module Makefiles previously spelled inline,
verified by diffing `make -n` before and after: **387 command lines, byte-identical**
across `shared lib`, `shared test`, `ralph lib` and `ralph test`.

### Flags that were deliberately not translated

| GCC | MSVC | why |
|---|---|---|
| `-march=native` | *(nothing)* | `/arch:AVX2` is a hard floor, not "this machine" — it produces binaries the target may not run |
| `-fopenmp` | *(nothing)* | 29 of Ralph's 33 pragmas are `omp simd` (OpenMP 4.0). MSVC's `/openmp` is 2.0 and has no `simd`; `/openmp:llvm` rejects a loop index declared in the for-init in C. With OpenMP off MSVC ignores the pragmas silently — zero warnings, identical results — and auto-vectorises under `/O2`. The three real `parallel` constructs in `lap.c` degrade to serial, which is a defined OpenMP property already covered by Ralph's "parallel disabled" tests |
| `-D_FORTIFY_SOURCE=2`, `-fPIE` | `/GS /guard:cf` | no `_FORTIFY_SOURCE` equivalent; ASLR is already the linker default |
| `-MMD -MP` | *(nothing)* | `/showIncludes` emits a different format needing a parser. MSVC builds do not track headers; `make clean` after a header change, and CI always builds clean |

### Floating point: `/fp:precise`, on evidence

GCC builds Ralph with `-ffast-math -fno-finite-math-only`: aggressive FP, NaN and
Inf still honoured. `/fp:fast` has no such carve-out — it assumes NaN and Inf do
not occur, which in a simplex is exactly the assumption that fails.

Both modes were built and the full Ralph suite run under each. **Both pass with
zero failures**, so pass/fail alone would have said "either is fine". It is not:

**1. `/fp:fast` breaks the non-finite parameter tests.** Seven
`warning C4756: overflow in constant arithmetic` appear under `/fp:fast` and
none under `/fp:precise`. Every site is an `INFINITY` argument in a test
asserting the solver *rejects* a non-finite parameter — `test_lp_algorithm_api.c`,
`test_lp_policy_glpk_compat.c`, `test_lp_bfcp_policy.c`. That is precisely the
guarantee `-fno-finite-math-only` exists to preserve, being lost.

**2. The simplex takes a different path.** Iteration counts diverge on six solves:

    273 -> 261      118 -> 114
     80 ->  83      134 -> 128
     75 ->  76

**3. A NETLIB objective changes.** lotfi:

    /fp:precise   -25.2647061510   (rel_err 3.53e-09)
    /fp:fast      -25.2647060619   (rel_err 7.94e-13)

`/fp:fast` happens to land closer to the reference here, which is exactly why
this is not a reason to choose it: the value moved, and nothing guarantees the
next problem moves the same direction.

Whole-suite wall clock was 26.4s (precise) vs 24.4s (fast), but that includes
compilation and is not a controlled benchmark — no performance claim is made.

Per the criteria set for this work, meaningful numerical divergence or weakened
robustness means `/fp:precise`, and both are present. `FP_MODE=fast` remains
available (`make CC=cl FP_MODE=fast`) so the comparison can be repeated; the
default is `precise` and the choice lives in `mk/toolchain.mk`.

### Portability fixes, made in the shared layer

Fixed once in `shared`, so the other six libraries inherit them:

- **PAL clocks.** `sh_monotonic_ns()` and `sh_wall_ns()` added alongside the
  millisecond forms. Seven files had their own `gettimeofday`/`clock_gettime`
  helpers; all now route through the PAL with no precision loss.
- **PAL misc.** `sh_stderr_is_tty()` (was `isatty(STDERR_FILENO)`) and
  `sh_sleep_ms()` (was `nanosleep`).
- **`sh_attr.h`.** `SH_UNUSED` / `SH_NOINLINE`, following the shape `lp_log.h`
  already used for its printf attribute.
- **`sh_json.c`** tested for infinity with `val == (1.0 / 0.0)`. MSVC rejects the
  constant division outright; `isinf()` is what it meant.
- **`lap.c`'s `GET_COST_CALLBACK`** was a GNU statement expression. It is now a
  `static inline` function, which is what the macro was emulating; the macro name
  is kept so no call site changed.
- Two dead `#include <unistd.h>` deleted (`sh_args.c`, `sh_dist.c` used nothing
  from it).

`strcasecmp`, `strncasecmp` and `strtok_r` are renamed to their MSVC spellings
by `/D` flags in `mk/toolchain.mk` rather than by an `#include` in each of the
seven files that call them — a build concern kept in the build layer, and the
sources stay POSIX-spelled. Without it MSVC treats them as implicit declarations
returning `int`, which truncates `strtok_r`'s pointer on a 64-bit build.

### The trap worth remembering

`shared/Makefile` defined `LIB = libshared.a`. `LIB` is also MSVC's library
search path, and **make re-exports any variable that also exists in the
environment** — so every recipe ran with the linker's search path replaced by a
filename, and nothing linked. Renamed to `LIB_FILE`. `mk/toolchain.mk` carries a
caution: never name a make variable `LIB`, `INCLUDE`, `LINK` or `CL`.

### Verified

| | GCC (UCRT64) | MSVC 19.44 |
|---|---|---|
| `shared` build + full suite | pass | pass |
| `ralph` build + full suite | pass | pass |
| failures | 0 | 0 |
| `make -n` command lines | byte-identical to before | — |

The two out-of-process suites SKIP under MSVC exactly as they already do on
MinGW: `lp_external_oop_run()` has no Windows implementation, which is a feature
port tracked separately.

### Not in this pass

- Velo, Carta, Locus, Surge, FuelWise, Arbor. Nothing here blocks them; they
  include `mk/toolchain.mk` and consume the same variables.
- **Velo needs a real decision, not a shim:** `vl_graph.c` and `vl_pbf.c` use
  `mmap` for continental-scale graph loading. `CreateFileMapping`/`MapViewOfFile`
  is a PAL design change, not a header swap.
- `ralph-benchmark` (the NETLIB harness) uses `dirent.h` to enumerate problems,
  so `test-netlib` does not yet run under MSVC. It is not part of `make test`.
- The API servers, which need Keel to build under MSVC first. Keel is
  MinGW-targeted (`CC = cc`, no CMake/sln, `_MSC_VER` nowhere in its own source)
  and is a submodule, so that is upstream work.

### Velo on MSVC

Velo needed no port. Its sources were already clean: commit 0a06781e had moved
file mapping into the PAL (`sh_map_file_readonly` / `sh_unmap_file`) for shared,
velo, carta and locus, and `vl_pbf.c` already carried a Windows read-the-file
fallback. All 9 sources and all 3 tests compile with `cl` untouched. The mmap
design decision this was expected to need had already been made elsewhere.

What it did need was the Makefile wiring, plus two things that will recur:

**The floating-point knobs had to split.** `CC_FP` was Ralph-shaped -- fast math
with `-fno-finite-math-only`. Velo wants fast math *without* that carve-out, and
adds `-ftree-vectorize`. One variable could not serve both without changing one
of them, so the toolchain now exposes `CC_FP_MODE`, `CC_FP_FASTMATH`,
`CC_FP_KEEP_NONFINITE` and `CC_VECTORIZE`, and each module says what it wants.
Same for OpenMP: Velo passes `-fopenmp-simd`, Ralph does not, so `CC_OMP_SIMD`
is its own knob.

**The `LIB` trap is not Velo's alone.** velo, carta, locus, surge and arbor all
define `LIB = lib<module>.a`. `LIB` is MSVC's library search path and make
re-exports any variable that also exists in the environment, so each of them
will fail to link with an error naming `bcrypt.lib` until renamed. Velo is
renamed here; the other four are waiting.

Two smaller notes for whoever takes the next module:

- Link lines are not spelled uniformly. Velo used `-L$(SHARED_DIR) -lshared`
  where ralph used a literal `-L../shared -lshared`, so a search-and-replace
  tuned to one will silently miss the other. `$(call link_lib,<dir>,<name>)`
  is the form to land on.
- Mixing toolchains in one tree does not work: an MSVC `libshared.a` linked by
  gcc, or the reverse, produces undefined references to things that are plainly
  in the archive. `make clean` between compilers, always.

Verified on Windows 11, full suites, zero failures either way:

| | GCC (UCRT64) | MSVC 19.44 |
|---|---|---|
| shared | pass | pass |
| ralph | pass | pass |
| velo | 51/51 + 32 | 51/51 + 32 |

GCC command lines for velo are unchanged apart from one collapsed double space,
left by dropping an `OPENMP_INCLUDES` variable that was always empty off macOS.
shared and ralph remain byte-identical.

#### vl_pbf.c: the file 0a06781e missed

That commit set out to remove direct mmap calls from six files and moved velo's
`vl_graph.c` and carta's `ct_pbf.c` onto the PAL, but `vl_pbf.c` kept its old
`#ifndef _WIN32` split: mmap on POSIX, read-the-whole-file on Windows. It
compiled cleanly on every platform, which is why nothing flagged it.

That mattered more than the duplication suggests. This is the OSM extract
loader -- the README's own example is a ~300 MB country and continental extracts
run to several GB. The POSIX path mapped the file; the Windows path malloc'd its
full size and read it in. Same routing engine, one platform paying resident
memory equal to the input.

Now one mapped implementation on both, via `sh_map_file_readonly` plus
`sh_map_advise_sequential`, and the guarded POSIX include block is gone.

Error reporting is unified rather than preserved verbatim, because the two
branches did not agree:

| input | old POSIX | old Windows | now |
|---|---|---|---|
| missing | File not found | File not found | File not found |
| empty | Out of memory | File read error | File read error |

The PAL reports one failure for missing, empty and unmappable alike, so the
common case is distinguished with an `fopen` probe on the failure path only --
the message a user sees is the only thing that differed, and no caller branches
on the code.

Verified against a real 676 KB Monaco extract plus missing, empty and garbage
inputs. GCC and MSVC now produce identical output on all four:

    real     OK nodes=41701 ways=6248 highway=1232
    missing  File not found
    empty    File read error
    garbage  OK nodes=0 ways=0 highway=0

One limit worth stating: MinGW defines `_WIN32`, so the old code already took
the Windows branch there. Nothing on this machine could execute the old POSIX
mmap branch, and the before/after comparison above is against the Windows one.
The Linux matrix is what exercises the new unified path on the side that used to
mmap. No velo test covers `vl_pbf_parse_file` at all, which is how the split
survived this long.
