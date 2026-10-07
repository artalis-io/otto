# OTTO Dispatch — Data onboarding (upload) design

Status: design (approved decisions below; not yet implemented).

## Goal

Bring-your-own-data onboarding. A dispatcher uploads a raw dataset (orders +
vehicles + historical routes, any CSV/XLSX), maps its columns to OTTO's canonical
fields, and the backend runs the **real** Nexus pipeline — reconcile gate,
semantic checks, geocoding, travel matrix, Surge request — behind a reviewed
dry-run. On admit it becomes a new dataset that yields selectable planning days.

No raw bytes are parsed in Node; the Python pipeline subprocess does that
(process isolation = OTTO's Role P). The reconcile gate is the trust boundary.

## Approved decisions

| Axis | Decision |
|------|----------|
| Scope | **Full re-ingest**: orders + vehicles + routes-fact (the whole canonical pipeline → a dataset → N days by delivery_date) |
| New addresses | **Hybrid**: reuse geocode cache + matrix for known locations; geocode + extend the Velo matrix only for genuinely new ones |
| Geocoding | **Keys if present, else offline**: Google/HERE when `GEOCODE_ENV` has keys; else Locus (local HU OSM) + GeoNames postcode centroids + cache |
| Input format | **Column mapping**: arbitrary CSV/XLSX; the user maps columns → canonical fields (new-customer onboarding, not a fixed schema) |

## Reuses the real pipeline (already on disk)

Generic engines (repo, dataset-agnostic, arg-driven): `nexus/scripts/`
`ingest.sh`, `reconcile.py`, `semantic_checks.py`, `normalize_address.py`,
`geocode_verify.py`. See `GYERMELYI_ROOT/PIPELINE.md`. The geocoder caches
Google/HERE and supports `GEOCODE_OFFLINE=1` (cache-only, no billing). Matrix +
request builders: `build_surge_perday.py`, `matrices/`. The dispatch backend
drives these as subprocesses, exactly as it shells `surge_solve`.

## User flow

```
Upload files ─▶ Map columns ─▶ Validate (dry-run) ─▶ Geocode ─▶ Build ─▶ Admit ─▶ Days appear
                                   │ reconcile gate (HARD)        │ matrix + request
                                   │ semantic (advisory)          │
                                   └─ reject on gate failure      └─ only for NEW locations
```

1. **Upload** (Import view → "New dataset"): drop orders/vehicles/routes files.
   Stored in an external uploads dir with a generated name; size + MIME allowlist;
   no path traversal from the client filename.
2. **Map columns**: backend returns detected headers + sample rows per file; the
   UI shows a mapping table (header → canonical-field dropdown) with auto-suggested
   matches and required/type hints. Saved as a per-dataset schema (equivalent to
   `schemas/gyermelyi-*-v1.json`).
3. **Validate (dry-run)**: `ingest.sh` per entity → `reconcile.py` (HARD: every
   canonical field must trace to the raw bytes) + `semantic_checks.py` (advisory).
   Preview shows canonical records, reconcile pass/fail, semantic warnings,
   rejected rows. **No dataset created yet.**
4. **Geocode**: cache-first per normalized address. Known → cache hit (free).
   New → online (keys) or offline (Locus/GeoNames). Preview shows tiers
   (GREEN/YELLOW/APPROX/RED) on a mini-map + an all-points envelope check.
5. **Build**: travel matrix — reuse `matrices/` + cache where both endpoints are
   known; call the Velo table API only for new O-D pairs (capped). Then build the
   per-day Surge requests (size-guarantee dims + small-van subs, per
   `build_surge_perday.py`).
6. **Admit** (enabled only if the reconcile gate passed): persist the dataset +
   its days to the registry; they appear in the day selector.

## Architecture

New backend modules (dispatch/server/src):
- `data/upload.ts` — multipart intake, storage, limits.
- `data/schema.ts` — canonical field catalog + mapping → dataset schema.
- `data/pipeline.ts` — a job runner mirroring `solve.ts` (concurrency cap,
  watchdog, bounded output, temp cleanup) that stages ingest → geocode → matrix →
  request, reporting sub-stage progress.
- `geometry/matrix.ts` — incremental Velo table for new locations; merges with
  cached/known matrix.
- `data/registry.ts` — dynamic datasets + days (`DAYS` becomes built-ins + registry).

Shared:
- `Job` gains `kind: 'solve' | 'import'` and a `stage` string so the existing job
  polling / cancel / progress UI works unchanged.
- `@fastify/multipart` (or raw) for upload.
- Persistence: `${DISPATCH_UPLOADS_DIR}` (default `${GYERMELYI_ROOT}/uploads`),
  one folder per dataset: `raw/ schema.json canonical/ orders.geocoded.json
  matrices/ requests/`. **Never committed** (customer data). A `registry.json`
  lists datasets + their days.

Canonical field catalog (for the mapping UI), from the existing canonical shapes:
- orders: `order_no, customer, country, zip, city, street, delivery_date,
  weight_kg, pallets(double), tw_start, tw_end, service_min, special_req,
  requires_tail_lift`
- vehicles: `id, plate, vehicle_type, vehicle_class, depot, capacity_kg,
  capacity_pallets, gross_weight_kg, special_req, requires_tail_lift,
  is_subcontractor, operator`
- routes_fact: `order_no, vehicle, sequence, …` (historical, for the factual baseline)

## Security & trust

- **Untrusted file**: size cap, extension/MIME allowlist (csv/xlsx), random
  server-side filename, stored outside the web root.
- **Parser isolation**: Nexus + geocode run in the Python subprocess (Role P);
  Node never parses raw bytes.
- **Admit boundary**: the reconcile gate must pass (provenance sha + every field
  traces to raw) before a dataset can be admitted — validate-before-admit.
- **Resource limits** (reuse the hardened job infra): watchdog + concurrency cap;
  row-count cap; geocode request cap; matrix size cap (N² guard); online geocode
  only when keys are configured.
- **Data hygiene**: raw + derived data stay in the external uploads dir, never
  committed; the anonymization flag still governs display.

## Milestones (sequenced; each is a shippable PR)

- **M1 — Intake + mapping + reconcile preview.** Upload, column-mapping UI,
  `ingest.sh` + reconcile/semantic, dry-run preview, reject on gate failure. Proves
  the trust boundary + mapping without any external calls. *(Highest value / risk.)*
- **M2 — Geocoding.** Cache-first; keys-if-present else offline; tier preview + map.
- **M3 — Matrix + request + admit.** Hybrid matrix (Velo table for new O-D),
  request build, registry, days become selectable.
- **M4 — Polish.** Sub-stage progress UI, cancel/resume, re-map + re-run, dataset
  management (list/delete), error recovery.

## Risks / open items

- **Matrix cost**: N² for new locations; needs a cap + async + progress; very large
  uploads may be rejected or chunked.
- **Geocoding quality without keys**: Locus is HU-biased and postcode-level for some;
  surface tiers and require review of RED/APPROX before admit.
- **Schema mis-mapping**: reconcile catches transform errors, not semantic
  mis-maps; the semantic checks + preview mitigate; consider a confirm step on
  low-confidence auto-maps.
- **Carried data-quality realities**: multi-country zips, fractional pallets,
  shared tractors (documented in PIPELINE.md) — the mapping/validation must not
  reject these.
- **API keys**: `.env` via `GEOCODE_ENV`, never committed; absence → offline path.
