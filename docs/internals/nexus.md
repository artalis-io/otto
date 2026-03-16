# Nexus Internals

Deep-dive into the document ingestion pipeline implementation. For API overview and roadmap, see `docs/roadmaps/nexus.md`.

## Pipeline Flow

```
nx_ingest()
  │
  ├─ Stage A: format-specific parser → nx_raw JSON (heap-allocated via sh_json_buf_take)
  │    Arena: 32 MB, freed after Stage A completes
  │
  ├─ Stage M: nx_merge_rows() — merge PDF continuation rows (PDF format only)
  │    Arena: 32 MB, freed after merge completes
  │    Output: heap-allocated merged JSON (replaces raw_json) or NULL (no-op)
  │    Guard: only runs for NX_FORMAT_PDF_JSON (XLSX/CSV skip this stage)
  │
  ├─ Stage B: nx_xform_apply() — schema-driven transform
  │    Arena: 32 MB for intermediate JSON parsing
  │    Output: heap-allocated canonical JSON
  │
  └─ Stage X: nx_validate() — semantic validation
       Arena: 32 MB
       Output: heap-allocated validated JSON
```

Each stage gets its own arena, created and freed within `nx_ingest()`. The arenas do not overlap — by the time Stage B starts, Stage A's arena is already freed and the raw JSON has been extracted to a heap allocation via `sh_json_buf_take()`.

## Memory Model

All Stage A parsers (nx_xlsx, nx_pdf, nx_csv) use the same pattern:

1. Parse input using arena for intermediate DOM/tree allocations
2. Build output JSON using `ShJsonBuilder` (writes to a growable heap buffer)
3. Return heap-allocated JSON string via `sh_json_buf_take()`
4. Caller frees the arena (intermediate allocations gone)
5. Output JSON survives on the heap — caller must `free()` it

This means the arena is only needed during parsing, not for the lifetime of the output. The pipeline can sequence stages without accumulating arena memory.

### Memory Ceilings

| Constant | Value | File | Governs |
|----------|-------|------|---------|
| `INGEST_ARENA_SIZE` | 32 MB | `nx_ingest.c:18` | Per-stage arena in orchestrator |
| `PIPELINE_ARENA_SIZE` | 64 MB | `nx_pipeline.c:34` | Per-stage arena in CLI tool |
| `MAX_PDF_TEXT_SIZE` | 16-32 MB | `nx_pipeline.c:35`, `nx_wasm.c:33` | PDF text-run JSON collector |
| `NX_WASM_ARENA_SIZE` | 32 MB | `nx_wasm.c:32` | WASM per-call arena |
| `MAX_ROWS` | 8192 | `nx_pdf.c:28` | Maximum rows in PDF table |
| `MAX_COLUMNS` | 64 | `nx_xform.c:40` | Maximum columns per row in transform |
| `MAX_VIRTUAL_COLS` | 32 | `nx_xform.c:41` | Maximum multi-transform outputs |
| `MAX_REJECTIONS` | 1024 | `nx_xform.c:194` | Rejection detail tracking |
| `NX_MAX_VALIDATION_DETAILS` | 1024 | `nx_validate.c:24` | Validation detail tracking |
| `NX_XLSX_DEFAULT_LIMITS.max_rows` | 100,000 | `nx_xlsx.h:52` | XLSX row limit |

## XLSX Parser (`nx_xlsx.c`, 609 lines)

Parses XLSX (Office Open XML) without external libraries beyond miniz for ZIP:

```
XLSX bytes → miniz ZIP reader → locate xl/sharedStrings.xml + xl/worksheets/sheet1.xml
  → sh_xml_init/sh_xml_next streaming XML parser
  → shared string table (arena-allocated array)
  → cell iteration: parse cell ref "AB12" → (col, row)
  → emit nx_raw JSON via ShJsonBuilder
```

Key internals:
- **Shared strings**: XLSX stores repeated strings in a lookup table. Parsed first, stored as `char*[]` in arena.
- **Cell references**: `parse_cell_ref()` converts Excel-style "AB12" to 0-based (col=27, row=11).
- **Streaming XML**: Uses `sh_xml` (shared library) — pull-based parser, no DOM construction.
- **SHA-256**: Computed over raw XLSX bytes for content fingerprinting.

Limitations:
- Single sheet only (takes first worksheet)
- No formula evaluation (stores last-computed value)
- No rich text / formatting
- `max_rows` default 100K

## PDF Table Reconstructor (`nx_pdf.c`, 789 lines)

Reconstructs tabular structure from positioned text blocks:

```
Text-run JSON: [{text, x, y, w, h, page}, ...]
  → Sort by (page, y, x)
  → Y-alignment clustering: group into rows by y-proximity
  → X-gap column detection: find column boundaries from gaps between text blocks
  → Assign each text block to (row, column)
  → Emit nx_raw JSON
```

### Row Clustering Algorithm

1. Sort text blocks by page, then y-coordinate
2. Walk sorted blocks: if `|block.y - current_row.y| < row_tolerance`, same row
3. Row tolerance auto-detected: `0.7 × median_text_height` (or user-specified)

### Column Detection Algorithm

1. Collect all x-coordinates across all text blocks
2. Find gaps between adjacent x-positions: gap = `x[i+1] - (x[i] + w[i])`
3. Gaps exceeding `col_gap_min` define column boundaries
4. `col_gap_min` auto-detected: `3.0 × median_text_height` (or user-specified)

### Auto-Detection

When options have `row_tolerance = 0` or `col_gap_min = 0`:
1. Collect all text block heights
2. Sort heights, take median
3. Derive tolerances from median height

This handles different font sizes across documents without per-document tuning.

## CSV Parser (`nx_csv.c`, 342 lines)

Wraps `sh_csv` (shared library RFC 4180 parser) with nx_raw output:

- Auto-detects delimiter from first line: `,`, `;`, `\t`, `|`
- Supports quoted fields (RFC 4180 escaping)
- Optional header row (first row as headers vs. synthetic Col_0, Col_1, ...)
- SHA-256 of input bytes

## Transform Engine (`nx_xform.c`, 1251 lines)

The largest and most complex module. Processes each row through a pipeline of operations:

```
For each row:
  1. Multi-transforms → virtual columns (appended after raw columns)
  2. Column mapping: source[i] → target field with type coercion
  3. Per-field transforms: trim, lowercase, uppercase, replace
  4. Validation: required fields, min/max ranges
  5. Derived fields: constant values
  6. Row ID: template substitution + slugification
  7. Emit record to ShJsonBuilder
```

### Multi-Transform Types

| Type | Operation | Example |
|------|-----------|---------|
| `split` | Split cell by delimiter or fixed positions | "47.5,19.0" → lat=47.5, lon=19.0 |
| `merge` | Concatenate cells with separator or template | city + street → full_address |
| `regex` | Extract capture groups | "1234 Budapest" → zip=1234, city=Budapest |
| `compute` | Call registered compute function | EOV coords → WGS84 lat/lon |
| `conditional` | Pattern-matching with fallback | If matches "^[A-Z]{2}$" → country code |

Multi-transforms produce **virtual columns** that are appended after the original raw columns. If raw has N columns, the first virtual column is at index N.

### Compute Function Registry (`nx_compute.c`, 345 lines)

Static registry of named functions callable from schema `"compute"` multi-transforms:

| Function | Signature | Domain |
|----------|-----------|--------|
| `eov_to_wgs84` | (eov_y, eov_x) → (lat, lon) | Hungarian cadastral conversion |
| `dms_to_dd` | (deg, min, sec) → decimal_degrees | GPS coordinate conversion |
| `coalesce` | (v1, v2, ...) → first non-empty | Fallback chains |
| `phone_normalize` | (raw_phone) → E.164 format | Hungarian phone numbers |
| `zip_to_region` | (zip) → region_name | Hungarian postal regions |
| `opening_hours` | (hungarian_hours) → ISO format | "H-P: 8-17" → "Mo-Fr 08:00-17:00" |

Functions use `strtod` with endptr validation for numeric parsing (no `atof`).

### Replace Transform Internals

The `replace` transform does in-place string substitution. A guard limits iterations to 1000 to prevent infinite loops when `from` is a substring of `to` (e.g., replace "a" → "aa").

### Audit Trail

Every transform run produces an audit section in the canonical JSON:

```json
{
  "audit": {
    "schema_version": "gls-hu-automata-v2",
    "rows_processed": 100,
    "rows_accepted": 95,
    "rows_rejected": 5,
    "rejections": [
      {"row": 12, "column": "city", "reason": "Required field is empty"}
    ]
  }
}
```

Rejection tracking is capped at `MAX_REJECTIONS = 1024` — beyond that, the count is accurate but details are silently dropped.

## Validation Engine (`nx_validate.c`, 750 lines)

Post-transform semantic validation. Operates on canonical JSON (Stage B output) and modifies it:

1. **Parse** canonical JSON and schema's `"validate"` array
2. **Apply rules** in order, accumulating errors/warnings per record
3. **Remove** records that fail "error"-severity rules
4. **Append** validation audit section to canonical JSON
5. **Rebuild** records array and update counts

### Rule Implementations

**geo_bounds**: Parse lat/lon fields as doubles, check against bounding box. Handles string and numeric field values.

**format**: Compile POSIX extended regex (`regcomp`), match against field value. Compiled once per rule, reused across all records.

**unique**: Build hash set of composite key values (fields joined with `|`). First occurrence kept, subsequent duplicates flagged.

**outlier**: Collect all numeric values for the field, compute Q1/Q3/IQR, flag values outside `Q1 - factor*IQR` or `Q3 + factor*IQR`. Default factor: 1.5.

### Bounds Check on records_to_remove

The `records_to_remove` array is sized to `record_count`. Since multiple rules (unique + outlier + format) can each flag records, all writes are guarded with `if (remove_count < (int)record_count)` to prevent buffer overruns.

## Continuation Row Merging (`nx_merge.c`, 368 lines)

Handles PDF table extraction artifacts where long cell text wraps across physical rows:

```
Row 7: ["2941", "Acs", "KoKo", "Fo u. 23.", "H-P: 8-17, Szo:8-12 E:12-"]
Row 8: ["",     "",    "",     "",           "13:00"]
→ Merged: ["2941", "Acs", "KoKo", "Fo u. 23.", "H-P: 8-17, Szo:8-12 E:12- 13:00"]
```

### Algorithm

1. Parse schema for `"row_merge"` config. If absent → `*out_json = NULL` (no-op).
2. Parse raw JSON, get `tables[0].rows`
3. Buffer a "parent" row. For each subsequent row:
   - If `strip_pattern` matches any cell → skip entirely (page header)
   - If ALL key_column cells empty → continuation: append non-empty cells to parent using separator
   - Otherwise → flush parent to output, start new parent
4. Build merged nx_raw JSON. Preserve source metadata, update row_count.

### Edge Cases

- **Continuation before first data row**: discarded (no parent)
- **Multiple sequential continuations**: all merge into same parent
- **Completely empty rows**: treated as content-less continuation, effectively skipped
- **Cell overflow**: `MAX_MERGE_CELL_LEN = 1024` — truncated silently
- **Column count mismatch**: only merge cells that exist in both parent and continuation

## Schema Discovery (`nx_discover.c`, 720 lines)

Generates a draft transform schema from raw JSON without LLMs:

### Profiling Pass

For each column in `tables[0]`, scan all rows and compute:
- **Fill rate**: percentage of non-empty cells
- **Numeric parse rate**: percentage that parse as double/int
- **Value range**: min/max for numeric columns
- **Uniqueness**: all values distinct?
- **Leading/trailing whitespace**: needs trim transform?
- **Tilde prefix**: Hungarian encoding artifact, needs replace transform

### Type Inference

- If >80% of non-empty values parse as integer → `"int"`
- If >80% parse as double → `"double"`
- Otherwise → `"string"` (default)

### Lat/Lon Detection

Three methods, checked in order:
1. **Header name**: matches "lat", "latitude", "szelesseg", "y_coord", etc.
2. **Range heuristic**: numeric column with values in [-90, 90] (lat) or [-180, 180] (lon)
3. **Pair detection**: two adjacent numeric columns with complementary ranges

When lat/lon detected, auto-generates `geo_bounds` validation rule with ±0.5° padding.

### Continuation Row Detection

After profiling:
1. Find columns with >80% fill rate → key candidates
2. Count rows where ALL key candidates empty but some other cells non-empty
3. If >5% of rows match → emit `"row_merge"` config

## WASM Wrapper (`nx_wasm.c`, 413 lines)

Exposes the pipeline to JavaScript as separate functions per stage:

| Function | Stage |
|----------|-------|
| `nx_wasm_extract()` | Stage A (format: 0=XLSX, 1=PDF_JSON, 2=CSV, 3=PDF bytes) |
| `nx_wasm_transform()` | Stage M + Stage B (merge then transform) |
| `nx_wasm_validate()` | Stage X |
| `nx_wasm_discover()` | Schema discovery |

Each function stores its result in a global buffer. JavaScript reads via `*_result()` and `*_result_len()` pointers. Buffers are freed on the next call to the same function.

For PDF bytes (format=3), the WASM wrapper runs `sh_pdf2struc` in-process to extract text-run JSON, then passes it through Stage A as PDF_JSON.

## CLI Pipeline (`nx_pipeline.c`, 745 lines)

The main CLI tool. Two modes:

### Single-File Mode

```
read_file() → detect format → [sh_pdf2struc if PDF] → nx_ingest() → write output
```

The pipeline tool creates its own 64 MB arena (larger than the orchestrator's 32 MB) and handles PDF text extraction separately from the library orchestrator, since `sh_pdf2struc` is only linked into the pipeline binary (not libnexus.a).

### Batch Mode

Reads a JSON config with `"sources"` array. Processes each source sequentially, writing individual output files to `"output_dir"`. Reports per-file success/failure count on stderr.

## Build Hardening

The Makefile enforces strict compilation:

```makefile
CFLAGS = -Wall -Wextra -Werror -O3 -std=c11 -D_GNU_SOURCE \
         -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -fno-common

# Vendor headers (miniz) as system includes to suppress their warnings
INCLUDES = -Iinclude -I../shared/include -isystem ../vendor/miniz

# Debug builds also use -Werror
DEBUG_CFLAGS = -Wall -Wextra -Werror -g -O0 ... -fsanitize=address,undefined
```

The module compiles with **zero warnings** on both macOS (Apple Clang) and Linux (GCC).

## Banned Patterns (Enforced by Audit)

| Banned | Replacement | Reason |
|--------|-------------|--------|
| `strcpy` | `snprintf` | Buffer overflow |
| `strcat` | `snprintf` | Buffer overflow |
| `sprintf` | `snprintf` | Buffer overflow |
| `atof` | `strtod` + endptr check | No error detection |
| `atoi` | `strtol` or `sh_parse_int` | No error detection, no bounds |
| Direct `jb.buf` access | `sh_json_buf_take()` | Consistent ownership semantics |

## Data Flow Through a Real Example

GLS Hungary PuDo list (PDF02): 1,074 raw rows across 40+ pages.

```
PDF bytes (2.4 MB)
  → sh_pdf2struc: 1,074 text-run clusters
  → nx_pdf: 1,074 rows × 10 columns (nx_raw)
  → nx_merge: strip 21 page headers, merge 217 continuation rows → 836 rows
  → nx_xform: apply gls-hu-pudo-v2 schema
    - split coordinates → lat, lon
    - compute opening_hours → ISO format
    - compute phone_normalize → E.164
    - derive facility_type = "pudo"
    - slugify row_id = "gls-hu-{city}-{name}"
  → nx_validate:
    - geo_bounds: reject if outside Hungary
    - format: reject if ZIP not 4 digits
    - unique: reject duplicate city+name
  → nx_canonical: ~800 validated facility records
```
