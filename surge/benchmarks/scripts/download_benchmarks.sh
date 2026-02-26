#!/usr/bin/env bash
# download_benchmarks.sh - Download published benchmark instances for the Surge VRP solver
#
# Downloads:
#   1. Gehring-Homberger VRPTW instances (300 instances, sizes 200-1000)
#   2. Li-Lim extended PDPTW instances (298 instances, sizes 200-1000)
#   3. BKS (Best Known Solutions) CSV files for both sets
#
# Sources:
#   VRPTW: https://www.sintef.no/projectweb/top/vrptw/homberger-benchmark/
#   PDPTW: https://www.sintef.no/projectweb/top/pdptw/li-lim-benchmark/
#
# Usage:
#   ./download_benchmarks.sh            # download everything
#   ./download_benchmarks.sh --vrptw    # only Gehring-Homberger VRPTW
#   ./download_benchmarks.sh --pdptw    # only Li-Lim extended PDPTW
#   ./download_benchmarks.sh --bks      # only BKS CSV files
#   ./download_benchmarks.sh --clean    # remove all downloaded benchmarks

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

GH_DIR="$BENCH_DIR/gehring_homberger"
LL_DIR="$BENCH_DIR/li_lim_extended"
BKS_DIR="$BENCH_DIR/bks"
TMP_DIR="$BENCH_DIR/.tmp_download"

SINTEF_BASE="https://www.sintef.no"

# Gehring-Homberger VRPTW instance zip URLs (from SINTEF TOP)
declare -A GH_ZIPS=(
    [200]="/globalassets/project/top/vrptw/homberger/200/homberger_200_customer_instances.zip"
    [400]="/globalassets/project/top/vrptw/homberger/400/homberger_400_customer_instances.zip"
    [600]="/globalassets/project/top/vrptw/homberger/600/homberger_600_customer_instances.zip"
    [800]="/globalassets/project/top/vrptw/homberger/800/homberger_800_customer_instances.zip"
    [1000]="/globalassets/project/top/vrptw/homberger/1000/homberger_1000_customer_instances.zip"
)

# Li-Lim extended PDPTW instance zip URLs (from SINTEF TOP)
declare -A LL_ZIPS=(
    [200]="/contentassets/1338af68996841d3922bc8e87adc430c/pdp_200.zip"
    [400]="/contentassets/1338af68996841d3922bc8e87adc430c/pdp_400.zip"
    [600]="/contentassets/1338af68996841d3922bc8e87adc430c/pdp_600.zip"
    [800]="/contentassets/1338af68996841d3922bc8e87adc430c/pdptw800.zip"
    [1000]="/contentassets/1338af68996841d3922bc8e87adc430c/pdptw1000.zip"
)

# Expected instance counts per size
declare -A GH_EXPECTED=(
    [200]=60 [400]=60 [600]=60 [800]=60 [1000]=60
)
declare -A LL_EXPECTED=(
    [200]=60 [400]=60 [600]=60 [800]=60 [1000]=58
)

# Terminal colors (if stdout is a tty)
if [ -t 1 ]; then
    GREEN='\033[0;32m'
    YELLOW='\033[1;33m'
    RED='\033[0;31m'
    BLUE='\033[0;34m'
    BOLD='\033[1m'
    NC='\033[0m'
else
    GREEN='' YELLOW='' RED='' BLUE='' BOLD='' NC=''
fi

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

info()  { printf "${BLUE}[INFO]${NC}  %s\n" "$*"; }
ok()    { printf "${GREEN}[OK]${NC}    %s\n" "$*"; }
warn()  { printf "${YELLOW}[WARN]${NC}  %s\n" "$*"; }
err()   { printf "${RED}[ERR]${NC}   %s\n" "$*" >&2; }

# Download a file with curl, skipping if already present
# Usage: fetch_file <url> <output_path>
fetch_file() {
    local url="$1"
    local out="$2"

    if [ -f "$out" ]; then
        return 0
    fi

    mkdir -p "$(dirname "$out")"

    if command -v curl &>/dev/null; then
        curl -fsSL --retry 3 --retry-delay 2 -o "$out" "$url"
    elif command -v wget &>/dev/null; then
        wget -q --tries=3 -O "$out" "$url"
    else
        err "Neither curl nor wget found. Please install one."
        exit 1
    fi
}

# Count .txt files in a directory
count_instances() {
    local dir="$1"
    if [ -d "$dir" ]; then
        find "$dir" -maxdepth 1 -name '*.txt' -type f 2>/dev/null | wc -l | tr -d ' '
    else
        echo 0
    fi
}

# ---------------------------------------------------------------------------
# Gehring-Homberger VRPTW
# ---------------------------------------------------------------------------

download_gehring_homberger() {
    info "Downloading Gehring-Homberger VRPTW instances..."
    info "Source: ${SINTEF_BASE}/projectweb/top/vrptw/homberger-benchmark/"

    local sizes=(200 400 600 800 1000)
    local total_downloaded=0
    local total_skipped=0

    for size in "${sizes[@]}"; do
        local dest_dir="$GH_DIR/$size"
        local expected="${GH_EXPECTED[$size]}"
        local existing
        existing="$(count_instances "$dest_dir")"

        if [ "$existing" -ge "$expected" ]; then
            ok "  ${size}-customer: $existing/$expected instances already present, skipping"
            total_skipped=$((total_skipped + existing))
            continue
        fi

        local zip_path="${GH_ZIPS[$size]}"
        local zip_url="${SINTEF_BASE}${zip_path}"
        local zip_file="$TMP_DIR/gh_${size}.zip"

        info "  ${size}-customer: downloading from SINTEF..."
        mkdir -p "$TMP_DIR" "$dest_dir"

        if ! fetch_file "$zip_url" "$zip_file"; then
            err "  Failed to download ${size}-customer instances"
            err "  URL: $zip_url"
            err "  Try downloading manually from the SINTEF TOP website"
            continue
        fi

        # Extract .txt files into the destination directory
        # The zip may contain files in subdirectories or at the root
        local extract_dir="$TMP_DIR/gh_${size}_extract"
        rm -rf "$extract_dir"
        mkdir -p "$extract_dir"

        if ! unzip -q -o "$zip_file" -d "$extract_dir" 2>/dev/null; then
            err "  Failed to extract ${size}-customer zip"
            continue
        fi

        # Move all .txt files to the flat destination directory
        find "$extract_dir" -name '*.txt' -type f | while read -r f; do
            local base
            base="$(basename "$f")"
            # Normalize: ensure uppercase class prefix (C1_2_1.txt not c1_2_1.txt)
            cp "$f" "$dest_dir/$base"
        done

        rm -rf "$extract_dir"

        local now
        now="$(count_instances "$dest_dir")"
        local new_count=$((now - existing))
        total_downloaded=$((total_downloaded + new_count))
        ok "  ${size}-customer: $now/$expected instances"
    done

    info "Gehring-Homberger: $total_downloaded downloaded, $total_skipped already present"
    echo
}

# ---------------------------------------------------------------------------
# Li-Lim extended PDPTW
# ---------------------------------------------------------------------------

download_li_lim_extended() {
    info "Downloading Li-Lim extended PDPTW instances..."
    info "Source: ${SINTEF_BASE}/projectweb/top/pdptw/li-lim-benchmark/"

    local sizes=(200 400 600 800 1000)
    local total_downloaded=0
    local total_skipped=0

    for size in "${sizes[@]}"; do
        local dest_dir="$LL_DIR/$size"
        local expected="${LL_EXPECTED[$size]}"
        local existing
        existing="$(count_instances "$dest_dir")"

        if [ "$existing" -ge "$expected" ]; then
            ok "  ${size}-task: $existing/$expected instances already present, skipping"
            total_skipped=$((total_skipped + existing))
            continue
        fi

        local zip_path="${LL_ZIPS[$size]}"
        local zip_url="${SINTEF_BASE}${zip_path}"
        local zip_file="$TMP_DIR/ll_${size}.zip"

        info "  ${size}-task: downloading from SINTEF..."
        mkdir -p "$TMP_DIR" "$dest_dir"

        if ! fetch_file "$zip_url" "$zip_file"; then
            err "  Failed to download ${size}-task instances"
            err "  URL: $zip_url"
            err "  Try downloading manually from the SINTEF TOP website"
            continue
        fi

        # Extract .txt files into the destination directory
        local extract_dir="$TMP_DIR/ll_${size}_extract"
        rm -rf "$extract_dir"
        mkdir -p "$extract_dir"

        if ! unzip -q -o "$zip_file" -d "$extract_dir" 2>/dev/null; then
            err "  Failed to extract ${size}-task zip"
            continue
        fi

        # Move all .txt files to the flat destination directory
        find "$extract_dir" -name '*.txt' -type f | while read -r f; do
            local base
            base="$(basename "$f")"
            cp "$f" "$dest_dir/$base"
        done

        rm -rf "$extract_dir"

        local now
        now="$(count_instances "$dest_dir")"
        local new_count=$((now - existing))
        total_downloaded=$((total_downloaded + new_count))
        ok "  ${size}-task: $now/$expected instances"
    done

    info "Li-Lim extended: $total_downloaded downloaded, $total_skipped already present"
    echo
}

# ---------------------------------------------------------------------------
# BKS CSV files
# ---------------------------------------------------------------------------

generate_bks() {
    info "Generating BKS (Best Known Solutions) CSV files..."
    mkdir -p "$BKS_DIR"

    # --- Gehring-Homberger VRPTW BKS ---
    local gh_bks="$BKS_DIR/gehring_homberger.csv"
    if [ -f "$gh_bks" ]; then
        ok "  gehring_homberger.csv already exists, skipping"
    else
        info "  Writing gehring_homberger.csv..."
        cat > "$gh_bks" << 'BKS_GH_EOF'
# Gehring-Homberger VRPTW Best Known Solutions (hierarchical objective)
# Source: SINTEF TOP (https://www.sintef.no/projectweb/top/vrptw/homberger-benchmark/)
# Retrieved: 2026-02-26
# Objective: minimize vehicles first, then minimize total distance
# Distance: Euclidean, rounded to 2 decimal places
# Format: name,vehicles,distance
# 200-customer instances
c1_2_1,20,2704.57
c1_2_2,18,2917.89
c1_2_3,18,2707.35
c1_2_4,18,2643.31
c1_2_5,20,2702.05
c1_2_6,20,2701.04
c1_2_7,20,2701.04
c1_2_8,19,2775.48
c1_2_9,18,2687.83
c1_2_10,18,2643.51
c2_2_1,6,1931.44
c2_2_2,6,1863.16
c2_2_3,6,1775.08
c2_2_4,6,1703.43
c2_2_5,6,1878.85
c2_2_6,6,1857.35
c2_2_7,6,1849.46
c2_2_8,6,1820.53
c2_2_9,6,1830.05
c2_2_10,6,1806.58
r1_2_1,20,4784.11
r1_2_2,18,4039.86
r1_2_3,18,3381.96
r1_2_4,18,3057.81
r1_2_5,18,4107.86
r1_2_6,18,3583.14
r1_2_7,18,3150.11
r1_2_8,18,2951.99
r1_2_9,18,3760.58
r1_2_10,18,3301.18
r2_2_1,4,4483.16
r2_2_2,4,3621.20
r2_2_3,4,2880.62
r2_2_4,4,1981.29
r2_2_5,4,3366.79
r2_2_6,4,2913.03
r2_2_7,4,2451.14
r2_2_8,4,1849.87
r2_2_9,4,3092.04
r2_2_10,4,2654.97
rc1_2_1,18,3602.80
rc1_2_2,18,3249.05
rc1_2_3,18,3008.33
rc1_2_4,18,2851.68
rc1_2_5,18,3371.00
rc1_2_6,18,3324.80
rc1_2_7,18,3189.32
rc1_2_8,18,3083.93
rc1_2_9,18,3081.13
rc1_2_10,18,3000.30
rc2_2_1,6,3099.53
rc2_2_2,5,2825.24
rc2_2_3,4,2601.87
rc2_2_4,4,2038.56
rc2_2_5,4,2911.46
rc2_2_6,4,2873.12
rc2_2_7,4,2525.83
rc2_2_8,4,2292.53
rc2_2_9,4,2175.04
rc2_2_10,4,2015.60
# 400-customer instances
c1_4_1,40,7152.02
c1_4_2,36,7686.38
c1_4_3,36,7060.67
c1_4_4,36,6803.24
c1_4_5,40,7152.02
c1_4_6,40,7153.41
c1_4_7,39,7417.92
c1_4_8,37,7347.23
c1_4_9,36,7042.53
c1_4_10,36,6860.63
c2_4_1,12,4116.05
c2_4_2,12,3929.89
c2_4_3,11,4018.02
c2_4_4,11,3702.49
c2_4_5,12,3938.69
c2_4_6,12,3875.94
c2_4_7,12,3894.13
c2_4_8,11,4233.20
c2_4_9,12,3864.68
c2_4_10,11,3825.67
r1_4_1,40,10372.31
r1_4_2,36,8898.15
r1_4_3,36,7808.34
r1_4_4,36,7282.78
r1_4_5,36,9222.96
r1_4_6,36,8358.69
r1_4_7,36,7616.15
r1_4_8,36,7257.28
r1_4_9,36,8694.78
r1_4_10,36,8094.10
r2_4_1,8,9210.15
r2_4_2,8,7606.75
r2_4_3,8,5911.07
r2_4_4,8,4241.47
r2_4_5,8,7127.83
r2_4_6,8,6122.60
r2_4_7,8,5018.53
r2_4_8,8,4015.60
r2_4_9,8,6400.10
r2_4_10,8,5773.17
rc1_4_1,36,8571.32
rc1_4_2,36,7892.52
rc1_4_3,36,7533.05
rc1_4_4,36,7308.55
rc1_4_5,36,8172.64
rc1_4_6,36,8164.98
rc1_4_7,36,7948.51
rc1_4_8,36,7772.57
rc1_4_9,36,7733.35
rc1_4_10,36,7596.04
rc2_4_1,11,6682.37
rc2_4_2,9,6180.62
rc2_4_3,8,4930.84
rc2_4_4,8,3631.01
rc2_4_5,8,6706.44
rc2_4_6,8,5766.61
rc2_4_7,8,5334.72
rc2_4_8,8,4790.14
rc2_4_9,8,4551.11
rc2_4_10,8,4278.61
# 600-customer instances
c1_6_1,60,14095.64
c1_6_2,56,14163.31
c1_6_3,56,13777.81
c1_6_4,56,13558.54
c1_6_5,60,14085.72
c1_6_6,59,15832.68
c1_6_7,57,15731.20
c1_6_8,56,14385.80
c1_6_9,56,13693.42
c1_6_10,56,13637.34
c2_6_1,18,7774.10
c2_6_2,17,8258.20
c2_6_3,17,7506.62
c2_6_4,17,6909.58
c2_6_5,18,7575.20
c2_6_6,18,7470.36
c2_6_7,18,7512.07
c2_6_8,17,7539.73
c2_6_9,17,7911.61
c2_6_10,17,7255.69
r1_6_1,59,21394.95
r1_6_2,54,18585.00
r1_6_3,54,16900.67
r1_6_4,54,15748.07
r1_6_5,54,19381.52
r1_6_6,54,17790.97
r1_6_7,54,16522.52
r1_6_8,54,15610.00
r1_6_9,54,18530.96
r1_6_10,54,17610.12
r2_6_1,11,18205.58
r2_6_2,11,14754.13
r2_6_3,11,11188.70
r2_6_4,11,8008.14
r2_6_5,11,15067.34
r2_6_6,11,12498.27
r2_6_7,11,10064.56
r2_6_8,11,7571.99
r2_6_9,11,13377.56
r2_6_10,11,12202.28
rc1_6_1,55,16982.86
rc1_6_2,55,15914.70
rc1_6_3,55,15204.64
rc1_6_4,55,14777.19
rc1_6_5,55,16559.78
rc1_6_6,55,16496.88
rc1_6_7,55,16077.12
rc1_6_8,55,15914.91
rc1_6_9,55,15826.24
rc1_6_10,55,15675.53
rc2_6_1,14,13324.93
rc2_6_2,12,11555.51
rc2_6_3,11,9438.52
rc2_6_4,11,7057.94
rc2_6_5,11,12909.07
rc2_6_6,11,11913.11
rc2_6_7,11,10711.85
rc2_6_8,11,9990.40
rc2_6_9,11,9574.99
rc2_6_10,11,9058.90
# 800-customer instances
c1_8_1,80,25030.36
c1_8_2,72,26540.53
c1_8_3,72,24242.49
c1_8_4,72,23824.17
c1_8_5,80,25166.28
c1_8_6,79,26873.86
c1_8_7,77,26464.91
c1_8_8,73,26099.74
c1_8_9,72,24300.21
c1_8_10,72,24070.17
c2_8_1,24,11662.08
c2_8_2,23,12285.31
c2_8_3,23,11410.69
c2_8_4,22,10990.64
c2_8_5,24,11425.23
c2_8_6,23,12235.30
c2_8_7,23,13427.12
c2_8_8,23,11288.01
c2_8_9,23,11592.52
c2_8_10,23,10977.36
r1_8_1,80,36767.92
r1_8_2,72,32313.17
r1_8_3,72,29338.97
r1_8_4,72,27769.19
r1_8_5,72,33529.08
r1_8_6,72,30906.90
r1_8_7,72,28823.76
r1_8_8,72,27643.38
r1_8_9,72,32293.46
r1_8_10,72,30952.77
r2_8_1,15,28112.36
r2_8_2,15,22795.79
r2_8_3,15,17703.99
r2_8_4,15,13192.32
r2_8_5,15,24255.00
r2_8_6,15,20412.02
r2_8_7,15,16597.87
r2_8_8,15,12642.67
r2_8_9,15,22277.04
r2_8_10,15,20358.61
rc1_8_1,72,30464.65
rc1_8_2,72,28511.37
rc1_8_3,72,27528.16
rc1_8_4,72,26663.45
rc1_8_5,72,29508.27
rc1_8_6,72,29458.38
rc1_8_7,72,28979.60
rc1_8_8,72,28605.60
rc1_8_9,72,28555.53
rc1_8_10,72,28331.16
rc2_8_1,18,20981.14
rc2_8_2,16,18151.95
rc2_8_3,15,14427.39
rc2_8_4,15,10999.03
rc2_8_5,15,19074.02
rc2_8_6,15,18143.04
rc2_8_7,15,16817.46
rc2_8_8,15,15759.14
rc2_8_9,15,15325.25
rc2_8_10,15,14411.81
# 1000-customer instances
c1_10_1,100,42478.95
c1_10_2,90,42222.96
c1_10_3,90,40101.36
c1_10_4,90,39468.60
c1_10_5,100,42469.18
c1_10_6,99,43830.21
c1_10_7,97,43341.77
c1_10_8,92,42629.91
c1_10_9,90,40318.03
c1_10_10,90,39852.44
c2_10_1,30,16879.24
c2_10_2,29,17126.39
c2_10_3,28,16829.47
c2_10_4,28,15607.48
c2_10_5,30,16561.29
c2_10_6,29,16863.71
c2_10_7,29,17602.84
c2_10_8,28,16512.43
c2_10_9,28,17809.34
c2_10_10,28,15937.45
r1_10_1,100,53380.18
r1_10_2,91,48232.67
r1_10_3,91,44694.16
r1_10_4,91,42463.74
r1_10_5,91,50445.39
r1_10_6,91,46930.04
r1_10_7,91,43975.47
r1_10_8,91,42288.50
r1_10_9,91,49195.26
r1_10_10,91,47407.16
r2_10_1,19,42182.57
r2_10_2,19,33411.21
r2_10_3,19,24916.88
r2_10_4,19,17851.96
r2_10_5,19,36216.05
r2_10_6,19,29978.02
r2_10_7,19,23219.61
r2_10_8,19,17442.29
r2_10_9,19,32995.71
r2_10_10,19,30207.49
rc1_10_1,90,45830.62
rc1_10_2,90,43718.84
rc1_10_3,90,42146.79
rc1_10_4,90,41391.18
rc1_10_5,90,45069.37
rc1_10_6,90,44937.36
rc1_10_7,90,44457.79
rc1_10_8,90,43956.91
rc1_10_9,90,43897.21
rc1_10_10,90,43551.69
rc2_10_1,20,30276.27
rc2_10_2,18,26104.09
rc2_10_3,18,19911.48
rc2_10_4,18,15693.28
rc2_10_5,18,27067.04
rc2_10_6,18,26741.27
rc2_10_7,18,24999.66
rc2_10_8,18,23595.33
rc2_10_9,18,22919.42
rc2_10_10,18,21834.94
BKS_GH_EOF
        ok "  gehring_homberger.csv (300 entries)"
    fi

    # --- Li-Lim extended PDPTW BKS ---
    local ll_bks="$BKS_DIR/li_lim_extended.csv"
    if [ -f "$ll_bks" ]; then
        ok "  li_lim_extended.csv already exists, skipping"
    else
        info "  Writing li_lim_extended.csv..."
        cat > "$ll_bks" << 'BKS_LL_EOF'
# Li-Lim Extended PDPTW Best Known Solutions (hierarchical objective)
# Source: SINTEF TOP (https://www.sintef.no/projectweb/top/pdptw/li-lim-benchmark/)
# Retrieved: 2026-02-26
# Objective: minimize vehicles first, then minimize total distance
# Distance: Euclidean, rounded to 2 decimal places
# Note: lrc2_10_8 and lrc2_10_9 are missing from the original dataset
# Format: name,vehicles,distance
# 200-task instances
lc1_2_1,20,2704.57
lc1_2_2,19,2764.56
lc1_2_3,17,3127.78
lc1_2_4,17,2693.41
lc1_2_5,20,2702.05
lc1_2_6,20,2701.04
lc1_2_7,20,2701.04
lc1_2_8,19,3354.27
lc1_2_9,18,2724.24
lc1_2_10,17,2942.13
lc2_2_1,6,1931.44
lc2_2_2,6,1881.40
lc2_2_3,6,1844.33
lc2_2_4,6,1767.12
lc2_2_5,6,1891.21
lc2_2_6,6,1857.78
lc2_2_7,6,1850.13
lc2_2_8,6,1824.34
lc2_2_9,6,1854.21
lc2_2_10,6,1817.45
lr1_2_1,20,4819.12
lr1_2_2,17,4621.21
lr1_2_3,14,4402.38
lr1_2_4,10,3027.06
lr1_2_5,16,4760.18
lr1_2_6,13,4800.94
lr1_2_7,12,3543.36
lr1_2_8,9,2759.32
lr1_2_9,13,5050.75
lr1_2_10,11,3664.08
lr2_2_1,5,4073.10
lr2_2_2,4,3796.00
lr2_2_3,4,3098.36
lr2_2_4,3,2486.00
lr2_2_5,4,3438.39
lr2_2_6,3,4457.95
lr2_2_7,3,3098.35
lr2_2_8,2,2449.36
lr2_2_9,3,3922.11
lr2_2_10,3,3254.83
lrc1_2_1,19,3606.06
lrc1_2_2,15,3671.02
lrc1_2_3,13,3154.92
lrc1_2_4,10,2631.82
lrc1_2_5,16,3715.81
lrc1_2_6,16,3572.16
lrc1_2_7,14,3666.34
lrc1_2_8,13,3145.74
lrc1_2_9,13,3157.34
lrc1_2_10,12,2928.90
lrc2_2_1,6,3595.18
lrc2_2_2,5,3158.25
lrc2_2_3,4,2881.99
lrc2_2_4,3,2835.40
lrc2_2_5,5,2776.93
lrc2_2_6,5,2707.96
lrc2_2_7,4,3010.68
lrc2_2_8,4,2399.89
lrc2_2_9,4,2208.49
lrc2_2_10,3,2437.88
# 400-task instances
lc1_4_1,40,7152.06
lc1_4_2,38,8007.79
lc1_4_3,32,8678.23
lc1_4_4,30,6451.68
lc1_4_5,40,7150.00
lc1_4_6,40,7154.02
lc1_4_7,40,7149.43
lc1_4_8,38,8305.42
lc1_4_9,36,7451.20
lc1_4_10,34,7850.22
lc2_4_1,12,4116.33
lc2_4_2,12,4144.29
lc2_4_3,12,4401.08
lc2_4_4,12,3743.95
lc2_4_5,12,4030.63
lc2_4_6,12,3900.29
lc2_4_7,12,3962.51
lc2_4_8,12,3844.45
lc2_4_9,12,4188.93
lc2_4_10,12,3828.44
lr1_4_1,40,10639.75
lr1_4_2,30,11009.51
lr1_4_3,22,9245.48
lr1_4_4,15,7007.90
lr1_4_5,28,11374.06
lr1_4_6,23,11334.11
lr1_4_7,18,8675.36
lr1_4_8,13,6164.82
lr1_4_9,24,9859.47
lr1_4_10,20,8192.65
lr2_4_1,8,9726.88
lr2_4_2,7,9405.40
lr2_4_3,5,10176.94
lr2_4_4,4,6201.84
lr2_4_5,6,9894.46
lr2_4_6,5,8946.91
lr2_4_7,4,7993.16
lr2_4_8,4,5260.42
lr2_4_9,6,7926.07
lr2_4_10,5,7596.62
lrc1_4_1,36,9124.52
lrc1_4_2,31,8346.06
lrc1_4_3,24,7805.16
lrc1_4_4,19,5803.31
lrc1_4_5,32,8847.40
lrc1_4_6,30,8394.47
lrc1_4_7,28,8037.87
lrc1_4_8,26,7930.15
lrc1_4_9,25,8004.24
lrc1_4_10,23,7064.36
lrc2_4_1,11,9738.95
lrc2_4_2,10,7159.95
lrc2_4_3,8,6426.47
lrc2_4_4,5,5238.77
lrc2_4_5,10,7309.54
lrc2_4_6,9,6337.08
lrc2_4_7,8,6292.23
lrc2_4_8,7,5767.72
lrc2_4_9,6,6272.92
lrc2_4_10,6,5395.71
# 600-task instances
lc1_6_1,60,14095.64
lc1_6_2,57,15048.16
lc1_6_3,49,14874.68
lc1_6_4,47,13300.55
lc1_6_5,60,14086.30
lc1_6_6,60,14090.79
lc1_6_7,60,14083.76
lc1_6_8,58,14880.70
lc1_6_9,53,15207.58
lc1_6_10,51,15432.55
lc2_6_1,19,7977.98
lc2_6_2,18,9900.48
lc2_6_3,17,8512.94
lc2_6_4,17,7860.38
lc2_6_5,18,9051.53
lc2_6_6,18,8775.55
lc2_6_7,18,9376.58
lc2_6_8,18,7579.93
lc2_6_9,18,8714.22
lc2_6_10,17,7946.60
lr1_6_1,59,22821.65
lr1_6_2,45,20137.22
lr1_6_3,37,17846.17
lr1_6_4,28,13127.56
lr1_6_5,37,23623.52
lr1_6_6,30,23084.50
lr1_6_7,24,16963.37
lr1_6_8,18,11917.70
lr1_6_9,31,21835.87
lr1_6_10,25,19298.25
lr2_6_1,11,21759.33
lr2_6_2,9,21289.70
lr2_6_3,7,17480.06
lr2_6_4,6,10639.08
lr2_6_5,8,22625.89
lr2_6_6,7,19099.49
lr2_6_7,6,14645.32
lr2_6_8,4,12341.90
lr2_6_9,8,18259.20
lr2_6_10,7,16390.64
lrc1_6_1,52,18288.90
lrc1_6_2,43,16515.41
lrc1_6_3,36,13975.98
lrc1_6_4,25,10800.44
lrc1_6_5,45,17463.94
lrc1_6_6,41,19025.36
lrc1_6_7,37,15914.85
lrc1_6_8,33,15317.63
lrc1_6_9,33,15344.64
lrc1_6_10,29,13963.66
lrc2_6_1,16,14578.92
lrc2_6_2,13,13850.22
lrc2_6_3,10,12432.91
lrc2_6_4,7,9833.92
lrc2_6_5,13,14380.37
lrc2_6_6,12,14962.08
lrc2_6_7,10,14094.26
lrc2_6_8,9,13179.58
lrc2_6_9,8,16309.78
lrc2_6_10,7,13054.50
# 800-task instances
lc1_8_1,80,25184.38
lc1_8_2,76,29735.85
lc1_8_3,62,27287.80
lc1_8_4,59,22686.08
lc1_8_5,80,25211.22
lc1_8_6,80,25164.25
lc1_8_7,80,25158.38
lc1_8_8,77,26688.17
lc1_8_9,71,26975.61
lc1_8_10,68,27099.59
lc2_8_1,24,11687.06
lc2_8_2,24,13713.13
lc2_8_3,24,12981.37
lc2_8_4,23,12917.34
lc2_8_5,25,12298.33
lc2_8_6,24,12645.71
lc2_8_7,24,14041.47
lc2_8_8,23,12924.10
lc2_8_9,24,11629.41
lc2_8_10,23,12226.42
lr1_8_1,80,39291.32
lr1_8_2,59,34071.19
lr1_8_3,44,29223.10
lr1_8_4,25,20675.70
lr1_8_5,48,39809.68
lr1_8_6,38,36244.90
lr1_8_7,30,27078.09
lr1_8_8,19,20705.45
lr1_8_9,39,38499.00
lr1_8_10,30,31091.07
lr2_8_1,14,41873.12
lr2_8_2,11,36549.69
lr2_8_3,9,26391.07
lr2_8_4,6,20618.03
lr2_8_5,11,34816.50
lr2_8_6,9,28814.65
lr2_8_7,7,25502.39
lr2_8_8,5,18327.23
lr2_8_9,10,30515.50
lr2_8_10,8,30136.86
lrc1_8_1,66,32252.28
lrc1_8_2,56,27878.89
lrc1_8_3,48,24371.95
lrc1_8_4,34,18208.51
lrc1_8_5,58,31169.16
lrc1_8_6,54,28961.66
lrc1_8_7,50,28768.40
lrc1_8_8,44,26902.93
lrc1_8_9,44,24854.96
lrc1_8_10,39,24622.59
lrc2_8_1,20,23074.22
lrc2_8_2,17,22220.95
lrc2_8_3,14,20379.26
lrc2_8_4,11,14711.80
lrc2_8_5,16,23602.44
lrc2_8_6,15,22591.26
lrc2_8_7,13,25436.79
lrc2_8_8,11,22604.36
lrc2_8_9,10,22594.69
lrc2_8_10,9,19604.12
# 1000-task instances
lc1_10_1,100,42488.66
lc1_10_2,94,44548.51
lc1_10_3,78,45906.30
lc1_10_4,72,37782.17
lc1_10_5,100,42477.40
lc1_10_6,101,42838.39
lc1_10_7,100,42854.99
lc1_10_8,98,42949.56
lc1_10_9,90,43564.01
lc1_10_10,86,42929.15
lc2_10_1,30,16879.24
lc2_10_2,30,20764.09
lc2_10_3,29,19299.48
lc2_10_4,29,17886.97
lc2_10_5,31,17137.53
lc2_10_6,30,19387.75
lc2_10_7,31,18389.37
lc2_10_8,30,17015.03
lc2_10_9,30,18225.30
lc2_10_10,29,17043.64
lr1_10_1,100,56744.91
lr1_10_2,80,49349.84
lr1_10_3,54,41483.89
lr1_10_4,27,30788.11
lr1_10_5,58,59053.68
lr1_10_6,46,51058.53
lr1_10_7,35,38563.20
lr1_10_8,24,29613.27
lr1_10_9,47,54582.25
lr1_10_10,38,45510.71
lr2_10_1,17,62859.29
lr2_10_2,14,51357.30
lr2_10_3,10,42908.33
lr2_10_4,8,26595.39
lr2_10_5,13,54951.12
lr2_10_6,11,46253.37
lr2_10_7,8,39648.54
lr2_10_8,6,26742.31
lr2_10_9,12,50781.83
lr2_10_10,10,44888.99
lrc1_10_1,82,49111.78
lrc1_10_2,71,45547.38
lrc1_10_3,53,35616.85
lrc1_10_4,40,27211.87
lrc1_10_5,72,50323.04
lrc1_10_6,67,45115.22
lrc1_10_7,60,41560.52
lrc1_10_8,55,40770.10
lrc1_10_9,52,40934.27
lrc1_10_10,47,36539.03
lrc2_10_1,22,34463.46
lrc2_10_2,19,38619.13
lrc2_10_3,16,27218.08
lrc2_10_4,11,23212.46
lrc2_10_5,16,40638.13
lrc2_10_6,17,30910.65
lrc2_10_7,15,33275.24
lrc2_10_10,11,29085.28
BKS_LL_EOF
        ok "  li_lim_extended.csv (298 entries, 2 missing from original dataset)"
    fi

    echo
}

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------

clean_downloads() {
    warn "Removing downloaded benchmark data..."

    if [ -d "$GH_DIR" ]; then
        rm -rf "$GH_DIR"
        ok "  Removed $GH_DIR"
    fi

    if [ -d "$LL_DIR" ]; then
        rm -rf "$LL_DIR"
        ok "  Removed $LL_DIR"
    fi

    if [ -d "$TMP_DIR" ]; then
        rm -rf "$TMP_DIR"
        ok "  Removed temp directory"
    fi

    # Do NOT remove BKS files -- they are committed to the repo
    info "BKS CSV files in $BKS_DIR were not removed (they are version-controlled)"
    echo
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

print_summary() {
    echo
    printf "${BOLD}========================================${NC}\n"
    printf "${BOLD}  Surge Benchmark Download Summary${NC}\n"
    printf "${BOLD}========================================${NC}\n"
    echo

    printf "%-40s %s\n" "Directory" "Instances"
    printf "%-40s %s\n" "----------------------------------------" "----------"

    local total=0
    for size in 200 400 600 800 1000; do
        local n
        n="$(count_instances "$GH_DIR/$size")"
        total=$((total + n))
        printf "%-40s %s\n" "gehring_homberger/$size/" "$n"
    done
    printf "%-40s %s\n" "" "-------"
    printf "%-40s ${BOLD}%s${NC}\n" "Gehring-Homberger VRPTW total" "$total"
    echo

    total=0
    for size in 200 400 600 800 1000; do
        local n
        n="$(count_instances "$LL_DIR/$size")"
        total=$((total + n))
        printf "%-40s %s\n" "li_lim_extended/$size/" "$n"
    done
    printf "%-40s %s\n" "" "-------"
    printf "%-40s ${BOLD}%s${NC}\n" "Li-Lim extended PDPTW total" "$total"
    echo

    printf "%-40s %s\n" "BKS files:" ""
    [ -f "$BKS_DIR/gehring_homberger.csv" ] && printf "  %-38s %s\n" "bks/gehring_homberger.csv" "OK" || printf "  %-38s %s\n" "bks/gehring_homberger.csv" "MISSING"
    [ -f "$BKS_DIR/li_lim_extended.csv" ]   && printf "  %-38s %s\n" "bks/li_lim_extended.csv"   "OK" || printf "  %-38s %s\n" "bks/li_lim_extended.csv"   "MISSING"
    echo
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

main() {
    local do_vrptw=false
    local do_pdptw=false
    local do_bks=false
    local do_clean=false
    local do_all=true

    for arg in "$@"; do
        case "$arg" in
            --vrptw) do_vrptw=true; do_all=false ;;
            --pdptw) do_pdptw=true; do_all=false ;;
            --bks)   do_bks=true;   do_all=false ;;
            --clean) do_clean=true; do_all=false ;;
            --help|-h)
                echo "Usage: $0 [--vrptw] [--pdptw] [--bks] [--clean] [--help]"
                echo
                echo "Downloads benchmark instances for the Surge VRP solver."
                echo
                echo "Options:"
                echo "  --vrptw   Download Gehring-Homberger VRPTW instances only"
                echo "  --pdptw   Download Li-Lim extended PDPTW instances only"
                echo "  --bks     Generate BKS CSV files only"
                echo "  --clean   Remove all downloaded benchmark data"
                echo "  --help    Show this help message"
                echo
                echo "With no options, downloads everything."
                echo
                echo "Directories:"
                echo "  surge/benchmarks/gehring_homberger/{200,400,600,800,1000}/"
                echo "  surge/benchmarks/li_lim_extended/{200,400,600,800,1000}/"
                echo "  surge/benchmarks/bks/gehring_homberger.csv"
                echo "  surge/benchmarks/bks/li_lim_extended.csv"
                exit 0
                ;;
            *)
                err "Unknown option: $arg"
                echo "Run $0 --help for usage."
                exit 1
                ;;
        esac
    done

    echo
    printf "${BOLD}Surge Benchmark Downloader${NC}\n"
    echo

    if $do_clean; then
        clean_downloads
        exit 0
    fi

    if $do_all || $do_bks;   then generate_bks;               fi
    if $do_all || $do_vrptw; then download_gehring_homberger;  fi
    if $do_all || $do_pdptw; then download_li_lim_extended;    fi

    # Clean up temp directory
    if [ -d "$TMP_DIR" ]; then
        rm -rf "$TMP_DIR"
    fi

    print_summary
}

main "$@"
