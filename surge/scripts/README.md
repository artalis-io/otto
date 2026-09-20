# Surge scripts

## Route visualization: `surge_map.py`

Turn a Surge solve into a browsable map — GeoJSON routes over a map basemap.

```
request.json ──▶ surge_solve ──▶ solution.json ─┐
                                                 ├─▶ surge_map.py ─▶ out/
request.json ────────────────────────────────────┘                    routes.geojson
                                                                       stops.geojson
                                                                       anim.json
                                                                       index.html
```

The page has a toggleable **timeline**: click "timeline", then play/scrub to move
each vehicle along its road path by the schedule clock (from `anim.json`, which
holds per-vehicle timed move-segments derived from the solution's arrival/service/
departure times). A vehicle appears on the road only between its first departure
and final return, and dwells at each stop for its service time.

Inputs are the two standard Surge JSON documents, so this works for **any**
Surge model (VRPTW/PDPTW, single- or multi-depot, multi-trip):

- **request** — the `POST /api/v1/solve` body (coordinates, depots, vehicles,
  tasks). Both request shapes are supported: `locations[]` + `location_id`, or
  inline `x`/`y` on tasks/depots.
- **solution** — `sg_api_write_solution` output (`routes[].stops[]`,
  `unassigned[]`).

### Quick start

```bash
# 1. Solve (CLI companion to the HTTP /api/v1/solve endpoint)
make -C surge tools
surge/surge_solve request.json > solution.json

# 2. Straight-line routes, using an existing tile source
python3 surge/scripts/surge_map.py \
    --request request.json --solution solution.json --out-dir out \
    --tiles 'http://localhost:8081/tiles/{z}/{x}/{y}.png'

# 3. Serve and open
( cd out && python3 -m http.server 8090 )   # http://127.0.0.1:8090/
```

### Real road-following routes (Velo)

By default route lines are straight stop-to-stop legs. Pass a Velo graph to
route every leg over the road network instead:

```bash
make -C velo tools        # builds velo/route_geometry
python3 surge/scripts/surge_map.py \
    --request request.json --solution solution.json --out-dir out \
    --velo-graph region.vlg --profile truck --weight distance
```

`region.vlg` is a Velo binary graph (`velo/route_geometry map.osm.pbf --save
region.vlg` builds one once; a `.osm.pbf` also works but re-parses each run).

### Offline basemap (Carta)

Pass a Carta graph to pre-render a self-contained PNG tile pyramid into
`out/tiles` (default `--tiles` template already points there), so the page needs
no tile server:

```bash
make -C carta tiledump    # builds carta/tiledump
python3 surge/scripts/surge_map.py \
    --request request.json --solution solution.json --out-dir out \
    --velo-graph region.vlg --carta-graph region.osm.pbf --min-zoom 6 --max-zoom 11
```

The tile bounding box is taken from the solution's geometry automatically.

### Options

| Flag | Default | Purpose |
|------|---------|---------|
| `--tiles` | `tiles/{z}/{x}/{y}.png` | Leaflet tile URL template |
| `--tile-size` | `256` | tile pixel size |
| `--velo-graph` | — | route legs on real roads (else straight lines) |
| `--profile` / `--weight` | `truck` / `distance` | Velo routing profile / objective |
| `--carta-graph` | — | pre-render an offline tile basemap into `out/tiles` |
| `--min-zoom` / `--max-zoom` | `6` / `11` | pre-rendered zoom range |
| `--no-html` | off | emit only the GeoJSON, skip `index.html` |

`build_geojson()` is a pure function (request + solution → GeoJSON); the
`surge_map_selftest.py` self-test (run by `make -C surge test`) exercises it with
no solver, subprocess, or files.

## Tuning campaign: `tune_matrix.sh`, `instructions.md`

Unrelated — hyperparameter tuning campaign runner; see `instructions.md`.
