# Nexus — Document Ingestion Pipeline

**Nexus** is OTTO's document ingestion pipeline for extracting tabular data from XLSX, PDF, and CSV files into structured JSON. It powers facility imports for logistics customers (GLS Hungary, Girteka, Waberer's).

## Architecture

Six-stage pipeline (A/M/B/X/D implemented, J planned) + change detection:

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
| `include/nx_issue.h` | Structured issue tracking API | 70 |
| `include/nx_diff.h` | Change detection between runs API | 63 |
| `src/nx_ingest.c` | Pipeline orchestrator | 174 |
| `src/nx_xlsx.c` | XLSX implementation | 631 |
| `src/nx_pdf.c` | PDF clustering | 810 |
| `src/nx_csv.c` | CSV parser | 343 |
| `src/nx_xform.c` | Transform engine | 1247 |
| `src/nx_compute.c` | Compute functions | 345 |
| `src/nx_validate.c` | Validation engine | 766 |
| `src/nx_merge.c` | Continuation row merging | 396 |
| `src/nx_discover.c` | Schema discovery | 720 |
| `src/nx_emit.c` | Output emitters | 280 |
| `src/nx_slug.c` | Slug utility | 51 |
| `src/nx_issue.c` | Issue list (dynamic, no caps) | 170 |
| `src/nx_diff.c` | Change detection (FNV-1a, hashmap diff) | 348 |
| `tools/nx_pipeline.c` | CLI pipeline (batch + single + diff) | 1165 |
| `tools/nx_run.c` | Stage A CLI | 131 |
| `tools/nx_pdf_run.c` | PDF clustering CLI | 99 |
| `tools/nx_xform_run.c` | Transform CLI | 81 |
| `wasm/src/nx_wasm.c` | WASM wrapper | 620 |
| `tests/test_pipeline.c` | Integration tests (pipeline, emit, diff, manifest) | 503 |
| **Total** | | **~9400** |

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
| Change Detection | `nx_diff` | `ERR_{NULL,PARSE_OLD,PARSE_NEW,NO_RECORDS,ARENA}` |
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

# Diff against previous run (change detection)
./nx_pipeline input.xlsx --schema s.json --baseline prev.json

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
| `test_issue` | 13 | init/free, add, dynamic growth, count, JSON output |
| `test_diff` | 14 | null input, identical/added/removed/modified, mixed, empty, parse error |
| `test_pipeline` | 9 | end-to-end pipeline, issues threading, emit GeoJSON/CSV, diff, manifest |
| **Total** | **192** | |

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
make test     # Run 192 tests
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

### P2: Structured Error Reporting — DONE

`NxIssueList` — heap-allocated, realloc-doubling, growable issue tracker threaded through all six stages as an optional parameter (NULL = same behavior as before). No caps on issue count. Each issue carries stage, severity, row, field, code, and message.

Changes: `nx_issue.h` (new, 70 lines), `nx_issue.c` (new, 170 lines), all stage headers/implementations gain `NxIssueList *issues` parameter, `nx_xform.c` replaces fixed `Rejection[1024]` with dynamic list, `nx_validate.c` replaces fixed `NxValidationDetail[1024]` with dynamic list, `nx_pipeline.c` gains per-stage summary and batch manifest.json, WASM wrapper gains 4 new exports (`issues_clear`, `issues_json`, `issues_result`, `issues_result_len`), demo.html gains issues panel with stage badges. 13 new tests in `test_issue`.

### P3: Change Detection Between Runs — DONE

`nx_diff` compares two canonical JSON documents by `row_id`, producing a structured diff with added/removed/modified/unchanged categories. Uses FNV-1a 64-bit hashing for both key lookup (via `SHHashmapI64`) and content comparison. Modified records include field-level old/new values.

Changes: `nx_diff.h` (new, 63 lines), `nx_diff.c` (new, 348 lines), WASM wrapper gains 3 exports (`diff`, `diff_result`, `diff_result_len`), demo.html gains baseline save/compare workflow with summary cards and detail tables. 14 new tests in `test_diff`.

### Stage A: Partial Results (Stability Fix) — DONE

Stage A parsers (XLSX, PDF, CSV) now recover from mid-parse errors and return partial results instead of failing entirely. If a document has 100 rows and row 50 triggers a parse error, the first 49 rows are still returned with issues logged via `NxIssueList`.

### P4: Dynamic Growth (No Hard Caps) — DONE

Removed all fixed-size caps that caused silent data loss:

- **PDF**: `MAX_TEXT_RUNS` (65536) and `MAX_ROWS` (8192) removed. `parse_text_runs` restructured to two-pass (count from parsed JSON tree, then exact arena alloc). `cluster_rows` array sized to `nruns` (natural upper bound).
- **XLSX**: `MAX_SHARED_STRINGS` (65536) removed. SharedStrings pointer array switched from arena to heap with realloc-doubling. Individual strings still arena-allocated.

Changes: `nx_pdf.c` (~30 lines changed), `nx_xlsx.c` (~20 lines changed). All 169 tests pass.

### Production Hardening — DONE

Four orthogonal improvements to close the gap between "all stages work" and "production-ready CLI + demo":

1. **CLI diff (`--baseline`)**: `nx_pipeline` gains `--baseline prev.json` flag. Compares current run against previous canonical JSON via `nx_diff()`, prints summary to stderr (`N added, N removed, N modified, N unchanged`), outputs diff JSON to stdout. Enables batch change-tracking workflows.

2. **Stage D in WASM demo**: Export GeoJSON / Export CSV buttons added to `demo.html`. Auto-detects lat/lon fields from schema's `geo_bounds` validation rule. Full timing display and truncation-with-expand for large outputs.

3. **Integration tests**: `test_pipeline.c` with 9 tests covering: full end-to-end pipeline (extract→transform→validate), issues threading across stages, GeoJSON/CSV emit from pipeline output, diff (identical/modified/removed runs), and manifest JSON structure with per-stage counts.

4. **Error messages**: File size guard before arena allocation (clear "file too large X MB, max Y MB" instead of silent failure). Arena allocation failures now report size in MB.

Changes: `nx_pipeline.c` (+75 lines), `demo.html` (+123 lines), `test_pipeline.c` (new, 503 lines), `Makefile` (+8 lines). Total: 183→192 tests.

---

## Production Hardening (Next)

Audit findings from Feb 2026. Module is architecturally sound (192 tests, arena memory model, no unsafe string functions) but has latent risks for untrusted input processing.

### P5: Input Bounds Enforcement (Critical)

Five must-fix issues before processing untrusted documents:

| # | Issue | File | Fix |
|---|-------|------|-----|
| 1 | XLSX shared strings grow unbounded via realloc | `nx_xlsx.c:119` | Add `max_shared_strings` to `NxXlsxLimits` (default 1M), return error when exceeded |
| 2 | Transform doesn't validate `source` index vs `MAX_COLUMNS=64` | `nx_xform.c` | Validate all `source` fields during schema parsing, reject out-of-bounds |
| 3 | `max_file_size` defined in limits struct but never checked | `nx_xlsx.c` | Check `len > limits->max_file_size` before ZIP parsing |
| 4 | Validation regex has no complexity/timeout guard (ReDoS) | `nx_validate.c:168` | Add `REG_NOSUB` (already present), add execution timeout or pattern length cap (e.g., 1KB) |
| 5 | WASM global buffers undocumented as non-thread-safe | `nx_wasm.c` | Add thread-safety warning to header comment; consider mutex if SharedArrayBuffer is ever used |

**Estimated effort:** 1 day. Changes are small and localized.

### P6: Silent Truncation Audit — DONE

All four issues resolved:
- Cell truncation reports `NX_ISSUE_WARNING` via `NxIssueList`
- Empty documents return `NX_XLSX_ERR_EMPTY` / `NX_CSV_ERR_EMPTY`
- Records writer OOM in `nx_xform.c` now checked — partial buffer discarded, rejection logged, audit still emitted
- WASM `nx_wasm_last_error()` returns stage + error code string

### P7: Fuzz Testing and Limits Verification

| Task | Target |
|------|--------|
| Fuzz `nx_xlsx_parse()` with AFL/libFuzzer | Malformed ZIP, XML, shared strings |
| Fuzz `nx_pdf_extract_tables()` | Malformed text-run JSON, negative coordinates |
| Fuzz `nx_csv_parse()` | BOM, embedded nulls, ragged rows, 100MB input |
| Limit enforcement tests | Verify all `NxXlsxLimits` and `NxCsvLimits` fields trigger truncation/error |
| Large document benchmarks | 100K rows XLSX, 1000-page PDF, 10MB CSV |
| Schema size cap | Reject schemas >1MB |

**Estimated effort:** 2-3 days (including fuzzer harness setup).

### Summary

| Phase | Scope | Status |
|-------|-------|--------|
| P5 | Input bounds (5 critical issues) | **DONE** |
| P6 | Silent truncation + WASM errors | **DONE** |
| P7 | Fuzz testing + benchmarks | Not started |

After P5+P6, the module is production-ready for **known document formats from trusted sources** (GLS Hungary, Girteka, etc.). After P7, it's ready for **untrusted documents from arbitrary sources**.

---

## Future

- **Nexus Gateway**: TMS/ELD integration REST API (see `docs/roadmaps/nexus-gateway.md`)
- **Streaming Stage A**: Parse and emit rows incrementally for large files
