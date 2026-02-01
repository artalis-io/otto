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
#include "ralph.h"
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
- **mongoose**: HTTP/WebSocket server (already vendored)
- **pthreads**: Thread pool for dispatcher (standard POSIX)

No external services required. Everything runs in a single process with SQLite for persistence.
