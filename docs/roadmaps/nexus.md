# Nexus - Document Ingestion Pipeline

**Nexus** is OTTO's document ingestion pipeline for extracting tabular data from XLSX, PDF, and CSV files into structured JSON.

## Architecture

Three-stage pipeline where Stage 1 applies only to PDF:

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
   │ (no Stage 1) │    │ STAGE 1:      │   │ (no Stage 1) │
   │              │    │ sh_pdf2struc  │   │              │
   │              │    │ → (text,x,y,  │   │              │
   │              │    │    w,h) tuples │   │              │
   │              │    │      │        │   │              │
   │ STAGE 2:     │    │ STAGE 2:      │   │ STAGE 2:     │
   │ nx_xlsx      │    │ nx_pdf        │   │ nx_csv       │
   │ ZIP→XML→     │    │ Y-align→rows  │   │ RFC 4180→    │
   │ rows × cols  │    │ X-gap→columns │   │ rows × cols  │
   └──────┬───────┘    └──────┬────────┘   └──────┬───────┘
          │                   │                    │
          └───────────────────┼────────────────────┘
                              ▼
                     ┌──────────────┐
                     │  nx_raw JSON │  (common tabular format)
                     └──────┬───────┘
                            │
                  ┌─────────┴──────────┐
                  │ Schema provided?   │
                  └─────┬────────┬─────┘
                    yes │        │ no
                        ▼        ▼
               ┌──────────────┐  Output nx_raw JSON
               │ STAGE 3:     │
               │ nx_xform     │
               │ schema-driven│
               │ transform    │
               └──────┬───────┘
                      ▼
               ┌──────────────┐
               │ nx_canonical │
               │    JSON      │
               └──────────────┘
```

### Stage 1: Text Extraction (PDF only)

PDF bytes are processed by `sh_pdf2struc` (shared library) to produce positioned text blocks: `(text, x, y, width, height)` 5-tuples per page.

### Stage 2: Table Formation

Format-specific parsers produce a common `nx_raw` JSON format:

| Parser | Input | Method |
|--------|-------|--------|
| `nx_xlsx` | XLSX bytes | ZIP → XML → shared strings → cells |
| `nx_pdf` | Text-run 5-tuples | Y-alignment clustering → X-gap column detection |
| `nx_csv` | CSV/TSV text | RFC 4180 streaming parser with auto-detect delimiter |

### Stage 3: Schema Transform

`nx_xform` applies a schema to map raw columns to canonical fields with type coercion, validation, slugified row IDs, and derived fields.

## nx_raw JSON Format

All Stage 2 parsers produce this common format:

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

## Error Handling

Each stage has typed error codes with human-readable status strings:

| Stage | Component | Error Codes |
|-------|-----------|-------------|
| Stage 1 (PDF) | `sh_pdf2struc` | `ERR_{INVALID_PDF,UNSUPPORTED,IO,OOM}` |
| Stage 2 (XLSX) | `nx_xlsx` | `ERR_{NULL,ZIP,NO_SHEETS,XML,ARENA,LIMITS}` |
| Stage 2 (PDF) | `nx_pdf` | `ERR_{NULL,JSON,NO_TEXT,ARENA}` |
| Stage 2 (CSV) | `nx_csv` | `ERR_{NULL,PARSE,NO_DATA,ARENA}` |
| Stage 3 | `nx_xform` | `ERR_{NULL,SCHEMA,RAW,NO_TABLE,ARENA}` |
| Orchestrator | `nx_ingest` | `ERR_{NULL,FORMAT,STAGE_A,STAGE_B,ARENA}` |

All stages use arena allocation (`SHArena`) and clean up on all error paths.

## Key Files

| File | Purpose | Lines |
|------|---------|-------|
| `include/nx_ingest.h` | Pipeline orchestrator API | ~70 |
| `include/nx_xlsx.h` | XLSX parser API | ~80 |
| `include/nx_pdf.h` | PDF table reconstructor API | ~90 |
| `include/nx_csv.h` | CSV parser API | ~60 |
| `include/nx_xform.h` | Schema-driven transform API | ~80 |
| `include/nx_slug.h` | Slugification for row IDs | ~20 |
| `src/nx_xlsx.c` | XLSX implementation | ~500 |
| `src/nx_pdf.c` | PDF clustering | ~400 |
| `src/nx_csv.c` | CSV → nx_raw JSON | ~260 |
| `src/nx_xform.c` | Transform engine | ~600 |
| `src/nx_slug.c` | Slug utility | ~100 |
| `src/nx_ingest.c` | Pipeline orchestrator | ~120 |

## CLI Tools

Build with `make tools`:

| Tool | Purpose |
|------|---------|
| `nx_pipeline` | End-to-end: XLSX/PDF/CSV → raw/canonical JSON |
| `nx_run` | Stage 2 only: XLSX/PDF-JSON/CSV → raw JSON |
| `nx_pdf_run` | PDF clustering: text-run JSON → raw JSON |
| `nx_xform_run` | Stage 3 only: raw JSON + schema → canonical JSON |

### nx_pipeline Usage

```bash
# Auto-detects format from extension
./nx_pipeline input.xlsx --schema schemas/config.json
./nx_pipeline input.pdf --raw
./nx_pipeline input.csv --schema schemas/config.json

# CSV options
./nx_pipeline input.csv --delimiter ";" --no-header
./nx_pipeline input.tsv --schema schemas/config.json

# PDF tuning
./nx_pipeline input.pdf --row-tol 1.0 --col-gap 4.0

# Batch mode
./nx_pipeline --config pipeline.json

# Output to file
./nx_pipeline input.xlsx --schema s.json -o output.json
```

## Dependencies

| Library | Purpose |
|---------|---------|
| `shared/libshared.a` | `sh_arena`, `sh_json`, `sh_xml`, `sh_csv`, `sh_hash_sha256`, `sh_fs` |
| `shared/libsh_pdf2struc.a` | Pure C PDF text extraction (nx_pipeline only) |
| `vendor/miniz/` | ZIP reading for XLSX |

## Test Coverage

| Test File | Tests | Coverage |
|-----------|-------|----------|
| `test_ingest` | 38 | 11 XLSX + 14 PDF + 13 CSV |
| `test_xform` | 17 | 5 slug + 8 xform + 4 pipeline |
| **Total** | **55** | |

## Schemas

Transform schemas in `schemas/`:

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

## Future

- **Nexus Gateway**: TMS/ELD integration layer (see `nexus-gateway.md`)
