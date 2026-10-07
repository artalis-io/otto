# OTTO Dispatch

A polished dispatch-planning workspace over the Gyermelyi dataset: load a
planning day, optimize deliveries with **Surge**, inspect vehicle trips, apply
manual overrides, replan, and compare plans — on a **Carta** basemap with
**Velo** road geometry. One desktop workspace; precision-industrial styling.

Dispatcher workflow features:

- **Manual overrides (human-in-the-loop):** pin an order to a vehicle, move it
  to another, or forbid a vehicle for it; the solver respects these on the next
  replan (Surge `allowed_vehicles` / `forbidden_vehicles`). Overrides accumulate
  on an editable scenario copy; the baseline is never mutated.
- **Cost in forint:** every plan carries an estimated operating cost under a
  carrier tariff, shown as a KPI and in the before/after comparison. Two tariff
  models: `per_vehicle` (fixed + per-km, the illustrative demo) and `per_trip`
  (base + per round-trip km + per-drop surcharge, own-fleet vs subcontractor
  multiplier) — the real rate card shape. Uses `${GYERMELYI_ROOT}/rates/tariff.json`
  when present (a `per_trip` tariff regressed from the real TS/BHS carrier rate
  card lands the two-day optimized plan at ~6.0M HUF, matching the known figure),
  else the demo tariff. The tariff file is confidential and never committed.
- **Solve controls:** choose the objective (fewest vehicles vs least distance)
  and the time budget; cancel a running solve; a solve survives a page reload
  (the job runs server-side and the UI reconnects to it).
- **Operational exports:** printable per-vehicle driver route sheets (stops,
  ETAs, windows, loads, nav links), a flat stops CSV, and the plan JSON.

This is a complete dispatch-planning slice. It reuses OTTO's existing engines
(optimization stays in C/Surge, geometry in C/Velo, tiles in C/Carta) behind a
thin Node/TS backend (`dispatch/server`) and a React frontend (`dispatch/web`).

The UI is bilingual (**English / Hungarian**, toggle in the top bar, persisted
locally). An optional **Sage** narration (DGX Spark LLM, `SAGE_ORIGIN`) summarizes
a plan or a comparison in the current language, built only from the plan's own
figures; it is on-demand and degrades gracefully when the LLM endpoint is down.

## Layout

| Path | What |
|------|------|
| `dispatch/server` | Node/TS app backend: scenarios, plans, solve jobs (via `surge_solve`), road geometry (via Velo), provenance, Carta tile proxy, static SPA serving. |
| `dispatch/web` | React + TypeScript + Vite + Tailwind + shadcn/ui + react-map-gl + motion frontend. |
| `dispatch/docs/data-api-mapping.md` | The data + API contract and the Surge-solution → Plan mapping. |

## Services & ports

| Service | Port | Start |
|---|---|---|
| Carta (vector tiles, Hungary) | 8097 | `./carta/api/carta-tile-server -p 8097 data/index/hungary-carta.idx` |
| Velo (road geometry, truck) | 8082 | `VELO_RATE_LIMIT_ENABLED=0 ./velo/api/velo-route-server data/index/hungary-velo.vlg` |
| Dispatch backend | 8091 | `cd dispatch/server && npm run dev` |
| Dispatch web (dev) | 5179 | `cd dispatch/web && npm run dev` |
| Surge | — | invoked as the `surge/surge_solve` CLI by the backend |

> Velo rate-limiting must be **off** for bulk geometry precompute (it caps at
> 10 rps by default). The backend precomputes each plan's road geometry once and
> caches it (in memory + on disk); it is never fetched per selection change.

## Prerequisites

- Built OTTO binaries in the main checkout: `surge/surge_solve`,
  `carta/api/carta-tile-server`, `velo/api/velo-route-server`
  (`make -C carta/api && make -C velo/api && make -C surge surge_solve`).
- Dataset indexes: `data/index/hungary-carta.idx`, `data/index/hungary-velo.vlg`.
- The immutable Gyermelyi dataset at `GYERMELYI_ROOT`
  (default `/Users/mark/artalis.io/data/gyermelyi`) — never committed.
- Node 20 (the repo has nvm: `nvm use 20`).

## Run (development)

```bash
# 1. tiles + geometry (from the OTTO repo root)
./carta/api/carta-tile-server -p 8097 data/index/hungary-carta.idx &
VELO_RATE_LIMIT_ENABLED=0 ./velo/api/velo-route-server data/index/hungary-velo.vlg &

# 2. backend (serves /api, proxies /tiles to Carta, runs surge_solve)
cd dispatch/server && npm install && npm run dev        # :8091

# 3. frontend (proxies /api and /tiles to the backend)
cd dispatch/web && npm install
VITE_API_ORIGIN=http://127.0.0.1:8091 npm run dev       # :5179 -> open this
```

Day 1 (2026-05-06) loads with its real saved baseline plan. Click **Optimize**
to re-solve live, select a vehicle to coordinate the map/timeline/inspector,
**Mark unavailable & replan** to solve a reduced fleet, then **Compare**.

## Run (production: backend serves the built SPA)

```bash
cd dispatch/web && npm run build          # -> dispatch/web/dist
cd dispatch/server && npm run build        # tsc -> dist (or use npm start/tsx)
# the backend serves dispatch/web/dist same-origin; open http://localhost:8091
cd dispatch/server && npm start
```

## Configuration (env)

| Var | Default | Meaning |
|---|---|---|
| `DISPATCH_PORT` | 8091 | Backend port. |
| `OTTO_ROOT` | `…/src/otto` | Where the built binaries + indexes live. |
| `GYERMELYI_ROOT` | `…/data/gyermelyi` | Immutable dataset (read-only). |
| `CARTA_ORIGIN` / `VELO_ORIGIN` | `127.0.0.1:8097` / `:8082` | Upstreams. |
| `SAGE_ORIGIN` | `127.0.0.1:8084` | Optional LLM narration (Spark). |
| `DISPATCH_SOLVE_SECONDS` | 240 | Default solve budget. |
| `DISPATCH_ANONYMIZE` | 0 | `1` pseudonymizes customer names (cities/coords kept) for shippable screenshots. |
| `DISPATCH_MAX_CONCURRENT_SOLVES` | 2 | Max `surge_solve` processes at once; the rest queue (jobs stay `pending`). |
| `DISPATCH_SOLVE_GRACE_SEC` | 20 | A solve overrunning its budget by this much is hard-killed (SIGTERM→SIGKILL). |
| `DISPATCH_MAX_SOLVE_OUTPUT_MB` | 128 | Cap on captured solver stdout; beyond it the job fails instead of OOMing. |
| `DISPATCH_RETAIN_PLANS` / `_JOBS` | 200 / 200 | Retention: oldest persisted plans/jobs (and their request files) are pruned. |
| `DISPATCH_RATE_RPS` / `_BURST` | 40 / 120 | Per-IP API rate limit (token bucket); `/tiles` and `/api/health` are exempt. |
| `DISPATCH_UPLOADS_DIR` | `${GYERMELYI_ROOT}/uploads` | Where uploaded datasets land (external, never committed). |
| `NEXUS_DIR` | `${OTTO_ROOT}/nexus` | Nexus engines (`ingest.sh`, `reconcile.py`, `nx_pipeline`) for onboarding. |
| `DISPATCH_MAX_UPLOAD_MB` / `_ROWS` | 25 / 50000 | Upload size / row caps. |
| `GEOCODE_CACHE_DIR` | `${GYERMELYI_ROOT}/.geocode_cache` | Geocode cache (reused; known addresses are free). |
| `GEOCODE_PBF` / `GEOCODE_ENV` | `${OTTO_ROOT}/data/…pbf` / `${OTTO_ROOT}/.env` | OSM PBF for the Locus cross-check; `.env` with API keys. |
| `DISPATCH_GEOCODE_OFFLINE` | 0 | `1` forces cache-only geocoding even when keys exist. |
| `MATRIX_BUILD_BIN` / `VELO_GRAPH` | `${OTTO_ROOT}/velo/matrix_build` / `…/data/index/hungary-velo.vlg` | All-pairs travel-matrix builder + graph (admitting a dataset; `make -C velo tools`). |
| `DISPATCH_MAX_MATRIX_LOCATIONS` | 1500 | Cap on an uploaded dataset's routable locations (matrix is N²). |
| `rates/tariff.json` | (demo) | Optional real carrier tariff under `GYERMELYI_ROOT` (uncommitted). Absent → an illustrative demo tariff drives the cost KPI, labelled as such. |
| `VITE_API_ORIGIN` | `localhost:8091` | Frontend → backend origin (dev proxy). |
| `VITE_SOLVE_SECONDS` | 60 | UI-triggered solve budget. |

## Tests & screenshots

- Backend unit tests: `cd dispatch/server && npm test` (result mapping,
  comparison incl. cost, scenario edits + stale-job association, override
  application, cost tariff, CSV/route-sheet export, week roll-up, edit
  validation/bounds, rate limiter).
- UI tests (Playwright, real browser against the running app): with the stack up
  (backend + Carta + Velo) and the web app on :5179 (or set `VIEWER_URL`), run
  `cd dispatch/web && npm run test:ui`. Covers load/KPIs/cost, i18n, inspector
  vehicle→trip→stop navigation, the staged edit stack + a full Replan, week view,
  constraint advisories, playback and solve settings. Uses the cached Chromium
  (`PW_CHROMIUM`) with software WebGL.
- Screenshots (real browser via Playwright, cached Chromium):
  `cd dispatch/web && PW_CHROMIUM=… node scripts/shot.mjs` (and `shot-replan`,
  `shot-import`, `shot-stage4`). Committed screenshots are captured with
  `DISPATCH_ANONYMIZE=1`.

## Scope & limitations

- Two real planning days (2026-05-06, 2026-05-07), solved independently.
- The **3L loading checker is not implemented** (design-only), so the Load tab
  shows aggregate scalar capacity only and states feasibility is unchecked.
- The saved per-day baselines are authoritative real Surge results; a clean
  day-1 117/0 solve needs the full ~240s budget (shorter budgets report their
  honest LIMIT status and may leave orders unassigned).
- Cost is an **estimate** under a tariff (demo tariff unless a real rate card is
  provided); it is not invoicing. Carrier management, driver apps, POD and ERP
  sync are out of scope.
