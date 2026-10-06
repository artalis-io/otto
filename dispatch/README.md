# OTTO Dispatch

A polished dispatch-planning workspace over the Gyermelyi dataset: load a
planning day, optimize deliveries with **Surge**, inspect vehicle trips, replan
after making a vehicle unavailable, and compare plans — on a **Carta** basemap
with **Velo** road geometry. One desktop workspace; precision-industrial styling.

This is a complete dispatch-planning slice. It reuses OTTO's existing engines
(optimization stays in C/Surge, geometry in C/Velo, tiles in C/Carta) behind a
thin Node/TS backend (`dispatch/server`) and a React frontend (`dispatch/web`).

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
| `VITE_API_ORIGIN` | `localhost:8091` | Frontend → backend origin (dev proxy). |
| `VITE_SOLVE_SECONDS` | 60 | UI-triggered solve budget. |

## Tests & screenshots

- Backend unit tests: `cd dispatch/server && npm test` (result mapping,
  comparison, scenario edits + stale-job association).
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
- Defers billing, carrier management, driver apps, POD, ERP sync.
