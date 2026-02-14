#!/usr/bin/env python3
"""
nx_schema_gen.py - LLM-Assisted Schema Generation for Nexus

Reads nx_raw JSON output and uses Claude to generate a draft transform schema.
Runs ensemble of N calls, votes per field, and reports confidence scores.

Usage:
    python3 nx_schema_gen.py input_raw.json [options]

Options:
    --examples DIR      Directory with few-shot example pairs (default: schemas/examples/)
    --runs N            Number of ensemble runs (default: 5)
    --model MODEL       Claude model to use (default: claude-sonnet-4-5-20250929)
    --output FILE       Output file (default: stdout)
    --max-rows N        Max sample rows to send (default: 20)
    --output-type TYPE  Output type hint (e.g., "facility", "vehicle", "route")

Requires ANTHROPIC_API_KEY environment variable.
"""

import json
import sys
import os
import argparse
import hashlib
from datetime import datetime, timezone
from collections import Counter, defaultdict
from pathlib import Path

try:
    import anthropic
except ImportError:
    print("Error: anthropic package required. Install with: pip install anthropic", file=sys.stderr)
    sys.exit(1)


SYSTEM_PROMPT = """You are a data schema analyst for trucking logistics data.

Your job is to analyze raw tabular data (extracted from XLSX, PDF, or CSV) and generate
an nx_schema transform configuration that maps the raw columns to clean, canonical fields.

## Schema Format

Generate a JSON schema with this structure:
{
  "nx_schema": 2,
  "version": "<domain>-<region>-<entity>-v1",
  "output_type": "<type>",
  "multi_transforms": [...],  // Optional: split, merge, regex, compute, conditional
  "columns": [...],           // Required: 1:1 column mappings
  "derived": [...],           // Optional: constant fields
  "row_id": {...},            // Optional: record ID template
  "validate": [...]           // Optional: validation rules
}

## Available Transform Types

### 1:1 Column Mapping (in "columns" array)
- source: column index (0-based)
- target: output field name
- type: "string", "int", "double", "bool"
- transforms: ["trim", "lowercase", "uppercase", {"replace": ["from", "to"]}]
- required: true/false (reject row if empty)
- precision: decimal digits for doubles
- validate: {"min": N, "max": N}

### Multi-Transforms (in "multi_transforms" array)
- split: Split one column into multiple by delimiter or fixed width
- merge: Combine multiple columns with separator or template
- regex: Extract capture groups from a column
- compute: Apply built-in function (eov_to_wgs84, dms_to_dd, coalesce, phone_normalize, zip_to_region, opening_hours)
- conditional: Pattern-matched value assignment

### Validation Rules (in "validate" array)
- geo_bounds: Check lat/lon within bounding box
- format: Regex pattern validation
- unique: Uniqueness constraint on field(s)
- outlier: IQR-based outlier detection

## Guidelines

1. Use descriptive English field names (snake_case)
2. Detect coordinate columns and validate ranges (-90..90 for lat, -180..180 for lon)
3. Mark essential columns as required
4. Add trim transform to string fields
5. For Hungarian data: use Hungary geo bounds (45.7-48.6N, 16.1-22.9E)
6. For phone numbers: use phone_normalize compute function
7. For ZIP codes: validate format with regex
8. Generate a meaningful row_id template using key identifying fields
9. Add derived fields for constants (country, operator, facility_type)
10. Use multi_transforms when columns need splitting, merging, or conversion
"""

TOOL_SCHEMA = {
    "name": "generate_schema",
    "description": "Generate an nx_schema transform configuration for the given raw data",
    "input_schema": {
        "type": "object",
        "required": ["schema"],
        "properties": {
            "schema": {
                "type": "object",
                "description": "The complete nx_schema JSON object",
                "required": ["nx_schema", "version", "output_type", "columns"],
                "properties": {
                    "nx_schema": {"type": "integer", "enum": [1, 2]},
                    "version": {"type": "string"},
                    "output_type": {"type": "string"},
                    "table_selector": {"type": "object"},
                    "skip_rows": {"type": "integer"},
                    "multi_transforms": {"type": "array"},
                    "columns": {"type": "array"},
                    "derived": {"type": "array"},
                    "row_id": {"type": "object"},
                    "validate": {"type": "array"}
                }
            },
            "reasoning": {
                "type": "string",
                "description": "Brief explanation of schema design decisions"
            }
        }
    }
}


def load_raw_json(path: str, max_rows: int = 20) -> dict:
    """Load raw JSON and truncate to max_rows for the prompt."""
    with open(path) as f:
        data = json.load(f)

    # Truncate rows
    if "tables" in data:
        for table in data["tables"]:
            if "rows" in table and len(table["rows"]) > max_rows:
                table["rows"] = table["rows"][:max_rows]
                table["row_count"] = max_rows

    return data


def load_examples(examples_dir: str) -> list[dict]:
    """Load few-shot example pairs from directory."""
    examples = []
    examples_path = Path(examples_dir)

    if not examples_path.exists():
        return examples

    # Find pairs: *_raw_sample.json + *_schema.json
    for raw_file in sorted(examples_path.glob("*_raw_sample.json")):
        prefix = raw_file.name.replace("_raw_sample.json", "")
        schema_file = examples_path / f"{prefix}_schema.json"

        if schema_file.exists():
            with open(raw_file) as f:
                raw_data = json.load(f)
            with open(schema_file) as f:
                schema_data = json.load(f)
            examples.append({"raw": raw_data, "schema": schema_data, "name": prefix})

    return examples


def build_prompt(raw_data: dict, examples: list[dict], output_type: str | None) -> str:
    """Build the user prompt with sample data and few-shot examples."""
    parts = []

    # Few-shot examples
    if examples:
        parts.append("## Examples\n")
        for ex in examples:
            parts.append(f"### Example: {ex['name']}")
            parts.append(f"**Raw data (sample):**\n```json\n{json.dumps(ex['raw'], indent=2, ensure_ascii=False)[:2000]}\n```\n")
            parts.append(f"**Generated schema:**\n```json\n{json.dumps(ex['schema'], indent=2, ensure_ascii=False)}\n```\n")

    # Target data
    parts.append("## Your Task\n")
    parts.append("Analyze this raw data and generate an appropriate nx_schema:\n")
    parts.append(f"```json\n{json.dumps(raw_data, indent=2, ensure_ascii=False)}\n```\n")

    if output_type:
        parts.append(f"**Output type hint:** {output_type}\n")

    # Extract useful context
    if "tables" in raw_data and raw_data["tables"]:
        table = raw_data["tables"][0]
        headers = table.get("headers", [])
        row_count = table.get("row_count", 0)
        col_count = table.get("col_count", 0)
        parts.append(f"**Headers ({col_count} columns):** {headers}")
        parts.append(f"**Row count:** {row_count}")

    if "source" in raw_data:
        fmt = raw_data["source"].get("format", "unknown")
        fname = raw_data["source"].get("filename", "unknown")
        parts.append(f"**Source:** {fname} ({fmt})")

    parts.append("\nCall the generate_schema tool with the complete schema JSON.")

    return "\n".join(parts)


def run_single(client: anthropic.Anthropic, model: str, system: str,
               user_prompt: str) -> dict | None:
    """Run a single schema generation call."""
    try:
        response = client.messages.create(
            model=model,
            max_tokens=4096,
            system=system,
            tools=[TOOL_SCHEMA],
            tool_choice={"type": "tool", "name": "generate_schema"},
            messages=[{"role": "user", "content": user_prompt}]
        )

        for block in response.content:
            if block.type == "tool_use" and block.name == "generate_schema":
                return block.input

    except Exception as e:
        print(f"  Warning: API call failed: {e}", file=sys.stderr)

    return None


def vote_schemas(results: list[dict]) -> dict:
    """Merge ensemble results with majority voting per field."""
    if not results:
        return {}

    schemas = [r["schema"] for r in results if "schema" in r]
    if not schemas:
        return {}

    # Use first schema as base, vote on individual fields
    base = schemas[0].copy()

    # Vote on columns
    column_votes = defaultdict(Counter)
    for schema in schemas:
        for col in schema.get("columns", []):
            target = col.get("target", "")
            if target:
                # Vote on source index for this target
                column_votes[target]["source:" + str(col.get("source", -1))] += 1
                column_votes[target]["type:" + col.get("type", "string")] += 1
                column_votes[target]["required:" + str(col.get("required", False))] += 1

    # Compute per-field confidence
    n = len(schemas)
    field_confidence = {}
    for target, votes in column_votes.items():
        # Max votes for any single source/type/required combination
        max_source = max(v for k, v in votes.items() if k.startswith("source:"))
        max_type = max(v for k, v in votes.items() if k.startswith("type:"))
        # Average agreement across dimensions
        conf = (max_source / n + max_type / n) / 2
        field_confidence[target] = {
            "confidence": round(conf, 2),
            "votes": f"{max_source}/{n}"
        }

    # Overall confidence
    if field_confidence:
        overall = sum(f["confidence"] for f in field_confidence.values()) / len(field_confidence)
    else:
        overall = 0.0

    return {
        "schema": base,
        "confidence": {
            "overall": round(overall, 2),
            "fields": field_confidence
        },
        "ensemble_size": n
    }


def main():
    parser = argparse.ArgumentParser(description="LLM-assisted schema generation for Nexus")
    parser.add_argument("input", help="Path to nx_raw JSON file")
    parser.add_argument("--examples", default="schemas/examples/",
                        help="Directory with few-shot example pairs")
    parser.add_argument("--runs", type=int, default=5,
                        help="Number of ensemble runs (default: 5)")
    parser.add_argument("--model", default="claude-sonnet-4-5-20250929",
                        help="Claude model to use")
    parser.add_argument("--output", "-o", default=None,
                        help="Output file (default: stdout)")
    parser.add_argument("--max-rows", type=int, default=20,
                        help="Max sample rows to send (default: 20)")
    parser.add_argument("--output-type", default=None,
                        help="Output type hint (e.g., facility, vehicle)")

    args = parser.parse_args()

    # Check API key
    api_key = os.environ.get("ANTHROPIC_API_KEY")
    if not api_key:
        print("Error: ANTHROPIC_API_KEY environment variable required", file=sys.stderr)
        sys.exit(1)

    # Load data
    print(f"Loading {args.input}...", file=sys.stderr)
    raw_data = load_raw_json(args.input, args.max_rows)

    # Compute source hash
    with open(args.input, "rb") as f:
        source_sha = hashlib.sha256(f.read()).hexdigest()

    # Load examples
    examples = load_examples(args.examples)
    print(f"Loaded {len(examples)} few-shot example(s)", file=sys.stderr)

    # Build prompt
    user_prompt = build_prompt(raw_data, examples, args.output_type)
    prompt_sha = hashlib.sha256(user_prompt.encode()).hexdigest()

    # Run ensemble
    client = anthropic.Anthropic(api_key=api_key)
    results = []

    print(f"Running {args.runs} ensemble calls with {args.model}...", file=sys.stderr)
    for i in range(args.runs):
        print(f"  Run {i+1}/{args.runs}...", file=sys.stderr, end="", flush=True)
        result = run_single(client, args.model, SYSTEM_PROMPT, user_prompt)
        if result:
            results.append(result)
            print(" done", file=sys.stderr)
        else:
            print(" failed", file=sys.stderr)

    if not results:
        print("Error: All ensemble runs failed", file=sys.stderr)
        sys.exit(1)

    print(f"Got {len(results)}/{args.runs} successful results", file=sys.stderr)

    # Vote and merge
    merged = vote_schemas(results)

    # Build output
    output = {
        "nx_schema_draft": 1,
        "generated_by": args.model,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "source_sha256": source_sha,
        "prompt_sha256": prompt_sha,
        "confidence": merged.get("confidence", {}),
        "ensemble_size": merged.get("ensemble_size", 0),
        "schema": merged.get("schema", {})
    }

    # Write output
    output_str = json.dumps(output, indent=2, ensure_ascii=False)

    if args.output:
        with open(args.output, "w") as f:
            f.write(output_str + "\n")
        print(f"Draft schema written to {args.output}", file=sys.stderr)
    else:
        print(output_str)

    # Print confidence summary
    confidence = merged.get("confidence", {})
    overall = confidence.get("overall", 0)
    print(f"\nOverall confidence: {overall:.0%}", file=sys.stderr)

    fields = confidence.get("fields", {})
    for field, info in sorted(fields.items()):
        conf = info["confidence"]
        votes = info["votes"]
        marker = " ***" if conf < 0.6 else ""
        print(f"  {field:20s} {conf:.0%} ({votes}){marker}", file=sys.stderr)

    if any(f["confidence"] < 0.6 for f in fields.values()):
        print("\n*** = low confidence, review carefully", file=sys.stderr)


if __name__ == "__main__":
    main()
