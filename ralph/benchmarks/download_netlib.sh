#!/bin/bash
#
# Download ALL ~90 NETLIB LP benchmark problems in standard MPS format
#
# Downloads compressed files from netlib.org and decompresses them using
# the 'emps' tool (built from source if not present).
#
# Source: https://www.netlib.org/lp/data/
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NETLIB_DIR="$SCRIPT_DIR/netlib"
EMPS_DIR="$SCRIPT_DIR/.emps"
EMPS_BIN="$EMPS_DIR/emps"
NETLIB_URL="https://www.netlib.org/lp/data"

# Full list of ~90 NETLIB LP problems
PROBLEMS=(
    # Tier 0: tiny (<100 vars)
    afiro
    sc50a
    sc50b
    kb2
    sc105
    blend
    share2b
    recipe

    # Tier 1: small (100-500 vars)
    adlittle
    lotfi
    scagr7
    israel
    scorpion
    brandy
    bandm
    beaconfd
    e226
    stocfor1
    sc205
    agg
    agg2
    agg3
    bore3d
    capri
    share1b
    scagr25

    # Tier 2: medium (500-2000 vars)
    bnl1
    degen2
    grow7
    grow15
    grow22
    scfxm1
    scfxm2
    scfxm3
    scsd1
    scsd6
    scsd8
    sctap1
    sctap2
    sctap3
    ship04s
    ship04l
    ship08s
    ship08l
    ship12s
    ship12l
    etamacro
    finnis
    perold
    stair
    shell
    seba
    forplan
    ganges
    sierra
    standata
    standmps
    nesm
    fffff800

    # Tier 3: large (2000+ vars)
    bnl2
    degen3
    pilot
    pilot87
    pilot.ja
    pilot.we
    pilot4
    pilotnov
    maros
    d2q06c
    stocfor2
    cycle
    czprob
    25fv47
    woodw
    wood1p

    # Tier 4: xlarge
    80bau3b
    fit1d
    fit1p
    fit2d
    fit2p
    maros-r7
    stocfor3
    greenbea
    greenbeb
    truss
    d6cube
    ken-07
    ken-11
    ken-13
    ken-18
)

# Build emps decompressor if not present
build_emps() {
    if [ -x "$EMPS_BIN" ]; then
        return 0
    fi

    echo "Building emps decompressor..."
    mkdir -p "$EMPS_DIR"

    # Download emps.c from netlib.org
    if ! curl -sfL "$NETLIB_URL/emps.c" -o "$EMPS_DIR/emps.c"; then
        echo "  ERROR: Failed to download emps.c from $NETLIB_URL/emps.c"
        return 1
    fi

    # Compile it
    if cc -O2 -o "$EMPS_BIN" "$EMPS_DIR/emps.c" -lm 2>/dev/null; then
        echo "  Built: $EMPS_BIN"
        return 0
    fi

    # Try with fewer flags
    if gcc -o "$EMPS_BIN" "$EMPS_DIR/emps.c" -lm 2>/dev/null; then
        echo "  Built: $EMPS_BIN"
        return 0
    fi

    echo "  ERROR: Failed to compile emps.c"
    return 1
}

# Download and decompress a single problem
download_problem() {
    local prob="$1"
    local outfile="$NETLIB_DIR/${prob}.mps"

    # Skip if already exists and is valid
    if [ -f "$outfile" ] && grep -q "ROWS" "$outfile" 2>/dev/null; then
        echo "  [skip] $prob"
        return 2  # 2 = skipped
    fi

    # Try netlib.org with emps decompression
    if [ -x "$EMPS_BIN" ]; then
        if curl -sfL "$NETLIB_URL/$prob" -o "$outfile.emps" 2>/dev/null; then
            if "$EMPS_BIN" < "$outfile.emps" > "$outfile.tmp" 2>/dev/null; then
                if grep -q "ROWS" "$outfile.tmp" 2>/dev/null; then
                    mv "$outfile.tmp" "$outfile"
                    rm -f "$outfile.emps"
                    echo "  [ok]   $prob"
                    return 0
                fi
            fi
            rm -f "$outfile.emps" "$outfile.tmp" 2>/dev/null
        fi
    fi

    # Fallback: try direct MPS from alternative sources
    local alt_urls=(
        "https://raw.githubusercontent.com/JuliaSmoothOptimizers/OptimizationProblems.jl/main/src/lp_problems/netlib/${prob}.mps"
    )

    for url in "${alt_urls[@]}"; do
        if curl -sfL "$url" -o "$outfile.tmp" 2>/dev/null; then
            if grep -q "ROWS" "$outfile.tmp" 2>/dev/null; then
                mv "$outfile.tmp" "$outfile"
                echo "  [ok]   $prob (alt source)"
                return 0
            fi
            rm -f "$outfile.tmp"
        fi
    done

    echo "  [FAIL] $prob"
    rm -f "$outfile.tmp" "$outfile.emps" 2>/dev/null
    return 1
}

# Main
echo "NETLIB LP Problem Downloader"
echo "============================"
echo ""

mkdir -p "$NETLIB_DIR"

# Build emps
if ! build_emps; then
    echo ""
    echo "WARNING: emps not available, will try alternative sources"
    echo ""
fi

echo ""
echo "Downloading ${#PROBLEMS[@]} problems to: $NETLIB_DIR"
echo ""

downloaded=0
skipped=0
failed=0
fail_list=""

for prob in "${PROBLEMS[@]}"; do
    download_problem "$prob" && ret=0 || ret=$?
    if [ $ret -eq 0 ]; then
        ((downloaded++)) || true
    elif [ $ret -eq 2 ]; then
        ((skipped++)) || true
    else
        ((failed++)) || true
        fail_list="$fail_list $prob"
    fi
done

# Validate existing files
echo ""
echo "Validating..."
invalid=0
for f in "$NETLIB_DIR"/*.mps; do
    [ -f "$f" ] || continue
    if ! grep -q "ROWS" "$f" 2>/dev/null; then
        echo "  [remove] $(basename "$f") (invalid)"
        rm -f "$f"
        ((invalid++)) || true
    fi
done
if [ $invalid -eq 0 ]; then
    echo "  All files valid"
fi

# Count total
total=$(ls -1 "$NETLIB_DIR"/*.mps 2>/dev/null | wc -l | tr -d ' ')

echo ""
echo "Summary"
echo "-------"
echo "  Downloaded: $downloaded"
echo "  Skipped:    $skipped (already exist)"
echo "  Failed:     $failed"
if [ -n "$fail_list" ]; then
    echo "  Failed:    $fail_list"
fi
echo "  Total:      $total valid MPS files"
echo ""

if [ "$total" -gt 0 ]; then
    echo "Run tests with:"
    echo "  make -C .. test-netlib       # Fast correctness test (tiers 0-1)"
    echo "  make -C .. test-netlib-full  # Full test (all tiers)"
    echo ""
fi

exit 0
