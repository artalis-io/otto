#!/usr/bin/env python3
"""
pdf-to-text-json.py - PDF to Intermediate JSON Preprocessor

Extracts positioned text runs from PDF files using pdfplumber.
Outputs JSON with text + (x, y, w, h) coordinates for each text run,
suitable for consumption by nx_pdf.c table reconstructor.

Usage:
    python3 pdf-to-text-json.py input.pdf > output.json
    python3 pdf-to-text-json.py input.pdf -o output.json

Requirements:
    pip install pdfplumber

Output format:
{
  "pages": [
    {
      "page": 1,
      "width": 595.0,
      "height": 842.0,
      "texts": [
        {"text": "Budapest", "x": 50.0, "y": 100.0, "w": 80.0, "h": 12.0}
      ]
    }
  ]
}
"""

import argparse
import json
import sys

try:
    import pdfplumber
except ImportError:
    print("Error: pdfplumber not installed. Run: pip install pdfplumber",
          file=sys.stderr)
    sys.exit(1)


def extract_text_runs(pdf_path):
    """Extract positioned text runs from all pages of a PDF."""
    pages = []

    with pdfplumber.open(pdf_path) as pdf:
        for i, page in enumerate(pdf.pages):
            page_data = {
                "page": i + 1,
                "width": float(page.width),
                "height": float(page.height),
                "texts": []
            }

            chars = page.chars
            if not chars:
                pages.append(page_data)
                continue

            # Group characters into words using pdfplumber's built-in method
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

    return {"pages": pages}


def main():
    parser = argparse.ArgumentParser(
        description="Extract positioned text runs from PDF to JSON")
    parser.add_argument("input", help="Input PDF file")
    parser.add_argument("-o", "--output", help="Output JSON file (default: stdout)")
    args = parser.parse_args()

    result = extract_text_runs(args.input)

    output = json.dumps(result, ensure_ascii=False, indent=2)

    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(output)
            f.write("\n")
    else:
        print(output)


if __name__ == "__main__":
    main()
