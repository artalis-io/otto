# Nexus — Document Ingestion Pipeline

Six-stage pipeline for extracting tabular data from XLSX, PDF, and CSV into canonical JSON, GeoJSON, or CSV.

## Architecture

```
Document Bytes → Stage A (extract) → nx_raw JSON
                                       ↓
                               Stage M (merge continuation rows, if schema)
                                       ↓
                               Stage B (transform) → nx_canonical JSON
                                       ↓
                               Stage X (validate) → validated JSON
                                       ↓
                               Stage D (emit) → GeoJSON / CSV / JSON
```

**Stage A**: Format-specific parsers (XLSX, PDF, CSV) → `nx_raw` format
**Stage M**: Continuation row merging for PDF tables (`nx_merge`)
**Stage B**: Schema-driven transform (`nx_xform` + `nx_compute`) → `nx_canonical` format
**Stage X**: Semantic validation (`nx_validate`) — geo_bounds, format, unique, outlier
**Stage D**: Output emitters (`nx_emit`) — GeoJSON (RFC 7946), CSV (RFC 4180)

## Key Files

| File | Purpose | Lines |
|------|---------|-------|
| `include/nx_ingest.h` | Pipeline orchestrator API | 73 |
| `include/nx_xlsx.h` | XLSX parser (ZIP→XML→cells) | 87 |
| `include/nx_pdf.h` | PDF table reconstructor (text-run clustering) | 91 |
| `include/nx_csv.h` | CSV/TSV parser (RFC 4180, auto-detect delimiter) | 81 |
| `include/nx_xform.h` | Schema-driven transform engine | 68 |
| `include/nx_compute.h` | Compute function registry API | 40 |
| `include/nx_validate.h` | Semantic validation engine (Stage X) | 107 |
| `include/nx_merge.h` | Continuation row merging API | 66 |
| `include/nx_discover.h` | Auto schema discovery API | 58 |
| `include/nx_emit.h` | Output emitter API (GeoJSON, CSV) | 50 |
| `include/nx_slug.h` | Slugification for row IDs | 30 |
| `include/nx_issue.h` | Structured issue tracking API | 70 |
| `include/nx_diff.h` | Change detection between runs API | 63 |
| `src/nx_xlsx.c` | XLSX implementation | 631 |
| `src/nx_pdf.c` | PDF clustering implementation | 810 |
| `src/nx_csv.c` | CSV → nx_raw JSON | 343 |
| `src/nx_xform.c` | Transform engine (multi-transforms, type coercion) | 1247 |
| `src/nx_compute.c` | Compute functions: EOV, DMS, coalesce, phone, ZIP, hours | 345 |
| `src/nx_validate.c` | Validation rules: geo_bounds, format, unique, outlier | 766 |
| `src/nx_merge.c` | Continuation row merging | 396 |
| `src/nx_discover.c` | Heuristic schema discovery | 720 |
| `src/nx_emit.c` | Output emitters (GeoJSON, CSV) | 280 |
| `src/nx_slug.c` | Slug utility | 51 |
| `src/nx_ingest.c` | Pipeline orchestrator | 174 |
| `src/nx_issue.c` | Issue list implementation | 170 |
| `src/nx_diff.c` | Change detection (FNV-1a hashing, hashmap diff) | 348 |

## Naming

- Functions: `nx_*` prefix
- Types: `Nx*` (e.g., `NxXlsxStatus`, `NxPdfOptions`, `NxCsvLimits`)
- Constants: `NX_*` (e.g., `NX_PDF_OK`, `NX_CSV_OK`)

## Build

```bash
make all      # Build library + tests (199 tests)
make test     # Run all tests
make tools    # Build CLI tools
make debug    # Build with ASan/UBSan + -Werror
make fuzz     # Build fuzz harnesses (requires clang with libFuzzer)
make clean    # Remove artifacts
```

Build flags: `-Wall -Wextra -Werror -O3`. Vendor headers via `-isystem`.
Hardening: `-fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -fno-common`.

## Dependencies

- `shared/libshared.a` — `sh_arena`, `sh_json`, `sh_xml`, `sh_csv`, `sh_hash_sha256`, `sh_fs`, `sh_eov`
- `shared/libsh_pdf2struc.a` — Pure C PDF text extraction (used by `nx_pipeline`)
- `vendor/miniz/` — ZIP reading for XLSX

## Test Counts

- test_ingest: 51 tests (11 XLSX + 14 PDF + 13 CSV + 6 limits + 7 golden)
- test_xform: 38 tests (5 slug + 8 xform + 16 multi-transform + 4 trucking + 4 pipeline + 1 schema cap)
- test_validate: 9 tests (geo_bounds, format, unique, outlier)
- test_discover: 26 tests (18 unit + 2 continuation + 6 golden)
- test_merge: 20 tests (4 error + 5 basic + 1 strip + 6 edge + 4 golden PDF02)
- test_emit: 19 tests (9 GeoJSON + 10 CSV)
- test_issue: 13 tests (init/free, add, dynamic growth, count, JSON output)
- test_diff: 14 tests (null input, identical/added/removed/modified, mixed, empty, parse error)
- test_pipeline: 9 tests (end-to-end pipeline, issues threading, GeoJSON/CSV emit, diff identical/modified/removed, manifest JSON/counts)
- Total: 199 tests

## Schemas

Transform schemas live in `schemas/`. Format (v2 with multi-transforms):

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
    {"type": "merge", "sources": [0, 1], "separator": ", ", "target": "addr"},
    {"type": "regex", "source": 3, "pattern": "^(\\d{4}) (.+)$",
     "targets": [{"field": "zip", "group": 1}, {"field": "city", "group": 2}]},
    {"type": "compute", "function": "coalesce", "sources": [5, 6],
     "targets": [{"field": "val"}]},
    {"type": "conditional", "source": 7,
     "conditions": [
       {"match": "^[A-Z]{2}$", "set": {"field": "country", "value": "{0}"}},
       {"match": ".*", "set": {"field": "country", "value": "HU"}}
     ]}
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

**Schema v1** (without `multi_transforms`) is fully backward-compatible.

**Processing order**: `row_merge` → `multi_transforms` → virtual columns → `columns` (1:1 mapping) → `derived` → `row_id` → `validate`

**Virtual columns**: Multi-transforms produce virtual columns appended after original columns. Reference by index: if raw has N columns, first virtual is at index N.

**Available compute functions**:

| Function | Inputs | Outputs | Use Case |
|----------|--------|---------|----------|
| `eov_to_wgs84` | EOV Y, EOV X | lat, lon | Hungarian cadastral data |
| `dms_to_dd` | degrees, minutes, seconds | decimal degrees | GPS in DMS format |
| `coalesce` | N fields | 1 field (first non-empty) | Fallback chains |
| `phone_normalize` | raw phone string | E.164 format | Hungarian phone numbers |
| `zip_to_region` | ZIP code | region name | Hungarian postal regions |
| `opening_hours` | Hungarian hours string | ISO format | "H-P: 8-17" → "Mo-Fr 08:00-17:00" |

**Validation rules** (in `"validate"` array, runs after transform as Stage X):

| Rule | Fields | Check | Default Severity |
|------|--------|-------|-----------------|
| `geo_bounds` | lat, lon | Bounding box check | error |
| `format` | any field | Regex pattern match | error |
| `unique` | field(s) | No duplicate values (first kept) | error |
| `outlier` | numeric field | IQR-based outlier detection | warning |

### Which regex engine

Both `format` validation rules and `regex` transform rules are compiled by the
vendored engine in `vendor/tre/`, on every platform -- not by the host libc.
That is deliberate: these patterns come from customer config, and a rule has to
mean the same thing on a Linux worker and on a Windows box. See
`vendor/tre/CLAUDE.md`.

They are POSIX **extended** regular expressions, plus the common GNU/TRE
escapes. One thing to know: `\d` is a digit class here. Under glibc it is
not -- GNU has no `\d` and reads it as a literal `d` -- so a rule using `\d`
behaves differently now than it did before the engine was vendored, in the
direction this document already assumed. `\w`, `\s`, `\S`, `\b` and `\<`
mean the same in both.

Bounded repeats are capped at `RE_DUP_MAX` = 255, so `x{1,300}` is rejected.

**Hungarian validation presets** (used in v2 schemas):
- Hungary geo bounds: 45.7-48.6N, 16.1-22.9E
- Hungarian ZIP: 4 digits, first digit 1-9
- Facility dedup: unique on city+name

## CLI Tools

Build C tools with `make tools`. Python tools require `pip install anthropic`.

| Tool | Language | Purpose |
|------|----------|---------|
| `nx_pipeline` | C | **Main tool**: end-to-end pipeline (XLSX/PDF/CSV → raw/canonical JSON) |
| `nx_run` | C | Stage A only: XLSX/PDF-JSON/CSV → raw JSON |
| `nx_pdf_run` | C | PDF clustering: text-run JSON → raw JSON (with --row-tol, --col-gap) |
| `nx_xform_run` | C | Stage B only: raw JSON + schema → canonical JSON |
| `nx_schema_gen.py` | Python | LLM schema generation (Claude ensemble, confidence scores) |
| `nx_schema_review.py` | Python | Interactive schema review/approve/edit |

## Pipeline Runner (`nx_pipeline`)

The C pipeline tool handles end-to-end processing. All stages run fully
in-process using `sh_pdf2struc` for PDF text extraction (no Python dependency).

```bash
# Single file (auto-detects format from extension)
./nx_pipeline input.xlsx --schema schemas/gls-hu-depots-v1.json
./nx_pipeline input.pdf --schema schemas/gls-hu-automata-v1.json
./nx_pipeline input.csv --schema schemas/config.json
./nx_pipeline input.pdf --raw   # Raw JSON only, no schema

# Emit downstream formats (Stage D)
./nx_pipeline input.xlsx --schema s.json --emit geojson
./nx_pipeline input.xlsx --schema s.json --emit csv

# Diff against previous run (change detection)
./nx_pipeline input.xlsx --schema s.json --baseline prev.json

# CSV options
./nx_pipeline input.csv --delimiter ";" --no-header
./nx_pipeline input.tsv --schema schemas/config.json

# With PDF tuning overrides
./nx_pipeline input.pdf --row-tol 1.0 --col-gap 4.0

# Pre-extracted text-run JSON (bypasses sh_pdf2struc)
./nx_pipeline text-runs.json --schema schemas/gls-hu-pudo-v1.json

# Batch mode from config
./nx_pipeline --config pipeline.json

# Write to file instead of stdout
./nx_pipeline input.xlsx --schema s.json -o output.json
```

**Batch config format** (`pipeline.json`):
```json
{
  "output_dir": "./output",
  "sources": [
    {"file": "depot.xlsx", "schema": "schemas/gls-hu-depots-v1.json"},
    {"file": "automata.pdf"},
    {"file": "data.csv", "schema": "schemas/csv-config.json"},
    {"file": "pudo.pdf", "pdf_options": {"row_tolerance": 3.0, "col_gap_min": 10.0}}
  ]
}
```

No external dependencies required (Python/pdfplumber no longer needed).

## Auto-Detection

**PDF clustering:** When no `--row-tol` or `--col-gap` is specified, the PDF clusterer auto-detects from text heights:
- `row_tolerance ≈ 0.7 * median_text_height`
- `col_gap_min ≈ 3.0 * median_text_height`

**CSV delimiter:** When no `--delimiter` is specified, `sh_csv` auto-detects from the first line (supports `,`, `;`, `\t`, `|`).

## LLM Schema Generation

For new document types where manual schema writing is tedious, use LLM-assisted generation:

```bash
# 1. Extract raw JSON from the document
./nx_pipeline input.xlsx --raw > raw.json

# 2. Generate draft schema with Claude (requires ANTHROPIC_API_KEY)
python3 tools/nx_schema_gen.py raw.json --runs 5 -o draft.json

# 3. Review and approve the draft interactively
python3 tools/nx_schema_review.py draft.json --output schemas/

# 4. Run pipeline with the approved schema
./nx_pipeline input.xlsx --schema schemas/approved-v1.json
```

**Design principles:**
- LLM generates, human approves — never auto-apply a generated schema
- Ensemble voting (5 runs by default) with per-field confidence scores
- Structured output via Claude tool_use for guaranteed valid JSON
- Few-shot prompting from `schemas/examples/` directory
- Versioned output: `{domain}-{region}-{entity}-v{N}`
