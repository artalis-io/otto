# OTTO Roadmaps

This directory contains development roadmaps and specifications for OTTO components.

## Core Solvers

| Module | Description | Status |
|--------|-------------|--------|
| [ralph.md](ralph.md) | LP/MIP solver - revised simplex, branch & bound | In Progress |
| [surge.md](surge.md) | Rich VRP/PDPTW solver - ALNS, population search | **Production-ready** |
| [arbor.md](arbor.md) | State-space search + ALNS metaheuristics framework | Done |

## Domain Services

| Module | Description | Status |
|--------|-------------|--------|
| [velo.md](velo.md) | Routing engine - continental scale, CCH | In Progress |
| [carta.md](carta.md) | Map tile generator - labels, styling | In Progress |
| [locus.md](locus.md) | Geocoding - performance, fuzzy search | In Progress |
| [nexus.md](nexus.md) | Document ingestion pipeline (XLSX/PDF/CSV) | Done |

## Planned Components

### Domain Engines

| Component | Description | Status |
|-----------|-------------|--------|
| [hose.md](hose.md) | **H**ours **o**f **S**ervice **E**ngine - HoS compliance | Planned |
| [tempo.md](tempo.md) | Business rules engine | Planned |
| [sigma.md](sigma.md) | Fleet plan selection engine | Planned |
| [pulse.md](pulse.md) | Execution tracker and PTA engine | Planned |
| [quota.md](quota.md) | Rate quoting engine | Planned |
| [atlas.md](atlas.md) | Network design engine | Planned |

### Infrastructure

| Component | Description | Status |
|-----------|-------------|--------|
| [forge.md](forge.md) | Async job queue | Planned |
| [infrastructure.md](infrastructure.md) | API server and observability | Planned |
| [security.md](security.md) | Role separation, process isolation, hardening | Planned |

**HTTP server — Keel v3 migration: ✅ Complete.** All six API servers (Surge,
Ralph, FuelWise, Velo, Carta, Locus) run on Keel v3 (MIT, `vendor/keel`),
replacing the previous GPL-licensed HTTP server. The shared helper layer is
`sh_httpserver.{c,h}` + `sh_httpasync.{c,h}` (public helpers `sh_http_*`), renamed
from `sh_keelserver`/`sh_keelasync` after the migration. See
[infrastructure.md](infrastructure.md) for the cross-cutting completion record and
each module roadmap's "Keel Migration" section for per-server detail.

### Cross-Cutting

| Document | Description |
|----------|-------------|
| [cross-cutting.md](cross-cutting.md) | Distance estimation, cost calculations, FuelWise integration |
| [fuel-demo.md](fuel-demo.md) | Browser-based fuel optimizer demo |
| [site.md](site.md) | Landing page, API docs |

## Surge Benchmark Summary (Mar 2026)

Surge is the most mature solver in the platform. Current benchmark results:

| Scale | Vehicle Match | Distance Gap | Notes |
|-------|---------------|--------------|-------|
| 100 (Solomon) | 80% | -0.1% | Competitive with published ALNS |
| 200 (GH-200) | **92%** | +13.1% | Near-BKS vehicle minimization |
| 400 (GH-400, 60s) | 45% | +33.3% | Time-starved at 60s |
| 400 (GH-400, 300s) | — | **+3.0%** (c1_4_1) | Near-BKS with adequate budget |

Rich constraint support: PDPTW, DARP, compartments, breaks, multi-trip, locking,
backhaul, LIFO/FIFO, precedence, setup times, speed profiles. 403 tests, ASAN clean.

## Implementation Priority

```
Phase 1 (Foundation):
  HoSE → Tempo

Phase 2 (Planning):
  Sigma (uses HoSE, Tempo, Arbor, Surge)

Phase 3 (Execution):
  Pulse (uses Sigma outputs)

Phase 4 (Network):
  Quota, Atlas (strategic layer)

Infrastructure (Parallel):
  Forge (can proceed independently)

Surge (Active):
  Tuning campaign → per-cell optimization → large-scale validation
```

## See Also

- [../ARCHITECTURE.md](../ARCHITECTURE.md) - System architecture
- [../MANIFESTO.md](../MANIFESTO.md) - Design philosophy
- [../business/](../business/) - Business strategy and valuation
