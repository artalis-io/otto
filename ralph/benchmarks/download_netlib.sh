#!/bin/bash
#
# Download NETLIB LP benchmark problems in standard MPS format
#
# These are classic LP test problems from the NETLIB collection.
# Pre-converted to standard MPS format (the original NETLIB uses
# compressed emps format which requires special decompression).
#
# Source: JuliaSmoothOptimizers/OptimizationProblems
# Alternative: Hans Mittelmann's benchmark collection

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NETLIB_DIR="$SCRIPT_DIR/netlib"

# Use JuliaSmoothOptimizers as source (has standard MPS format)
BASE_URL="https://raw.githubusercontent.com/JuliaSmoothOptimizers/OptimizationProblems.jl/main/src/lp_problems/netlib"

# Alternatively use Hans Mittelmann's collection
MITTEL_URL="https://plato.asu.edu/ftp/lptestset"

# Create output directory
mkdir -p "$NETLIB_DIR"

# List of problems to download (curated for LP testing)
# These are the classic NETLIB problems available in standard MPS format
PROBLEMS=(
    # Tiny problems (< 100 vars) - quick smoke tests
    "afiro"
    "sc50a"
    "sc50b"
    "kb2"
    "sc105"

    # Small problems (100-500 vars) - fast iteration
    "adlittle"
    "blend"
    "share2b"
    "lotfi"
    "scagr7"
    "israel"

    # Medium problems (500-2000 vars) - moderate challenge
    "agg"
    "bandm"
    "brandy"
    "e226"
    "scorpion"
    "scsd1"

    # Numerically challenging problems
    "bore3d"
    "capri"
    "degen2"
    "etamacro"
    "finnis"
    "grow7"
    "grow15"
    "perold"
    "recipe"
    "sc205"
    "seba"
    "share1b"
    "shell"
    "stair"
)

echo "Downloading NETLIB LP problems to: $NETLIB_DIR"
echo ""

downloaded=0
skipped=0
failed=0

for prob in "${PROBLEMS[@]}"; do
    outfile="$NETLIB_DIR/${prob}.mps"

    # Skip if already exists and is valid
    if [ -f "$outfile" ] && grep -q "^NAME" "$outfile" 2>/dev/null; then
        echo "  [skip] $prob (already exists)"
        ((skipped++))
        continue
    fi

    echo "  [download] $prob..."

    # Try JuliaSmoothOptimizers repo first
    url="${BASE_URL}/${prob}.mps"
    if curl -sfL "$url" -o "$outfile.tmp" 2>/dev/null; then
        # Verify it looks like valid MPS
        if grep -q "^NAME\|^ROWS\|^COLUMNS" "$outfile.tmp" 2>/dev/null; then
            mv "$outfile.tmp" "$outfile"
            ((downloaded++))
            continue
        fi
        rm -f "$outfile.tmp"
    fi

    # Try Mittelmann collection
    url="${MITTEL_URL}/${prob}.mps"
    if curl -sfL "$url" -o "$outfile.tmp" 2>/dev/null; then
        if grep -q "^NAME\|^ROWS\|^COLUMNS" "$outfile.tmp" 2>/dev/null; then
            mv "$outfile.tmp" "$outfile"
            ((downloaded++))
            continue
        fi
        rm -f "$outfile.tmp"
    fi

    # Try Mittelmann .mps.gz
    url="${MITTEL_URL}/${prob}.mps.gz"
    if curl -sfL "$url" -o "$outfile.gz" 2>/dev/null; then
        if gunzip -f "$outfile.gz" 2>/dev/null; then
            if grep -q "^NAME\|^ROWS\|^COLUMNS" "$outfile" 2>/dev/null; then
                ((downloaded++))
                continue
            fi
        fi
        rm -f "$outfile.gz" "$outfile" 2>/dev/null
    fi

    echo "    [failed] Could not download $prob"
    ((failed++))
    rm -f "$outfile.tmp" "$outfile.gz" 2>/dev/null
done

# Clean up any invalid files (from previous broken downloads)
for f in "$NETLIB_DIR"/*.mps; do
    [ -f "$f" ] || continue
    if ! grep -q "^NAME\|^ROWS\|^COLUMNS" "$f" 2>/dev/null; then
        echo "  [remove] $(basename "$f") (invalid format)"
        rm -f "$f"
    fi
done

echo ""
echo "Summary:"
echo "  Downloaded: $downloaded"
echo "  Skipped:    $skipped (already exist)"
echo "  Failed:     $failed"
echo ""

valid_count=$(ls -1 "$NETLIB_DIR"/*.mps 2>/dev/null | while read f; do
    grep -q "^NAME\|^ROWS\|^COLUMNS" "$f" 2>/dev/null && echo "$f"
done | wc -l | tr -d ' ')

echo "Total valid problems available: $valid_count"
echo ""

if [ "$downloaded" -gt 0 ] || [ "$skipped" -gt 0 ]; then
    echo "Run benchmarks with:"
    echo "  ./ralph-benchmark --suite tiny      # Quick test (5 problems)"
    echo "  ./ralph-benchmark --suite small     # Fast test (15 problems)"
    echo "  ./ralph-benchmark --suite all       # Full suite"
    echo ""
fi

exit 0
