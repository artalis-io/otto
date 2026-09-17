#!/usr/bin/env sh
# Ingest gate: transform a client file AND reconcile the result against the raw
# bytes in one step. Reconciliation is a HARD gate -- if any verifiable field in
# the canonical output diverges from a faithful re-execution of the schema
# against the raw cells, this exits non-zero and the ingest is rejected.
#
# Wire this into every ingest instead of calling nx_pipeline directly.
#
#   scripts/ingest.sh <input> <schema> [output.json]
#
# Exit: 0 = transformed and reconciled clean; 1 = reconciliation mismatch;
#       2 = setup error (tool/args).
set -eu

if [ $# -lt 2 ]; then
    echo "usage: $0 <input> <schema> [output.json]" >&2
    exit 2
fi

INPUT="$1"
SCHEMA="$2"
OUT="${3:-}"

NEXUS_DIR="$(cd "$(dirname "$0")/.." && pwd)"
NX="$NEXUS_DIR/nx_pipeline"

if [ ! -x "$NX" ]; then
    echo "nx_pipeline not built: run 'make -C $NEXUS_DIR tools' first" >&2
    exit 2
fi

# 1. Transform (Stage A->X). Write output if requested, else discard.
if [ -n "$OUT" ]; then
    "$NX" "$INPUT" --schema "$SCHEMA" -o "$OUT"
else
    "$NX" "$INPUT" --schema "$SCHEMA" >/dev/null
fi

# 2. Gate: reconcile canonical output back to raw. Non-zero exit fails the ingest.
echo "--- reconciliation gate ---"
python3 "$NEXUS_DIR/scripts/reconcile.py" "$INPUT" --schema "$SCHEMA"

# 3. Semantic sanity summary (NON-BLOCKING): flags values that are faithful to
#    the raw but implausible (bad time windows, density, GVW, duplicates). Does
#    not affect the ingest's exit status -- the reconciliation gate above is the
#    hard gate; this is advisory.
echo "--- semantic sanity (advisory) ---"
python3 "$NEXUS_DIR/scripts/semantic_checks.py" "$INPUT" --schema "$SCHEMA" || true
