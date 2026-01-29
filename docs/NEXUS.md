# Nexus - External System Integration Gateway

**Nexus** (**N**ormalized **Ex**ternal **U**nified **S**napshots) is OTTO's data ingress layer for integrating with external systems like TMS, ELD, and load boards.

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
┌─────────────────────────────────────────────────────────────────┐
│                    External Systems (Source of Truth)            │
├─────────────┬─────────────┬─────────────┬──────────────────────┤
│     TMS     │     ELD     │ Load Boards │   Fuel Price APIs    │
│  (tasks,    │  (HOS,      │  (spot,     │   (OPIS, etc.)       │
│  assignments)│  locations) │  contract)  │                      │
└──────┬──────┴──────┬──────┴──────┬──────┴──────────┬───────────┘
       │             │             │                  │
       ▼             ▼             ▼                  ▼
┌─────────────────────────────────────────────────────────────────┐
│                         Nexus Gateway                            │
│  ┌───────────┐ ┌───────────┐ ┌───────────┐ ┌───────────┐       │
│  │ TMS       │ │ ELD       │ │ LoadBoard │ │ FuelPrice │       │
│  │ Adapter   │ │ Adapter   │ │ Adapter   │ │ Adapter   │       │
│  └─────┬─────┘ └─────┬─────┘ └─────┬─────┘ └─────┬─────┘       │
│        └─────────────┴─────────────┴─────────────┘              │
│                         ▼                                        │
│              Canonical Planning Request                          │
│              (drivers, trucks, loads, constraints)               │
└─────────────────────────────┬───────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    OTTO Optimization Core                        │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐        │
│  │ HoSE   │ │ Tempo  │ │ Velo   │ │FuelWise│ │ Sigma  │        │
│  │ (HOS)  │ │(TimeWin)│ │(Route) │ │ (Fuel) │ │(Assign)│        │
│  └────────┘ └────────┘ └────────┘ └────────┘ └────────┘        │
│                         │                                        │
│              ┌──────────┴──────────┐                            │
│              │ Ralph (LP/MIP/LAP)  │                            │
│              └─────────────────────┘                            │
└─────────────────────────────┬───────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    Planning Response                             │
│  • Recommended assignments (load → driver)                       │
│  • Optimized routes with ETAs                                    │
│  • Fuel stop recommendations                                     │
│  • HOS-compliant schedules with breaks                          │
│  • Confidence scores / alternatives                              │
└─────────────────────────────┬───────────────────────────────────┘
                              │
                              ▼
                    TMS accepts/rejects/modifies
                    TMS persists decisions
```

## Module Structure

```
nexus/
├── include/
│   ├── nexus.h               # Main API: NxPlanningRequest, NxPlanningResponse
│   ├── nx_driver.h           # Driver/HOS state structures
│   ├── nx_load.h             # Load/shipment structures
│   ├── nx_vehicle.h          # Vehicle/equipment structures
│   ├── nx_facility.h         # Facility/location structures
│   └── nx_constraint.h       # Business rules/constraints
├── src/
│   ├── nx_request.c          # Request building and validation
│   ├── nx_response.c         # Response formatting
│   ├── nx_validate.c         # Input validation
│   └── nx_normalize.c        # Unit conversion, canonicalization
├── adapters/                 # Format-specific adapters
│   ├── tms/
│   │   ├── mcleod.c          # McLeod TMS format
│   │   ├── trimble.c         # Trimble TMS format
│   │   ├── mercurygate.c     # MercuryGate format
│   │   └── generic_edi.c     # EDI 204/214/990 formats
│   ├── eld/
│   │   ├── samsara.c         # Samsara ELD API
│   │   ├── motive.c          # Motive (KeepTruckin) API
│   │   ├── omnitracs.c       # Omnitracs API
│   │   └── generic_aobrd.c   # Generic AOBRD format
│   └── loadboard/
│       ├── dat.c             # DAT load board
│       ├── truckstop.c       # Truckstop.com
│       └── uber_freight.c    # Uber Freight API
├── api/
│   └── nexus-api/            # REST API server
│       ├── main.c
│       └── handlers.c
└── tests/
    ├── test_nexus.c
    └── test_adapters.c
```

## Core Data Structures

### Driver State (from ELD/TMS)

```c
/* nx_driver.h */
typedef struct {
    char id[64];                  /* External ID (from TMS) */
    char name[128];

    /* Current location (from ELD) */
    double lat, lon;
    time_t location_time;

    /* HOS clocks (from ELD) - current state snapshot */
    int drive_remaining_min;      /* Minutes left on 11h drive clock */
    int shift_remaining_min;      /* Minutes left on 14h shift clock */
    int cycle_remaining_min;      /* Minutes left on 70h/8day cycle */
    int break_required_min;       /* Minutes until 30-min break needed */
    time_t shift_start;           /* When current shift started */
    NxDutyStatus duty_status;     /* OFF_DUTY, SLEEPER, DRIVING, ON_DUTY */

    /* Qualifications */
    int num_endorsements;
    char endorsements[16][8];     /* HAZMAT, TANKER, DOUBLES, etc. */

    /* Home base (for deadhead calculations) */
    double home_lat, home_lon;
    int max_days_out;             /* Driver preference */

    /* Current assignment (from TMS) - NULL if available */
    char current_load_id[64];
    char current_vehicle_id[64];
} NxDriver;

typedef enum {
    NX_DUTY_OFF_DUTY,
    NX_DUTY_SLEEPER_BERTH,
    NX_DUTY_DRIVING,
    NX_DUTY_ON_DUTY_NOT_DRIVING
} NxDutyStatus;
```

### Load (from TMS/LoadBoard)

```c
/* nx_load.h */
typedef struct {
    char id[64];                  /* External ID */
    NxLoadSource source;          /* TMS, SPOT, CONTRACT */

    /* Origin */
    double origin_lat, origin_lon;
    char origin_facility_id[64];
    char origin_name[128];
    time_t pickup_earliest;       /* Appointment window */
    time_t pickup_latest;
    int pickup_duration_min;      /* Expected dwell time */

    /* Destination */
    double dest_lat, dest_lon;
    char dest_facility_id[64];
    char dest_name[128];
    time_t delivery_earliest;
    time_t delivery_latest;
    int delivery_duration_min;

    /* Requirements */
    double weight_lbs;
    int length_ft;
    NxEquipmentType equipment;    /* VAN, REEFER, FLATBED, etc. */
    int temp_min_f, temp_max_f;   /* For reefer */
    int requires_hazmat;
    int requires_tanker;
    int requires_twic;
    int team_required;            /* Needs team drivers */

    /* Economics */
    double revenue;               /* Total line haul */
    double fuel_surcharge;
    double accessorial_estimate;
    double deadhead_allowance;    /* Max acceptable deadhead */

    /* Status (from TMS) */
    NxLoadStatus status;
    char assigned_driver_id[64];  /* If already assigned */
    char assigned_vehicle_id[64];
} NxLoad;

typedef enum {
    NX_LOAD_SOURCE_TMS,           /* From carrier's TMS */
    NX_LOAD_SOURCE_SPOT,          /* Spot market */
    NX_LOAD_SOURCE_CONTRACT       /* Contract/dedicated */
} NxLoadSource;

typedef enum {
    NX_LOAD_AVAILABLE,            /* Open for assignment */
    NX_LOAD_ASSIGNED,             /* Assigned but not started */
    NX_LOAD_IN_TRANSIT,           /* Currently being executed */
    NX_LOAD_DELIVERED             /* Completed */
} NxLoadStatus;

typedef enum {
    NX_EQUIP_VAN,
    NX_EQUIP_REEFER,
    NX_EQUIP_FLATBED,
    NX_EQUIP_STEP_DECK,
    NX_EQUIP_TANKER,
    NX_EQUIP_HOPPER,
    NX_EQUIP_LOWBOY
} NxEquipmentType;
```

### Vehicle (from TMS)

```c
/* nx_vehicle.h */
typedef struct {
    char id[64];
    char unit_number[32];

    NxEquipmentType type;
    int length_ft;
    double max_weight_lbs;

    /* For reefer */
    int has_reefer;
    int reefer_operational;

    /* Current state */
    double current_lat, current_lon;
    time_t location_time;
    double fuel_level_gallons;
    double tank_capacity_gallons;
    double mpg_estimate;

    /* Maintenance */
    time_t next_service_due;
    int miles_to_service;

    /* Assignment */
    char assigned_driver_id[64];
} NxVehicle;
```

### Planning Request/Response

```c
/* nexus.h */
typedef struct {
    /* Snapshot metadata */
    time_t snapshot_time;
    char request_id[64];          /* For tracking/correlation */

    /* Available resources */
    int num_drivers;
    NxDriver *drivers;

    int num_vehicles;
    NxVehicle *vehicles;

    /* Loads to plan */
    int num_loads;
    NxLoad *loads;

    /* Facilities (for dwell times, hours) */
    int num_facilities;
    NxFacility *facilities;

    /* Business constraints */
    NxConstraints constraints;

    /* Optimization preferences */
    NxObjective objective;        /* REVENUE, UTILIZATION, SERVICE */
    int max_planning_horizon_hrs;
    double max_deadhead_miles;
    int include_spot_market;      /* Consider spot loads? */
    int max_solutions;            /* For k-best alternatives */
} NxPlanningRequest;

typedef struct {
    char request_id[64];          /* Correlation with request */
    time_t computed_at;
    int computation_time_ms;

    /* Recommended assignments */
    int num_assignments;
    NxAssignment *assignments;

    /* Unassigned loads with reasons */
    int num_unassigned;
    NxUnassigned *unassigned;

    /* Fleet-level metrics */
    double total_revenue;
    double total_miles;
    double total_deadhead_miles;
    double total_fuel_cost;
    double utilization_pct;       /* % of available hours used */
} NxPlanningResponse;

typedef struct {
    char load_id[64];
    char driver_id[64];
    char vehicle_id[64];

    double score;                 /* Quality/confidence 0-100 */
    int rank;                     /* 1 = best option for this load */

    /* Predicted execution */
    time_t depart_time;
    time_t eta_pickup;
    time_t eta_delivery;

    /* Distances */
    double deadhead_miles;        /* Empty miles to pickup */
    double loaded_miles;          /* Pickup to delivery */
    double total_miles;

    /* Economics */
    double revenue;
    double fuel_cost;
    double cost_per_mile;
    double profit_estimate;

    /* HOS analysis */
    int hos_feasible;
    int requires_break;
    int requires_reset;
    NxBreakPlan break_plan;       /* Where/when to take breaks */

    /* Fuel plan */
    int num_fuel_stops;
    NxFuelStop *fuel_stops;

    /* Route summary */
    int num_route_points;
    NxRoutePoint *route;          /* Key waypoints */
} NxAssignment;

typedef struct {
    char load_id[64];
    NxUnassignedReason reason;
    char reason_detail[256];

    /* Nearest feasible option (if any) */
    char nearest_driver_id[64];
    double nearest_driver_miles;
    char infeasibility[256];      /* Why nearest can't do it */
} NxUnassigned;

typedef enum {
    NX_UNASSIGNED_NO_CAPACITY,        /* No drivers available */
    NX_UNASSIGNED_NO_EQUIPMENT,       /* No matching equipment */
    NX_UNASSIGNED_HOS_INFEASIBLE,     /* Can't make it legally */
    NX_UNASSIGNED_OUTSIDE_NETWORK,    /* Too far from any driver */
    NX_UNASSIGNED_TIME_WINDOW,        /* Can't meet appointment */
    NX_UNASSIGNED_QUALIFICATION,      /* No qualified driver */
    NX_UNASSIGNED_UNPROFITABLE        /* Below minimum revenue */
} NxUnassignedReason;
```

## REST API

### Endpoints

```
POST /api/v1/plan
  Full planning request with complete snapshot
  Request: NxPlanningRequest
  Response: NxPlanningResponse

POST /api/v1/plan/incremental
  Update existing plan with changes
  Request: { base_request_id, changes: [...] }
  Response: NxPlanningResponse

POST /api/v1/validate
  Check feasibility of a specific assignment
  Request: { driver_id, load_id, vehicle_id }
  Response: { feasible, issues: [...] }

POST /api/v1/simulate
  Simulate execution of an assignment
  Request: NxAssignment
  Response: { timeline: [...], risks: [...] }

POST /api/v1/eta
  Get ETA for a driver to a location
  Request: { driver_id, dest_lat, dest_lon }
  Response: { eta, route_miles, hos_feasible }

GET /api/v1/health
  Health check
  Response: { status: "ok", version: "..." }
```

### Example Request

```json
POST /api/v1/plan
{
  "snapshot_time": "2024-01-29T14:00:00Z",
  "request_id": "plan-123",

  "drivers": [
    {
      "id": "DRV-001",
      "name": "John Smith",
      "lat": 41.8781,
      "lon": -87.6298,
      "location_time": "2024-01-29T13:55:00Z",
      "drive_remaining_min": 540,
      "shift_remaining_min": 720,
      "cycle_remaining_min": 3600,
      "duty_status": "ON_DUTY_NOT_DRIVING",
      "endorsements": ["HAZMAT", "TANKER"],
      "current_load_id": null
    }
  ],

  "loads": [
    {
      "id": "LOAD-456",
      "source": "CONTRACT",
      "origin_lat": 41.8819,
      "origin_lon": -87.6278,
      "pickup_earliest": "2024-01-29T16:00:00Z",
      "pickup_latest": "2024-01-29T20:00:00Z",
      "dest_lat": 39.7392,
      "dest_lon": -104.9903,
      "delivery_earliest": "2024-01-30T08:00:00Z",
      "delivery_latest": "2024-01-30T16:00:00Z",
      "weight_lbs": 42000,
      "equipment": "VAN",
      "revenue": 2850.00
    }
  ],

  "objective": "REVENUE",
  "max_deadhead_miles": 150
}
```

### Example Response

```json
{
  "request_id": "plan-123",
  "computed_at": "2024-01-29T14:00:05Z",
  "computation_time_ms": 127,

  "assignments": [
    {
      "load_id": "LOAD-456",
      "driver_id": "DRV-001",
      "score": 94.5,
      "rank": 1,

      "eta_pickup": "2024-01-29T16:15:00Z",
      "eta_delivery": "2024-01-30T09:30:00Z",

      "deadhead_miles": 2.3,
      "loaded_miles": 1004.2,
      "total_miles": 1006.5,

      "revenue": 2850.00,
      "fuel_cost": 423.50,
      "profit_estimate": 2426.50,

      "hos_feasible": true,
      "requires_break": true,
      "break_plan": {
        "break_location": "Kearney, NE",
        "break_lat": 40.6995,
        "break_lon": -99.0817,
        "break_start": "2024-01-29T22:30:00Z",
        "break_duration_min": 30
      },

      "fuel_stops": [
        {
          "station_id": "FS-789",
          "name": "Pilot Travel Center",
          "lat": 40.8207,
          "lon": -96.7002,
          "gallons": 85.0,
          "price_per_gallon": 3.45,
          "cost": 293.25
        }
      ]
    }
  ],

  "unassigned": [],

  "total_revenue": 2850.00,
  "total_miles": 1006.5,
  "total_deadhead_miles": 2.3,
  "total_fuel_cost": 423.50,
  "utilization_pct": 78.5
}
```

## Adapter Interface

Each adapter implements a standard interface for translating external formats:

```c
/* Adapter interface */
typedef struct {
    const char *name;
    const char *version;

    /* Parse external format to canonical */
    int (*parse_drivers)(const char *json, NxDriver **drivers, int *count);
    int (*parse_loads)(const char *json, NxLoad **loads, int *count);
    int (*parse_vehicles)(const char *json, NxVehicle **vehicles, int *count);

    /* Format canonical to external (for responses) */
    char* (*format_assignments)(const NxAssignment *assignments, int count);

    /* Webhook handlers */
    int (*handle_webhook)(const char *event_type, const char *payload);
} NxAdapter;

/* Register adapters */
void nx_register_adapter(const NxAdapter *adapter);
const NxAdapter* nx_get_adapter(const char *name);
```

## Implementation Priority

1. **Core structures** - NxDriver, NxLoad, NxVehicle, NxPlanningRequest/Response
2. **Validation** - Input validation, constraint checking
3. **Generic adapter** - JSON-based generic format
4. **REST API** - Basic /plan endpoint
5. **TMS adapters** - Start with most common (McLeod, Trimble)
6. **ELD adapters** - Samsara, Motive

## Dependencies

| Nexus Uses | For |
|------------|-----|
| HoSE | HOS feasibility checking |
| Velo | Route calculations, ETA |
| FuelWise | Fuel stop optimization |
| Tempo | Time window validation |
| Sigma | Assignment optimization |
| Ralph | Core optimization (via Sigma) |

## Files to Create

| File | Purpose |
|------|---------|
| `nexus/include/nexus.h` | Main API, request/response types |
| `nexus/include/nx_driver.h` | Driver structures |
| `nexus/include/nx_load.h` | Load structures |
| `nexus/include/nx_vehicle.h` | Vehicle structures |
| `nexus/include/nx_facility.h` | Facility structures |
| `nexus/include/nx_constraint.h` | Constraint structures |
| `nexus/src/nx_request.c` | Request handling |
| `nexus/src/nx_response.c` | Response formatting |
| `nexus/src/nx_validate.c` | Input validation |
| `nexus/adapters/generic.c` | Generic JSON adapter |
| `nexus/api/main.c` | REST API server |
| `nexus/tests/test_nexus.c` | Unit tests |
