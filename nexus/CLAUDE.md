# Nexus — Document Ingestion Pipeline

Three-stage pipeline for extracting tabular data from XLSX, PDF, and CSV into canonical JSON.

## Architecture

```
Document Bytes → Stage A (extraction) → Raw Rows JSON → Stage B (transform) → Canonical JSON
```

**Stage A**: Format-specific parsers (XLSX, PDF text-run JSON, CSV) → `nx_raw` format
**Stage B**: Schema-driven transform (`nx_xform`) → `nx_canonical` format

## Key Files

| File | Purpose |
|------|---------|
| `include/nx_ingest.h` | Pipeline orchestrator API |
| `include/nx_xlsx.h` | XLSX parser (ZIP→XML→cells) |
| `include/nx_pdf.h` | PDF table reconstructor (text-run clustering) |
| `include/nx_csv.h` | CSV/TSV parser (RFC 4180, auto-detect delimiter) |
| `include/nx_xform.h` | Schema-driven transform engine |
| `include/nx_slug.h` | Slugification for row IDs |
| `src/nx_xlsx.c` | XLSX implementation (~500 lines) |
| `src/nx_pdf.c` | PDF clustering implementation (~400 lines) |
| `src/nx_csv.c` | CSV → nx_raw JSON (~260 lines) |
| `src/nx_xform.c` | Transform implementation (~600 lines) |
| `src/nx_slug.c` | Slug utility (~100 lines) |
| `src/nx_ingest.c` | Pipeline orchestrator (~120 lines) |

## Naming

- Functions: `nx_*` prefix
- Types: `Nx*` (e.g., `NxXlsxStatus`, `NxPdfOptions`, `NxCsvLimits`)
- Constants: `NX_*` (e.g., `NX_PDF_OK`, `NX_CSV_OK`)

## Build

```bash
make all      # Build library + tests
make test     # Run all tests
make tools    # Build CLI tools
make debug    # Build with ASan/UBSan
make clean    # Remove artifacts
```

## Dependencies

- `shared/libshared.a` — `sh_arena`, `sh_json`, `sh_xml`, `sh_csv`, `sh_hash_sha256`, `sh_fs`
- `shared/libsh_pdf2struc.a` — Pure C PDF text extraction (used by `nx_pipeline`)
- `vendor/miniz/` — ZIP reading for XLSX

## Test Counts

- test_ingest: 45 tests (11 XLSX + 14 PDF + 13 CSV + 7 golden)
- test_xform: 33 tests (5 slug + 8 xform + 16 multi-transform + 4 pipeline)
- test_validate: 9 tests (geo_bounds, format, unique, outlier)
- Total: 87 tests

## Schemas

Transform schemas live in `schemas/`. Format (v2 with multi-transforms):

```json
{
  "nx_schema": 2,
  "version": "gls-hu-automata-v2",
  "output_type": "facility",
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
  "row_id": {"template": "gls-hu-{city}-{name}", "slugify": true}
}
```

**Schema v1** (without `multi_transforms`) is fully backward-compatible.

**Processing order**: `multi_transforms` → virtual columns → `columns` (1:1 mapping) → `derived` → `row_id`

**Virtual columns**: Multi-transforms produce virtual columns appended after original columns. Reference by index: if raw has N columns, first virtual is at index N.

**Available compute functions**: `eov_to_wgs84`, `dms_to_dd`, `coalesce`, `phone_normalize`
```

## CLI Tools

Build with `make tools`:

| Tool | Purpose |
|------|---------|
| `nx_pipeline` | **Main tool**: end-to-end pipeline (XLSX/PDF/CSV → raw/canonical JSON) |
| `nx_run` | Stage A only: XLSX/PDF-JSON/CSV → raw JSON |
| `nx_pdf_run` | PDF clustering: text-run JSON → raw JSON (with --row-tol, --col-gap) |
| `nx_xform_run` | Stage B only: raw JSON + schema → canonical JSON |

## Pipeline Runner (`nx_pipeline`)

The C pipeline tool handles end-to-end processing. All stages run fully
in-process using `sh_pdf2struc` for PDF text extraction (no Python dependency).

```bash
# Single file (auto-detects format from extension)
./nx_pipeline input.xlsx --schema schemas/gls-hu-depots-v1.json
./nx_pipeline input.pdf --schema schemas/gls-hu-automata-v1.json
./nx_pipeline input.csv --schema schemas/config.json
./nx_pipeline input.pdf --raw   # Raw JSON only, no schema

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
