# Nexus — Document Ingestion Pipeline

**Nexus** is OTTO's document ingestion pipeline for extracting tabular data from XLSX, PDF, and CSV files into structured JSON. It powers facility imports for logistics customers (GLS Hungary, Girteka, Waberer's).

## Architecture

Five-stage pipeline (A/M/B/X implemented, J/D planned):

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
| `src/nx_slug.c` | Slug utility | 51 |
| `tools/nx_pipeline.c` | CLI pipeline (batch + single) | 745 |
| `tools/nx_run.c` | Stage A CLI | 131 |
| `tools/nx_pdf_run.c` | PDF clustering CLI | 99 |
| `tools/nx_xform_run.c` | Transform CLI | 81 |
| `wasm/src/nx_wasm.c` | WASM wrapper | 413 |
| **Total** | | **~7500** |

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
| **Total** | **137** | |

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
| `shared/libshared.a` | sh_arena, sh_json, sh_xml, sh_csv, sh_hash_sha256, sh_fs, sh_eov |
| `shared/libsh_pdf2struc.a` | Pure C PDF text extraction (nx_pipeline only) |
| `vendor/miniz/` | ZIP reading for XLSX |

## Build

```bash
make all      # Library + tests
make test     # Run 137 tests
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

### P1: Downstream Output (Stage J + Stage D)

**Status: Planned, not implemented.** The pipeline produces canonical JSON that no other OTTO module can consume directly. There is no adapter to Surge `SGRequest`, no GeoJSON emitter, no database upsert.

**Impact:** The demo stops at "here's clean JSON." For the Girteka/Paketa pitch ("ingest PDFs, route trucks"), steps 2-4 are entirely manual.

**Estimated scope:**
- Stage J (join/union/filter): ~500 lines
- Stage D per target: ~100-200 lines each
- Priority targets: GeoJSON (Carta visualization), Surge (VRP), CSV export

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
