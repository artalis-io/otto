#!/bin/bash
#
# generate_instances.sh - Generate synthetic VRPTW and PDPTW benchmark instances
#
# Runs sg_gen_solomon and sg_gen_li_lim for a range of instance sizes.
# Must be run from the surge/ directory (or adjust paths accordingly).
#
# Makefile rules needed (add to surge/Makefile):
# -----------------------------------------------
# BENCH_GEN_SOL = sg_gen_solomon
# BENCH_GEN_PD = sg_gen_li_lim
# $(BENCH_GEN_SOL): benchmarks/sg_gen_solomon.c
# 	$(CC) $(CFLAGS) $< -lm -o $@
# $(BENCH_GEN_PD): benchmarks/sg_gen_li_lim.c
# 	$(CC) $(CFLAGS) $< -lm -o $@
# -----------------------------------------------

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SURGE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

SOL_BIN="${SURGE_DIR}/sg_gen_solomon"
PD_BIN="${SURGE_DIR}/sg_gen_li_lim"

# Verify binaries exist
if [ ! -x "$SOL_BIN" ]; then
    echo "Error: $SOL_BIN not found. Build it first (make sg_gen_solomon)." >&2
    exit 1
fi
if [ ! -x "$PD_BIN" ]; then
    echo "Error: $PD_BIN not found. Build it first (make sg_gen_li_lim)." >&2
    exit 1
fi

SIZES=(50 150 250 500 750 1500 2000)

echo "=== Generating benchmark instances ==="
echo "Sizes: ${SIZES[*]}"
echo ""

for size in "${SIZES[@]}"; do
    echo "--- Size: $size ---"
    "$SOL_BIN" --size "$size" --output-dir "${SURGE_DIR}/benchmarks/generated/vrptw/$size"
    "$PD_BIN"  --size "$size" --output-dir "${SURGE_DIR}/benchmarks/generated/pdptw/$size"
    echo ""
done

echo "=== All instances generated ==="
