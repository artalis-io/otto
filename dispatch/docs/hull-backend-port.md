# Porting the Dispatch backend to Hull — exploration & plan

**Status:** exploration only (no code). Decision memo (a) + route-by-route
TS→QuickJS map and service contracts (b) + the two resolved unknowns (c).

**Constraints assumed** (set by the product owner):
1. App language = **QuickJS / ES2023** on Hull (not Lua).
2. The **Python scripts become Hull-internal** QuickJS components (or services).
3. **Surge / Velo / Carta run as separate API services** that Hull calls over HTTP.
4. The **React/Vite frontend is retained unchanged** (it only speaks the REST contract).

---

## 1. Summary / recommendation

A *straight* port (keep spawning `surge_solve` / `matrix_build` / Python) is
**impossible on Hull** — its capability sandbox forbids subprocess execution by
design. Under the four constraints above the blocker disappears and the port
becomes a **feasible, moderate re-platform of ~1–1.5 months**, dominated by
(i) rewriting the transport/plumbing to Hull idioms and (ii) the ingest/geocode
reimplementation. The ~1,170 LOC of pure domain logic and the entire frontend
are cheap because they stay in the JS family and behind a stable REST contract.

**Recommendation:** do it only if you actually want Hull's platform benefits
(single signed sandboxed binary, SQLite, durable job queue, and **built-in
auth/JWT/CORS/CSP/rate-limiting** — which closes the commercial-gate auth gap).
It is not worth doing for its own sake, and auth specifically is far cheaper to
add to the current Node backend. If pursued, use the **strangler plan** in §6
(run Hull beside Node, flip routes one at a time) and de-risk geocoding and the
Velo matrix endpoint first (§5).

Confidence: medium. The biggest residual unknowns are QuickJS stdlib
completeness (Intl/structuredClone) and solver/matrix latency over HTTP — both
checkable with the small experiments in §7.

---

## 2. Why Hull blocks the straight port, and what the constraints unblock

Hull is a capability-secure runtime: every effect (fs, net, env, exec) is
manifest-declared and kernel-enforced, and **arbitrary subprocess exec is not a
capability** (`os.execute`/`spawn` are unavailable; the only exec path is a
build-time compiler allowlist). The current Dispatch backend is, at its core, a
**subprocess orchestrator**: it spawns `surge_solve`, `matrix_build`, and the
Nexus/Python ingest + geocode tools and parses their stdout. That pattern has no
Hull equivalent.

The constraints remove every subprocess:
- Surge/Velo/Carta (and, we argue, **Nexus**) become **HTTP services** → Hull
  reaches them via its `net` capability (outbound HTTP is fully supported).
- The genuinely-Python bits (`sample_table.py`, `geocode_verify.py`,
  `normalize_address.py`) become **Hull-internal QuickJS** — except the Nexus C
  core (`nx_pipeline`) which is better exposed as a service than reimplemented.

Everything else Hull already does well and maturely: REST routing + JSON +
`hull/validate`, streaming multipart upload, SQLite + migrations, a durable
DB-backed **job queue** with retries/polling, timers, structured logging, and
the security middleware stack (auth/JWT, CORS, CSP, CSRF, rate-limit).

---

## 3. Target architecture

```
                         ┌───────────────────────────── Hull app (QuickJS, single binary) ─────────────┐
  React/Vite SPA  ──────▶│  static dist (embedded) + /* history fallback                                │
  (unchanged)            │  REST: /api/* (40 routes)   middleware: auth · cors · csp · ratelimit         │
                         │  store: SQLite (hull/db) + migrations      jobs: hull/jobs (durable)          │
                         │  geocode: QuickJS (normalizer + cache in SQLite)                              │
                         └───┬──────────────┬───────────────┬───────────────┬───────────────┬───────────┘
                             │ HTTP         │ HTTP          │ HTTP          │ HTTP          │ HTTP
                        ┌────▼───┐     ┌────▼────┐     ┌────▼────┐     ┌────▼────┐     ┌────▼────┐
                        │ Surge  │     │ Velo    │     │ Nexus   │     │ Carta   │     │ Locus / │
                        │ /solve │     │ /route  │     │ /canon  │     │ /tiles  │     │ Sage /  │
                        │ (NEW   │     │ +/matrix│     │ (wrap   │     │ (exists)│     │ Google/ │
                        │  svc)  │     │ (NEW ep)│     │ nx_pipe)│     │         │     │ HERE    │
                        └────────┘     └─────────┘     └─────────┘     └─────────┘     └─────────┘
```

What already exists vs. what's new:
- **Velo** route server exists (`/api/v1/route`), **matrix endpoint is NEW** (§5.1).
- **Carta** tile API exists → Hull proxies `/tiles/*`.
- **Locus** geocoder service exists (:8083) → geocode cross-check.
- **Sage** LLM exists (:8084) → narration, already called over HTTP today.
- **Surge** has no server → **NEW** thin service wrapping `surge_solve`.
- **Nexus** core is a C binary (`nx_pipeline`) → **NEW** thin service wrapping it.

Operational trade: one Node process + CLI spawns becomes Hull + Surge-svc +
Velo-svc + Nexus-svc + Carta-svc. Hull itself is one sandboxed binary, but the
*system* now has more long-lived services to run and health-check (Velo/Carta
already are). This matches OTTO's transport-agnostic manifesto.

---

## 4. Effort estimate

| Area | Current LOC | Effort | Notes |
|------|-------------|--------|-------|
| Pure domain logic (`mapper`, `cost`, `evaluate`, `schedule`, `week`, `compare`, `export`, `types`, `classifyUnassigned`) | ~1,170 | **Low** | TS→JS near-verbatim; swap `createHash`→Hull crypto, `toLocale`→manual fmt |
| Routes + transport (`server.ts`) | 634 | Medium | 40 handlers → `app.*`; CORS/CSP/ratelimit/**auth** become Hull middleware |
| Store → SQLite (`hull/db`) | 516 | Medium | CRUD/GC → SQL + migrations (upgrade over JSON-file store) |
| Jobs (`solve.ts`) → `hull/jobs` | 280 | Medium | durable job calling Surge-svc over HTTP + mapping result |
| Ingest/geocode cluster (`onboard`,`admit`,`gyermelyi`,`import`,`fleet`,`catalog`,`matrix`) | ~970 | **Medium–High** | geocode reimpl (libpostal-free, §5.2); Nexus→service; matrix→Velo HTTP |
| External clients (`velo`,`sage`) | 270 | Low | fetch → Hull `net` + timeout |
| Stand up **Surge-svc** + **Nexus-svc** + Velo matrix ep | — | Medium | thin Keel/HTTP wrappers around existing binaries |
| Frontend | — | Low (~1–2 d) | build `dist/` → Hull `static/` + `/*` fallback + `/tiles` proxy; **no app-code changes** |
| Re-author tests | ~1,620 | Medium | node:test → Hull test runner; Playwright-against-live still works (point at Hull) |

**Total: ~4–7 focused weeks** for one engineer fluent in both. Critical path is
the ingest/geocode cluster + standing up the two new services.

---

## 5. Resolved unknowns (c)

### 5.1 Velo matrix endpoint — ABSENT, must be added

Evidence: `velo/api/` (`velo-route-server`) exposes only `/api/v1/route`,
`/api/v1/health`, `/api/v1/stats` (`velo/api/test_api.sh`). The all-pairs matrix
the backend needs comes from the **`matrix_build` CLI**, not the server. Going
services-only therefore requires a **new endpoint** on the Velo API that wraps
the same `matrix_build` code path:

```
POST /api/v1/matrix
  body: { "profile": "truck", "weight": "duration",
          "locations": [ { "id": 0, "lat": 47.5, "lon": 19.0 }, ... ] }   // ≤ maxMatrixLocations
  200:  { "location_count": N,
          "durations": [ ... N*N, row-major, seconds ... ],
          "distances": [ ... N*N, row-major, metres ... ],
          "snap_warnings": [ { "index": i, "meters": m }, ... ] }
```
This mirrors exactly what `geometry/matrix.ts` parses today (the `M i j dur dist`
and `S idx node snap` lines), so Hull-side parsing becomes trivial JSON. Effort:
a modest C addition to `velo-route-server` (the matrix compute already exists).

### 5.2 libpostal-free geocode — feasible; the fallback already exists

Evidence: `nexus/scripts/geocode_verify.py` imports
`from normalize_address import expand, parse, looks_like_poi, HAVE_LIBPOSTAL` —
i.e. **libpostal is already optional** with a non-libpostal fallback branch.
Providers are **Google** (`google_geocode`, `google_components` with a
country+postal_code filter) and **HERE** (`here_geocode`) — both plain HTTP —
plus a file cache (`--cache-dir`), a GeoNames postcode envelope for RED
detection, and GREEN/YELLOW/APPROX/RED tiering. `GEOCODE_OFFLINE=1` forbids live
calls (cache-only, reproducible).

Proposed Hull-internal geocode (QuickJS), no libpostal:
- **Normalizer** (replaces libpostal `expand`/`parse`): a small HU-focused JS
  function — accent-fold, lowercase, expand street abbreviations (`u.`→`utca`,
  `krt`→`körút`, `út`/`tér`…), extract the 4-digit postcode and city by regex.
  This is the same job the Python fallback does when `HAVE_LIBPOSTAL` is false.
- **Primary geocoder:** Google Geocoding API over Hull `net` (it parses the
  address itself and returns structured components + confidence), using the
  `components=country:HU|postal_code:NNNN` filter to lift centroid/partial hits.
- **Cross-check:** Locus service (:8083, already exists) and/or HERE, to compute
  the provider-agreement distance that drives the tier.
- **Tiering:** keep GREEN/YELLOW/APPROX/RED from confidence + partial-match flag
  + inter-provider distance + GeoNames postcode envelope (ship the GeoNames
  table as data; the envelope check is pure arithmetic).
- **Cache:** SQLite table keyed by normalized address; only TERMINAL statuses
  cached (never cache transient/quota errors → no lost spend on re-run), exactly
  as today.

Accuracy note: dropping libpostal loses robust multilingual POI parsing, but for
HU-only addresses leaning on Google's own parsing + a targeted HU normalizer
matches the existing non-libpostal path. **De-risk by diffing tiers** on the
Gyermelyi set: run libpostal-on vs libpostal-off today and confirm the tier
distribution is acceptable before committing (§7).

### 5.3 Bonus: Nexus is C, not Python

`nx_pipeline` is a Mach-O binary (C), and the reconcile/canonicalize gate lives
there, not in Python. So "rewrite the Python" only really covers
`sample_table.py` (CSV/XLSX sampling) + the geocode scripts. The Nexus core
should become a **service** (consistent with Surge/Velo/Carta) rather than be
reimplemented in QuickJS. Caveat: **XLSX parsing** (today via `sample_table.py`)
needs a zip+XML reader; prefer handing XLSX to the Nexus service too rather than
reimplementing an XLSX parser in QuickJS.

---

## 6. Route-by-route TS→QuickJS map (strangler-ordered)

All 40 routes keep their **path + method + JSON shape** (the frontend contract
is frozen). "Hull piece" is what each handler leans on.

| Route | Hull piece | Port notes |
|-------|-----------|------------|
| `GET /api/health` | — | trivial |
| `GET /api/config` | env | returns currency/source only (no secrets) |
| `GET /api/days` | db | list builtin + admitted days |
| `GET /api/week` | db + `week` logic | pure logic ports verbatim |
| `GET /api/sage/status` | net→Sage | HTTP health ping |
| `POST /api/scenarios` | db | create base/copy |
| `POST /api/scenarios/:id/edit` | db + `validate` | `validateScenarioEdit` → `hull/validate` schema |
| `POST /api/scenarios/:id/edits` | db + `validate` | batch; bound ≤1000 |
| `GET /api/scenarios` / `/:id` / `/:id/plans` | db | reads |
| `POST /api/scenarios/:id/rename` · `DELETE /:id` | db | base protected |
| `GET /api/scenarios/:id/export` · `POST /import` | db + `validate` | `parseScenarioImport` → schema |
| `GET /api/scenarios/:id/solve` | **jobs** + net→Surge | enqueue solve job (see §7 Surge contract) |
| `GET /api/jobs/:id` · `DELETE /:id` | jobs | `hull/jobs` status/cancel |
| `GET /api/plans` · `/:id` | db + net→Velo | `/:id` fills road geometry via Velo `/route` |
| `GET /api/plans/:id/export.csv` · `routesheet.html` · `routes.csv` · `handoff.json` | `export` logic | pure; `toLocale`→manual HU fmt |
| `GET /api/compare` · `POST narrate` · `POST compare/narrate` | `compare` + net→Sage | pure compare + HTTP to Sage |
| `GET /api/import/summary` · `raw` · `canonical` · `catalog` | db / net→Nexus | reads + sampling (Nexus `/sample`) |
| `POST /api/import/upload` | multipart + blob | Hull streaming multipart → content-addressed store |
| `POST /api/import/preview` | net→Nexus | canonicalize + reconcile gate (§7 Nexus contract) |
| `POST /api/import/geocode` | geocode (QuickJS) | §5.2 |
| `POST /api/import/admit` | **jobs** + net→Velo matrix + db | geocode→matrix→request→register (durable job) |
| `GET /api/import/datasets` · `DELETE /:id` | db + fs | registry in SQLite; dataset dir on fs (path-guarded) |
| `GET /tiles.vector.json` · `GET /tiles/*` | net→Carta | reverse-proxy to Carta tile API |

**Transport specifics that change shape (not contract):**
- Fastify typed routes → `app.get/post(path, (req,res)=>…)`; body via
  `json.decode(req.body)` + `hull/validate`; `reply.code(n).send(x)` →
  `res:status(n):json(x)`; headers via `res:header(...)`.
- `onRequest` security-headers/CORS + `ratelimit.ts` → Hull middleware
  (`app.use("*","/api/*", …)`), **plus a real auth middleware** (today's gap).
- `@fastify/static` SPA + `@fastify/multipart` → Hull `static/` embed + streaming
  `req:multipart()`.

---

## 7. Service contracts (new/changed)

**Surge-svc (NEW — wrap `surge_solve`).** `surge_solve` today reads a request
JSON file and writes a solution JSON. Thin server:
```
POST /api/v1/solve        body: <SurgeRequest JSON>   →  200 <SurgeSolution JSON>
     (long solves: either block with a server-side timeout, or return 202 + a
      job id with GET /api/v1/solve/:id; Hull wraps whichever in a hull/job so
      the existing /api/scenarios/:id/solve → /api/jobs/:id polling UX is kept.)
GET  /api/v1/health · /stats
```
Hull's `solve.ts` equivalent: build the request (apply pins/forbids/sequences),
POST it, map `SurgeSolution`→`Plan` with the (ported) `mapper`, persist.

**Velo matrix endpoint (NEW on existing Velo svc).** See §5.1.

**Nexus-svc (NEW — wrap `nx_pipeline`).**
```
POST /api/v1/sample       body: {fileRef, limit}      → {headers[], rows[][], totalRows}   (CSV + XLSX)
POST /api/v1/canonical    body: {fileRef, schema}      → {canonical:{rows[],count}, reconcile:{rowsReconciled,mismatches,fieldsVerified[],mismatchSamples[]}, semantic:{…}}
GET  /api/v1/health
```
Replaces `sample_table.py` + `nx_pipeline`/`ingest` subprocess calls in
`onboard.ts`/`admit.ts`. XLSX handled here (avoids an XLSX parser in QuickJS).

**Carta tiles (exists).** Hull proxies `GET /tiles/*` and `/tiles.vector.json`.

**Already HTTP, unchanged:** Velo `/route`, Locus geocode (:8083), Sage
`/v1/chat/completions` (:8084), Google/HERE geocoding APIs.

**Strangler migration plan**
1. Stand up Hull serving ONLY `GET /api/health`,`/config`,`/days`,`/plans/:id`
   (read-only) backed by a SQLite import of the current JSON store. Point a dev
   copy of the frontend's `api` base at Hull for just those paths; everything
   else still hits Node. Validates routing, store, static embed, tiles proxy.
2. Add Surge-svc + the Velo matrix endpoint; migrate the **solve** + **admit**
   job routes. Validates jobs + service HTTP + solve latency.
3. Migrate the ingest/geocode routes (Nexus-svc + QuickJS geocode).
4. Migrate the remaining reads/edits/exports; add the **auth** middleware.
5. Cut the frontend's `api` base fully to Hull; retire Node.
Because the engines are services and the REST contract is frozen, each step is
independently shippable and reversible (flip the base back).

---

## 8. Risks & de-risking experiments

| Risk | Severity | De-risk before committing |
|------|----------|---------------------------|
| **QuickJS stdlib gaps** — `Intl`/`toLocaleString` (HU currency/number fmt in `export.ts`), `structuredClone` (`solve.ts`), `Date` nuances | Medium | In a scratch Hull app, format one HUF value + deep-clone a request; if `Intl` is stubbed, write a tiny HU `format.js` (trivial) |
| **Geocode accuracy without libpostal** | Medium | Diff GREEN/YELLOW/APPROX/RED tier counts libpostal-on vs -off on the Gyermelyi set today |
| **Solve/matrix over HTTP** — latency + payload size (N² matrix for ~260 locations is MBs) + long solves | Medium | Prototype Surge-svc + Velo `/matrix`; time an end-to-end solve and a 260-loc matrix round-trip |
| **Operational**: 4–5 services vs 1 process; health/supervision | Low–Med | `hull deploy` + systemd/compose; Velo/Carta already run as services |
| **XLSX parsing** in QuickJS | Low | Route XLSX through Nexus-svc `/sample`, don't parse in Hull |
| **Pre-1.0 Hull** (v0.14) | Low–Med | Pin a version; the REST+DB+jobs surface is the mature part |

**Not a risk:** the frontend. It changes zero application code — only its `api`
base URL — so the port cannot regress the UI as long as each ported route
preserves its JSON shape (lock this with the existing Playwright suite pointed
at Hull).

---

## 9. Verdict

Feasible under the stated constraints; ~1–1.5 months; no frontend rewrite. Worth
it **iff** the Hull platform benefits (sandbox, single-binary, SQLite, durable
jobs, built-in auth) are the actual goal. If the near-term driver is only auth,
add it to the Node backend first (days, not weeks) and revisit Hull as a
deliberate re-platform later, starting with the §7 strangler step 1 and the §8
experiments.
