#!/usr/bin/env sh
# CI self-test for the reconciliation gate. Runs on committed SYNTHETIC fixtures
# only (client data is never committed). Proves two things:
#   1. positive: a correct schema reconciles clean (exit 0)
#   2. negative: a schema that silently truncates fractional data is CAUGHT
#      (reconciler exits 1) -- i.e. the gate has teeth.
set -eu

# The reconciler is a Python tool. Where python3 is not on PATH (e.g. the Windows
# CI runners), skip the gate rather than fail the build; it still runs everywhere
# python3 exists (Linux CI, dev machines, and every real ingest).
if ! command -v python3 >/dev/null 2>&1; then
    echo "skipping reconciliation self-test: python3 not found" >&2
    exit 0
fi

NEXUS_DIR="$(cd "$(dirname "$0")/.." && pwd)"
FIX="$NEXUS_DIR/tests/reconcile"
REC="$NEXUS_DIR/scripts/reconcile.py"

if [ ! -x "$NEXUS_DIR/nx_pipeline" ]; then
    echo "building nx_pipeline..." >&2
    make -C "$NEXUS_DIR" tools >/dev/null
fi

echo "== positive: good schema must reconcile clean =="
python3 "$REC" "$FIX/sample.csv" --schema "$FIX/sample-good.json"

echo
echo "== negative: bad (int-truncating) schema must be flagged =="
if python3 "$REC" "$FIX/sample.csv" --schema "$FIX/sample-bad.json" >/dev/null 2>&1; then
    echo "FAIL: reconciler passed a truncating schema -- the gate has no teeth" >&2
    exit 1
else
    echo "OK: reconciler correctly flagged the truncating schema (exit non-zero)"
fi

echo
echo "reconciliation self-test PASSED"
