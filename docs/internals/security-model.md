# Security Model

## Transport-Agnostic, Capability-Minimal C Backend

### with Process-Isolated Parsing and Read-Only mmap Dataset Access

---

## 0. Prime Directive

> **Raw bytes are toxic.**
> Any component that receives attacker-controlled bytes (network payloads, JSON, MPS/LP, protobuf, websocket frames, etc.) is *untrusted* and must be isolated from:
>
> * filesystem access
> * process control
> * privilege escalation
> * valuable application handles
> * datasets in raw or complex formats

The system is designed so that **compromise collapses into a crash or a bounded computation**, not lateral movement.

---

## 1. Core Design Goal

* Support **multiple transports** (HTTP/Keel today; gRPC, WebSocket, UNIX socket tomorrow).
* Keep **transport logic disposable**.
* Keep **logic/compute reusable**, optionally packaged as **WASM**.
* Enforce **least privilege by construction**, not by convention.
* Allow **high-performance access to large OSM-derived datasets** without sacrificing safety.

---

## 2. Privilege Domains (Roles)

The system is composed of explicit roles.
Transport choice MUST NOT change these trust boundaries.

### Role A — Supervisor / Launcher (optional)

* Reads config
* Opens resources that must be opened once
* Drops privileges
* Spawns other roles
* Restarts crashed workers

Long-lived logic MUST NOT live here.

---

### Role B — Transport Frontend (pluggable)

Examples:

* HTTP (Keel or other)
* gRPC
* WebSocket
* UNIX domain socket

Responsibilities:

* Protocol framing
* Connection lifecycle
* TLS termination (optional)
* Transport-level limits (size, timeouts)

Must NOT:

* Parse complex payload formats
* Access datasets
* Access application handles
* Open arbitrary files
* Exec subprocesses

This role is **replaceable** without touching the rest of the system.

---

### Role P — Parser / Sanitizer (untrusted parsing)

Responsibilities:

* Parse JSON, MPS/LP, protobuf, etc.
* Validate semantics
* Normalize into a **small internal representation (IR)**

Must NOT:

* Access the network (except IPC in process-isolated mode)
* Access datasets
* Access secrets
* Perform compute that depends on application state

This role exists to **absorb parsing bugs safely**.

**Deployment modes** (from least to most isolated):

| Mode | How It Works | Use Case |
|------|--------------|----------|
| **In-process** | Parsing in worker threads via sh_worker_pool | Default, performance-critical |
| **WASM** | Parsing in WASM sandbox (Role W) | Browser demos, edge deployment |
| **Process pool** | Long-lived parser processes (like PHP-FPM) | High-security deployments |

**In-process mode (default):**
```
┌─────────────────────────────────────────────────────────┐
│                    Single Process                        │
│  ┌───────────┐   sh_workqueue   ┌───────────────────┐  │
│  │ Transport │ ───────────────► │   Worker Pool     │  │
│  │  (Role B) │                  │ (Roles P + C)     │  │
│  │           │ ◄─────────────── │ parse → compute   │  │
│  │   Keel    │   KlAsyncOp      │ KlThreadPool      │  │
│  └───────────┘                  └───────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

This is the current architecture. Security comes from:
- systemd hardening (filesystem, syscalls, capabilities)
- Input validation and size limits
- No exec/system/popen in library code

**Process pool mode (optional, high-security):**
```
┌───────────────┐     IPC      ┌───────────────┐
│  Transport    │ ──────────── │ Parser Pool   │  (long-lived processes)
│  + Compute    │   raw bytes  │ (Role P)      │
│  (Roles B+C)  │ ◄─────────── │               │
└───────────────┘      IR      └───────────────┘
```

Parser pool processes are LONG-LIVED (not per-request). They:
- Handle many requests sequentially
- Can be restarted periodically for safety
- Communicate via Unix socket, not fork-per-request

---

### Role C — Compute / Handlers ("handles live here")

Responsibilities:

* Business logic
* Solver orchestration
* DB handles / pools
* Application secrets

Rules:

* MUST NOT receive raw untrusted bytes
* MUST NOT parse JSON / MPS / LP
* MUST NOT accept internet-facing sockets

This is the **most valuable** role and must be the **most constrained**.

---

### Role D — Dataset / Index Service (trusted input, complex)

Responsibilities:

* Own `.osm.pbf` parsing (offline only)
* Own derived dataset/index formats
* Serve bounded queries over IPC

Rules:

* `.osm.pbf` MUST NOT be parsed online
* File access is read-only in steady state
* Network access is forbidden

---

### Role W — WASM Runner (demos and edge deployment)

In WASM mode, **Roles P and C are bundled together** in a single module. This is intentional:

* WASM itself IS the sandbox
* No process separation needed within WASM
* Single module simplifies deployment and demos

**WASM architecture:**
```
┌─────────────────────────────────────────────────────────┐
│                    WASM Module (Role W)                  │
│  ┌─────────────────────────────────────────────────┐   │
│  │              Bundled P + C                       │   │
│  │  JSON/LP parsing → validation → solver/compute  │   │
│  └─────────────────────────────────────────────────┘   │
│                         │                               │
│                    WASM boundary                        │
└─────────────────────────┼───────────────────────────────┘
                          │ minimal host API
                          ▼
┌─────────────────────────────────────────────────────────┐
│                    Host (Browser/Node)                   │
│  Transport, Dataset access (if needed)                  │
└─────────────────────────────────────────────────────────┘
```

WASM modules have **zero ambient authority**:
* No filesystem access
* No network access
* No process spawning
* Memory is bounds-checked
* Can enforce fuel/instruction limits

---

## 3. Security Boundaries by Deployment Mode

### In-process mode (default)

Security boundary is the **process + systemd hardening**:
- All roles (B, P, C) in single process
- Worker threads handle parsing and compute together
- systemd restricts filesystem, syscalls, capabilities
- No IPC needed between roles

```
┌────────────────────────────────────────┐
│            systemd sandbox             │
│  ┌──────────────────────────────────┐ │
│  │     Single Process (B + P + C)   │ │
│  │  Keel → KlThreadPool             │ │
│  └──────────────────────────────────┘ │
└────────────────────────────────────────┘
```

### WASM mode (demos, edge)

Security boundary is the **WASM sandbox**:
- All roles (P, C) bundled in single WASM module
- WASM provides memory isolation, no filesystem, no network
- Host handles transport (B)
- No IPC within WASM

### Process pool mode (optional, high-security)

Security boundary is **process isolation + IPC**:
- Parser runs in separate sandboxed processes
- Communication via Unix sockets with strict framing
- Only use when regulatory/security requirements demand it

**IPC requirements (process pool mode only):**

* `AF_UNIX` sockets (length-framed streams)
* Validation on both sides
* Enforced limits: max message size, timeouts, quotas

**Forbidden in all modes:**

* `system()`, `exec*()`, `popen()`, `dlopen()`
* Passing filesystem paths as commands
* Unbounded streaming

---

## 4. Parsing Policy (JSON, MPS/LP, etc.)

### Rule

**All complex parsing happens outside Role C.**

### Flow

1. B receives bytes → applies transport limits only
2. B forwards bytes to P (or W in WASM mode)
3. P parses + validates → produces IR
4. Only IR crosses into C

### Mandatory limits in P / W

* Max input size
* Max nesting depth
* Max tokens / constraints / variables
* Numeric range caps
* Parse time budget

---

## 5. OSM Dataset Policy

### Absolute rule

`.osm.pbf` parsing never happens in B, P, or C at runtime.

### Two supported patterns

#### Pattern A — Offline ingest (preferred)

* Separate batch job parses `.osm.pbf`
* Produces a **safe, derived index format**
* Online system never sees `.pbf`

**OTTO derived formats:**
| Module | Format | Purpose |
|--------|--------|---------|
| Velo | `.vlg` | Routing graph with CSR edges, landmarks |
| Carta | `.idx` | Ways, R-tree, multipolygons |
| Locus | `.lcx` | Trie, ngrams, spatial grid |

#### Pattern B — Online dataset service (Role D)

* D parses `.osm.pbf` or loads derived index
* D serves bounded queries over IPC
* C and W never open dataset files

---

## 6. High-Performance Escape Hatch: Read-Only mmap in C

### When it is allowed

If and only if:

* `.osm.pbf` parsing is **offline**
* The derived index format is:

  * deterministic
  * bounds-checked
  * safe to traverse without complex parsing
* The index is **read-only**

Then Role C **may** mmap the derived index directly.

### Security posture

* Filesystem access:

  * `unveil("/data/index", "r")` (OpenBSD)
  * `ReadOnlyPaths=/data/index` (systemd)
* Syscalls:

  * `pledge("stdio rpath unix", NULL)` (OpenBSD)
  * `SystemCallFilter=~@mount @reboot @swap` (systemd)
* No write access
* No `.pbf` files
* No dynamic file opens after init

### Why this exists

* Eliminates IPC overhead on the hottest path
* Preserves isolation from raw `.pbf`
* Keeps attack surface minimal

### WASM note

Pure WASM deployments **cannot mmap files**.
For WASM:

* Dataset access MUST go through Role D
* Queries MUST be batched and bounded

**Therefore:**

> Design IPC dataset APIs first.
> mmap is an optimization, not a dependency.

---

## 7. WASM Deployment Mode

WASM is the **primary sandbox for browser demos** and a deployment option for edge/embedded.

### WASM bundles P + C together

In WASM mode, parsing and compute are in the **same module**. This is correct because:

* WASM itself provides the isolation (no filesystem, network, exec)
* Single module = simpler deployment, faster loading
* The `*_api_handle()` interface works identically

**Transport-agnostic compatibility:**
```c
// Same handler interface works in all modes:
int vl_api_handle(VLAPIContext *ctx, const VLAPIRequest *req, VLAPIResponse *resp);

// Native HTTP: Keel calls this from a pool worker thread
// Native process-isolated: Parser process calls this after parsing
// WASM: JavaScript wrapper calls this directly
```

### What's in the WASM module

* Parsing (JSON, MPS/LP)
* Validation
* Business logic
* Solver formulation
* Response generation
* Embedded dataset (e.g., Monaco PBF for demos)

### What stays in the host

* Transport (fetch, WebSocket)
* Large dataset access (if not embedded)
* UI rendering

### Host ↔ WASM API

The existing pattern:

```javascript
// site/js/handlers/velo.js
const result = await veloDemo.route(from, to, options);
// Calls vl_api_handle() inside WASM
```

All calls implicitly enforce:

* max input size (WASM memory limits)
* wall-clock timeout (browser/host enforced)

WASM gets **no filesystem, no network, no exec** by construction.

---

## 8. Hardening

### Primary: Linux (systemd)

Use systemd as the primary enforcement layer.

**Minimum directives for all services:**
```ini
[Service]
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
RestrictSUIDSGID=true
CapabilityBoundingSet=
```

**Role-specific additions:**

| Role | RestrictAddressFamilies | ReadOnlyPaths | Notes |
|------|-------------------------|---------------|-------|
| B (Transport) | AF_INET AF_INET6 AF_UNIX | - | Network access |
| P (Parser) | AF_UNIX | - | IPC only |
| C (Compute) | AF_UNIX | /data/index | IPC + mmap index |
| D (Dataset) | AF_UNIX | /data | IPC + read-only data |

**Syscall filtering:**
```ini
SystemCallFilter=@system-service
SystemCallFilter=~@mount @reboot @swap @clock @cpu-emulation @debug @module @obsolete @raw-io
```

### Secondary: OpenBSD (reference)

For OpenBSD deployments, use pledge/unveil:

**Steady-state profiles:**

| Role | pledge | unveil |
|------|--------|--------|
| B (Transport) | `stdio inet unix` | none |
| P (Parser) | `stdio unix` | none |
| C (Compute) | `stdio rpath unix` | `/data/index:r` then lock |
| D (Dataset) | `stdio rpath unix` | `/data:r` then lock |

**Pattern:**
```c
// During init
unveil("/data/index", "r");
unveil(NULL, NULL);  // Lock

// Before main loop
pledge("stdio rpath unix", NULL);
```

---

## 9. Transport Independence

Any transport MUST:

* Act only as a framer + limiter
* Emit a canonical internal envelope
* Forward opaque payloads into the same pipeline

No transport-specific logic in P, C, D, or W.

---

## 10. Non-Negotiables

* No `system()`, `exec*()`, `popen()`
* No runtime plugin loading
* No broad filesystem access
* No raw bytes in Role C
* No `.osm.pbf` parsing at runtime

---

## 11. Mental Model (for implementers)

* **B**: "I touch the internet and nothing else."
* **P**: "I parse dangerous stuff but own nothing."
* **C**: "I own value but see only clean IR."
* **D**: "I touch big data but never the network."
* **mmap**: "Read-only speed, not a privilege leak."

---

## 12. Success Criteria

If any single component is compromised:

* Attacker cannot read arbitrary files
* Cannot write to disk
* Cannot exec or fork
* Cannot pivot to other roles
* At worst: crashes the process or burns its budget

---

## 13. Implementation Status

### Deployment Modes

| Mode | Status | Notes |
|------|--------|-------|
| **In-process** (default) | ✅ | sh_worker_pool, sh_workqueue, sh_httpserver |
| **WASM** (demos) | ✅ | Single bundled module with embedded data |
| **Process pool** (high-security) | 🔲 Optional | Only if required by deployment |

### Currently Implemented

| Principle | Status | Notes |
|-----------|--------|-------|
| Transport-agnostic handlers | ✅ | `*_api_handle()` pattern |
| WASM sandboxing (Role W) | ✅ | Browser demos, P+C bundled |
| Derived index formats | ✅ | .vlg, .lcx, .idx |
| Read-only mmap of indexes | ✅ | Native mode |
| Rate limiting | ✅ | `sh_ratelimit.h` |
| Work queue | ✅ | `sh_workqueue.h` |
| Worker pool | ✅ | `sh_worker_pool.h` |
| Completion signaling | ✅ | `sh_completion.h` |

### Planned

| Principle | Status | Tracking |
|-----------|--------|----------|
| systemd hardening units | 🔲 Planned | `docs/roadmaps/security.md` Phase 1 |
| c-audit security warnings | ✅ Done | Warnings only |
| Process pool mode | 🔲 Optional | `docs/roadmaps/security.md` Phase 3 |

---

## See Also

* [MANIFESTO.md](../MANIFESTO.md) - Design philosophy
* [ARCHITECTURE.md](../ARCHITECTURE.md) - System architecture
* [c-audit skill](../../.claude/skills/c-audit/SKILL.md) - Security checks in audits
