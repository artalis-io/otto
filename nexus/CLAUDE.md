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
| `nx_run` | General pipeline: XLSX/PDF-JSON → raw JSON, with optional schema |
| `nx_pdf_run` | PDF clustering: text-run JSON → raw JSON (with --row-tol, --col-gap) |
| `nx_xform_run` | Schema transform: raw JSON + schema → canonical JSON |

## Pipeline Runner

The unified pipeline handles end-to-end processing:

```bash
# Single file (auto-detects format from extension)
python3 scripts/nx-pipeline.py input.pdf --schema schemas/gls-hu-automata-v1.json
python3 scripts/nx-pipeline.py input.xlsx --schema schemas/gls-hu-depots-v1.json

# With PDF tuning overrides
python3 scripts/nx-pipeline.py input.pdf --row-tol 1.0 --col-gap 4.0

# Batch mode from config
python3 scripts/nx-pipeline.py --config pipeline.json
```

Requires: `pip install pdfplumber` (for PDF files)

## Auto-Detection

When no `--row-tol` or `--col-gap` is specified, the PDF clusterer auto-detects from text heights:
- `row_tolerance ≈ 0.7 * median_text_height`
- `col_gap_min ≈ 3.0 * median_text_height`

Use explicit values when auto-detection produces suboptimal results (e.g., dense PDFs with small gaps).
