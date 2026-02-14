# Nexus — Document Ingestion Pipeline

**Nexus** is OTTO's document ingestion pipeline for extracting tabular data from XLSX, PDF, and CSV files into structured JSON. It powers facility imports for logistics customers (GLS Hungary, Girteka, Waberer's).

## Architecture

Six-stage pipeline (A/M/B/X/D implemented, J planned):

```
                         ┌──────────────┐
                         │  Input File  │
                         └──────┬───────┘
                                │
                      Format Detection (extension)
                                │
          ┌─────────────────────┼──────────────────┐
          ▼                     ▼                   ▼
   ┌──────────────┐    ┌───────────────┐   ┌──────────────┐
   │    XLSX      │    │     PDF       │   │     CSV      │
   │              │    │               │   │              │
   │              │    │ sh_pdf2struc  │   │              │
   │              │    │ → text blocks │   │              │
   │              │    │      │        │   │              │
   │ STAGE A:     │    │ STAGE A:      │   │ STAGE A:     │
   │ nx_xlsx      │    │ nx_pdf        │   │ nx_csv       │
   │ ZIP→XML→     │    │ Y-align→rows  │   │ RFC 4180→    │
   │ rows × cols  │    │ X-gap→columns │   │ rows × cols  │
   └──────┬───────┘    └──────┬────────┘   └──────┬───────┘
          │                   │                    │
          └───────────────────┼────────────────────┘
                              ▼
                     ┌──────────────────┐
                     │   nx_raw JSON    │  (common tabular format)
                     └──────┬───────────┘
                            │
                  STAGE M: nx_merge (if schema has "row_merge")
                     Merge PDF continuation rows, strip page headers
                            │
                            ▼
                  ┌─────────┴──────────┐
                  │ Schema provided?   │
                  └─────┬────────┬─────┘
                    yes │        │ no
                        ▼        ▼
               ┌──────────────┐  Output nx_raw JSON
               │ STAGE B:     │
               │ nx_xform     │
               │ + nx_compute │
               │ schema-driven│
               │ transform    │
               └──────┬───────┘
                      ▼
               ┌──────────────┐
               │ STAGE X:     │
               │ nx_validate  │
               │ geo_bounds,  │
               │ format,      │
               │ unique,      │
               │ outlier      │
               └──────┬───────┘
                      ▼
               ┌──────────────┐
               │ nx_canonical │
               │    JSON      │
               └──────┬───────┘
                      │
               ┌──────────────┐
               │ STAGE D:     │
               │ nx_emit      │
               │ --emit fmt   │
               │ geojson, csv │
               └──────┬───────┘
                      ▼
               ┌──────────────┐
               │  GeoJSON /   │
               │  CSV / JSON  │
               └──────────────┘
```

### Stage A: Extract

Format-specific parsers produce a common `nx_raw` JSON format:

| Parser | Input | Method |
|--------|-------|--------|
| `nx_xlsx` | XLSX bytes | ZIP → XML → shared strings → cells |
| `nx_pdf` | PDF bytes (via sh_pdf2struc) or text-run JSON | Y-alignment clustering → X-gap column detection |
| `nx_csv` | CSV/TSV text | RFC 4180 with auto-detect delimiter (`,` `;` `\t` `\|`) |

### Stage M: Merge (PDF continuation rows)

PDF tables often split long cell text across multiple physical rows. `nx_merge` detects continuation rows (where key columns are empty) and appends their content to the parent row. Controlled by `"row_merge"` in the schema. Also strips repeated page headers via `"strip_pattern"`.

### Stage B: Transform

`nx_xform` applies a schema to map raw columns to canonical fields. Features:

- **1:1 column mapping** with type coercion (string, int, double, bool)
- **Multi-transforms** (v2 schemas): split, merge, regex, compute, conditional
- **Compute functions** (`nx_compute`): eov_to_wgs84, dms_to_dd, coalesce, phone_normalize, zip_to_region, opening_hours
- **Row ID generation** with slugification
- **Derived fields**: constants added to every record
- **Per-field transforms**: trim, lowercase, uppercase, replace

### Stage X: Validate

`nx_validate` runs semantic validation rules on canonical JSON:

| Rule | Check | Effect |
|------|-------|--------|
| `geo_bounds` | Lat/lon within bounding box | Remove or warn |
| `format` | POSIX regex match on field | Remove or warn |
| `unique` | No duplicate values (first kept) | Remove duplicates |
| `outlier` | IQR-based outlier detection | Warn |

Rules have configurable severity (`"error"` = remove record, `"warning"` = flag only).

### Stage D: Emit

`nx_emit` converts canonical JSON to downstream formats:

| Format | Output | Use Case |
|--------|--------|----------|
| GeoJSON | RFC 7946 FeatureCollection with Point geometry | Carta map visualization |
| CSV | RFC 4180 with header row | Operations team export |

Features:
- **GeoJSON**: Auto-detects lat/lon fields from schema's `geo_bounds` validation rule. Coordinates in `[lon, lat]` order per RFC 7946. Non-geo fields become Feature properties.
- **CSV**: Discovers field names from first record. RFC 4180 quoting (double-quote fields containing commas, quotes, or newlines).
- **Callback-based streaming**: Both CSV (`ShCsvWriter`) and GeoJSON (`ShJsonWriter`) use callback-based writers, decoupled from output target.

CLI: `./nx_pipeline input.xlsx --schema s.json --emit geojson|csv`

### Schema Discovery

`nx_discover` profiles Stage A output and generates a draft schema without LLMs:
- Type inference (double/int/string by parse success rate)
- Lat/lon detection (range + header name matching, incl. Hungarian headers)
- Required field detection (non-empty in all rows)
- Auto-transforms (trim, tilde-prefix replace)
- Geo bounds with padding from detected lat/lon
- Uniqueness detection for row ID candidates
- Continuation row detection (emits `row_merge` config)

## nx_raw JSON Format

```json
{
  "nx_raw": 1,
  "source": {
    "filename": "data.xlsx",
    "format": "xlsx",
    "sha256": "abc123..."
  },
  "tables": [{
    "name": "Sheet1",
    "row_count": 100,
    "col_count": 5,
    "headers": ["city", "name", "address", "zip", "phone"],
    "rows": [["Budapest", "Main Depot", "Fo utca 1", "1011", "+36..."]]
  }]
}
```

## Schema Format (v2)

```json
{
  "nx_schema": 2,
  "version": "gls-hu-automata-v2",
  "output_type": "facility",
  "row_merge": {
    "key_columns": [0, 1, 2],
    "separator": " ",
    "strip_pattern": "GLS CsomagPontok"
  },
  "multi_transforms": [
    {"type": "split", "source": 8, "delimiter": ",",
     "targets": [{"field": "lat", "index": 0}, {"field": "lon", "index": 1}]},
    {"type": "compute", "function": "coalesce", "sources": [5, 6],
     "targets": [{"field": "val"}]},
    {"type": "regex", "source": 3, "pattern": "^(\\d{4}) (.+)$",
     "targets": [{"field": "zip", "group": 1}, {"field": "city", "group": 2}]}
  ],
  "columns": [
    {"source": 0, "target": "city", "type": "string", "transforms": ["trim"], "required": true}
  ],
  "derived": [
    {"target": "facility_type", "value": "parcel_automata"}
  ],
  "row_id": {"template": "gls-hu-{city}-{name}", "slugify": true},
  "validate": [
    {"type": "geo_bounds", "lat_field": "lat", "lon_field": "lon",
     "bounds": {"min_lat": 45.7, "max_lat": 48.6, "min_lon": 16.1, "max_lon": 22.9},
     "severity": "error"},
    {"type": "format", "field": "zip", "pattern": "^[1-9][0-9]{3}$", "severity": "error"},
    {"type": "unique", "fields": ["city", "name"], "severity": "error"}
  ]
}
```

Processing order: `row_merge` → `multi_transforms` → virtual columns → `columns` → `derived` → `row_id` → `validate`

## Key Files

| File | Purpose | Lines |
|------|---------|-------|
| `include/nx_ingest.h` | Pipeline orchestrator API | 73 |
| `include/nx_xlsx.h` | XLSX parser API | 87 |
| `include/nx_pdf.h` | PDF table reconstructor API | 91 |
| `include/nx_csv.h` | CSV parser API | 81 |
| `include/nx_xform.h` | Transform engine API | 68 |
| `include/nx_compute.h` | Compute function registry | 40 |
| `include/nx_validate.h` | Validation engine API | 107 |
| `include/nx_merge.h` | Continuation row merging API | 66 |
| `include/nx_discover.h` | Schema discovery API | 58 |
| `include/nx_emit.h` | Output emitter API (GeoJSON, CSV) | 50 |
| `include/nx_slug.h` | Slugification utility | 30 |
| `src/nx_ingest.c` | Pipeline orchestrator | 137 |
| `src/nx_xlsx.c` | XLSX implementation | 609 |
| `src/nx_pdf.c` | PDF clustering | 789 |
| `src/nx_csv.c` | CSV parser | 342 |
| `src/nx_xform.c` | Transform engine | 1251 |
| `src/nx_compute.c` | Compute functions | 345 |
| `src/nx_validate.c` | Validation engine | 750 |
| `src/nx_merge.c` | Continuation row merging | 368 |
| `src/nx_discover.c` | Schema discovery | 720 |
| `src/nx_emit.c` | Output emitters | 256 |
| `src/nx_slug.c` | Slug utility | 51 |
| `tools/nx_pipeline.c` | CLI pipeline (batch + single) | 932 |
| `tools/nx_run.c` | Stage A CLI | 131 |
| `tools/nx_pdf_run.c` | PDF clustering CLI | 99 |
| `tools/nx_xform_run.c` | Transform CLI | 81 |
| `wasm/src/nx_wasm.c` | WASM wrapper | 490 |
| **Total** | | **~8200** |

## Error Handling

Each stage has typed error codes with `*_status_str()` for human-readable messages:

| Stage | Component | Error Codes |
|-------|-----------|-------------|
| Stage A (XLSX) | `nx_xlsx` | `ERR_{NULL,ZIP,NO_SHEETS,XML,ARENA,LIMITS}` |
| Stage A (PDF) | `nx_pdf` | `ERR_{NULL,JSON,NO_TEXT,ARENA}` |
| Stage A (CSV) | `nx_csv` | `ERR_{NULL,PARSE,NO_DATA,ARENA}` |
| Stage M | `nx_merge` | `ERR_{NULL,JSON,SCHEMA,NO_TABLE,ARENA}` |
| Stage B | `nx_xform` | `ERR_{NULL,SCHEMA,RAW,NO_TABLE,ARENA}` |
| Stage X | `nx_validate` | `ERR_{NULL,JSON,ARENA}` |
| Stage D | `nx_emit` | `ERR_{NULL,JSON,NO_RECORDS,NO_LATLON,ALLOC}` |
| Discovery | `nx_discover` | `ERR_{NULL,JSON,NO_TABLE,NO_ROWS,ARENA}` |
| Orchestrator | `nx_ingest` | `ERR_{NULL,FORMAT,STAGE_A,STAGE_B,ARENA}` |

## CLI Tools

Build with `make tools`:

| Tool | Purpose |
|------|---------|
| `nx_pipeline` | End-to-end: XLSX/PDF/CSV → raw/canonical JSON (single + batch) |
| `nx_run` | Stage A only: XLSX/PDF-JSON/CSV → raw JSON |
| `nx_pdf_run` | PDF clustering: text-run JSON → raw JSON (--row-tol, --col-gap) |
| `nx_xform_run` | Stage B only: raw JSON + schema → canonical JSON |

```bash
# Auto-detects format from extension
./nx_pipeline input.xlsx --schema schemas/config.json
./nx_pipeline input.pdf --raw
./nx_pipeline input.csv --delimiter ";" --no-header

# Emit downstream formats (Stage D)
./nx_pipeline input.xlsx --schema s.json --emit geojson
./nx_pipeline input.xlsx --schema s.json --emit csv

# Batch mode
./nx_pipeline --config pipeline.json

# Output to file
./nx_pipeline input.xlsx --schema s.json -o output.json
```

## Test Coverage

| Test File | Tests | Coverage |
|-----------|-------|----------|
| `test_ingest` | 45 | 11 XLSX + 14 PDF + 13 CSV + 7 golden |
| `test_xform` | 37 | 5 slug + 8 xform + 16 multi-transform + 4 trucking + 4 pipeline |
| `test_validate` | 9 | geo_bounds, format, unique, outlier |
| `test_discover` | 26 | 18 unit + 2 continuation + 6 golden |
| `test_merge` | 20 | 4 error + 5 basic + 1 strip + 6 edge + 4 golden (PDF02) |
| `test_emit` | 19 | 9 GeoJSON + 10 CSV |
| **Total** | **156** | |

## Schemas

Six schemas in `schemas/` for GLS Hungary:

| Schema | Type | v1 | v2 |
|--------|------|----|----|
| `gls-hu-automata` | Parcel automata | Basic columns | + multi-transforms, validation |
| `gls-hu-pudo` | PuDo locations | Basic columns | + row_merge, validation |
| `gls-hu-depots` | Depots | Basic columns | + EOV compute, validation |

## Dependencies

| Library | Purpose |
|---------|---------|
| `shared/libshared.a` | sh_arena, sh_json, sh_xml, sh_csv, sh_geojson, sh_hash_sha256, sh_fs, sh_eov |
| `shared/libsh_pdf2struc.a` | Pure C PDF text extraction (nx_pipeline only) |
| `vendor/miniz/` | ZIP reading for XLSX |

## Build

```bash
make all      # Library + tests
make test     # Run 156 tests
make tools    # CLI tools (nx_pipeline, nx_run, nx_pdf_run, nx_xform_run)
make debug    # Build with ASan/UBSan + -Werror
make clean    # Remove artifacts
```

Build flags: `-Wall -Wextra -Werror -O3`, vendor headers via `-isystem`.
Hardening: `-fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -fno-common`.

---

## Production Gaps (Prioritized)

### P0: Wire Stage X into Pipeline Orchestrator — DONE

Stage X (validation) is now wired into `nx_ingest()`, `nx_pipeline.c` (single-file and batch modes). Stage M (merge) is guarded to PDF-only formats. The WASM wrapper keeps validate as a separate call for browser flexibility.

Changes: `nx_ingest.h` (+1 error code), `nx_ingest.c` (+25 lines), `nx_pipeline.c` (+70 lines merge+validate+summary).

### P1: Downstream Output (Stage D) — DONE

Stage D emitters implemented: GeoJSON (RFC 7946 FeatureCollection) and CSV (RFC 4180). Shared library gains `sh_geojson.h/c` (GeoJSON encoder) and CSV writer extension in `sh_csv.h/c`. Both use callback-based streaming writers decoupled from output target.

Changes: `shared/include/sh_csv.h` (+75 lines writer API), `shared/src/sh_csv.c` (+154 lines), `shared/include/sh_geojson.h` (new, 50 lines), `shared/src/sh_geojson.c` (new, 120 lines), `nexus/include/nx_emit.h` (new, 50 lines), `nexus/src/nx_emit.c` (new, 256 lines), `nexus/tools/nx_pipeline.c` (+82 lines `--emit` flag), `nexus/wasm/src/nx_wasm.c` (+77 lines WASM exports). Tests: 12 CSV writer + 12 GeoJSON + 19 nexus emit = 43 new tests.

**Remaining for P1:** Stage J (join/union/filter across multiple files) and Surge adapter (deferred until Surge is implemented).

### P2: Partial Results and Structured Error Reporting

**Status: All-or-nothing.** One bad row in Stage A kills the entire file. Rejections capped at `MAX_REJECTIONS = 1024` in nx_xform.c (silently dropped after that). Batch mode reports only a count, not per-file structured errors.

**Impact:** 50K-row XLSX with 1 corrupt cell = zero output. Operations team cannot use the 49,999 good rows.

**Fix:** Per-sheet error tracking in nx_xlsx.c, dynamic rejection accumulator, batch result manifest (JSON). ~500 lines.

### P3: Change Detection Between Runs

**Status: Not implemented.** Every run is a full reprocess. `row_id` provides the natural diff key but nothing uses it for comparison.

**Impact:** Cannot answer "what changed this month?" for monthly facility re-ingestion. Database upsert target (Stage D) requires full replace instead of efficient delta.

**Estimated scope:** ~300 lines — hashmap of `row_id` → record for old vs. new, emit `{added, removed, modified, unchanged_count}`.

### P4: Configurable Memory Limits

**Status: Hardcoded ceilings.** PDF max 8192 rows, arenas 32-64 MB, `MAX_REJECTIONS = 1024`.

**Impact:** Silent data loss on large documents. A 40-page PDF manifest exceeds the PDF row limit.

**Fix (interim):** Make limits configurable via CLI/env, raise defaults, add explicit error messages when limits are hit. ~100 lines. Full streaming architecture is a separate, larger effort.

---

## Future

- **Nexus Gateway**: TMS/ELD integration REST API (see `docs/roadmaps/nexus-gateway.md`)
- **Streaming Stage A**: Parse and emit rows incrementally for large files
- **WASM demo enhancements**: Wire Stage X into the browser demo pipeline
