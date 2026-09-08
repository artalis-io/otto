# Forge - Async Job Queue

**F**lexible **O**rchestration and **R**untime for **G**eneral **E**xecution

## Overview

Forge is a generic job broker for running long-running async tasks. It provides:

- **Job submission** - POST a job with type and payload, get back a job ID
- **Job polling** - GET job status/progress/result by ID
- **WebSocket streaming** - Real-time progress updates
- **Webhook callbacks** - HTTP POST when job completes
- **Job persistence** - SQLite for durability across restarts
- **Process isolation** - Jobs run as separate processes

**Key Design Principle**: Forge is a dumb pipe. It doesn't know about LP solvers or geocoding - it just spawns registered consumer processes and captures their output. Consumers are standalone executables that follow a simple stdin/stdout protocol.

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                        Forge Broker                          │
│  POST /jobs         - Submit job, get ID                     │
│  GET  /jobs/:id     - Poll status/progress                   │
│  GET  /jobs/:id/result - Fetch result (when complete)        │
│  DELETE /jobs/:id   - Cancel job                             │
│  WS   /jobs/:id/stream - Real-time progress via WebSocket    │
└──────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────┐
│                    Job Store (SQLite)                        │
│  Persistent queue, survives restarts, tracks history         │
└──────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────┐
│                      Dispatcher                              │
│  - Spawns consumer processes for pending jobs                │
│  - Manages stdin/stdout/stderr pipes                         │
│  - Enforces max_concurrent limits per consumer type          │
│  - Handles timeouts and cancellation                         │
└──────────────────────────────────────────────────────────────┘
                              │
          ┌───────────┬───────┴───────┬───────────┐
          ▼           ▼               ▼           ▼
    ┌──────────┐ ┌──────────┐  ┌──────────┐ ┌──────────┐
    │ fg-ralph │ │fg-fuelwise│  │ fg-locus │ │ fg-velo  │
    │  LP/MIP  │ │  Refuel  │  │ Geocode  │ │  Route   │
    └──────────┘ └──────────┘  └──────────┘ └──────────┘
```

## Consumer Protocol

A consumer is any executable that follows this contract:

### Input/Output

| Channel | Direction | Format | Purpose |
|---------|-----------|--------|---------|
| **stdin** | Broker → Consumer | JSON | Job payload |
| **stdout** | Consumer → Broker | JSON | Job result (on exit) |
| **stderr** | Consumer → Broker | Text | Progress updates |
| **exit code** | Consumer → Broker | int | 0 = success, non-zero = failure |

### Progress Format (stderr)

```
PROGRESS 0.0 Starting...
PROGRESS 0.25 Loading data...
PROGRESS 0.5 Solving phase 1...
PROGRESS 0.75 Solving phase 2...
PROGRESS 1.0 Complete
LOG Debug information here
WARN Something suspicious but continuing
ERROR Something bad happened
```

Progress lines are parsed by Forge and:
- Stored in the job record
- Streamed to WebSocket clients
- Included in webhook payloads

### Example Consumer Invocation

```bash
# Forge spawns:
./consumers/solve_lp < payload.json > result.json 2> progress.log

# Or with job ID for logging:
./consumers/solve_lp --job-id fg_abc123 < payload.json > result.json
```

### Example Consumer (C)

```c
#include <stdio.h>
#include <stdlib.h>
#include "ralph_lp.h"
#include "fg_consumer.h"  // Optional helper library

int main(int argc, char **argv) {
    // Read payload from stdin
    char *payload = fg_read_stdin();
    if (!payload) {
        fprintf(stderr, "ERROR Failed to read payload\n");
        return 1;
    }

    fg_progress(0.1, "Parsing problem...");

    // Parse and solve
    RalphProblem *prob = ralph_problem_from_json(payload);

    fg_progress(0.3, "Starting solver...");
    RalphSolution *sol = ralph_solve(prob, NULL);

    if (sol->status != RALPH_OPTIMAL) {
        fprintf(stderr, "ERROR Solver failed: %s\n",
                ralph_status_string(sol->status));
        return 1;
    }

    fg_progress(1.0, "Optimal solution found");

    // Output result as JSON to stdout
    char *result = ralph_solution_to_json(sol);
    printf("%s\n", result);

    // Cleanup
    free(result);
    ralph_solution_free(sol);
    ralph_problem_free(prob);
    free(payload);

    return 0;
}
```

## Consumer Registration

### Option 1: Configuration File (forge.yaml)

```yaml
consumers:
  solve_lp:
    command: ./consumers/fg-ralph
    args: ["--mode", "lp"]
    timeout: 300s
    max_concurrent: 4

  solve_mip:
    command: ./consumers/fg-ralph
    args: ["--mode", "mip"]
    timeout: 600s
    max_concurrent: 2

  optimize_refuel:
    command: ./consumers/fg-fuelwise
    timeout: 120s
    max_concurrent: 4

  batch_geocode:
    command: ./consumers/fg-locus
    args: ["--mode", "forward"]
    timeout: 60s
    max_concurrent: 8

  batch_route:
    command: ./consumers/fg-velo
    timeout: 300s
    max_concurrent: 4

  # External consumer - could be Python, Node, anything
  custom_analysis:
    command: python3
    args: ["./scripts/analyze.py"]
    timeout: 600s
    max_concurrent: 1
```

### Option 2: Directory Convention

```
forge/consumers/
├── solve_lp           # executable, handles "solve_lp" jobs
├── solve_mip          # executable, handles "solve_mip" jobs
├── optimize_refuel    # executable, handles "optimize_refuel" jobs
├── batch_geocode      # executable, handles "batch_geocode" jobs
└── batch_route        # executable, handles "batch_route" jobs
```

Forge auto-discovers executables in the consumers directory. Job type = filename.

## SQLite Schema

```sql
CREATE TABLE jobs (
    id TEXT PRIMARY KEY,           -- UUID or nanoid (e.g., "fg_x7kP9q")
    type TEXT NOT NULL,            -- Job type (e.g., "solve_lp")
    status TEXT NOT NULL,          -- pending|running|completed|failed|cancelled
    priority INTEGER DEFAULT 0,    -- Higher = processed sooner
    payload TEXT,                  -- JSON input
    result TEXT,                   -- JSON output (when complete)
    progress REAL DEFAULT 0,       -- 0.0 to 1.0
    progress_message TEXT,         -- Latest progress message
    error TEXT,                    -- Error message if failed
    callback_url TEXT,             -- Optional webhook URL
    created_at INTEGER NOT NULL,   -- Unix timestamp
    started_at INTEGER,            -- When processing began
    completed_at INTEGER,          -- When finished
    expires_at INTEGER,            -- Auto-cleanup time
    pid INTEGER                    -- Consumer process ID (when running)
);

CREATE TABLE consumers (
    type TEXT PRIMARY KEY,
    command TEXT NOT NULL,
    args TEXT,                     -- JSON array
    timeout_ms INTEGER DEFAULT 300000,
    max_concurrent INTEGER DEFAULT 1,
    current_running INTEGER DEFAULT 0
);

CREATE TABLE job_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    job_id TEXT NOT NULL,
    event_type TEXT NOT NULL,      -- progress|log|warning|error
    message TEXT,
    timestamp INTEGER NOT NULL,
    FOREIGN KEY (job_id) REFERENCES jobs(id) ON DELETE CASCADE
);

-- Indexes for efficient queries
CREATE INDEX idx_jobs_status ON jobs(status, priority DESC, created_at);
CREATE INDEX idx_jobs_expires ON jobs(expires_at) WHERE expires_at IS NOT NULL;
CREATE INDEX idx_job_events_job ON job_events(job_id, timestamp);
```

## REST API

### Submit Job

```http
POST /api/v1/jobs
Content-Type: application/json

{
  "type": "solve_lp",
  "payload": {
    "objective": "min",
    "c": [1, 2, 3],
    "A": [[1, 1, 0], [0, 1, 1]],
    "b": [10, 8]
  },
  "callback_url": "https://myapp.com/webhook",
  "priority": 10,
  "ttl_seconds": 3600
}
```

Response:
```json
{
  "id": "fg_x7kP9q",
  "status": "pending",
  "created_at": "2024-01-15T10:30:00Z"
}
```

### Poll Status

```http
GET /api/v1/jobs/fg_x7kP9q
```

Response:
```json
{
  "id": "fg_x7kP9q",
  "type": "solve_lp",
  "status": "running",
  "progress": 0.65,
  "progress_message": "Solving phase 2...",
  "created_at": "2024-01-15T10:30:00Z",
  "started_at": "2024-01-15T10:30:01Z"
}
```

### Get Result

```http
GET /api/v1/jobs/fg_x7kP9q/result
```

Response (when completed):
```json
{
  "status": "optimal",
  "objective": 12.5,
  "values": [4, 6, 2]
}
```

### Cancel Job

```http
DELETE /api/v1/jobs/fg_x7kP9q
```

Response:
```json
{
  "id": "fg_x7kP9q",
  "status": "cancelled"
}
```

### WebSocket Streaming

```javascript
const ws = new WebSocket('ws://localhost:8084/api/v1/jobs/fg_x7kP9q/stream');

ws.onmessage = (e) => {
  const event = JSON.parse(e.data);

  switch (event.type) {
    case 'progress':
      console.log(`Progress: ${event.progress * 100}% - ${event.message}`);
      break;
    case 'completed':
      console.log('Result:', event.result);
      ws.close();
      break;
    case 'failed':
      console.error('Error:', event.error);
      ws.close();
      break;
  }
};
```

### Webhook Callback

When a job completes (or fails), Forge POSTs to `callback_url`:

```http
POST https://myapp.com/webhook
Content-Type: application/json

{
  "job_id": "fg_x7kP9q",
  "type": "solve_lp",
  "status": "completed",
  "result": {
    "status": "optimal",
    "objective": 12.5,
    "values": [4, 6, 2]
  },
  "started_at": "2024-01-15T10:30:01Z",
  "completed_at": "2024-01-15T10:30:05Z"
}
```

### List Consumers

```http
GET /api/v1/consumers
```

Response:
```json
{
  "consumers": [
    {
      "type": "solve_lp",
      "command": "./consumers/fg-ralph",
      "timeout_ms": 300000,
      "max_concurrent": 4,
      "current_running": 2
    },
    {
      "type": "batch_geocode",
      "command": "./consumers/fg-locus",
      "timeout_ms": 60000,
      "max_concurrent": 8,
      "current_running": 0
    }
  ]
}
```

## Directory Structure

```
forge/
├── include/
│   ├── forge.h            # Broker API
│   ├── fg_job.h           # Job structures
│   ├── fg_store.h         # SQLite persistence
│   ├── fg_dispatch.h      # Consumer spawning
│   └── fg_consumer.h      # Consumer helper library (optional)
├── src/
│   ├── forge.c            # Main broker logic
│   ├── fg_job.c           # Job CRUD operations
│   ├── fg_store.c         # SQLite operations
│   ├── fg_dispatch.c      # fork/exec, pipe management
│   ├── fg_progress.c      # Parse stderr progress
│   └── fg_config.c        # Load consumer registry
├── api/
│   └── src/main.c         # REST + WebSocket server (:8084)
├── consumers/             # Built-in consumers for OTTO modules
│   ├── fg-ralph.c         # Ralph LP/MIP consumer
│   ├── fg-fuelwise.c      # FuelWise consumer
│   ├── fg-velo.c          # Velo batch routing consumer
│   ├── fg-locus.c         # Locus batch geocoding consumer
│   └── fg-carta.c         # Carta tile generation consumer
├── tests/
│   └── test_forge.c       # Unit tests
├── forge.yaml             # Consumer configuration
└── CLAUDE.md              # Development instructions
```

## C API

### Broker API

```c
// Initialize broker
FGBroker *fg_broker_create(const char *db_path);

// Load consumer registry
int fg_broker_load_config(FGBroker *broker, const char *config_path);
int fg_broker_discover_consumers(FGBroker *broker, const char *consumers_dir);

// Submit a job (returns job ID, caller must free)
char *fg_submit(FGBroker *broker, const char *type,
                const char *payload_json, FGJobOptions *opts);

// Query job
FGJobInfo *fg_get_job(FGBroker *broker, const char *job_id);
void fg_job_info_free(FGJobInfo *info);

// Get result (NULL if not complete, caller must free)
char *fg_get_result(FGBroker *broker, const char *job_id);

// Cancel job
int fg_cancel(FGBroker *broker, const char *job_id);

// Start dispatcher loop (call in separate thread or after event loop setup)
int fg_broker_start(FGBroker *broker);

// Stop dispatcher and cleanup
void fg_broker_shutdown(FGBroker *broker);
```

### Consumer Helper Library (Optional)

```c
// Read entire stdin into malloc'd buffer
char *fg_read_stdin(void);

// Report progress (writes to stderr in correct format)
void fg_progress(double progress, const char *fmt, ...);

// Log messages
void fg_log(const char *fmt, ...);
void fg_warn(const char *fmt, ...);
void fg_error(const char *fmt, ...);

// Write result to stdout
void fg_write_result(const char *json);
```

## Job Lifecycle

```
┌─────────┐    submit    ┌─────────┐
│ (none)  │ ──────────▶  │ pending │
└─────────┘              └────┬────┘
                              │
                              │ dispatcher picks up
                              ▼
                        ┌─────────┐
                        │ running │◀──────┐
                        └────┬────┘       │
                             │            │ progress updates
             ┌───────────────┼────────────┘
             │               │
             │               │
    ┌────────┴────┐    ┌─────┴─────┐    ┌───────────┐
    │  completed  │    │  failed   │    │ cancelled │
    └─────────────┘    └───────────┘    └───────────┘
           │                 │                │
           └─────────────────┴────────────────┘
                             │
                             │ TTL expires
                             ▼
                      (auto-deleted)
```

## Benefits

1. **Language agnostic** - Consumers can be C, Python, Go, Rust, shell scripts
2. **Process isolation** - Consumer crash doesn't kill Forge broker
3. **Simple debugging** - Run consumer manually: `echo '{"x":1}' | ./consumer`
4. **Easy scaling** - `max_concurrent` limits per consumer type
5. **Hot reload** - Replace consumer executable without restarting Forge
6. **External consumers** - Third parties can add job types without touching Forge
7. **Durable** - SQLite persistence survives broker restarts
8. **Observable** - Progress streaming, webhook notifications

## Integration with OTTO

Forge enables async execution of all OTTO optimization modules:

| Job Type | Consumer | Use Case |
|----------|----------|----------|
| `solve_lp` | fg-ralph | LP optimization |
| `solve_mip` | fg-ralph | MIP optimization (slower) |
| `optimize_refuel` | fg-fuelwise | Refueling route optimization |
| `batch_route` | fg-velo | Calculate multiple routes |
| `batch_geocode` | fg-locus | Geocode many addresses |
| `batch_reverse` | fg-locus | Reverse geocode coordinates |
| `build_index` | fg-locus | Build geocoding index from PBF |
| `generate_tiles` | fg-carta | Pre-generate tile range |

### Example: Async Refueling Optimization

```bash
# Submit optimization job
curl -X POST http://localhost:8084/api/v1/jobs \
  -H "Content-Type: application/json" \
  -d '{
    "type": "optimize_refuel",
    "payload": {
      "route": [[47.5, 19.0], [48.2, 16.4], [52.5, 13.4]],
      "stations": [...],
      "tank_capacity": 300,
      "current_fuel": 100
    },
    "callback_url": "https://myapp.com/optimization-complete"
  }'

# Response: {"id": "fg_abc123", "status": "pending"}

# Poll for completion (or wait for webhook)
curl http://localhost:8084/api/v1/jobs/fg_abc123

# Get result when complete
curl http://localhost:8084/api/v1/jobs/fg_abc123/result
```

## Dependencies

- **SQLite**: Embedded database (public domain, will be vendored)
- **Keel**: HTTP server (vendor/keel submodule)
- **pthreads**: Thread pool for dispatcher (standard POSIX)

No external services required. Everything runs in a single process with SQLite for persistence.

## Distributed Deployment

The single-node design scales horizontally with minimal changes. The architecture cleanly separates concerns (API, queue, workers), making distribution straightforward.

### Level 1: Remote Workers

Keep single broker, but workers connect over the network instead of being spawned locally:

```
┌──────────────────────────────────────────────────────────┐
│                    Forge Broker                           │
│  - SQLite for job persistence                             │
│  - REST API for job submission                            │
│  - WebSocket for worker connections                       │
└──────────────────────────────────────────────────────────┘
              │              │              │
              │ WebSocket    │ WebSocket    │ WebSocket
              ▼              ▼              ▼
        ┌──────────┐   ┌──────────┐   ┌──────────┐
        │ Worker 1 │   │ Worker 2 │   │ Worker 3 │
        │ (local)  │   │ (remote) │   │ (k8s pod)│
        └──────────┘   └──────────┘   └──────────┘
```

**Worker WebSocket Protocol:**

```json
// Worker → Broker
{"type": "register", "job_types": ["solve_lp", "solve_mip"], "capacity": 4}
{"type": "heartbeat"}
{"type": "progress", "job_id": "fg_abc", "progress": 0.5, "message": "..."}
{"type": "result", "job_id": "fg_abc", "result": {...}}
{"type": "error", "job_id": "fg_abc", "error": "..."}

// Broker → Worker
{"type": "job", "job_id": "fg_abc", "type": "solve_lp", "payload": {...}}
{"type": "cancel", "job_id": "fg_abc"}
```

**Characteristics:**
- No new dependencies (SQLite still works)
- Workers can run anywhere with network access
- Single broker is coordination point
- Good for small-to-medium deployments

### Level 2: Horizontal Broker Scaling (Redis)

Replace SQLite with Redis for multiple stateless API servers:

```
                    Load Balancer
                          │
          ┌───────────────┼───────────────┐
          ▼               ▼               ▼
     ┌─────────┐     ┌─────────┐     ┌─────────┐
     │ Forge   │     │ Forge   │     │ Forge   │
     │ API 1   │     │ API 2   │     │ API 3   │
     └────┬────┘     └────┬────┘     └────┬────┘
          │               │               │
          └───────────────┼───────────────┘
                          ▼
                   ┌─────────────┐
                   │    Redis    │
                   │  (cluster)  │
                   └──────┬──────┘
                          │
          ┌───────────────┼───────────────┐
          ▼               ▼               ▼
     ┌─────────┐     ┌─────────┐     ┌─────────┐
     │ Worker  │     │ Worker  │     │ Worker  │
     │ Pool 1  │     │ Pool 2  │     │ Pool 3  │
     └─────────┘     └─────────┘     └─────────┘
```

**Redis Data Model:**

```redis
# Job queue (list per job type)
LPUSH   forge:queue:solve_lp  job_id
BRPOP   forge:queue:solve_lp  timeout

# Job state (hash per job)
HSET    forge:job:fg_abc  status running  progress 0.5  payload "..."
HGETALL forge:job:fg_abc

# Progress streaming (pub/sub)
PUBLISH   forge:progress:fg_abc  '{"progress": 0.5, "message": "..."}'
SUBSCRIBE forge:progress:fg_abc

# Worker registry (sorted set by heartbeat timestamp)
ZADD           forge:workers  timestamp  worker_id
ZRANGEBYSCORE  forge:workers  -inf  (now-30s)   # Find dead workers
```

**Characteristics:**
- Stateless API servers (horizontal scaling)
- Redis handles coordination and pub/sub
- Workers pull jobs directly from Redis
- Automatic failover with Redis Sentinel/Cluster

### Level 3: Kubernetes Native

Full cloud-native deployment with autoscaling:

```yaml
# Worker deployment with HPA based on queue depth
apiVersion: apps/v1
kind: Deployment
metadata:
  name: forge-worker-ralph
spec:
  replicas: 2
  template:
    spec:
      containers:
      - name: worker
        image: otto/forge-worker:latest
        args: ["--job-types", "solve_lp,solve_mip", "--broker", "redis://forge-redis:6379"]
        resources:
          requests: { cpu: "1", memory: "2Gi" }
          limits:   { cpu: "2", memory: "4Gi" }
---
apiVersion: autoscaling/v2
kind: HorizontalPodAutoscaler
metadata:
  name: forge-worker-ralph-hpa
spec:
  scaleTargetRef:
    kind: Deployment
    name: forge-worker-ralph
  minReplicas: 1
  maxReplicas: 20
  metrics:
  - type: External
    external:
      metric:
        name: redis_list_length
        selector:
          matchLabels:
            queue: forge:queue:solve_lp
      target:
        type: AverageValue
        averageValue: 5   # Scale up when >5 queued jobs per worker
```

### Scaling Comparison

| Aspect | Single Node | Remote Workers | Redis + K8s |
|--------|-------------|----------------|-------------|
| **Broker scaling** | 1 | 1 | Horizontal |
| **Worker scaling** | Local only | Manual | Autoscale |
| **Dependencies** | SQLite | SQLite | Redis |
| **Fault tolerance** | None | Worker restart | Full HA |
| **Complexity** | Low | Low | Medium |
| **Best for** | Dev/small | Medium | Production |

### Migration Path

The design supports incremental scaling:

1. **Start simple** - Single node with local workers (SQLite)
2. **Add remote workers** - Same broker, workers on other machines (WebSocket)
3. **Add Redis** - Swap storage backend, enable horizontal API scaling
4. **Add K8s** - Autoscaling, health checks, rolling updates

Each level is a superset of the previous. The consumer protocol (stdin/stdout) remains unchanged regardless of deployment model - workers don't care how they're scheduled.

---

## Implementation TODOs

**Phase 1: Core Infrastructure**
- [ ] Define job and consumer data structures
- [ ] Implement SQLite persistence layer
- [ ] Implement consumer registry (config file or directory convention)
- [ ] Implement job lifecycle (pending → running → completed/failed)

**Phase 2: Process Management**
- [ ] Implement process spawning (fork/exec on POSIX)
- [ ] Implement stdin/stdout/stderr pipe management
- [ ] Implement progress parsing from stderr
- [ ] Implement timeout handling and job cancellation
- [ ] Implement max_concurrent limits per consumer type

**Phase 3: REST API**
- [ ] Implement job submission endpoint
- [ ] Implement job status/result endpoints
- [ ] Implement job cancellation endpoint
- [ ] Implement WebSocket streaming for progress

**Phase 4: Built-in Consumers**
- [ ] Create fg-ralph consumer (LP/MIP solving)
- [ ] Create fg-fuelwise consumer (refueling optimization)
- [ ] Create fg-locus consumer (batch geocoding)
- [ ] Create fg-velo consumer (batch routing)

**Phase 5: Advanced Features**
- [ ] Implement webhook callbacks for job completion
- [ ] Implement job TTL and auto-cleanup
- [ ] Implement job priority queue
- [ ] Add comprehensive job statistics
