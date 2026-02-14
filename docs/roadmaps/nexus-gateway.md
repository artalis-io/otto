# Nexus Gateway - External System Integration (Future)

This document describes the planned TMS/ELD integration gateway. This is **not yet implemented** - see `nexus.md` for the current document ingestion pipeline.

---

**Nexus Gateway** (**N**ormalized **Ex**ternal **U**nified **S**napshots) will be OTTO's data ingress layer for integrating with external systems like TMS, ELD, and load boards.

## Design Philosophy

**OTTO is a computation engine, not a system of record.**

- TMS remains the source of truth for assignments, drivers, loads
- ELD remains the source of truth for HOS and location
- OTTO receives snapshots, computes optimizations, returns recommendations
- TMS decides what to accept and persists decisions

### What OTTO Does NOT Store

| Never Store | Why |
|-------------|-----|
| Driver records | TMS is system of record |
| Load/shipment history | TMS is system of record |
| Assignment decisions | TMS is system of record |
| HOS logs | ELD is system of record |
| Customer data | TMS is system of record |

### What OTTO Can Cache

| OK to Cache | Refresh Strategy |
|-------------|------------------|
| Road network (Velo) | Weekly or on OSM update |
| Fuel station locations | Daily |
| Facility operating hours | Daily |
| Historical travel times | Rolling 30-day |
| Fuel price snapshots | Hourly |

## Architecture

```
External Systems (Source of Truth)
  TMS (tasks, assignments)
  ELD (HOS, locations)
  Load Boards (spot, contract)
  Fuel Price APIs (OPIS, etc.)
       |
       v
Nexus Gateway
  TMS Adapter | ELD Adapter | LoadBoard Adapter | FuelPrice Adapter
       |
       v
  Canonical Planning Request
  (drivers, trucks, loads, constraints)
       |
       v
OTTO Optimization Core
  HoSE | Tempo | Velo | FuelWise | Sigma | Ralph
       |
       v
Planning Response
  - Recommended assignments (load -> driver)
  - Optimized routes with ETAs
  - Fuel stop recommendations
  - HOS-compliant schedules with breaks
  - Confidence scores / alternatives
       |
       v
TMS accepts/rejects/modifies
TMS persists decisions
```

## Adapter Interface

Each adapter implements a standard interface for translating external formats:

```c
typedef struct {
    const char *name;
    const char *version;
    int (*parse_drivers)(const char *json, NxDriver **drivers, int *count);
    int (*parse_loads)(const char *json, NxLoad **loads, int *count);
    int (*parse_vehicles)(const char *json, NxVehicle **vehicles, int *count);
    char* (*format_assignments)(const NxAssignment *assignments, int count);
    int (*handle_webhook)(const char *event_type, const char *payload);
} NxAdapter;
```

## Dependencies

| Nexus Gateway Uses | For |
|--------------------|-----|
| HoSE | HOS feasibility checking |
| Velo | Route calculations, ETA |
| FuelWise | Fuel stop optimization |
| Tempo | Time window validation |
| Sigma | Assignment optimization |
| Ralph | Core optimization (via Sigma) |

## Implementation Priority

1. Core structures - NxDriver, NxLoad, NxVehicle, NxPlanningRequest/Response
2. Validation - Input validation, constraint checking
3. Generic adapter - JSON-based generic format
4. REST API - Basic /plan endpoint
5. TMS adapters - Start with most common (McLeod, Trimble)
6. ELD adapters - Samsara, Motive
