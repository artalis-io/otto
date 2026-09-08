# Security Roadmap

Implementation plan for the [security model](../internals/security-model.md).

## Overview

OTTO supports multiple deployment modes with different security/performance tradeoffs:

| Mode | Architecture | Security | Performance | Use Case |
|------|--------------|----------|-------------|----------|
| **In-process** | sh_worker_pool threads | systemd hardening | Best | Default, most deployments |
| **WASM** | Single bundled module | WASM sandbox | Good | Demos, edge, browser |
| **Process pool** | Long-lived parser processes | Process isolation | Lower | High-security deployments |

**Default architecture (in-process with sh_worker_pool):**
```
┌─────────────────────────────────────────────────────────────────┐
│                       Single Process                             │
│                                                                  │
│  ┌───────────────┐   sh_workqueue   ┌─────────────────────┐    │
│  │   Transport   │ ───────────────► │    Worker Pool      │    │
│  │   (Role B)    │                  │   (Roles P + C)     │    │
│  │               │ ◄─────────────── │                     │    │
│  │     Keel      │   sh_completion  │  parse → compute    │    │
│  │  rate limit   │                  │  sh_worker_pool     │    │
│  └───────────────┘                  └─────────────────────┘    │
│                                              │                  │
│                                              │ mmap             │
│                                              ▼                  │
│                                     ┌─────────────────┐        │
│                                     │ Dataset (D)     │        │
│                                     │ .vlg, .lcx      │        │
│                                     │ read-only       │        │
│                                     └─────────────────┘        │
└─────────────────────────────────────────────────────────────────┘
```

This is the **current architecture** and works with existing `sh_workqueue`, `sh_worker_pool`, and `sh_httpserver`.

**WASM architecture (demos and edge):**
```
┌─────────────────────────────────────────────────────────────────┐
│                    WASM Module (Role W)                          │
│                                                                  │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │              Bundled P + C + embedded data               │   │
│  │  JSON/LP parsing → validation → solver/compute          │   │
│  │  vl_api_handle(), ct_api_handle(), etc.                 │   │
│  └─────────────────────────────────────────────────────────┘   │
│                              │                                  │
└──────────────────────────────┼──────────────────────────────────┘
                               │ WASM boundary (sandbox)
                               ▼
┌─────────────────────────────────────────────────────────────────┐
│                    Host (Browser/Node)                           │
│  Transport, UI, large dataset access                            │
└─────────────────────────────────────────────────────────────────┘
```

This is the **demo architecture** — everything bundled in one WASM module. The WASM sandbox IS the security boundary.

---

## Phase 1: systemd Hardening (Default Mode)

**Status:** ✅ Complete

**Goal:** Harden the existing in-process architecture with systemd directives.

### 1.1 Base Unit Template

Created `docker/systemd/otto-base.conf`:

```ini
[Service]
# Security baseline
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
PrivateDevices=true
ProtectKernelTunables=true
ProtectKernelModules=true
ProtectControlGroups=true
RestrictSUIDSGID=true
CapabilityBoundingSet=

# Syscall filtering
SystemCallFilter=@system-service
SystemCallFilter=~@mount @reboot @swap @clock @cpu-emulation @debug @module @obsolete @raw-io

# Resource limits
MemoryMax=2G
TasksMax=64
```

### 1.2 Per-Service Units

**carta-tile-server.service:**
```ini
[Service]
# Network access for HTTP
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX

# Read-only access to data
ReadOnlyPaths=/data/carta

# Include base hardening
.include /etc/systemd/system/otto-base.conf
```

Service files created for all 5 APIs:
- `carta-tile-server.service`
- `velo-route-server.service`
- `locus-geocoder.service`
- `fuelwise-api.service`
- `ralph-solver.service`

**Container hardening also complete:**
- Distroless base images (`gcr.io/distroless/cc-debian12:nonroot`)
- Non-root user (uid 65532)
- Read-only root filesystem
- All capabilities dropped
- No-new-privileges enabled

---

## Phase 2: Audit Integration

**Goal:** `/c-audit` warns about security model violations.

### 2.1 New Warning Checks

Add to `c-audit` skill:

| Check | Severity | Pattern |
|-------|----------|---------|
| `system()` call | Warning | `system\s*\(` |
| `exec*()` call | Warning | `exec[lv]p?\s*\(` |
| `popen()` call | Warning | `popen\s*\(` |
| `dlopen()` call | Warning | `dlopen\s*\(` |
| Static mutable state in library | Warning | `static` non-const in lib code |

### 2.2 Audit Report Section

Add "Security Model Compliance" section to audit output.

**Status:** ✅ Implemented (warnings only)

---

## Phase 3: Process Pool Mode (Optional, High-Security)

**Goal:** For deployments requiring process isolation, provide a parser process pool.

This is OPTIONAL and NOT required for normal deployments. The in-process mode with systemd hardening is sufficient for most cases.

### 3.1 When to use process pool mode

- Regulatory requirements mandate process isolation
- Parsing untrusted input from external sources (not just API clients)
- Defense-in-depth for critical infrastructure

### 3.2 Architecture

```
┌───────────────────┐     Unix socket    ┌───────────────────┐
│  Main Process     │ ◄────────────────► │  Parser Pool      │
│  (Transport + C)  │    framed msgs     │  (long-lived)     │
│                   │                    │                   │
│  sh_worker_pool   │                    │  N parser procs   │
│  sh_workqueue     │                    │  seccomp sandbox  │
└───────────────────┘                    └───────────────────┘
```

Parser pool processes are **long-lived** (not per-request):
- Handle many requests sequentially
- Restart periodically for safety
- Communicate via length-framed Unix socket

### 3.3 IPC Framework (`shared/include/sh_ipc.h`)

```c
// Framed message send/receive
int sh_ipc_send(int fd, const void *data, size_t len);
int sh_ipc_recv(int fd, void *buf, size_t max_len, size_t *out_len);

// Process pool management
typedef struct ShParserPool ShParserPool;
ShParserPool *sh_parser_pool_create(const char *exe, int num_workers);
int sh_parser_pool_parse(ShParserPool *pool, const void *in, size_t in_len,
                         void *out, size_t out_max, size_t *out_len);
void sh_parser_pool_free(ShParserPool *pool);
```

**Estimate:** 2-3 weeks (if needed)

---

## Implementation Order

1. **Phase 1** - systemd hardening ✅ Complete
2. **Phase 2** - Audit integration ✅ Complete
3. **Phase 3** - Process pool (only if required by deployment)

---

## Compatibility Matrix

| Component | In-process | WASM | Process Pool |
|-----------|------------|------|--------------|
| sh_workqueue | ✅ | N/A | ✅ |
| sh_worker_pool | ✅ | N/A | ✅ (main process) |
| sh_httpserver | ✅ | N/A | ✅ |
| sh_completion | ✅ | N/A | ✅ |
| *_api_handle() | ✅ | ✅ | ✅ |
| Embedded data | ✅ | ✅ | ✅ |
| mmap indexes | ✅ | ❌ | ✅ |

The transport-agnostic `*_api_handle()` interface works identically in all modes.

---

## See Also

- [security-model.md](../internals/security-model.md) - Full security architecture
- [infrastructure.md](infrastructure.md) - Deployment infrastructure
- [c-audit skill](../../.claude/skills/c-audit/SKILL.md) - Code auditing
