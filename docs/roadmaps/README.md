# OTTO Roadmaps

This directory contains development roadmaps and specifications for planned components.

## Existing Module Enhancements

| Module | Description | Status |
|--------|-------------|--------|
| [ralph.md](ralph.md) | LP/MIP solver - performance, numerical stability | In Progress |
| [carta.md](carta.md) | Map tile generator - labels, styling | In Progress |
| [velo.md](velo.md) | Routing engine - continental scale, CCH | In Progress |
| [locus.md](locus.md) | Geocoding - performance, fuzzy search | In Progress |

## Planned Components

### Domain Engines

| Component | Description | Status |
|-----------|-------------|--------|
| [hose.md](hose.md) | **H**ours **o**f **S**ervice **E**ngine - HoS compliance | Planned |
| [tempo.md](tempo.md) | Business rules engine | Planned |
| [arbor.md](arbor.md) | State-space search engine | Planned |
| [sigma.md](sigma.md) | Fleet plan selection engine | Planned |
| [pulse.md](pulse.md) | Execution tracker and PTA engine | Planned |
| [quota.md](quota.md) | Rate quoting engine | Planned |
| [atlas.md](atlas.md) | Network design engine | Planned |

### Infrastructure

| Component | Description | Status |
|-----------|-------------|--------|
| [forge.md](forge.md) | Async job queue | Planned |
| [nexus.md](nexus.md) | External data integration gateway | Planned |
| [infrastructure.md](infrastructure.md) | API server and observability | Planned |
| [security.md](security.md) | Role separation, process isolation, hardening | Planned |

### Cross-Cutting

| Document | Description |
|----------|-------------|
| [cross-cutting.md](cross-cutting.md) | Distance estimation, cost calculations, FuelWise integration |
| [fuel-demo.md](fuel-demo.md) | Browser-based fuel optimizer demo |

## Implementation Priority

```
Phase 1 (Foundation):
  HoSE → Tempo → Arbor

Phase 2 (Planning):
  Sigma (uses HoSE, Tempo, Arbor)

Phase 3 (Execution):
  Pulse (uses Sigma outputs)

Phase 4 (Network):
  Quota, Atlas (strategic layer)

Infrastructure (Parallel):
  Forge, Nexus (can proceed independently)

Security (Near-term):
  Process-isolated parsing → IPC framework → systemd hardening
```

## See Also

- [../ARCHITECTURE.md](../ARCHITECTURE.md) - System architecture
- [../MANIFESTO.md](../MANIFESTO.md) - Design philosophy
- [../business/](../business/) - Business strategy and valuation
