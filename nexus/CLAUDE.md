# Nexus — Document Ingestion Pipeline

Two-stage pipeline for extracting tabular data from Excel/PDF into canonical JSON.

## Architecture

```
Document Bytes → Stage A (extraction) → Raw Rows JSON → Stage B (transform) → Canonical JSON
```

**Stage A**: Format-specific parsers (XLSX, PDF text-run JSON) → `nx_raw` format
**Stage B**: Schema-driven transform (`nx_xform`) → `nx_canonical` format

## Key Files

| File | Purpose |
|------|---------|
| `include/nx_ingest.h` | Pipeline orchestrator API |
| `include/nx_xlsx.h` | XLSX parser (ZIP→XML→cells) |
| `include/nx_pdf.h` | PDF table reconstructor (text-run clustering) |
| `include/nx_xform.h` | Schema-driven transform engine |
| `include/nx_slug.h` | Slugification for row IDs |
| `src/nx_xlsx.c` | XLSX implementation (~500 lines) |
| `src/nx_pdf.c` | PDF clustering implementation (~400 lines) |
| `src/nx_xform.c` | Transform implementation (~600 lines) |
| `src/nx_slug.c` | Slug utility (~100 lines) |
| `src/nx_ingest.c` | Pipeline orchestrator (~100 lines) |

## Naming

- Functions: `nx_*` prefix
- Types: `Nx*` (e.g., `NxXlsxStatus`, `NxPdfOptions`)
- Constants: `NX_*` (e.g., `NX_PDF_OK`)

## Build

```bash
make all      # Build library + tests
make test     # Run all tests
make debug    # Build with ASan/UBSan
make clean    # Remove artifacts
```

## Dependencies

- `shared/libshared.a` — `sh_arena`, `sh_json`, `sh_xml`, `sh_hash_sha256`
- `vendor/miniz/` — ZIP reading for XLSX

## Test Counts

- test_ingest: 25 tests (11 XLSX + 14 PDF including auto-detection)
- test_xform: 17 tests (5 slug + 8 xform + 4 pipeline)
- Total: 42 tests

## Schemas

Transform schemas live in `schemas/`. Format:

```json
{
  "nx_schema": 1,
  "version": "gls-hu-automata-v1",
  "output_type": "facility",
  "columns": [
    {"source": 0, "target": "city", "type": "string", "transforms": ["trim"], "required": true}
  ],
  "derived": [
    {"target": "facility_type", "value": "parcel_automata"}
  ],
  "row_id": {"template": "gls-hu-{city}-{name}", "slugify": true}
}
```

## CLI Tools

Build with `make tools`:

| Tool | Purpose |
|------|---------|
| `nx_pipeline` | **Main tool**: end-to-end pipeline (XLSX/PDF → raw/canonical JSON) |
| `nx_run` | Stage A only: XLSX/PDF-JSON → raw JSON |
| `nx_pdf_run` | PDF clustering: text-run JSON → raw JSON (with --row-tol, --col-gap) |
| `nx_xform_run` | Stage B only: raw JSON + schema → canonical JSON |

## Pipeline Runner (`nx_pipeline`)

The C pipeline tool handles end-to-end processing. All stages run in-process
except PDF text extraction (calls `pdfplumber` via `popen()`).

```bash
# Single file (auto-detects format from extension)
./nx_pipeline input.xlsx --schema schemas/gls-hu-depots-v1.json
./nx_pipeline input.pdf --schema schemas/gls-hu-automata-v1.json
./nx_pipeline input.pdf --raw   # Raw JSON only, no schema

# With PDF tuning overrides
./nx_pipeline input.pdf --row-tol 1.0 --col-gap 4.0

# Pre-extracted text-run JSON (skips pdfplumber)
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
    {"file": "pudo.pdf", "pdf_options": {"row_tolerance": 3.0, "col_gap_min": 10.0}}
  ]
}
```

**Script discovery**: The tool finds `pdf-to-text-json.py` via:
1. `./scripts/`, `../scripts/`, `../nexus/scripts/`, `nexus/scripts/`
2. `NEXUS_SCRIPT_DIR` environment variable

Requires: `pip install pdfplumber` (for PDF files only)

## Auto-Detection

When no `--row-tol` or `--col-gap` is specified, the PDF clusterer auto-detects from text heights:
- `row_tolerance ≈ 0.7 * median_text_height`
- `col_gap_min ≈ 3.0 * median_text_height`

Use explicit values when auto-detection produces suboptimal results (e.g., dense PDFs with small gaps).
