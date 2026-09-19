#!/usr/bin/env sh
# CI self-test for the reconciliation gate. Runs on committed SYNTHETIC fixtures
# only (client data is never committed). Proves two things:
#   1. positive: a correct schema reconciles clean (exit 0)
#   2. negative: a schema that silently truncates fractional data is CAUGHT
#      (reconciler exits 1) -- i.e. the gate has teeth.
set -eu

# The reconciler is a Python tool. This used to exit 0 when python3 was absent,
# which is how the Windows runners reported a green reconciliation gate while
# running none of it -- on the one check whose entire purpose is to prove the
# reconciler CATCHES a schema that silently truncates data. It passes on
# Windows; the runners simply had no python3. Missing python3 is now an error.
if ! command -v python3 >/dev/null 2>&1; then
    echo "reconciliation self-test needs python3 on PATH" >&2
    exit 1
fi

NEXUS_DIR="$(cd "$(dirname "$0")/.." && pwd)"
FIX="$NEXUS_DIR/tests/reconcile"
REC="$NEXUS_DIR/scripts/reconcile.py"

# python3 on Windows is a native program and cannot read an MSYS path like
# /c/Users/...; it resolves it against the drive root and looks for
# C:/c/Users/... . MSYS normally rewrites arguments that look like paths, but
# scripts/msvc-env.sh exports MSYS_NO_PATHCONV=1 and MSYS2_ARG_CONV_EXCL='*'
# so that cl.exe receives /Fo: and /std:c11 verbatim -- and the windows-msvc
# job sources it before running the nexus suite. Converting here rather than
# relying on the ambient setting keeps this working under both.
if command -v cygpath >/dev/null 2>&1; then
    NEXUS_DIR="$(cygpath -m "$NEXUS_DIR")"
    FIX="$(cygpath -m "$FIX")"
    REC="$(cygpath -m "$REC")"
fi

# The Makefile writes `nx_pipeline`; on Windows the compiler appends `.exe`.
# Checking only the bare name meant this rebuilt on every run there, and the
# reconciler then could not find the binary it had just built.
if [ ! -x "$NEXUS_DIR/nx_pipeline" ] && [ ! -x "$NEXUS_DIR/nx_pipeline.exe" ]; then
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
