# Carta MapLibre Viewer

A focused MapLibre GL JS viewer backed entirely by **Carta vector tiles**: a quiet
custom basemap, locally served fonts (accented place names render), and a demo
dispatch overlay of depot + delivery routes drawn from **real Velo road geometry**.

Everything renders from Carta data and local assets. No external map provider,
no CDN, no tile service beyond the Carta server you run yourself.

This integration is deliberately self-contained under `carta/maplibre-viewer/`.
It consumes Carta's HTTP API and its documented vector schema
([`carta/docs/vector-schema.md`](../docs/vector-schema.md)); it adds nothing to
and changes nothing in the Carta C core.

## What it shows

- **Quiet basemap** styled from Carta's vector schema: warm ivory background,
  subdued landuse, pale-blue water, a road hierarchy with casing and
  zoom-dependent widths, railways, boundaries, and labels with halos. Kept muted
  so the overlaid routes dominate.
- **Readable labels**, including Hungarian `ő`/`ű` and punctuation such as `–`
  and `'`, from SDF glyphs generated locally from IBM Plex Sans (OFL).
- **Demo operational overlay**: a depot, three delivery routes, and their stops.
  Route geometry follows real roads (requested from Velo), not straight lines.
  Selecting a route highlights it, fades the others, fits the camera, and shows
  its stop list.

## Prerequisites

- **Node 18+** for the viewer and glyph generation. **Node 20+** only for the
  Playwright screenshot script (`npm run shot`).
- A built **Carta tile server** and the Monaco PBF (both produced from this repo).
- A built **Velo route server** and a Monaco Velo index, only to (re)generate the
  demo route plan. A committed `public/routes/plan.json` is used otherwise.

## Reproducible launch

All commands run from `carta/maplibre-viewer/` unless noted. Repo root is `../..`.

### 1. Start the Carta tile server (serves MVT + vector TileJSON)

```bash
# from repo root
make carta-api
./carta/api/carta-tile-server -p 8097 data/monaco-latest.osm.pbf
```

`http://localhost:8097/tiles.vector.json` should now resolve, with
`"tiles": ["/tiles/{z}/{x}/{y}.mvt"]` and a `vector_layers` array. The existing
raster endpoints (`/tiles.json`, `/tiles/{z}/{x}/{y}.png`) are unchanged.

### 2. (Optional) Regenerate the demo route plan from Velo

The committed `public/routes/plan.json` already carries real Velo geometry
(`"source": "velo"`). To rebuild it:

```bash
# from repo root: build + run the Velo route server
make velo-api
./velo/api/velo-route-server data/index/monaco-velo.vlg      # listens on :8082

# from carta/maplibre-viewer
VELO_ORIGIN=http://127.0.0.1:8082 npm run plan
```

Each leg (depot → stop → … → depot) is requested from Velo and its polyline
decoded and stitched into one road-following `LineString`. Stops Velo cannot
reach on the extract are skipped and logged. If no route can be built the script
exits non-zero rather than emitting straight-line fallbacks.

### 3. Install deps and generate glyphs

```bash
npm install
npm run fonts      # writes public/fonts/IBM Plex Sans Regular/<range>.pbf
```

`npm run fonts` decompresses the bundled IBM Plex Sans woff2 subsets (latin +
latin-ext, OFL) and emits one SDF glyph PBF per 256-codepoint range the font
covers. Ranges the font does not cover are intentionally absent so the server
returns 404 and MapLibre simply omits those glyphs.

### 4. Run it

```bash
# dev server, proxies /tiles.vector.json and /tiles/* to the Carta origin
VITE_CARTA_ORIGIN=http://127.0.0.1:8097 npm run dev     # http://localhost:5178

# or a production build (static, deployable anywhere that also serves the tiles)
npm run build && npm run preview
```

The dev server resolves the Carta origin at request time, so the viewer stays
same-origin and the style can use relative `/tiles.vector.json` and `/tiles/...`
templates. A small dev-only plugin returns 404 for glyph ranges that do not
exist on disk (a static host does this on its own).

### 5. (Optional) Capture screenshots of the actual rendered map

```bash
# needs Node 20+ and a Chromium available to Playwright
nvm use 20
VIEWER_URL=http://localhost:5178 npm run shot          # writes screenshots/*.png
```

Set `PW_CHROMIUM=/path/to/chrome` to reuse a Chromium already on disk instead of
downloading one.

## Layout

| Path | Purpose |
|------|---------|
| `src/style.ts` | The quiet Carta basemap style (layers, colors, zoom widths). |
| `src/MapView.tsx` | Map init, route/stop/depot sources and layers, selection. |
| `src/RoutePanel.tsx` | Route list and selected-route detail panel. |
| `src/App.tsx` | Loads `public/routes/plan.json`, wires panel ↔ map. |
| `scripts/make-glyphs.mjs` | Local SDF glyph generation from IBM Plex Sans. |
| `scripts/make-plan.mjs` | Builds the demo plan from real Velo geometry. |
| `scripts/screenshot.mjs` | Playwright capture of the rendered viewer. |

## Design notes

- **Transport-request absolutization.** Carta serves same-origin-relative tile
  and glyph templates, which is correct server-side. MapLibre builds tile
  requests inside a Web Worker with no document base, so `MapView.tsx` installs a
  `transformRequest` that resolves root-relative URLs against `location.origin`.
- **Orthogonality.** The viewer depends only on Carta's HTTP surface and vector
  schema. It is not wired into the Carta build and imports no Carta C or JS.

## Known limitations

- The dataset is **Monaco**. "Country" and "regional" views are the widest
  extents the extract supports; open-sea water is not in the OSM extract, so the
  Mediterranean reads as background rather than filled water.
- The demo plan is three illustrative Monaco routes. Geometry is real (Velo),
  but the route set is a fixture, not a live Surge solve.
- Screenshots were captured headless with software WebGL (SwiftShader); a GPU
  browser renders identically but faster.
