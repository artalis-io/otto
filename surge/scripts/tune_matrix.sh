#!/usr/bin/env bash
#
# tune_matrix.sh — Autonomous profile × scale matrix tuning campaign
#
# Tunes all 20 cells of the profile × scale matrix using bench_tune's
# tiered grid search. Each cell gets its own checkpoint file for
# resume support. Can be killed and re-run at any time.
#
# Usage:
#   ./scripts/tune_matrix.sh                    # Full campaign
#   ./scripts/tune_matrix.sh --threads 40       # Override thread count
#   ./scripts/tune_matrix.sh --priority-only    # Top 8 cells only
#   ./scripts/tune_matrix.sh --dry-run          # Show plan without running
#   ./scripts/tune_matrix.sh --cell 1,3         # Run specific cell(s) by index
#
# Requirements:
#   - bench_tune binary (make bench_tune)
#   - Benchmark data (make bench-download)
#   - BKS files in benchmarks/bks/
#
# Resume: Just re-run the script. Checkpoint files in results/matrix/
# track per-evaluation progress. Completed tiers are skipped.
#
# Machine sizing:
#   Ryzen 5950X (16C/32T): --threads 14, ~7 days for priority cells
#   Hetzner CCX63 (48 vCPU): --threads 40, ~4 days for priority cells
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SURGE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BENCH_TUNE="$SURGE_DIR/bench_tune"
RESULTS_DIR="$SURGE_DIR/benchmarks/results/matrix"

# --- Helpers ---
to_lower() {
    echo "$1" | tr '[:upper:]' '[:lower:]'
}

# --- Defaults ---
THREADS=14
SEED=42
VERIFY=3
PRIORITY_ONLY=0
DRY_RUN=0
SPECIFIC_CELLS=""

# --- Parse args ---
while [ $# -gt 0 ]; do
    case "$1" in
        --threads)     THREADS="$2"; shift 2 ;;
        --seed)        SEED="$2"; shift 2 ;;
        --verify)      VERIFY="$2"; shift 2 ;;
        --priority-only) PRIORITY_ONLY=1; shift ;;
        --dry-run)     DRY_RUN=1; shift ;;
        --cell)        SPECIFIC_CELLS="$2"; shift 2 ;;
        --help|-h)
            head -30 "$0" | grep '^#' | sed 's/^# *//'
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# --- Cell definitions ---
# Each cell: "idx priority profile scale_size dir bks"
#
# Profile: 0=REALTIME, 1=FAST, 2=NEAR_OPTIMAL, 3=BEST
# Scale sizes: SMALL=100, MEDIUM=200, LARGE=400, XLARGE=600, MASSIVE=1000
#
# Priority order (highest impact first):
#  1. FAST × LARGE         — most common production use case
#  2. FAST × MEDIUM        — medium fleet, common
#  3. NEAR_OPTIMAL × LARGE — quality-sensitive large problems
#  4. BEST × LARGE         — best quality, large problems
#  5. FAST × SMALL         — baseline (already tuned for 100-req)
#  6. NEAR_OPTIMAL × MEDIUM
#  7. REALTIME × LARGE
#  8. REALTIME × MEDIUM
#  9-20: remaining cells (XLARGE, MASSIVE, remaining profiles)

CELL_COUNT=20

# Fields: idx priority profile scale_size dir bks
CELL_IDX=(    0  1  2  3  4  5  6  7  8  9  10 11 12 13 14 15 16 17 18 19)
CELL_PRI=(    1  2  3  4  5  6  7  8  9  10 11 12 13 14 15 16 17 18 19 20)
CELL_PROF=(   1  1  2  3  1  2  0  0  0  2  3  3  0  1  2  3  0  1  2  3)
CELL_SCALE=(  400 200 400 400 100 200 400 200 100 100 100 200 600 600 600 600 1000 1000 1000 1000)
CELL_DIR=(
    "benchmarks/gehring_homberger/400"
    "benchmarks/gehring_homberger/200"
    "benchmarks/gehring_homberger/400"
    "benchmarks/gehring_homberger/400"
    "REPRESENTATIVE"
    "benchmarks/gehring_homberger/200"
    "benchmarks/gehring_homberger/400"
    "benchmarks/gehring_homberger/200"
    "REPRESENTATIVE"
    "REPRESENTATIVE"
    "REPRESENTATIVE"
    "benchmarks/gehring_homberger/200"
    "benchmarks/gehring_homberger/600"
    "benchmarks/gehring_homberger/600"
    "benchmarks/gehring_homberger/600"
    "benchmarks/gehring_homberger/600"
    "benchmarks/gehring_homberger/1000"
    "benchmarks/gehring_homberger/1000"
    "benchmarks/gehring_homberger/1000"
    "benchmarks/gehring_homberger/1000"
)
CELL_BKS=(
    "benchmarks/bks/gehring_homberger.csv"
    "benchmarks/bks/gehring_homberger.csv"
    "benchmarks/bks/gehring_homberger.csv"
    "benchmarks/bks/gehring_homberger.csv"
    "NONE"
    "benchmarks/bks/gehring_homberger.csv"
    "benchmarks/bks/gehring_homberger.csv"
    "benchmarks/bks/gehring_homberger.csv"
    "NONE"
    "NONE"
    "NONE"
    "benchmarks/bks/gehring_homberger.csv"
    "benchmarks/bks/gehring_homberger.csv"
    "benchmarks/bks/gehring_homberger.csv"
    "benchmarks/bks/gehring_homberger.csv"
    "benchmarks/bks/gehring_homberger.csv"
    "benchmarks/bks/gehring_homberger.csv"
    "benchmarks/bks/gehring_homberger.csv"
    "benchmarks/bks/gehring_homberger.csv"
    "benchmarks/bks/gehring_homberger.csv"
)

# Profile and scale names for display
PROFILE_NAMES=("REALTIME" "FAST" "NEAR_OPTIMAL" "BEST")
SCALE_NAMES=("SMALL" "MEDIUM" "LARGE" "XLARGE" "MASSIVE")

# Map scale_size to scale name index
scale_name_for_size() {
    case "$1" in
        100)  echo 0 ;; # SMALL
        200)  echo 1 ;; # MEDIUM
        400)  echo 2 ;; # LARGE
        600)  echo 3 ;; # XLARGE
        1000) echo 4 ;; # MASSIVE
        *)    echo 0 ;;
    esac
}

# Time budget from the matrix (seconds per instance solve)
# k_profile_matrix[profile][scale].max_time_seconds
TIME_BUDGET=(
    # SMALL MEDIUM LARGE XLARGE MASSIVE
    1   2   5   10  15      # REALTIME
    5  10  30   60  90      # FAST
   15  45 120  300 600      # NEAR_OPTIMAL
   60 180 600 1200 1800     # BEST
)

get_time_budget() {
    local profile=$1 scale_idx=$2
    local idx=$((profile * 5 + scale_idx))
    echo "${TIME_BUDGET[$idx]}"
}

# Instance counts per directory
count_instances() {
    local dir="$1"
    if [ "$dir" = "REPRESENTATIVE" ]; then
        echo 18
    else
        ls "$SURGE_DIR/$dir"/*.txt 2>/dev/null | wc -l | tr -d ' '
    fi
}

# Estimate total configs across all 7 tiers
# T0:25 + T1:80 + T2:~100 + T3:256 + T4:64 + T5:81 + T6:9 = ~615
TOTAL_CONFIGS_ALL_TIERS=615

# Instance cap per time budget tier (keeps per-config cost reasonable)
# Budget < 60s:  use all instances (60)
# Budget 60-119s: cap at 30 instances
# Budget 120-599s: cap at 18 instances
# Budget 600-1199s: cap at 12 instances
# Budget >= 1200s: cap at 6 instances
max_instances_for_budget() {
    local budget=$1
    if   [ "$budget" -ge 1200 ]; then echo 6
    elif [ "$budget" -ge 600 ]; then echo 12
    elif [ "$budget" -ge 120 ]; then echo 18
    elif [ "$budget" -ge 60 ]; then echo 30
    else echo 0  # 0 = no limit
    fi
}

# --- Time estimation ---
estimate_cell_hours() {
    local profile=$1 scale_size=$2 dir="$3"
    local scale_idx budget n_instances max_inst secs_per_config total_secs
    scale_idx=$(scale_name_for_size "$scale_size")
    budget=$(get_time_budget "$profile" "$scale_idx")
    n_instances=$(count_instances "$dir")
    max_inst=$(max_instances_for_budget "$budget")
    if [ "$max_inst" -gt 0 ] && [ "$n_instances" -gt "$max_inst" ]; then
        n_instances=$max_inst
    fi
    secs_per_config=$((n_instances * budget))
    total_secs=$(( TOTAL_CONFIGS_ALL_TIERS * secs_per_config / THREADS ))
    echo $(( (total_secs + 3599) / 3600 ))
}

# --- Build bench_tune if needed ---
ensure_bench_tune() {
    if [ ! -x "$BENCH_TUNE" ]; then
        echo "Building bench_tune..."
        make -C "$SURGE_DIR" bench_tune
    fi
}

# --- Run one cell ---
run_cell() {
    local ci=$1
    local profile=${CELL_PROF[$ci]}
    local scale_size=${CELL_SCALE[$ci]}
    local dir=${CELL_DIR[$ci]}
    local bks=${CELL_BKS[$ci]}
    local pri=${CELL_PRI[$ci]}

    local scale_idx pname sname pname_lower sname_lower checkpoint logfile
    scale_idx=$(scale_name_for_size "$scale_size")
    pname="${PROFILE_NAMES[$profile]}"
    sname="${SCALE_NAMES[$scale_idx]}"
    pname_lower=$(to_lower "$pname")
    sname_lower=$(to_lower "$sname")
    checkpoint="$RESULTS_DIR/tune_${pname_lower}_${sname_lower}.jsonl"
    logfile="$RESULTS_DIR/tune_${pname_lower}_${sname_lower}.log"

    # Check if already complete (all 7 tiers done)
    if [ -f "$checkpoint" ]; then
        local done_tiers
        done_tiers=$(grep -c '"type":"tier_done"' "$checkpoint" 2>/dev/null || echo 0)
        if [ "$done_tiers" -ge 7 ]; then
            echo "  [SKIP] $pname x $sname — all 7 tiers complete"
            return 0
        fi
        echo "  [RESUME] $pname x $sname — $done_tiers/7 tiers complete"
    fi

    local est_hours
    est_hours=$(estimate_cell_hours "$profile" "$scale_size" "$dir")
    echo "  [RUN] $pname x $sname — est ~${est_hours}h"
    echo "  Checkpoint: $checkpoint"

    if [ "$DRY_RUN" -eq 1 ]; then
        return 0
    fi

    # Build command
    local cmd="$BENCH_TUNE --profile $profile --scale-size $scale_size"
    cmd="$cmd --all-tiers --threads $THREADS --seed $SEED"
    cmd="$cmd --verify $VERIFY --baseline --json"
    cmd="$cmd --checkpoint $checkpoint"

    if [ "$dir" != "REPRESENTATIVE" ]; then
        cmd="$cmd --dir $SURGE_DIR/$dir"
        if [ "$bks" != "NONE" ]; then
            cmd="$cmd --bks $SURGE_DIR/$bks"
        fi
        # Subsample instances for expensive cells
        local budget max_inst
        budget=$(get_time_budget "$profile" "$scale_idx")
        max_inst=$(max_instances_for_budget "$budget")
        if [ "$max_inst" -gt 0 ]; then
            cmd="$cmd --max-instances $max_inst"
        fi
    fi

    echo "  Command: $cmd"
    echo "  Log: $logfile"
    echo ""

    eval "$cmd" > "$logfile" 2>&1 || {
        local exit_code=$?
        echo "  [WARN] bench_tune exited with code $exit_code"
        echo "         Check $logfile for details"
        echo "         Re-run this script to resume from checkpoint"
        return 0  # Don't abort the whole campaign
    }

    echo "  [DONE] $pname x $sname"
}

# --- Main ---

echo "================================================================"
echo "  Surge Profile x Scale Matrix Tuning Campaign"
echo "================================================================"
echo ""
echo "  Threads:    $THREADS"
echo "  Seed:       $SEED"
echo "  Verify:     top $VERIFY configs with 4 seeds"
echo "  Results:    $RESULTS_DIR/"
echo ""

ensure_bench_tune
mkdir -p "$RESULTS_DIR"

# Select cells to run
selected=()
if [ -n "$SPECIFIC_CELLS" ]; then
    IFS=',' read -ra cell_indices <<< "$SPECIFIC_CELLS"
    for ci in "${cell_indices[@]}"; do
        selected+=("$ci")
    done
    echo "Running ${#selected[@]} specific cell(s)"
elif [ "$PRIORITY_ONLY" -eq 1 ]; then
    selected=(0 1 2 3 4 5 6 7)
    echo "Running top 8 priority cells only"
else
    selected=()
    for i in $(seq 0 $((CELL_COUNT - 1))); do
        selected+=("$i")
    done
    echo "Running all 20 cells"
fi

echo ""

# Show time estimate
total_est=0
echo "Estimated time per cell:"
printf "  %-4s %-20s %-10s %-6s %-10s %s\n" "Pri" "Cell" "Instances" "Budget" "Est.Hrs" "Status"
echo "  ---- -------------------- ---------- ------ ---------- ------"
for ci in "${selected[@]}"; do
    local_prof=${CELL_PROF[$ci]}
    local_scale=${CELL_SCALE[$ci]}
    local_dir=${CELL_DIR[$ci]}
    local_pri=${CELL_PRI[$ci]}

    local_scale_idx=$(scale_name_for_size "$local_scale")
    local_pname="${PROFILE_NAMES[$local_prof]}"
    local_sname="${SCALE_NAMES[$local_scale_idx]}"
    local_ninst=$(count_instances "$local_dir")
    local_budget=$(get_time_budget "$local_prof" "$local_scale_idx")
    local_max_inst=$(max_instances_for_budget "$local_budget")
    if [ "$local_max_inst" -gt 0 ] && [ "$local_ninst" -gt "$local_max_inst" ]; then
        local_ninst_eff=$local_max_inst
    else
        local_ninst_eff=$local_ninst
    fi
    local_est=$(estimate_cell_hours "$local_prof" "$local_scale" "$local_dir")
    total_est=$((total_est + local_est))

    # Check status
    local_pname_lower=$(to_lower "$local_pname")
    local_sname_lower=$(to_lower "$local_sname")
    local_cp="$RESULTS_DIR/tune_${local_pname_lower}_${local_sname_lower}.jsonl"
    local_status="pending"
    if [ -f "$local_cp" ]; then
        local_done=$(grep -c '"type":"tier_done"' "$local_cp" 2>/dev/null || echo 0)
        if [ "$local_done" -ge 7 ]; then
            local_status="DONE"
        else
            local_status="${local_done}/7 tiers"
        fi
    fi

    local_inst_label="${local_ninst_eff}inst"
    if [ "$local_ninst_eff" -lt "$local_ninst" ]; then
        local_inst_label="${local_ninst_eff}/${local_ninst}"
    fi
    printf "  %-4s %-20s %-10s %-6s %-10s %s\n" \
        "#$local_pri" "${local_pname}x${local_sname}" \
        "$local_inst_label" "${local_budget}s" "~${local_est}h" "$local_status"
done
echo ""
echo "  Total estimated: ~${total_est} hours (~$((total_est / 24)) days)"
echo ""

if [ "$DRY_RUN" -eq 1 ]; then
    echo "(dry run — no tuning performed)"
    exit 0
fi

echo "Starting tuning campaign..."
echo "  Kill with Ctrl-C at any time. Re-run to resume."
echo ""

# Run cells in order
campaign_start=$(date +%s)
cells_done=0
cells_total=${#selected[@]}

for ci in "${selected[@]}"; do
    cells_done=$((cells_done + 1))
    local_pri=${CELL_PRI[$ci]}
    echo "--- Cell $cells_done/$cells_total (priority #$local_pri) ---"
    run_cell "$ci"
    echo ""
done

campaign_end=$(date +%s)
campaign_elapsed=$(( (campaign_end - campaign_start) / 3600 ))

echo "================================================================"
echo "  Campaign complete! Elapsed: ~${campaign_elapsed}h"
echo "  Results in: $RESULTS_DIR/"
echo ""
echo "  Next steps:"
echo "    1. Review JSON results in $RESULTS_DIR/*.log"
echo "    2. Extract winning params from checkpoint JSONL files"
echo "    3. Update k_profile_matrix in src/sg_profile_matrix.c"
echo "================================================================"
