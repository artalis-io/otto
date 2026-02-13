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

- test_ingest: 22 tests (11 XLSX + 11 PDF)
- test_xform: 17 tests (5 slug + 8 xform + 4 pipeline)
- Total: 39 tests

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

## PDF Preprocessing

PDF files require a Python preprocessing step:

```bash
python3 scripts/pdf-to-text-json.py input.pdf > text_runs.json
# Then feed text_runs.json to nx_pdf_extract_tables() or nx_ingest()
```

Requires: `pip install pdfplumber`
