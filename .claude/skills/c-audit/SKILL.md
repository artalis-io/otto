---
name: c-audit
description: Audit C code for security, safety, memory management, and OTTO best practices. Use when reviewing or hardening C modules.
user-invocable: true
---

# C Code Audit Skill

Perform comprehensive security, safety, and quality audits on OTTO C modules.

**Target module:** $ARGUMENTS

## Usage

```
/c-audit <module>           # Audit a module (e.g., /c-audit carta)
/c-audit <module> --fix     # Audit and apply fixes
/c-audit <path/to/file.c>   # Audit a specific file
```

## Audit Categories

### 0. Security Model Compliance (Warnings)

OTTO follows a [security model](../../../docs/internals/security-model.md) with role-based privilege separation. These checks warn about violations (not failures, for now).

| Check | Pattern | Severity | Rationale |
|-------|---------|----------|-----------|
| `system()` call | `system\s*\(` | Warning | No shell execution allowed |
| `exec*()` call | `exec[lv]p?\s*\(` | Warning | No process spawning in libraries |
| `popen()` call | `popen\s*\(` | Warning | No shell pipes allowed |
| `dlopen()` call | `dlopen\s*\(` | Warning | No runtime plugin loading |
| Inline parsing in handlers | Handler directly calls `json_*`, `parse_*` | Warning | Parsing should be in Role P |
| Broad file access | `fopen` without path validation | Warning | Restrict to known paths |

**Role Reference:**
- **Role B (Transport):** Mongoose handlers - should only frame/limit, not parse
- **Role P (Parser):** Parsing code - runs in isolated process
- **Role C (Compute):** Business logic - receives only clean IR, no raw bytes
- **Role D (Dataset):** Index access - read-only mmap of derived formats

**Audit Output Section:**
```markdown
### Security Model Compliance

| Check | Status | Notes |
|-------|--------|-------|
| No system/exec/popen | ✅ | |
| No dlopen | ✅ | |
| Parsing isolated | ⚠️ | JSON parsed inline in `handle_request()` |
| File access restricted | ✅ | |

See docs/internals/security-model.md for target architecture.
```

### 1. Memory Safety (Critical)

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| Buffer overflow | `strcpy`, `strcat`, `sprintf`, `gets`, unbounded loops | Critical |
| Unbounded string ops | `strlen`, `strcmp` on untrusted input | Critical |
| Integer overflow | `malloc(a * b)` without overflow check | Critical |
| Use-after-free | Pointer used after `free()` | Critical |
| Double-free | `free()` called twice on same pointer | Critical |
| Null dereference | Pointer used without NULL check | High |
| Uninitialized memory | Variables used before assignment | High |
| Missing null terminator | String buffer not explicitly terminated | High |
| Memory leak | `malloc`/`calloc` without corresponding `free` | Medium |
| Stack buffer overflow | Large stack arrays, VLAs | Medium |

**Safe String Function Replacements:**
```c
// UNSAFE -> SAFE (always use bounded versions)

// Copying
strcpy(dst, src)           -> strncpy(dst, src, sizeof(dst)-1); dst[sizeof(dst)-1] = '\0';
strcat(dst, src)           -> strncat(dst, src, sizeof(dst)-strlen(dst)-1);

// Formatting
sprintf(buf, fmt, ...)     -> snprintf(buf, sizeof(buf), fmt, ...);
gets(buf)                  -> fgets(buf, sizeof(buf), stdin);

// Length checking
strlen(untrusted)          -> strnlen(untrusted, MAX_EXPECTED_LEN);

// Comparison
strcmp(a, b)               -> strncmp(a, b, MAX_LEN);

// Memory allocation
malloc(count * size)       -> calloc(count, size);  // or check overflow first
```

**Critical Rule: ALWAYS null-terminate strings explicitly!**
```c
// BAD: strncpy doesn't guarantee null termination
char buf[64];
strncpy(buf, input, sizeof(buf));  // If input >= 64, buf is NOT terminated!

// GOOD: Explicit null termination
char buf[64];
strncpy(buf, input, sizeof(buf) - 1);
buf[sizeof(buf) - 1] = '\0';  // ALWAYS do this!

// GOOD: Use snprintf (always null-terminates, returns needed size)
char buf[64];
int needed = snprintf(buf, sizeof(buf), "%s", input);
if (needed >= (int)sizeof(buf)) {
    // Truncation occurred - handle if needed
}

// GOOD: Helper macro for safe string copy
#define SAFE_STRCPY(dst, src) do { \
    strncpy((dst), (src), sizeof(dst) - 1); \
    (dst)[sizeof(dst) - 1] = '\0'; \
} while(0)
```

**Why bounded string functions matter:**
```c
// DANGEROUS: strlen on untrusted input can scan forever
size_t len = strlen(user_input);  // What if no '\0' in buffer?

// SAFE: Bounded length check
size_t len = strnlen(user_input, MAX_INPUT_LEN);
if (len == MAX_INPUT_LEN) {
    // Input may be unterminated or too long
    return ERROR_INPUT_TOO_LONG;
}

// DANGEROUS: strcmp on potentially unterminated strings
if (strcmp(user_input, "expected") == 0) { ... }

// SAFE: Bounded comparison
if (strncmp(user_input, "expected", sizeof("expected")) == 0) { ... }
```

### 2. Input Validation

| Issue | What to Check |
|-------|---------------|
| Array bounds | All array indices validated before access |
| Pointer validity | NULL checks before dereference |
| Size parameters | Non-negative, within reasonable bounds |
| String length | Length checked before copy/concat |
| Numeric ranges | Values within expected domain |

**Pattern:**
```c
// GOOD: Validate before use
if (index < 0 || (size_t)index >= count) {
    return ERROR_OUT_OF_BOUNDS;
}
data[index] = value;

// GOOD: NULL check
if (!ptr) {
    return ERROR_NULL_POINTER;
}
```

### 3. Resource Management

| Issue | What to Check |
|-------|---------------|
| File handles | `fopen` paired with `fclose` |
| Memory | `malloc`/`calloc` paired with `free` |
| Contexts | `*_create()` paired with `*_destroy()` |
| Error paths | Resources freed on all exit paths |

**OTTO Pattern - Ownership Convention:**
```c
// Functions that CREATE resources (caller owns, caller frees)
Graph *graph_create(void);           // Returns owned pointer
Solution *solver_solve(Problem *p);  // Returns owned pointer

// Functions that BORROW resources (caller retains ownership)
void process_data(const Data *d);    // Borrows, does not free
int validate_input(const char *s);   // Borrows, does not free
```

### 4. OTTO-Specific Patterns

#### Arena Allocation (Preferred)
Check if module uses arena allocation where appropriate:
```c
// GOOD: Arena allocation
Arena arena = arena_create(buffer, size);
Node *nodes = arena_alloc(&arena, n * sizeof(Node));
// ... use nodes ...
arena_reset(&arena);  // Free everything at once
```

#### Naming Conventions
| Component | Prefix | Example |
|-----------|--------|---------|
| Ralph | `ralph_`, `RALPH_` | `ralph_create()`, `RALPH_INFINITY` |
| Velo | `vl_`, `VL_` | `vl_route()`, `VL_ROUTE_ASTAR` |
| Carta | `ct_`, `CT_` | `ct_tile_init()`, `CT_LAYER_WATER` |
| Locus | `lc_`, `LC_` | `lc_search()`, `LC_MAX_RESULTS` |
| FuelWise | `fw_`, `FW_` | `fw_optimize()`, `FW_STATUS_OK` |
| Shared | `sh_`, `SH_` | `sh_haversine()`, `SH_EARTH_RADIUS` |

#### Error Handling
```c
// GOOD: Consistent error returns
typedef enum {
    XX_OK = 0,
    XX_ERR_NULL,
    XX_ERR_BOUNDS,
    XX_ERR_MEMORY,
    XX_ERR_IO
} XXError;

// GOOD: Check return values
XXError err = xx_operation();
if (err != XX_OK) {
    // Handle error
    return err;
}
```

#### Header Guards
```c
// GOOD: Project-prefixed guards
#ifndef CARTA_CT_TILE_H
#define CARTA_CT_TILE_H
// ...
#endif /* CARTA_CT_TILE_H */
```

### 5. Defensive Macros

Check for and suggest these common macros:
```c
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define SAFE_FREE(p) do { free(p); (p) = NULL; } while(0)

#define CHECK_NULL(p) do { \
    if ((p) == NULL) return XX_ERR_NULL; \
} while(0)

#define CHECK_BOUNDS(i, n) do { \
    if ((i) < 0 || (size_t)(i) >= (n)) return XX_ERR_BOUNDS; \
} while(0)

#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
```

### 6. Test Coverage

Check test file for:
- [ ] Basic functionality tests
- [ ] Edge cases (empty input, max values, NULL)
- [ ] Error path tests (what happens when things fail)
- [ ] Memory leak tests (if using sanitizers)
- [ ] Bounds checking tests

### 7. Memory Pools (SHPool and SHArena)

OTTO provides two memory management patterns in `shared/` for high-performance allocation:

#### SHArena - Bump Allocator (`shared/include/sh_arena.h`)
Use for many allocations with the same lifetime:
```c
#include "sh_arena.h"

// GOOD: Arena for batch allocations
SHArena *arena = sh_arena_create(1024 * 1024);  // 1MB
double *arr1 = sh_arena_alloc(arena, 100 * sizeof(double));
int *arr2 = sh_arena_calloc(arena, 50, sizeof(int));  // zero-init
// ... use arrays ...
sh_arena_reset(arena);  // Reuse (no individual frees needed)
sh_arena_free(arena);   // Final cleanup

// BAD: Many individual mallocs for same-lifetime data
double *arr1 = malloc(100 * sizeof(double));
int *arr2 = calloc(50, sizeof(int));
// ... must free each individually, easy to leak ...
```

**When to use SHArena:**
- Parsing: allocate nodes, then free all at once
- Per-request allocations in servers
- Graph algorithms with temporary storage
- Any "allocate many, free all at once" pattern

#### SHPool - Typed Contiguous Pool (`shared/include/sh_pool.h`)
Use for storing many arrays of the same type with offset-based access:
```c
#include "sh_pool.h"

// GOOD: Pool for coordinate storage
SHPool coord_pool;
sh_pool_init(&coord_pool, sizeof(Coord), 100000);  // 100K coords

// Allocate space for a way's coordinates
size_t offset = sh_pool_alloc(&coord_pool, way->num_coords);
Coord *coords = sh_pool_ptr(&coord_pool, offset);
// ... fill coords ...

// Store offset in way struct (not pointer!)
way->coord_offset = offset;

// Later: access via offset
Coord *way_coords = SH_POOL_PTR(&coord_pool, Coord, way->coord_offset);

sh_pool_reset(&coord_pool);  // Reuse
sh_pool_free(&coord_pool);   // Cleanup
```

**When to use SHPool:**
- OSM ways (variable-length coordinate arrays)
- Sparse matrix storage
- Any "many small arrays of same type" pattern

**Audit Checks:**
- [ ] Uses `SHArena` instead of many individual mallocs for same-lifetime data
- [ ] Uses `SHPool` for variable-length arrays of same type (e.g., coordinates)
- [ ] Calls `sh_arena_free()` / `sh_pool_free()` on all code paths
- [ ] Checks return value of `sh_arena_alloc()` / `sh_pool_alloc()` for NULL/INVALID

### 8. Dead Code Detection

Unused code increases maintenance burden and can hide bugs.

#### Unused Variables
```c
// BAD: Unused variable
int result = compute_value();  // Warning: 'result' unused
do_something_else();

// GOOD: Use or remove
int result = compute_value();
if (result < 0) return ERROR;
```

**Compiler flags to detect:**
```makefile
CFLAGS += -Wunused-variable -Wunused-parameter -Wunused-function
CFLAGS += -Wunused-but-set-variable  # GCC only
```

#### Unused Functions
```c
// BAD: Static function never called
static int helper_never_used(int x) { return x * 2; }

// GOOD: Remove unused static functions, or mark intentionally unused
#ifdef DEBUG
static void debug_print(const char *msg) { printf("%s\n", msg); }
#endif
```

**Audit Procedure:**
1. Compile with `-Wunused` flags
2. Search for `static` functions not referenced elsewhere in file
3. Search for variables assigned but never read
4. Check for commented-out code (remove or document why kept)

**Auto-fixable:**
- Remove clearly unused static functions
- Remove unused local variables
- Add `(void)param;` for intentionally unused parameters

**NOT Auto-fixable:**
- Public API functions (may be used by external code)
- Functions used via function pointers
- Conditionally-compiled code

#### Dead Code Patterns to Flag
| Pattern | Issue | Fix |
|---------|-------|-----|
| `if (0) { ... }` | Dead branch | Remove |
| `return; code_after;` | Unreachable code | Remove |
| `#if 0 ... #endif` | Disabled code | Remove or document |
| Unused `#define` | Dead macro | Remove |
| Unused `typedef` | Dead type | Remove |

### 9. Mongoose API Hardening

OTTO C APIs using mongoose should implement rate limiting and work queues from `shared/`.

#### Rate Limiting (`shared/include/sh_ratelimit.h`)

**Required for all public-facing HTTP endpoints.**

```c
#include "sh_ratelimit.h"

// Initialize at startup
static ShRateLimiter *s_rate_limiter = NULL;

int main(void) {
    // 10 requests/sec, burst of 100, 4096 tracked IPs
    s_rate_limiter = sh_ratelimit_create(10.0, 100.0, 4096);
    // ...
    sh_ratelimit_free(s_rate_limiter);
}

// In HTTP handler
static void handle_request(struct mg_connection *c, struct mg_http_message *hm) {
    // Check rate limit
    ShRateLimitAddr client_addr;
    if (c->rem.is_ip6) {
        sh_ratelimit_addr_ipv6(&client_addr,
                               c->rem.addr.ip6[0], c->rem.addr.ip6[1]);
    } else {
        sh_ratelimit_addr_ipv4(&client_addr, c->rem.addr.ip4);
    }

    if (!sh_ratelimit_check(s_rate_limiter, &client_addr)) {
        mg_http_reply(c, 429, "Content-Type: text/plain\r\n",
                      "Too Many Requests");
        return;
    }

    // ... handle request ...
}
```

**Audit Checks:**
- [ ] Rate limiter initialized at startup
- [ ] All endpoints check rate limit before processing
- [ ] Returns HTTP 429 when rate limited
- [ ] Rate limiter freed on shutdown
- [ ] Stats exposed via `/api/v1/stats` endpoint

#### Work Queue (`shared/include/sh_workqueue.h`)

**Required for CPU-intensive operations (rendering, routing, solving).**

```c
#include "sh_workqueue.h"

static ShWorkQueue *s_work_queue = NULL;

// Initialize at startup
s_work_queue = sh_workqueue_create(1000, 5.0);  // max 1000 items, 5s timeout

// Producer (HTTP handler)
static void handle_expensive_request(struct mg_connection *c) {
    ShWorkItem item = {
        .data = request_copy,
        .data_len = len,
        .user_ctx = connection_context
    };

    double pressure;
    if (!sh_workqueue_try_push(s_work_queue, &item, &pressure)) {
        mg_http_reply(c, 503, "Content-Type: text/plain\r\n",
                      "Service Unavailable - Queue Full");
        return;
    }
    // Request will be processed asynchronously
}

// Consumer (worker thread)
static void *worker_thread(void *arg) {
    while (!shutdown) {
        ShWorkItem *item = sh_workqueue_pop_timeout(s_work_queue, 100);
        if (!item) continue;

        if (sh_workqueue_item_expired(s_work_queue, item)) {
            // Client already timed out, skip processing
            sh_workqueue_item_free(item);
            continue;
        }

        process_request(item);
        sh_workqueue_item_free(item);
    }
    return NULL;
}

// Shutdown
sh_workqueue_shutdown(s_work_queue);  // Wake waiting consumers
// join worker threads...
sh_workqueue_free(s_work_queue);
```

**Audit Checks:**
- [ ] Work queue used for CPU-intensive endpoints (render, route, solve)
- [ ] Returns HTTP 503 when queue full (backpressure)
- [ ] Checks `sh_workqueue_item_expired()` before processing
- [ ] Returns HTTP 504 for expired requests
- [ ] Work item has `cancelled` flag, set on HTTP timeout, checked by workers
- [ ] Proper shutdown sequence: `shutdown()` -> join threads -> `free()`
- [ ] Stats exposed via `/api/v1/stats` endpoint

#### Required API Endpoints

All mongoose-based API servers MUST implement these standard endpoints:

```c
// Health check - NOT rate limited, NOT queued
GET /api/v1/health

// Statistics - NOT rate limited, NOT queued (for monitoring)
GET /api/v1/stats
```

**Health endpoint** returns:
```json
{
  "status": "healthy",
  "service": "<module>-<purpose>-server",
  "version": "<version>"
}
```

**Stats endpoint** MUST include work queue and rate limiter status (following carta pattern):
```json
{
  "work_queue": {
    "enabled": true,
    "depth": 5,
    "capacity": 256,
    "pushed": 1234,
    "popped": 1230,
    "dropped": 2,
    "expired": 2
  },
  "rate_limit": {
    "enabled": true,
    "rps": 10.0,
    "burst": 100,
    "allowed": 5678,
    "denied": 42
  }
}
```

**CRITICAL:** Health and stats endpoints must NOT be blocked by the work queue. Route them before the work queue logic:
```c
static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    // Rate limiting first (all endpoints)
    if (s_rate_limiter && !sh_ratelimit_check(s_rate_limiter, &client_addr)) {
        mg_http_reply(c, 429, ...);
        return;
    }

    // Health and stats bypass work queue
    if (mg_match(hm->uri, mg_str("/api/v1/health"), NULL)) {
        handle_health(c);  // Fast, no queuing
        return;
    }
    if (mg_match(hm->uri, mg_str("/api/v1/stats"), NULL)) {
        handle_stats(c);   // Fast, no queuing
        return;
    }

    // CPU-intensive endpoints use work queue
    if (mg_match(hm->uri, mg_str("/api/v1/route"), NULL)) {
        handle_route_via_queue(c, hm);  // Uses work queue
        return;
    }
}
```

#### Adaptive Capacity (`shared/include/sh_adaptive.h`)

**Optional: For servers that need to auto-tune rate limits based on measured response times.**

```c
#include "sh_adaptive.h"

static ShAdaptiveTracker *s_adaptive_tracker = NULL;

// Initialize at startup
ShAdaptiveConfig adaptive_cfg;
sh_adaptive_config_init(&adaptive_cfg);
adaptive_cfg.num_workers = num_workers;
adaptive_cfg.target_utilization = 0.7;      // 70% target
adaptive_cfg.client_timeout_ms = 10000.0;   // 10s client timeout
adaptive_cfg.burst_tiles = 25;              // Initial map view tiles
adaptive_cfg.window_size = 1000;            // Sample window
adaptive_cfg.recalc_interval = 1000;        // Recalc every 1000 requests

s_adaptive_tracker = sh_adaptive_create(&adaptive_cfg);

// In worker thread, after processing each request
gettimeofday(&end, NULL);
double response_ms = (end.tv_sec - start.tv_sec) * 1000.0 +
                     (end.tv_usec - start.tv_usec) / 1000.0;

sh_adaptive_record(s_adaptive_tracker, response_ms);

// Check if rate limit should be updated
ShCapacityParams new_params;
if (sh_adaptive_update(s_adaptive_tracker, &new_params)) {
    sh_ratelimit_update_rate(s_rate_limiter,
                             new_params.rate_limit_rps,
                             new_params.rate_limit_burst);
}

// Stats endpoint should include adaptive info
ShAdaptiveStats adaptive_stats;
sh_adaptive_stats(s_adaptive_tracker, &adaptive_stats);
// Report: p50_ms, p90_ms, p99_ms, avg_ms, ema_ms, sample_count, recalc_count
```

**Environment Variables for Adaptive Capacity:**
```bash
# Enable/disable
<MODULE>_ADAPTIVE_ENABLED=0      # Default: disabled

# Tuning parameters
<MODULE>_TARGET_UTILIZATION=0.7  # Target system utilization (0.0-1.0)
<MODULE>_CLIENT_TIMEOUT=10000    # Client timeout in ms
<MODULE>_BURST_TILES=25          # Tiles in initial map view
<MODULE>_ADAPTIVE_WINDOW=1000    # Sample window size
<MODULE>_ADAPTIVE_INTERVAL=1000  # Recalc interval in requests
```

#### Slow Client Protection (`shared/include/sh_httpserver.h`)

**Required: Protect against slow client DoS attacks.**

Slow clients that read responses very slowly can tie up server threads. Use socket write timeouts:

```c
#include "sh_httpserver.h"

static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    /* Set socket write timeout on new connections */
    if (ev == MG_EV_ACCEPT) {
        sh_mg_set_write_timeout(c, 5000);  /* 5 second write timeout */
        return;
    }

    if (ev == MG_EV_HTTP_MSG) {
        // ... handle request ...
    }
}
```

**Why this matters:** A slow client downloading a 2MB tile at 1KB/s blocks the event loop thread for ~2000 seconds. With socket write timeout, the OS terminates stalled sends.

#### Work Item Cancellation

**Required: Prevent wasted CPU when HTTP handler times out.**

When HTTP handler times out waiting for a worker, mark the work item as cancelled so workers can skip processing:

```c
/* Work item struct needs cancelled flag */
typedef struct {
    // ... other fields ...
    volatile int cancelled;  /* Set by HTTP handler on timeout */
} WorkItem;

/* HTTP handler on timeout */
if (!work_item_wait(&item, timeout)) {
    item.cancelled = 1;  /* Mark as cancelled */
    mg_http_reply(c, 504, headers, "Request timeout\n");
    return;
}

/* Worker thread checks before processing */
if (sh_workqueue_item_expired(queue, item) || work->cancelled) {
    // Skip processing - HTTP handler already returned 504
    work_item_complete(work);
    sh_workqueue_item_free(item);
    continue;
}
```

**Audit Checks:**
- [ ] `MG_EV_ACCEPT` handler calls `sh_mg_set_write_timeout(c, 5000)`
- [ ] Work item struct has `volatile int cancelled` field
- [ ] HTTP handler sets `cancelled = 1` on timeout
- [ ] Workers check `cancelled` before CPU-intensive processing

#### Thread Completion Signaling (`shared/include/sh_completion.h`)

**Required: Use ShCompletion for work item completion signaling instead of manual mutex/cond patterns.**

```c
#include "sh_completion.h"

/* Work item struct uses ShCompletion instead of manual fields */
typedef struct {
    /* Request parameters */
    int x, y, z;

    /* Response (set by worker) */
    uint8_t *response_data;
    size_t response_size;
    int status_code;

    /* Completion signaling (replaces manual mutex/cond/completed/cancelled) */
    ShCompletion completion;
} RenderWorkItem;

/* Initialize work item */
static void render_work_item_init(RenderWorkItem *item) {
    memset(item, 0, sizeof(*item));
    item->status_code = 500;  /* Default to error */
    sh_completion_init(&item->completion);
}

/* Clean up work item */
static void render_work_item_cleanup(RenderWorkItem *item) {
    sh_completion_cleanup(&item->completion);
    free(item->response_data);
}

/* HTTP handler waits for completion */
if (!sh_completion_wait(&item->completion, timeout_ms)) {
    /* Timeout - mark as cancelled */
    sh_completion_cancel(&item->completion);
    mg_http_reply(c, 504, headers, "Request timeout\n");
    return;
}

/* Worker signals completion */
sh_completion_signal(&item->completion);

/* Worker checks for cancellation before CPU work */
if (sh_completion_is_cancelled(&item->completion)) {
    /* Skip processing - HTTP handler already returned 504 */
    sh_completion_signal(&item->completion);
    return;
}
```

**Why use ShCompletion:**
- Eliminates ~20 lines of manual mutex/cond boilerplate per work item type
- Thread-safe, well-tested implementation in shared library
- Consistent pattern across all API servers
- Handles edge cases (spurious wakeups, cleanup order)

**Audit Checks:**
- [ ] Uses `ShCompletion` instead of manual `pthread_mutex_t`/`pthread_cond_t`/`completed`/`cancelled` fields
- [ ] Calls `sh_completion_init()` in work item init
- [ ] Calls `sh_completion_cleanup()` in work item cleanup
- [ ] Uses `sh_completion_wait()` with timeout in HTTP handler
- [ ] Uses `sh_completion_cancel()` on timeout (not direct field assignment)
- [ ] Uses `sh_completion_is_cancelled()` in worker (not direct field access)
- [ ] Calls `sh_completion_signal()` when work complete

#### Worker Pool Management (`shared/include/sh_worker_pool.h`)

**Required: Use ShWorkerPool for work queue consumer threads instead of manual thread management.**

```c
#include "sh_worker_pool.h"

static ShWorkerPool *s_worker_pool = NULL;

/* Worker callback function (called by pool for each work item) */
static void render_worker_callback(ShWorkItem *queue_item, void *ctx) {
    (void)ctx;

    RenderWorkItem *item = (RenderWorkItem *)queue_item->user_ctx;
    if (!item) {
        sh_workqueue_item_free(queue_item);
        return;
    }

    /* Check expiration/cancellation before CPU work */
    if (sh_workqueue_item_expired(s_work_queue, queue_item) ||
        sh_completion_is_cancelled(&item->completion)) {
        item->status_code = 504;
        sh_completion_signal(&item->completion);
        sh_workqueue_item_free(queue_item);
        return;
    }

    /* Do the work */
    process_request(item);

    /* Signal completion and free queue item */
    sh_completion_signal(&item->completion);
    sh_workqueue_item_free(queue_item);
}

/* Initialize at startup */
ShWorkerPoolConfig pool_cfg = {
    .queue = s_work_queue,
    .callback = render_worker_callback,
    .ctx = NULL,
    .poll_timeout_ms = 100
};
s_worker_pool = sh_worker_pool_create(num_workers, &pool_cfg);  /* 0 = auto-detect CPU count */

printf("Created %d workers\n", sh_worker_pool_size(s_worker_pool));

/* Shutdown sequence */
sh_worker_pool_stop(s_worker_pool);   /* Signals queue shutdown, wakes threads */
sh_worker_pool_join(s_worker_pool);   /* Waits for all threads to exit */
sh_worker_pool_free(s_worker_pool);   /* Frees resources */
```

**Why use ShWorkerPool:**
- Eliminates ~40 lines of manual pthread_create/join boilerplate
- Auto-detects CPU count when num_workers=0
- Handles partial thread creation failures gracefully
- Proper shutdown sequence with queue signaling
- Consistent pattern across all API servers

**Audit Checks:**
- [ ] Uses `ShWorkerPool` instead of manual `pthread_t` arrays
- [ ] Worker function is a callback (`void (*)(ShWorkItem*, void*)`) not a thread function
- [ ] Uses `sh_worker_pool_create()` with 0 for auto-detect or specific count
- [ ] Shutdown uses `stop()` then `join()` then `free()` sequence
- [ ] Uses `sh_worker_pool_size()` for stats instead of manual counter
- [ ] No manual `pthread_create()` / `pthread_join()` for work queue consumers

#### API Hardening Checklist

| Check | Severity | Description |
|-------|----------|-------------|
| Rate limiting | High | All endpoints protected from abuse |
| Work queue | High | CPU-intensive ops don't block event loop |
| Socket write timeout | High | `sh_mg_set_write_timeout()` on MG_EV_ACCEPT |
| ShCompletion | High | Use `sh_completion.h` for work item signaling |
| ShWorkerPool | High | Use `sh_worker_pool.h` for worker thread management |
| Health endpoint | High | `/api/v1/health` exists and bypasses work queue |
| Stats endpoint | High | `/api/v1/stats` exists, bypasses queue, includes queue/limiter stats |
| 429 response | Medium | Correct status code for rate limiting |
| 503 response | Medium | Correct status code for queue full |
| 504 response | Medium | Correct status code for timeout |
| Stats format | Medium | Stats match carta pattern (work_queue, rate_limit objects) |
| Adaptive capacity | Low | Optional: auto-tune rate limits from response times |
| Graceful shutdown | Medium | Clean thread termination via `sh_worker_pool_stop/join/free` |
| Structured logging | Medium | Use `sh_log.h` for JSON/text logging |
| Trace ID propagation | Medium | Extract/generate trace IDs via `sh_trace.h` |
| Metrics endpoint | Medium | `/metrics` endpoint for Prometheus scraping |

**Reference Implementation:** See `carta/api/src/main.c` for complete example.

#### Observability Requirements (`shared/include/sh_log.h`, `sh_trace.h`, `sh_metrics.h`)

**Required: Structured Logging**

Use the shared logging library for consistent, parseable logs:

```c
#include "sh_log.h"

/* Initialize at startup */
sh_log_init(NULL);  /* Reads SH_LOG_LEVEL, SH_LOG_FORMAT from env */

/* Log with structured fields */
SH_LOG_INFO("Request received", "method", "GET", "path", "/api/health");
SH_LOG_ERROR("Database error", "code", "ECONNREFUSED", "retry", "3");

/* Shutdown before exit */
sh_log_shutdown();
```

Environment variables:
```bash
SH_LOG_LEVEL=INFO    # TRACE, DEBUG, INFO, WARN, ERROR, FATAL
SH_LOG_FORMAT=json   # json or text
SH_LOG_COLOR=1       # ANSI colors in text mode (auto-detected)
```

**Required: Trace ID Propagation**

Extract trace IDs from incoming requests, generate if missing, and include in all logs:

```c
#include "sh_trace.h"

static void handle_request(struct mg_connection *c, struct mg_http_message *hm) {
    /* Extract or generate trace ID */
    sh_trace_from_headers(my_header_getter, hm);

    SH_LOG_INFO("Processing request");  /* Trace ID auto-included */

    /* ... process ... */

    /* Add trace ID to response */
    char trace_hdr[64];
    sh_trace_format_header(trace_hdr, sizeof(trace_hdr));
    mg_http_reply(c, 200, trace_hdr, "{\"status\":\"ok\"}");

    /* Clear trace context */
    sh_trace_clear();
}
```

**Required: Metrics Collection**

Record key metrics for monitoring:

```c
#include "sh_metrics.h"

/* Initialize at startup */
ShMetricsConfig cfg = SH_METRICS_CONFIG_DEFAULT;
cfg.service = "carta";
cfg.statsd_host = getenv("SH_METRICS_STATSD_HOST");
sh_metrics_init(&cfg);

/* Record metrics */
ShMetricsTimer timer = sh_metrics_timer_start();
/* ... process request ... */
sh_metrics_timer_observe(timer, "http_request_duration_ms", "method:GET", NULL);
sh_metrics_counter_inc("http_requests_total", 1, "status:200", NULL);

/* Add Prometheus endpoint */
if (mg_match(hm->uri, mg_str("/metrics"), NULL)) {
    char *prom = sh_metrics_prometheus_output();
    mg_http_reply(c, 200, "Content-Type: text/plain\r\n", "%s", prom);
    free(prom);
}
```

#### Observability Checklist

- [ ] Logging initialized with `sh_log_init(NULL)` at startup
- [ ] Logging shutdown with `sh_log_shutdown()` before exit
- [ ] All significant operations logged with appropriate level
- [ ] Error logs include context fields (error code, retry count, etc.)
- [ ] Trace ID extracted from `X-Trace-Id` or `X-Request-Id` headers
- [ ] Trace ID generated if not present in request
- [ ] Trace ID included in response headers
- [ ] Trace context cleared after request completes
- [ ] Metrics initialized with service name
- [ ] HTTP request duration recorded as histogram
- [ ] HTTP request count recorded as counter with status code
- [ ] `/metrics` endpoint returns Prometheus format
- [ ] Work queue metrics exposed (depth, dropped, expired)
- [ ] Rate limiter metrics exposed (allowed, denied)

### 10. API Client Resilience (HTTP Clients)

When OTTO components act as HTTP **clients** (e.g., calling external APIs, tile servers, routing services), they must handle backpressure gracefully.

**Shared Library APIs (Preferred):**

OTTO provides production-ready implementations in `shared/`:
- `sh_circuit.h` - Thread-safe circuit breaker
- `sh_backoff.h` - Exponential backoff with jitter
- `sh_retry.h` - HTTP retry logic combining circuit + backoff
- `sh_cors.h` - CORS header generation utilities

```c
#include "sh_circuit.h"
#include "sh_backoff.h"
#include "sh_retry.h"
#include "sh_cors.h"

// Example: Using the retry API
ShCircuitBreaker *circuit = sh_circuit_create(NULL);  // Uses defaults
ShRetryContext ctx;
sh_retry_init(&ctx, circuit, NULL);  // Uses defaults

while (sh_retry_should_attempt(&ctx)) {
    int status = make_http_request();
    double delay = sh_retry_after_response(&ctx, status, get_retry_after_header());

    if (!sh_retry_should_continue(&ctx)) {
        break;  // Success or non-retryable error
    }

    usleep((useconds_t)(delay * 1000));  // Wait before retry
}

sh_circuit_free(circuit);
```

Configuration via environment variables (using `sh_args.h`):
```
{PREFIX}_CIRCUIT_ENABLED=1
{PREFIX}_CIRCUIT_FAILURE_THRESHOLD=5
{PREFIX}_CIRCUIT_SUCCESS_THRESHOLD=2
{PREFIX}_CIRCUIT_OPEN_MS=30000
{PREFIX}_RETRY_MAX_ATTEMPTS=3
{PREFIX}_RETRY_BASE_DELAY_MS=100
{PREFIX}_RETRY_MAX_DELAY_MS=10000
{PREFIX}_RETRY_JITTER_FACTOR=0.5
{PREFIX}_CORS_ORIGINS=https://app.example.com
{PREFIX}_CORS_CREDENTIALS=0
```

**Implementation details follow for understanding the patterns (prefer using shared/ APIs):**

#### HTTP Status Code Handling

| Status | Meaning | Client Action |
|--------|---------|---------------|
| 429 | Rate limited | Back off, respect `Retry-After` header |
| 503 | Service unavailable | Back off, retry with exponential delay |
| 504 | Gateway timeout | Retry once, then fail |
| 502 | Bad gateway | Retry with backoff |
| 5xx | Server error | Retry with backoff, circuit breaker |

```c
typedef enum {
    HTTP_RETRY_NONE = 0,      // Don't retry (4xx client errors)
    HTTP_RETRY_IMMEDIATE,     // Retry once immediately (timeout)
    HTTP_RETRY_BACKOFF,       // Exponential backoff (5xx, 429)
    HTTP_RETRY_CIRCUIT_OPEN   // Circuit breaker tripped, fail fast
} HttpRetryAction;

HttpRetryAction classify_http_status(int status) {
    if (status >= 200 && status < 300) return HTTP_RETRY_NONE;  // Success
    if (status == 429) return HTTP_RETRY_BACKOFF;  // Rate limited
    if (status == 503) return HTTP_RETRY_BACKOFF;  // Overloaded
    if (status == 504) return HTTP_RETRY_IMMEDIATE;  // Timeout
    if (status >= 500) return HTTP_RETRY_BACKOFF;  // Server error
    return HTTP_RETRY_NONE;  // Client error (4xx) - don't retry
}
```

#### Exponential Backoff with Jitter

```c
typedef struct {
    uint32_t base_delay_ms;      // Initial delay (e.g., 100ms)
    uint32_t max_delay_ms;       // Maximum delay cap (e.g., 30000ms)
    uint32_t max_retries;        // Maximum retry attempts (e.g., 5)
    double jitter_factor;        // Randomization (0.0-1.0, e.g., 0.2)
} RetryConfig;

// Default configuration
static const RetryConfig RETRY_CONFIG_DEFAULT = {
    .base_delay_ms = 100,
    .max_delay_ms = 30000,
    .max_retries = 5,
    .jitter_factor = 0.2
};

uint32_t calculate_backoff(const RetryConfig *cfg, uint32_t attempt) {
    // Exponential: base * 2^attempt
    uint32_t delay = cfg->base_delay_ms * (1u << attempt);
    if (delay > cfg->max_delay_ms) {
        delay = cfg->max_delay_ms;
    }

    // Add jitter: delay * (1 - jitter + random * 2 * jitter)
    double jitter = cfg->jitter_factor;
    double random = (double)rand() / RAND_MAX;
    double factor = 1.0 - jitter + random * 2.0 * jitter;

    return (uint32_t)(delay * factor);
}
```

#### Circuit Breaker Pattern

```c
typedef enum {
    CIRCUIT_CLOSED,      // Normal operation, requests flow through
    CIRCUIT_OPEN,        // Failing fast, rejecting requests
    CIRCUIT_HALF_OPEN    // Testing if service recovered
} CircuitState;

typedef struct {
    CircuitState state;
    uint32_t failure_count;
    uint32_t success_count;
    uint64_t last_failure_time_ms;
    uint64_t open_duration_ms;      // How long to stay open (e.g., 30000ms)
    uint32_t failure_threshold;     // Failures before opening (e.g., 5)
    uint32_t success_threshold;     // Successes to close from half-open (e.g., 3)
    pthread_mutex_t mutex;
} CircuitBreaker;

// Configuration via environment variables
// <PREFIX>_CIRCUIT_FAILURE_THRESHOLD=5
// <PREFIX>_CIRCUIT_OPEN_DURATION_MS=30000
// <PREFIX>_CIRCUIT_SUCCESS_THRESHOLD=3

bool circuit_breaker_allow(CircuitBreaker *cb) {
    pthread_mutex_lock(&cb->mutex);

    uint64_t now = get_time_ms();

    switch (cb->state) {
        case CIRCUIT_CLOSED:
            pthread_mutex_unlock(&cb->mutex);
            return true;

        case CIRCUIT_OPEN:
            if (now - cb->last_failure_time_ms > cb->open_duration_ms) {
                cb->state = CIRCUIT_HALF_OPEN;
                cb->success_count = 0;
                pthread_mutex_unlock(&cb->mutex);
                return true;  // Allow one request through
            }
            pthread_mutex_unlock(&cb->mutex);
            return false;  // Fail fast

        case CIRCUIT_HALF_OPEN:
            pthread_mutex_unlock(&cb->mutex);
            return true;  // Testing recovery
    }
    pthread_mutex_unlock(&cb->mutex);
    return false;
}

void circuit_breaker_record_success(CircuitBreaker *cb) {
    pthread_mutex_lock(&cb->mutex);
    cb->failure_count = 0;

    if (cb->state == CIRCUIT_HALF_OPEN) {
        cb->success_count++;
        if (cb->success_count >= cb->success_threshold) {
            cb->state = CIRCUIT_CLOSED;
        }
    }
    pthread_mutex_unlock(&cb->mutex);
}

void circuit_breaker_record_failure(CircuitBreaker *cb) {
    pthread_mutex_lock(&cb->mutex);
    cb->failure_count++;
    cb->last_failure_time_ms = get_time_ms();

    if (cb->state == CIRCUIT_HALF_OPEN) {
        cb->state = CIRCUIT_OPEN;
    } else if (cb->failure_count >= cb->failure_threshold) {
        cb->state = CIRCUIT_OPEN;
    }
    pthread_mutex_unlock(&cb->mutex);
}
```

#### CORS Handling (Server-Side)

**Using sh_cors.h (Preferred):**

```c
#include "sh_cors.h"

// Initialize CORS config (in server init)
ShCorsConfig cors;
sh_cors_init(&cors);
sh_cors_parse_origins(&cors, getenv("CARTA_CORS_ORIGINS"));  // "https://app.example.com,https://other.com"
cors.allow_credentials = atoi(getenv("CARTA_CORS_CREDENTIALS") ?: "0");

// In request handler
static void handle_request(struct mg_connection *c, struct mg_http_message *hm) {
    struct mg_str origin_hdr = mg_http_get_header(hm, "Origin");
    char origin[256] = "";
    if (origin_hdr.buf) {
        snprintf(origin, sizeof(origin), "%.*s", (int)origin_hdr.len, origin_hdr.buf);
    }

    // Handle OPTIONS preflight
    if (mg_strcmp(hm->method, mg_str("OPTIONS")) == 0) {
        char cors_headers[512];
        int len = sh_cors_preflight_headers(&cors, origin, cors_headers, sizeof(cors_headers));
        mg_printf(c, "HTTP/1.1 204 No Content\r\n%s\r\n", cors_headers);
        return;
    }

    // Regular response with CORS headers
    char cors_headers[256];
    sh_cors_headers(&cors, origin, cors_headers, sizeof(cors_headers));
    mg_printf(c, "HTTP/1.1 200 OK\r\n%sContent-Type: application/json\r\n\r\n", cors_headers);
}
```

**Manual implementation (for reference):**

```c
// Standard CORS headers for API responses
static void add_cors_headers(struct mg_connection *c, struct mg_http_message *hm) {
    // Check Origin header for allowed origins (configurable)
    struct mg_str origin = mg_http_get_header(hm, "Origin");
    const char *allowed_origin = "*";  // Or validate against allowlist

    mg_printf(c,
        "Access-Control-Allow-Origin: %s\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
        "Access-Control-Max-Age: 86400\r\n",
        allowed_origin);
}

// Handle OPTIONS preflight
if (mg_strcmp(hm->method, mg_str("OPTIONS")) == 0) {
    mg_printf(c, "HTTP/1.1 204 No Content\r\n");
    add_cors_headers(c, hm);
    mg_printf(c, "\r\n");
    return;
}
```

#### API Client Audit Checklist

| Check | Severity | Description |
|-------|----------|-------------|
| 429 handling | High | Respects rate limit, backs off |
| 503/504 handling | High | Retries with backoff, not immediately |
| Circuit breaker | High | Fails fast when downstream is unhealthy |
| Retry limits | High | Maximum retries configured, not infinite |
| Backoff jitter | Medium | Randomized delays prevent thundering herd |
| Retry-After header | Medium | Respects server-specified delay |
| Timeout configuration | Medium | Reasonable timeouts, not infinite |
| CORS preflight | Medium | OPTIONS handled correctly |
| Configurable | Low | Retry/circuit settings via env vars |

### 11. Static/Global Variables and Thread Safety

#### Rule: No Static/Global State in Libraries

**Libraries (ralph, velo, carta, locus, fuelwise, shared) must NEVER use static or global variables.**

```c
// BAD: Static state in library - breaks reentrancy and thread safety
static Graph *s_cached_graph = NULL;  // NEVER in a library!
static int s_last_error = 0;          // NEVER in a library!

// GOOD: Pass state through parameters
int vl_route(VLGraph *graph, VLArena *arena, int from, int to, VLRoute *out);

// GOOD: Use context structs for state
typedef struct {
    Graph *graph;
    int last_error;
    // ... other state ...
} VLContext;

VLContext *vl_context_create(void);
int vl_route(VLContext *ctx, int from, int to, VLRoute *out);
```

**Why no static/global in libraries:**
- **Thread safety**: Multiple threads can't safely use library simultaneously
- **Reentrancy**: Can't call library functions from callbacks/signal handlers
- **Testing**: Hard to reset state between tests
- **Multiple instances**: Can't have two independent instances of library state

#### Exception: Mongoose API Servers

**API servers (mongoose-based) MAY use static/global variables** for:
- Configuration loaded at startup
- Shared resources (rate limiter, work queue, caches)
- Statistics counters

```c
// ACCEPTABLE in API server (NOT in library)
static ShRateLimiter *s_rate_limiter = NULL;
static ShWorkQueue *s_work_queue = NULL;
static CTPBFContext *s_pbf_ctx = NULL;  // Read-only after init
static volatile sig_atomic_t s_signo = 0;
```

#### Thread Safety Requirements

**Rule: Any resource written by multiple threads MUST be thread-safe.**

| Access Pattern | Thread Safety | Example |
|----------------|---------------|---------|
| Read-only after init | Safe | `s_pbf_ctx` (loaded once at startup) |
| Single writer, multiple readers | Needs atomic or mutex | Statistics counters |
| Multiple writers | MUST use mutex/atomic | Rate limiter hash table |
| Signal handlers | Use `volatile sig_atomic_t` | `s_signo` for shutdown |

**Thread-Safe Patterns:**

```c
// Pattern 1: Atomic counters for statistics
#include <stdatomic.h>
static atomic_uint_fast64_t s_requests_total = 0;
static atomic_uint_fast64_t s_requests_failed = 0;

void handle_request(...) {
    atomic_fetch_add(&s_requests_total, 1);
    if (error) {
        atomic_fetch_add(&s_requests_failed, 1);
    }
}

// Pattern 2: Mutex for complex shared state
static pthread_mutex_t s_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static Cache *s_cache = NULL;

void cache_insert(Key k, Value v) {
    pthread_mutex_lock(&s_cache_mutex);
    cache_insert_internal(s_cache, k, v);
    pthread_mutex_unlock(&s_cache_mutex);
}

// Pattern 3: Read-write lock for read-heavy workloads
static pthread_rwlock_t s_data_lock = PTHREAD_RWLOCK_INITIALIZER;
static Data *s_shared_data = NULL;

Data *get_data(void) {
    pthread_rwlock_rdlock(&s_data_lock);
    Data *copy = copy_data(s_shared_data);
    pthread_rwlock_unlock(&s_data_lock);
    return copy;
}

void update_data(Data *new_data) {
    pthread_rwlock_wrlock(&s_data_lock);
    free(s_shared_data);
    s_shared_data = new_data;
    pthread_rwlock_unlock(&s_data_lock);
}

// Pattern 4: Signal-safe shutdown flag
static volatile sig_atomic_t s_shutdown = 0;

void signal_handler(int signo) {
    s_shutdown = 1;  // Only assignment, no complex operations
}

while (!s_shutdown) {
    // Main loop
}
```

**What DOESN'T need synchronization:**

```c
// Thread-local storage - each thread has its own copy
static __thread int t_local_counter = 0;

// Read-only after initialization
static const Config *s_config = NULL;  // Set once in main(), never modified

// Already thread-safe (by design)
static ShRateLimiter *s_limiter = NULL;  // sh_ratelimit_check() is thread-safe
static ShWorkQueue *s_queue = NULL;      // sh_workqueue_* are thread-safe
```

**Audit Checks:**

For **libraries**:
- [ ] No `static` variables (except `const` compile-time constants)
- [ ] No global variables
- [ ] All state passed through parameters or context structs
- [ ] Thread-safe by design (no shared mutable state)

For **API servers**:
- [ ] Static/global variables documented with thread-safety notes
- [ ] Read-only globals marked `const` or documented as "init once"
- [ ] Shared mutable state protected by mutex/atomic
- [ ] Statistics counters use atomics
- [ ] Shutdown flag uses `volatile sig_atomic_t`
- [ ] Uses thread-safe shared library APIs (`sh_ratelimit`, `sh_workqueue`)

### 12. Shared Library Delegation

**Rule: Reusable functionality MUST live in `shared/`.**

When auditing a module, check if any functionality can be delegated to or already exists in the shared library.

#### What Belongs in `shared/`

| Category | Examples in `shared/` | Check for Duplication |
|----------|----------------------|----------------------|
| **Geo** | `sh_haversine()`, `SHCoord`, `SH_EARTH_RADIUS_M` | Module has own distance calc? |
| **Units** | `sh_km_to_miles()`, `sh_l100km_to_mpg()`, `SHUnitSystem` | Module defines unit constants? |
| **Data Structures** | `sh_hashmap`, `sh_heap`, `sh_pool`, `sh_arena` | Module has own hashmap/heap? |
| **HTTP** | `sh_ratelimit`, `sh_workqueue`, `sh_completion` | Module reimplements rate limiting? |
| **Protobuf** | `sh_protobuf_*` | Module has own protobuf parsing? |
| **Resilience** | `sh_circuit`, `sh_backoff`, `sh_retry` | Module has retry logic inline? |

#### Audit Checks

**Duplication Detection:**
```c
// BAD: Module duplicates shared functionality
#define FW_EARTH_RADIUS_MILES 3958.8  // Use SH_EARTH_RADIUS_M instead!
double fw_haversine(...) { ... }       // Use sh_haversine() instead!

// GOOD: Delegate to shared
#include "sh_geo.h"
double distance_m = sh_haversine(a, b);
```

**Candidate for Extraction:**
```c
// If you find utility code that's not domain-specific, extract to shared/
// Examples:
// - Generic string helpers → shared/include/sh_string.h
// - Time formatting → shared/include/sh_time.h
// - Math utilities → shared/include/sh_math.h
```

#### Audit Questions

When reviewing a module, ask:

1. **Does this code exist in shared?** Search `shared/include/` for similar functionality
2. **Is this domain-specific?** If no, it belongs in shared
3. **Would other modules benefit?** If yes, extract to shared
4. **Does the module reinvent geo/units/data structures?** Red flag for delegation

#### Shared Delegation Checklist

- [ ] No duplicate distance calculations (use `sh_haversine()`)
- [ ] No duplicate unit definitions (use `sh_units.h` constants)
- [ ] No custom coordinate types (use `SHCoord`)
- [ ] No inline rate limiting (use `sh_ratelimit.h`)
- [ ] No inline exponential backoff (use `sh_backoff.h`)
- [ ] No custom hashmap implementations (use `sh_hashmap.h`)
- [ ] Utility functions extracted to shared if reusable

### 13. SI Units Enforcement (Internal Representation)

**Rule: Internal calculations MUST use SI units. Imperial units only at API boundaries.**

OTTO uses a consistent internal unit system for all calculations:

| Quantity | Internal SI Unit | API Metric | API Imperial |
|----------|-----------------|------------|--------------|
| Distance | **meters** | km | miles |
| Volume | **liters** | liters | gallons |
| Efficiency | **L/100km** | L/100km | MPG |
| Mass | **kilograms** | kg | lbs |
| Price | **per liter** | $/L | $/gallon |

#### Why SI Internally

1. **Consistency**: `sh_haversine()` returns meters, all geo functions use meters
2. **No conversion errors**: Single unit system eliminates mid-calculation conversions
3. **Precision**: SI units are the international standard
4. **Simplicity**: Convert once at API boundary, not throughout code

#### Boundary Conversion Pattern

```c
// API INPUT (boundary): Convert to internal SI
if (units == SH_UNITS_IMPERIAL) {
    distance_m = sh_miles_to_m(input_distance);
    consumption_l100km = sh_mpg_to_l100km(input_mpg);
    volume_l = sh_gallons_to_liters(input_gallons);
}

// INTERNAL: All calculations in SI (meters, liters, L/100km, kg)
double fuel_consumed_l = (distance_m / 100000.0) * consumption_l100km;

// API OUTPUT (boundary): Convert from internal SI
if (units == SH_UNITS_IMPERIAL) {
    output_gallons = sh_liters_to_gallons(fuel_consumed_l);
}
```

#### Audit Patterns to Flag

**BAD: Imperial constants in internal code**
```c
// WRONG: Imperial radius used internally
#define EARTH_RADIUS_MILES 3958.8
double dist = haversine_miles(a, b);

// RIGHT: SI radius, convert at boundary if needed
#include "sh_geo.h"
double dist_m = sh_haversine(a, b);  // Returns meters
```

**BAD: Unit-specific field names in internal structs**
```c
// WRONG: Field name implies imperial
typedef struct {
    double price_per_gallon;    // Imperial-specific name!
    double consumption_mpg;     // Imperial-specific name!
} Station;

// RIGHT: Unit-agnostic names, document SI
typedef struct {
    double price;        // Price per liter (SI internal)
    double consumption;  // L/100km (SI internal)
} Station;
```

**BAD: Conversion mid-calculation**
```c
// WRONG: Converting in the middle of algorithm
double dist_miles = dist_m / 1609.344;
double fuel_gallons = dist_miles / mpg;

// RIGHT: Work in SI, convert only at output
double fuel_liters = (dist_m / 100000.0) * l100km;
// Convert to gallons only when outputting to imperial API
```

**BAD: MPG formula internally (L/100km is inverse!)**
```c
// WRONG: MPG formula (higher = better)
double fuel = distance / mpg;

// RIGHT: L/100km formula (fuel per distance unit)
// L/100km means: liters consumed per 100 kilometers
// So: fuel(L) = distance(m) / 100000 * consumption(L/100km)
double fuel = (distance_m / 100000.0) * l100km;
```

#### Unit System Documentation

Every module should document its internal unit system:

```c
/*
 * FuelWise uses SI units internally:
 * - Distance: meters
 * - Volume: liters
 * - Fuel efficiency: L/100km
 * - Mass: kilograms
 * - Price: currency per liter
 *
 * API boundaries handle unit conversion via sh_units.h.
 */
```

#### SI Units Checklist

- [ ] No imperial constants defined (use `sh_units.h` for conversion)
- [ ] No imperial unit suffixes in internal field names (`_miles`, `_mpg`, `_gallons`, `_lbs`)
- [ ] Internal distance calculations use meters (from `sh_haversine()`)
- [ ] Fuel consumption uses L/100km formula (not MPG formula)
- [ ] Unit conversion only at API boundaries (input parsing, output formatting)
- [ ] Module documents its internal SI unit system
- [ ] Uses `SHUnitSystem` enum for API unit selection
- [ ] Uses `sh_units.h` functions for conversions (`sh_miles_to_m()`, `sh_mpg_to_l100km()`, etc.)

#### Common L/100km Formula Mistakes

Remember: **L/100km is the inverse of MPG**

| MPG | L/100km | Relationship |
|-----|---------|--------------|
| 30 | 7.84 | Higher MPG = lower L/100km |
| 10 | 23.52 | Lower MPG = higher L/100km |
| 6.5 | 36.19 | Typical truck efficiency |

```c
// Conversion formula
double sh_mpg_to_l100km(double mpg) {
    if (mpg <= 0) return 0;
    return 235.214583 / mpg;  // The conversion constant
}

// Fuel consumption formula for L/100km:
// distance is in meters, consumption is L/100km
double fuel_liters = (distance_m / 100000.0) * consumption_l100km;
// NOT: fuel = distance / consumption (that's the MPG formula!)
```

### 14. Memory Management Tradeoffs and Limitations

Choosing a memory strategy affects API usability, problem size limits, and performance. This section documents real-world tradeoffs to help you choose wisely.

#### Overview: Strategy Comparison

| Strategy | Problem Size | Performance | API Ergonomics | Safety |
|----------|--------------|-------------|----------------|--------|
| **SHArena** | Limited by arena size (growable) | Fastest alloc, good locality | Pointers invalid after reset | High |
| **SHPool** | Limited by pool size (growable) | Fast, excellent locality | Offset-based access | High |
| **Guarded malloc** | Unlimited (system memory) | Slower, fragmentation | Full pointer flexibility | Medium |

---

#### SHArena Limitations

**What it limits:**

| Limitation | Impact | Mitigation |
|------------|--------|------------|
| **Fixed lifetime** | All allocations freed together; can't free individual items | Design APIs around batch processing |
| **Pointer validity** | Pointers invalid after `sh_arena_reset()` | Copy results out before reset; document lifetime |
| **Initial size** | Too small = allocation failures; too large = wasted memory | Use `sh_arena_remaining()` to monitor; start conservative |
| **No realloc** | Can't resize existing allocations | Over-allocate or use multiple allocations |

**API Design Impact:**

```c
// LIMITATION: Caller can't keep the returned pointer
Route *vl_route(VLGraph *g, SHArena *arena, int from, int to);
// ^^^ Route* is only valid until arena is reset!

// SOLUTION 1: Document the lifetime clearly
/**
 * @return Route pointer valid until arena is reset.
 *         Caller must copy if needed longer.
 */

// SOLUTION 2: Let caller provide arena (they control lifetime)
Route *vl_route(VLGraph *g, SHArena *caller_arena, int from, int to);

// SOLUTION 3: Copy-out API (safer but slower)
int vl_route_copy(VLGraph *g, int from, int to, Route *out);
```

**Problem Size:**
- Arena can grow via realloc (but invalidates all existing pointers)
- For truly unbounded problems, use guarded malloc or chunked arenas
- Typical sizing: 1-16MB for per-request work; larger for batch processing

**Performance:**
- Allocation: O(1) bump pointer - fastest possible
- Deallocation: O(1) reset - no per-object overhead
- Locality: Excellent - sequential memory access

**When Arena is WRONG:**
- Objects need independent lifetimes (use malloc)
- API must return pointers caller keeps indefinitely (use malloc + ownership transfer)
- Problem size completely unknown and potentially huge (use malloc with growth)

---

#### SHPool Limitations

**What it limits:**

| Limitation | Impact | Mitigation |
|------------|--------|------------|
| **Fixed element type** | All elements must be same size | Use separate pools for different types |
| **Offset-based access** | Less ergonomic than raw pointers | Use `SH_POOL_PTR()` macro; store offsets not pointers |
| **Growth invalidates pointers** | `sh_pool_grow()` may realloc | Always use offsets; re-fetch pointers after growth |
| **No individual free** | Can't return elements to pool | Use pool reset for batch operations |

**API Design Impact:**

```c
// LIMITATION: Can't store raw pointers to pool elements
typedef struct {
    Coord *coords;      // BAD: Pointer invalidated if pool grows!
    size_t num_coords;
} Way_Bad;

typedef struct {
    size_t coord_offset; // GOOD: Offset survives pool growth
    size_t num_coords;
} Way_Good;

// Access pattern changes:
// Before: way->coords[i]
// After:  SH_POOL_PTR(&pool, Coord, way->coord_offset)[i]
```

**Problem Size:**
- Pool can grow via `sh_pool_grow()` or `sh_pool_ensure_capacity()`
- Growth is O(n) copy but amortized O(1) with doubling strategy
- For huge datasets: pre-calculate required size or accept growth cost

**Performance:**
- Allocation: O(1) bump pointer
- Access: One pointer addition vs direct pointer (negligible)
- Locality: Excellent - all elements contiguous

**When Pool is WRONG:**
- Elements have different sizes (use arena or malloc)
- Need to free individual elements (use malloc or free-list pool)
- Storing pointers to elements in external data structures (use offsets or malloc)

---

#### Guarded malloc/free Limitations

**What it limits:**

| Limitation | Impact | Mitigation |
|------------|--------|------------|
| **Fragmentation** | Long-running processes degrade | Periodic restart; use pools for hot paths |
| **Overhead** | ~16-32 bytes per allocation + alignment | Batch small allocations |
| **Error-prone** | Leaks, double-free, use-after-free | Strict ownership rules; sanitizers |
| **Cache unfriendly** | Scattered allocations | Allocate arrays, not individual nodes |

**API Design Impact:**

```c
// Guarded malloc allows flexible ownership

// OPTION 1: Caller owns (most flexible, most error-prone)
/**
 * @return Newly allocated Solution. Caller must free with ralph_solution_free().
 */
RalphSolution *ralph_solve(RalphModel *m);
void ralph_solution_free(RalphSolution *s);

// OPTION 2: Library owns (simpler for caller, limits usage)
/**
 * @return Solution owned by model. Valid until next solve() or model_free().
 */
const RalphSolution *ralph_solve(RalphModel *m);

// OPTION 3: Caller provides buffer (zero allocations, most control)
int ralph_solve_into(RalphModel *m, RalphSolution *out, size_t out_size);
```

**Problem Size:**
- Unlimited (constrained only by system memory)
- Can handle problems of unknown/unbounded size
- Best for: solvers, parsers with unknown input size

**Performance:**
- Allocation: O(1) but with syscall overhead and lock contention
- Deallocation: O(1) but coalescing overhead
- Locality: Poor unless you allocate arrays

**When malloc is RIGHT:**
- Problem size unknown or unbounded
- Objects need independent lifetimes
- API must return long-lived pointers
- Interop with code expecting standard malloc semantics

---

#### Decision Guide

```
START
  │
  ├─► Do all allocations have the same lifetime?
  │     │
  │     ├─► YES: Are they the same type/size?
  │     │         │
  │     │         ├─► YES: Use SHPool (best locality)
  │     │         │
  │     │         └─► NO: Use SHArena (flexible types)
  │     │
  │     └─► NO: Do objects need to outlive the call?
  │               │
  │               ├─► YES: Use guarded malloc (ownership transfer)
  │               │
  │               └─► NO: Use SHArena with copy-out API
  │
  └─► Is problem size bounded and known?
        │
        ├─► YES: Pre-size arena/pool (fastest)
        │
        └─► NO: Use malloc OR growable arena/pool with offset-based access
```

#### Hybrid Patterns (Recommended for OTTO)

Most real systems use a combination:

```c
// Pattern: Arena for temporaries, malloc for results
Route *compute_route(Graph *g, int from, int to) {
    // Arena for algorithm scratch space (freed automatically)
    SHArena *scratch = sh_arena_create(64 * 1024);

    Node *visited = sh_arena_alloc(scratch, g->num_nodes * sizeof(Node));
    Edge *queue = sh_arena_alloc(scratch, g->num_edges * sizeof(Edge));

    // ... algorithm uses scratch space ...

    // Result allocated with malloc (caller owns)
    Route *result = malloc(sizeof(Route) + path_len * sizeof(int));
    memcpy(result->nodes, path, path_len * sizeof(int));

    sh_arena_free(scratch);  // Scratch freed, result survives
    return result;           // Caller must free
}
```

```c
// Pattern: Pool for bulk storage, arena for per-request work
typedef struct {
    SHPool coord_pool;      // Long-lived: all OSM coordinates
    SHPool way_pool;        // Long-lived: all OSM ways
} MapData;

Tile *render_tile(MapData *map, TileCoord coord) {
    SHArena *req = sh_arena_create(256 * 1024);  // Per-request

    // Temporaries in arena
    Feature *features = sh_arena_alloc(req, ...);

    // Access long-lived data via pool offsets
    Way *way = SH_POOL_PTR(&map->way_pool, Way, way_offset);
    Coord *coords = SH_POOL_PTR(&map->coord_pool, Coord, way->coord_offset);

    // Result allocated for caller
    Tile *tile = malloc(sizeof(Tile));
    // ... fill tile ...

    sh_arena_free(req);
    return tile;
}
```

#### OTTO Module Recommendations

| Module | Primary Strategy | Rationale |
|--------|------------------|-----------|
| **Ralph** | Guarded malloc | Solutions must outlive solve(); problem size unbounded |
| **Velo** | SHPool + Arena | Graph is long-lived (pool); per-route scratch (arena) |
| **Carta** | SHPool + Arena | PBF data in pools; per-tile rendering in arena |
| **Locus** | SHPool | Index data (tries, grids) long-lived, same-type elements |
| **FuelWise** | Guarded malloc | Small problems; interop with Ralph solutions |
| **API servers** | Arena per request | Natural request lifetime; auto-cleanup on errors |

#### Audit Questions

When reviewing memory strategy choices, ask:

1. **Lifetime**: Do all allocations share the same lifetime? → Arena/Pool
2. **Size**: Is the maximum size known? → Pre-size; otherwise growable or malloc
3. **Ownership**: Who frees? → Document clearly; prefer single owner
4. **API stability**: Will pointers be held across calls? → Malloc or offset-based
5. **Performance**: Is allocation on hot path? → Arena/Pool; malloc adds latency
6. **Complexity**: Is manual memory management adding bugs? → Simplify with arena

## Audit Procedure

When `/c-audit <module>` is invoked:

1. **Locate Files**
   ```
   <module>/
   ├── include/     # Public headers
   ├── src/         # Implementation files
   ├── api/src/     # API server (if mongoose-based)
   └── tests/       # Test files
   ```

2. **Check Security Model Compliance** (Warnings)
   - Search for: `system(`, `execl(`, `execlp(`, `execv(`, `execvp(`, `popen(`, `dlopen(`
   - In API servers: check if handlers parse complex input directly vs. delegating
   - Check for broad `fopen()` without path restrictions
   - Reference: `docs/internals/security-model.md`

3. **Scan for Critical Issues**
   - Search for unsafe functions: `strcpy`, `sprintf`, `gets`, `strcat`
   - Search for unchecked allocations: `malloc` without NULL check
   - Search for missing bounds checks on array access

3. **Review Public API**
   - Check all public functions in headers
   - Verify NULL checks on pointer parameters
   - Verify bounds checks on size parameters

4. **Check Resource Management**
   - Every `*_create()` has matching `*_destroy()`
   - Error paths free allocated resources
   - No memory leaks on early returns

5. **Check Memory Pool Usage**
   - Identify patterns that benefit from `SHArena` or `SHPool`
   - Verify pools are freed on all code paths
   - Check for NULL/INVALID return values

6. **Evaluate Memory Strategy Tradeoffs**
   - Does chosen strategy limit problem size? (see Section 10)
   - Are API pointer lifetimes clearly documented?
   - Is arena/pool sized appropriately for expected workload?
   - Would hybrid approach (arena for scratch, malloc for results) be better?
   - Are there unnecessary limitations from overly strict strategy?

7. **Detect Dead Code**
   - Compile with `-Wunused` flags
   - Find unused static functions
   - Find unused variables and parameters
   - Flag commented-out or `#if 0` code blocks

8. **Audit Mongoose APIs** (if `api/` directory exists)
   - Check for `#include "sh_ratelimit.h"`
   - Check for `#include "sh_workqueue.h"` (if CPU-intensive ops)
   - Verify rate limiter initialization and cleanup
   - Verify work queue for expensive operations
   - Check for proper HTTP status codes (429, 503, 504)

9. **Check Static/Global Variables and Thread Safety**
   - **Libraries**: Flag ANY static/global mutable state (not allowed)
   - **API servers**: Verify thread safety of shared mutable state
   - Check statistics counters use atomics
   - Check shutdown flags use `volatile sig_atomic_t`
   - Verify mutex/rwlock usage for complex shared state

10. **Check Shared Library Delegation**
    - Search for duplicate functionality that exists in `shared/`
    - Flag custom distance calculations (should use `sh_haversine()`)
    - Flag custom unit constants (should use `sh_units.h`)
    - Flag custom coordinate types (should use `SHCoord`)
    - Identify reusable utilities that should be extracted to shared

11. **Check SI Units Enforcement**
    - Flag imperial constants defined internally (`EARTH_RADIUS_MILES`, etc.)
    - Flag imperial field name suffixes (`_miles`, `_mpg`, `_gallons`, `_lbs`)
    - Verify internal distance calculations use meters
    - Verify fuel consumption uses L/100km formula, not MPG formula
    - Check unit conversions happen only at API boundaries
    - Verify module documents its internal SI unit system

12. **Verify OTTO Patterns**
    - Correct naming prefix for module
    - Consistent error handling
    - Proper header guards

13. **Generate Report**
    Format: Markdown table with findings, severity, file:line, and suggested fix

## Report Format

```markdown
## C Audit Report: <module>

**Date:** YYYY-MM-DD
**Files Scanned:** N
**Issues Found:** N (Critical: N, High: N, Medium: N, Low: N)

### Critical Issues

| File:Line | Issue | Current Code | Suggested Fix |
|-----------|-------|--------------|---------------|
| src/foo.c:42 | Buffer overflow | `strcpy(buf, src)` | `strncpy(buf, src, sizeof(buf)-1)` |

### High Issues
...

### Recommendations

1. **Memory Safety**: ...
2. **API Hardening**: ...
3. **Test Coverage**: ...
```

## Fix Mode (--fix)

When `--fix` is specified:

1. Generate the audit report first
2. For each fixable issue, apply the transformation
3. Show diff of changes
4. Re-run tests if available (`make test-<module>`)
5. Report any test failures

**Auto-fixable Issues:**
- `strcpy` -> `strncpy` with explicit null terminator
- `strcat` -> `strncat` with remaining buffer size
- `sprintf` -> `snprintf` with buffer size
- `strlen` -> `strnlen` with max length (on untrusted input)
- `strcmp` -> `strncmp` with bounded length
- Missing null terminator after `strncpy` (add explicit `buf[size-1] = '\0'`)
- Missing NULL checks (add early return)
- Missing `= {0}` initialization
- Unused local variables (remove)
- Unused static functions (remove)
- Unused parameters (add `(void)param;`)

**NOT Auto-fixable (require manual review):**
- Logic errors
- Resource leaks in complex control flow
- Integer overflow in calculations
- Missing error handling
- Converting malloc patterns to SHArena/SHPool
- Adding rate limiting to APIs (architecture decision)
- Adding work queues to APIs (requires worker threads)

## Example Invocations

```bash
# Full audit of carta module
/c-audit carta

# Audit with automatic fixes
/c-audit velo --fix

# Audit single file
/c-audit shared/src/sh_geo.c

# Audit all API servers
/c-audit fuelwise/api
/c-audit velo/api
/c-audit carta/api
```

## Integration with Build

Recommended build flags for development:
```makefile
# Add to module Makefile for hardened builds
CFLAGS += -Wall -Wextra -Werror
CFLAGS += -Wconversion -Wshadow -Wformat=2
CFLAGS += -Wstrict-prototypes -Wmissing-prototypes

# Debug builds: Sanitizers
DEBUG_CFLAGS += -fsanitize=address,undefined -g

# Static analysis (if available)
# scan-build make
# cppcheck --enable=all src/
```

## Checklist Summary

Before marking a module as "hardened":

**Security Model Compliance:**
- [ ] No `system()`, `exec*()`, `popen()`, `dlopen()` calls
- [ ] Parsing not done inline in HTTP handlers (delegate to Role P)
- [ ] File access restricted to known paths
- [ ] See `docs/internals/security-model.md` for full requirements

**Memory Safety:**
- [ ] No Critical or High severity issues
- [ ] All public API functions validate inputs
- [ ] All allocations checked for failure
- [ ] All resources properly freed (including error paths)

**String Safety:**
- [ ] No unbounded string functions (`strcpy`, `strcat`, `sprintf`, `gets`)
- [ ] Uses `strnlen` instead of `strlen` on untrusted input
- [ ] Uses `strncmp` instead of `strcmp` where appropriate
- [ ] All `strncpy` calls followed by explicit null termination
- [ ] `snprintf` used instead of `sprintf` (always null-terminates)

**Memory Pools:**
- [ ] Uses `SHArena` for batch allocations with same lifetime
- [ ] Uses `SHPool` for variable-length typed arrays
- [ ] Pools freed on all code paths

**Memory Strategy Tradeoffs:**
- [ ] Strategy doesn't unnecessarily limit problem size
- [ ] Pointer lifetimes documented in API (when invalid, who owns)
- [ ] Arena/pool sizing appropriate for expected workload
- [ ] Hybrid approach used where beneficial (arena for scratch, malloc for results)
- [ ] No over-engineering: malloc is fine when flexibility needed

**Dead Code:**
- [ ] Compiles clean with `-Wunused` flags
- [ ] No unused static functions
- [ ] No unused variables or parameters
- [ ] No `#if 0` or commented-out code blocks

**API Hardening (mongoose servers):**
- [ ] Rate limiting enabled via `sh_ratelimit`
- [ ] Work queue for CPU-intensive operations via `sh_workqueue`
- [ ] Socket write timeout via `sh_mg_set_write_timeout()` on `MG_EV_ACCEPT`
- [ ] Uses `ShCompletion` for work item signaling (not manual mutex/cond)
- [ ] Uses `ShWorkerPool` for worker threads (not manual pthread_create)
- [ ] Worker callback checks `sh_completion_is_cancelled()` before CPU work
- [ ] HTTP handler uses `sh_completion_cancel()` on timeout
- [ ] Shutdown sequence: `sh_worker_pool_stop()` → `join()` → `free()`
- [ ] Proper HTTP status codes (429, 503, 504)
- [ ] Stats endpoint exposes limiter/queue health
- [ ] CORS headers on all responses (including OPTIONS preflight)

**API Client Resilience (HTTP clients):**
- [ ] Uses `sh_retry.h` or equivalent for HTTP retry logic
- [ ] Uses `sh_circuit.h` for circuit breaker protection
- [ ] Uses `sh_backoff.h` for exponential backoff with jitter
- [ ] HTTP 429/503/504 handled with backoff (via `sh_retry_is_retryable_status()`)
- [ ] Maximum retry count bounded (not infinite)
- [ ] Retry-After header respected when present
- [ ] Circuit breaker settings configurable via env vars (`{PREFIX}_CIRCUIT_*`)
- [ ] Uses `sh_cors.h` for CORS header generation (server-side)

**Static/Global and Thread Safety:**
- [ ] **Libraries**: No static/global mutable state (use context structs)
- [ ] **API servers**: Static/global variables documented with thread-safety notes
- [ ] Shared mutable state protected by mutex or atomics
- [ ] Statistics counters use `atomic_uint_fast64_t` or similar
- [ ] Shutdown flags use `volatile sig_atomic_t`
- [ ] Read-only-after-init globals clearly documented

**Shared Library Delegation:**
- [ ] No duplicate distance calculations (use `sh_haversine()`)
- [ ] No duplicate unit definitions (use `sh_units.h` constants)
- [ ] No custom coordinate types (use `SHCoord`)
- [ ] No inline rate limiting (use `sh_ratelimit.h`)
- [ ] No inline exponential backoff (use `sh_backoff.h`)
- [ ] No custom hashmap implementations (use `sh_hashmap.h`)
- [ ] Utility functions extracted to shared if reusable

**SI Units Enforcement (Internal Representation):**
- [ ] No imperial constants defined internally (use `sh_units.h` for conversion)
- [ ] No imperial unit suffixes in internal field names (`_miles`, `_mpg`, `_gallons`, `_lbs`)
- [ ] Internal distance calculations use meters (from `sh_haversine()`)
- [ ] Fuel consumption uses L/100km formula: `fuel = (distance_m / 100000.0) * l100km`
- [ ] Unit conversion only at API boundaries (input parsing, output formatting)
- [ ] Module documents its internal SI unit system
- [ ] Uses `SHUnitSystem` enum for API unit selection
- [ ] Uses `sh_units.h` functions for conversions

**Standards:**
- [ ] Follows OTTO naming conventions
- [ ] Has comprehensive test coverage
- [ ] Compiles clean with `-Wall -Wextra -Werror`
- [ ] Passes AddressSanitizer and UBSan checks
