#!/usr/bin/env python3
"""
nx-pipeline.py - Unified Nexus Document Ingestion Pipeline

Orchestrates the full extraction pipeline:
  PDF  → pdfplumber → text-run JSON → nx_pdf_run → raw JSON → nx_run → canonical JSON
  XLSX → nx_run → raw JSON → nx_run → canonical JSON

Usage:
    # Single file (auto-detect format from extension)
    nx-pipeline.py input.pdf --schema schemas/gls-hu-automata-v1.json
    nx-pipeline.py input.xlsx --schema schemas/gls-hu-depots-v1.json

    # Raw output only (no schema transform)
    nx-pipeline.py input.pdf

    # With PDF tuning overrides
    nx-pipeline.py input.pdf --row-tol 1.0 --col-gap 4.0 --schema schemas/s.json

    # Batch mode from config file
    nx-pipeline.py --config pipeline.json

    # Output to file instead of stdout
    nx-pipeline.py input.pdf --schema s.json -o output.json

Config file format (pipeline.json):
{
  "sources": [
    {
      "file": "01_automata.pdf",
      "schema": "schemas/gls-hu-automata-v1.json",
      "pdf_options": {"row_tolerance": 1.0, "col_gap_min": 4.0}
    },
    {
      "file": "03_depo_location.xlsx",
      "schema": "schemas/gls-hu-depots-v1.json"
    }
  ],
  "output_dir": "./output"
}

Requirements:
    pip install pdfplumber   (only for PDF files)
    make tools               (builds nx_run and nx_pdf_run)
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
NEXUS_DIR = os.path.dirname(SCRIPT_DIR)

# Tool paths (relative to nexus/)
NX_RUN = os.path.join(NEXUS_DIR, "nx_run")
NX_PDF_RUN = os.path.join(NEXUS_DIR, "nx_pdf_run")
NX_XFORM_RUN = os.path.join(NEXUS_DIR, "nx_xform_run")


def find_tools():
    """Check that C tools are built."""
    missing = []
    if not os.path.isfile(NX_RUN):
        missing.append(NX_RUN)
    if not os.path.isfile(NX_PDF_RUN):
        missing.append(NX_PDF_RUN)
    if not os.path.isfile(NX_XFORM_RUN):
        missing.append(NX_XFORM_RUN)
    if missing:
        print(f"Error: tools not found: {', '.join(missing)}", file=sys.stderr)
        print("Run: make -C nexus tools", file=sys.stderr)
        sys.exit(1)


def detect_format(filepath):
    """Detect document format from file extension."""
    ext = os.path.splitext(filepath)[1].lower()
    if ext == ".xlsx":
        return "xlsx"
    elif ext == ".pdf":
        return "pdf"
    elif ext == ".json":
        return "pdf-json"
    else:
        return None


def extract_pdf_text(pdf_path, output_path):
    """Extract text runs from PDF using pdfplumber."""
    try:
        import pdfplumber
    except ImportError:
        print("Error: pdfplumber not installed. Run: pip install pdfplumber",
              file=sys.stderr)
        sys.exit(1)

    pages = []
    with pdfplumber.open(pdf_path) as pdf:
        for i, page in enumerate(pdf.pages):
            page_data = {
                "page": i + 1,
                "width": float(page.width),
                "height": float(page.height),
                "texts": []
            }

            words = page.extract_words(
                keep_blank_chars=False,
                x_tolerance=3,
                y_tolerance=3,
                extra_attrs=["fontname", "size"]
            )

            for word in words:
                text = word["text"].strip()
                if not text:
                    continue
                page_data["texts"].append({
                    "text": text,
                    "x": round(float(word["x0"]), 1),
                    "y": round(float(word["top"]), 1),
                    "w": round(float(word["x1"] - word["x0"]), 1),
                    "h": round(float(word["bottom"] - word["top"]), 1)
                })

            pages.append(page_data)

    result = {"pages": pages}
    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(result, f, ensure_ascii=False)

    total_texts = sum(len(p["texts"]) for p in pages)
    print(f"  Extracted {total_texts} text runs from {len(pages)} pages",
          file=sys.stderr)
    return output_path


def run_pdf_clustering(text_json_path, row_tol=None, col_gap=None):
    """Run nx_pdf_run to cluster text runs into table rows."""
    cmd = [NX_PDF_RUN]
    if row_tol is not None:
        cmd.extend(["--row-tol", str(row_tol)])
    if col_gap is not None:
        cmd.extend(["--col-gap", str(col_gap)])
    cmd.append(text_json_path)

    result = subprocess.run(cmd, capture_output=True)
    if result.returncode != 0:
        print(f"  nx_pdf_run failed: {result.stderr.decode()}", file=sys.stderr)
        return None

    # Print clustering info from stderr
    stderr = result.stderr.decode().strip()
    if stderr:
        for line in stderr.split("\n"):
            print(f"  {line}", file=sys.stderr)

    return result.stdout


def run_xlsx_extraction(xlsx_path):
    """Run nx_run --xlsx to extract raw rows."""
    cmd = [NX_RUN, "--xlsx", xlsx_path]
    result = subprocess.run(cmd, capture_output=True)
    if result.returncode != 0:
        print(f"  nx_run failed: {result.stderr.decode()}", file=sys.stderr)
        return None

    return result.stdout


def run_schema_transform(raw_json_bytes, schema_path):
    """Run nx_xform_run to apply Stage B transform."""
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False, mode="wb") as tmp:
        tmp.write(raw_json_bytes)
        tmp_path = tmp.name

    try:
        cmd = [NX_XFORM_RUN, tmp_path, schema_path]
        result = subprocess.run(cmd, capture_output=True)
        if result.returncode != 0:
            print(f"  Schema transform failed: {result.stderr.decode().strip()}",
                  file=sys.stderr)
            return None

        return result.stdout
    finally:
        os.unlink(tmp_path)


def process_source(filepath, schema_path=None, row_tol=None, col_gap=None):
    """Process a single source file through the pipeline."""
    fmt = detect_format(filepath)
    if not fmt:
        print(f"Error: cannot detect format of {filepath}", file=sys.stderr)
        return None, None

    basename = os.path.basename(filepath)
    print(f"\n[{basename}] format={fmt}", file=sys.stderr)

    raw_json = None

    if fmt == "pdf":
        # Stage A.1: PDF → text runs
        print("  Stage A.1: Extracting text runs with pdfplumber...",
              file=sys.stderr)
        with tempfile.NamedTemporaryFile(
                suffix=".json", delete=False, mode="w") as tmp:
            tmp_path = tmp.name

        try:
            extract_pdf_text(filepath, tmp_path)

            # Stage A.2: Text runs → raw rows
            print("  Stage A.2: Clustering into table rows...", file=sys.stderr)
            raw_json = run_pdf_clustering(tmp_path, row_tol, col_gap)
        finally:
            os.unlink(tmp_path)

    elif fmt == "pdf-json":
        # Already preprocessed — go straight to clustering
        print("  Stage A: Clustering text runs...", file=sys.stderr)
        raw_json = run_pdf_clustering(filepath, row_tol, col_gap)

    elif fmt == "xlsx":
        # Stage A: XLSX → raw rows
        print("  Stage A: Parsing XLSX...", file=sys.stderr)
        raw_json = run_xlsx_extraction(filepath)

    if raw_json is None:
        print(f"  FAILED: Stage A extraction", file=sys.stderr)
        return None, None

    # Parse raw to show summary
    try:
        raw = json.loads(raw_json)
        t = raw["tables"][0]
        print(f"  Raw: {t['row_count']} rows, {t['col_count']} cols",
              file=sys.stderr)
    except (json.JSONDecodeError, KeyError, IndexError):
        pass

    # Stage B: Schema transform (optional)
    canonical = None
    if schema_path:
        print(f"  Stage B: Applying schema {os.path.basename(schema_path)}...",
              file=sys.stderr)
        canonical = run_schema_transform(raw_json, schema_path)
        if canonical:
            try:
                canon = json.loads(canonical)
                audit = canon.get("audit", {})
                print(f"  Canonical: {audit.get('rows_accepted', '?')} accepted, "
                      f"{audit.get('rows_rejected', '?')} rejected",
                      file=sys.stderr)
            except (json.JSONDecodeError, KeyError):
                pass

    return raw_json, canonical


def run_batch(config_path):
    """Run batch processing from config file."""
    with open(config_path, "r") as f:
        config = json.load(f)

    base_dir = os.path.dirname(os.path.abspath(config_path))
    output_dir = config.get("output_dir", "./output")
    if not os.path.isabs(output_dir):
        output_dir = os.path.join(base_dir, output_dir)
    os.makedirs(output_dir, exist_ok=True)

    sources = config.get("sources", [])
    print(f"Batch: {len(sources)} sources, output to {output_dir}",
          file=sys.stderr)

    for source in sources:
        filepath = source["file"]
        if not os.path.isabs(filepath):
            filepath = os.path.join(base_dir, filepath)

        schema = source.get("schema")
        if schema and not os.path.isabs(schema):
            schema = os.path.join(base_dir, schema)

        pdf_opts = source.get("pdf_options", {})
        row_tol = pdf_opts.get("row_tolerance")
        col_gap = pdf_opts.get("col_gap_min")

        # Handle "auto" string values
        if row_tol == "auto":
            row_tol = None
        if col_gap == "auto":
            col_gap = None

        raw_json, canonical = process_source(
            filepath, schema, row_tol, col_gap)

        # Write outputs
        name = os.path.splitext(os.path.basename(filepath))[0]
        if raw_json:
            raw_path = os.path.join(output_dir, f"{name}_raw.json")
            with open(raw_path, "wb") as f:
                f.write(raw_json)
            print(f"  Wrote: {raw_path}", file=sys.stderr)

        if canonical:
            canon_path = os.path.join(output_dir, f"{name}_canonical.json")
            with open(canon_path, "wb") as f:
                f.write(canonical)
            print(f"  Wrote: {canon_path}", file=sys.stderr)

    print(f"\nBatch complete.", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(
        description="Nexus Document Ingestion Pipeline",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s input.pdf --schema schemas/gls-hu-automata-v1.json
  %(prog)s input.xlsx --schema schemas/gls-hu-depots-v1.json
  %(prog)s input.pdf --row-tol 1.0 --col-gap 4.0
  %(prog)s --config pipeline.json
""")
    parser.add_argument("input", nargs="?", help="Input file (PDF, XLSX, or text-run JSON)")
    parser.add_argument("--schema", "-s", help="Transform schema JSON file")
    parser.add_argument("--config", "-c", help="Batch config JSON file")
    parser.add_argument("--row-tol", type=float, help="PDF row tolerance (default: auto)")
    parser.add_argument("--col-gap", type=float, help="PDF column gap minimum (default: auto)")
    parser.add_argument("--output", "-o", help="Output file (default: stdout)")
    parser.add_argument("--raw", action="store_true",
                        help="Output raw JSON (default when no schema)")
    parser.add_argument("--canonical", action="store_true",
                        help="Output canonical JSON (default when schema given)")
    args = parser.parse_args()

    # Batch mode
    if args.config:
        find_tools()
        run_batch(args.config)
        return

    # Single file mode
    if not args.input:
        parser.print_help()
        sys.exit(1)

    find_tools()

    raw_json, canonical = process_source(
        args.input, args.schema, args.row_tol, args.col_gap)

    if raw_json is None:
        sys.exit(1)

    # Determine what to output
    if args.canonical and canonical:
        output = canonical
    elif args.schema and canonical and not args.raw:
        output = canonical
    else:
        output = raw_json

    # Write output
    if args.output:
        with open(args.output, "wb") as f:
            f.write(output)
        print(f"\nWrote: {args.output}", file=sys.stderr)
    else:
        sys.stdout.buffer.write(output)
        sys.stdout.buffer.write(b"\n")


if __name__ == "__main__":
    main()
