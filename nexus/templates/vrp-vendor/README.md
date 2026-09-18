# VRP vendor onboarding template

Copy this directory into a dataset (never the repo) and adapt it to turn a
vendor's raw order/vehicle exports into a solver-ready bundle:

```
raw/*.csv --(nexus ingest: transform + reconcile gate + semantic check)--> input/*.json
          --(geocode_verify: 3-way geocode, tiered)--------------------> input/orders.geocoded.json
          --(build_matrix: one-to-all Dijkstra over a region graph)----> matrices/velo_matrix.*.json
          --(emit_vrp_bundle: standard CSVs)---------------------------> bundle/{orders,distance,duration,vehicles}.csv
```

The engine (`nexus/scripts/*`, `velo/tools/matrix_build`) is generic; everything
vendor-specific lives here in the dataset. Client data is never committed.

## Onboard a new vendor

1. **Copy** this dir to `~/artalis.io/data/<vendor>/`; drop raw exports in `raw/`.
2. **Schemas** — edit `schemas/orders.schema.json` and `schemas/vehicles.schema.json`
   so the raw columns map to the canonical target fields (below). For a brand-new
   layout, generate a draft with `nexus/tools/nx_schema_gen.py` and review it.
3. **Driver** — edit `reproduce.sh`: set `VENDOR`, `COUNTRY` (ISO code to keep in
   the VRP), `DEPOT_*` (address or lat/lon), and `GRAPH` (a Velo `.vlg` covering
   the vendor's region — build one from OSM with Velo if you don't have it).
4. **Run** `sh reproduce.sh`. Outputs land in `input/`, `matrices/`, `bundle/`.

## Canonical fields the pipeline expects

Your schema's `target` names should include these (extra fields are fine and are
carried through):

**orders** — `row_id` must produce a stable `id` (the bijection key; append a
line suffix if ids can repeat). Geocoding reads `address_geocode`. Useful demand
/ window fields: `order_no`, `ship_addr_id`, `customer`, `city`, `zip`, `street`,
`weight_kg`, `pallets`, `tw_start`, `tw_end`, `service_min`. `lat`/`lon` are added
by `geocode_verify`.

**vehicles** — `row_id` -> `id`; then whatever the fleet has, e.g.
`capacity_pallets`, `capacity_kg`, `depot`, `vehicle_class`, `requires_tail_lift`,
`operator`. `emit_vrp_bundle` flattens these to `vehicles.csv` verbatim; map
columns to VRP roles when you build the solve request.

## Bundle outputs (what a vendor/solver consumes)

- `orders.csv`  — one row per location (depot at index 0 + order-lines), keyed by
  `key`; columns configurable via `--order-cols`.
- `distance.csv`, `duration.csv` — square N×N (first row/col = keys); meters and
  seconds, for the chosen objective (fastest vs shortest — see the matrix's
  `meta.weight_optimized`).
- `vehicles.csv` — the fleet, flattened.

## Notes / gotchas

- **Objective**: emit one bundle per objective you need. `--weight duration`
  (fastest, for driver-hours) or `--weight distance` (shortest, for fuel/km).
- **Scope**: `--country` drops out-of-region stops (e.g. dedicated export loads)
  and keeps the matrix on a single-region graph.
- **Cost**: geocoding calls a paid API on first run (cached afterwards). Keep
  `GEOCODE_OFFLINE=1` once the cache is warm.
- **Graph**: the matrix needs a Velo `.vlg` covering the vendor's region.
