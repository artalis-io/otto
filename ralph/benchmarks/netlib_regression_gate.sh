#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RALPH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

BASELINE_FILE="$SCRIPT_DIR/netlib_regression_baseline.json"
NETLIB_DIR="$SCRIPT_DIR/netlib"
BENCH_EXEC="$RALPH_DIR/ralph-benchmark"

HARD_CAP_SEC=""
OUTER_TIMEOUT_SEC=""
METHOD=""
OBJ_REL_TOL=""
OUTDIR=""
FILTER_REGEX=""
ALLOWLIST_FILE=""
NO_BUILD=0
METHOD_FROM_CLI=0
OBJ_REL_TOL_FROM_CLI=0

usage() {
    cat <<'EOF'
Usage: netlib_regression_gate.sh [options]

Options:
  --baseline <file>        Baseline manifest JSON
  --netlib-dir <dir>       Directory containing .mps files
  --bench <path>           ralph-benchmark executable path
  --hard-cap <sec>         --hard-cap passed to ralph-benchmark
  --outer-timeout <sec>    External timeout wrapper seconds
  --method <n>             --method passed to ralph-benchmark
  --obj-rel-tol <tol>      --obj-rel-tol passed to ralph-benchmark
  --outdir <dir>           Output directory for run artifacts
  --filter <regex>         Only run files where basename matches regex
  --allowlist <file>       Only run basenames listed in file (one per line)
  --no-build               Skip rebuilding ralph-benchmark
  -h, --help               Show help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --baseline)
            BASELINE_FILE="$2"
            shift 2
            ;;
        --netlib-dir)
            NETLIB_DIR="$2"
            shift 2
            ;;
        --bench)
            BENCH_EXEC="$2"
            shift 2
            ;;
        --hard-cap)
            HARD_CAP_SEC="$2"
            shift 2
            ;;
        --outer-timeout)
            OUTER_TIMEOUT_SEC="$2"
            shift 2
            ;;
        --method)
            METHOD="$2"
            METHOD_FROM_CLI=1
            shift 2
            ;;
        --obj-rel-tol)
            OBJ_REL_TOL="$2"
            OBJ_REL_TOL_FROM_CLI=1
            shift 2
            ;;
        --outdir)
            OUTDIR="$2"
            shift 2
            ;;
        --filter)
            FILTER_REGEX="$2"
            shift 2
            ;;
        --allowlist)
            ALLOWLIST_FILE="$2"
            shift 2
            ;;
        --no-build)
            NO_BUILD=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage
            exit 2
            ;;
    esac
done

if ! command -v jq >/dev/null 2>&1; then
    echo "ERROR: jq is required for NETLIB regression gate." >&2
    exit 2
fi

TIMEOUT_BIN=""
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_BIN="$(command -v timeout)"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_BIN="$(command -v gtimeout)"
else
    echo "ERROR: timeout (or gtimeout) is required for NETLIB regression gate." >&2
    exit 2
fi

if [[ ! -f "$BASELINE_FILE" ]]; then
    echo "ERROR: baseline file not found: $BASELINE_FILE" >&2
    exit 2
fi

if [[ ! -d "$NETLIB_DIR" ]]; then
    echo "ERROR: NETLIB directory not found: $NETLIB_DIR" >&2
    exit 2
fi

if [[ -n "$ALLOWLIST_FILE" && ! -f "$ALLOWLIST_FILE" ]]; then
    echo "ERROR: allowlist file not found: $ALLOWLIST_FILE" >&2
    exit 2
fi

if [[ -z "$HARD_CAP_SEC" ]]; then
    HARD_CAP_SEC="$(jq -r '.defaults.hard_cap_sec // 20' "$BASELINE_FILE")"
fi
if [[ -z "$OUTER_TIMEOUT_SEC" ]]; then
    OUTER_TIMEOUT_SEC="$(jq -r '.defaults.outer_timeout_sec // 25' "$BASELINE_FILE")"
fi
if [[ -z "$METHOD" ]]; then
    METHOD="$(jq -r '.defaults.method // 0' "$BASELINE_FILE")"
fi
if [[ -z "$OBJ_REL_TOL" ]]; then
    OBJ_REL_TOL="$(jq -r '.defaults.obj_rel_tol // empty' "$BASELINE_FILE")"
fi

# Compare runs can consume both GLPK and Ralph hard-cap windows. Keep an
# external timeout floor high enough to avoid truncating valid runs.
OUTER_TIMEOUT_ORIGINAL="$OUTER_TIMEOUT_SEC"
OUTER_TIMEOUT_MIN="$(awk -v hc="$HARD_CAP_SEC" 'BEGIN { t = (hc * 2.0) + 10.0; if (t < 30.0) t = 30.0; printf "%.3f", t }')"
OUTER_TIMEOUT_AUTO_ADJUSTED=0
if awk -v cur="$OUTER_TIMEOUT_SEC" -v min="$OUTER_TIMEOUT_MIN" 'BEGIN { exit !((cur + 0.0) < (min + 0.0)) }'; then
    OUTER_TIMEOUT_SEC="$OUTER_TIMEOUT_MIN"
    OUTER_TIMEOUT_AUTO_ADJUSTED=1
fi

write_timeout_stub_json() {
    local json_path="$1"
    local problem_name="$2"
    local hard_cap_sec="$3"
    local outer_timeout_sec="$4"
    local exit_code="$5"
    local timeout_ms

    timeout_ms="$(awk -v s="$outer_timeout_sec" 'BEGIN { printf "%.3f", s * 1000.0 }')"
    jq -n \
        --arg name "$problem_name" \
        --arg timeout_ms "$timeout_ms" \
        --arg hard_cap_sec "$hard_cap_sec" \
        --arg outer_timeout_sec "$outer_timeout_sec" \
        --arg exit_code "$exit_code" \
        '{
            problem: {name: $name},
            ralph: {status: "command_timeout", objective: 0, time_ms: ($timeout_ms | tonumber), iterations: 0},
            glpk: {status: "unknown", objective: 0, time_ms: 0, iterations: 0},
            validation: {status_match: false, objective_match: false, solution_valid: false},
            timing: {},
            performance: {},
            phase_hotspots: {phase1: {}, phase2: {}},
            refactor: {all_ms: 0, count: 0, reason_direction_stabilize: 0},
            lu: {sparse_dense_fallbacks: 0, retry_count: 0, markowitz_failures: 0},
            diagnosis: {
                issues: "benchmark command timed out before JSON completion",
                recommendations: [
                    "Increase outer timeout for compare mode if command timeouts persist",
                    "Run this instance standalone with ralph-benchmark for detailed profiling"
                ]
            },
            timeout: {
                exit_code: ($exit_code | tonumber),
                hard_cap_sec: ($hard_cap_sec | tonumber),
                outer_timeout_sec: ($outer_timeout_sec | tonumber)
            }
        }' > "$json_path"
}

if [[ "$NO_BUILD" -eq 0 ]]; then
    make -C "$RALPH_DIR" build-ralph-benchmark >/dev/null
fi

if [[ ! -x "$BENCH_EXEC" ]]; then
    echo "ERROR: benchmark executable is not runnable: $BENCH_EXEC" >&2
    exit 2
fi

if [[ -z "$OUTDIR" ]]; then
    stamp="$(date +%Y%m%d-%H%M%S)"
    OUTDIR="/tmp/netlib-regression-gate-$stamp"
fi
mkdir -p "$OUTDIR/results"

FILES_TXT="$OUTDIR/files.txt"
STATUS_TSV="$OUTDIR/status.tsv"
SELECTED_NAMES="$OUTDIR/selected.names.txt"
ALLOWLIST_NAMES="$OUTDIR/allowlist.names.txt"

find "$NETLIB_DIR" -maxdepth 1 -type f -name '*.mps' | LC_ALL=C sort > "$FILES_TXT"
if [[ -n "$FILTER_REGEX" ]]; then
    FILTERED_TXT="$OUTDIR/files.filtered.txt"
    : > "$FILTERED_TXT"
    while IFS= read -r fp; do
        bn="$(basename "$fp")"
        if [[ "$bn" =~ $FILTER_REGEX ]]; then
            echo "$fp" >> "$FILTERED_TXT"
        fi
    done < "$FILES_TXT"
    mv "$FILTERED_TXT" "$FILES_TXT"
fi

if [[ -n "$ALLOWLIST_FILE" ]]; then
    sed -e 's/#.*$//' \
        -e 's/[[:space:]]*$//' \
        -e 's/^[[:space:]]*//' \
        -e '/^$/d' "$ALLOWLIST_FILE" \
        | awk '{name=$0; if (name !~ /\.mps$/) name=name ".mps"; print name}' \
        | LC_ALL=C sort -u > "$ALLOWLIST_NAMES"

    FILTERED_TXT="$OUTDIR/files.allowlist.txt"
    : > "$FILTERED_TXT"
    while IFS= read -r fp; do
        bn="$(basename "$fp")"
        if grep -Fqx "$bn" "$ALLOWLIST_NAMES"; then
            echo "$fp" >> "$FILTERED_TXT"
        fi
    done < "$FILES_TXT"
    mv "$FILTERED_TXT" "$FILES_TXT"
else
    : > "$ALLOWLIST_NAMES"
fi

total="$(wc -l < "$FILES_TXT" | tr -d ' ')"
if [[ "$total" -eq 0 ]]; then
    echo "ERROR: no NETLIB files selected for regression gate." >&2
    exit 2
fi

awk -F/ '{print $NF}' "$FILES_TXT" | LC_ALL=C sort -u > "$SELECTED_NAMES"

echo "NETLIB regression gate"
echo "  baseline: $BASELINE_FILE"
echo "  netlib:   $NETLIB_DIR"
echo "  bench:    $BENCH_EXEC"
echo "  files:    $total"
echo "  hard-cap: $HARD_CAP_SEC sec"
if [[ "$OUTER_TIMEOUT_AUTO_ADJUSTED" -eq 1 ]]; then
    echo "  timeout:  $OUTER_TIMEOUT_SEC sec (external, auto-adjusted from $OUTER_TIMEOUT_ORIGINAL sec)"
else
    echo "  timeout:  $OUTER_TIMEOUT_SEC sec (external)"
fi
echo "  method:   $METHOD"
if [[ -n "$OBJ_REL_TOL" ]]; then
    echo "  obj-tol:  $OBJ_REL_TOL"
fi
if [[ -n "$ALLOWLIST_FILE" ]]; then
    echo "  allowlist:$ALLOWLIST_FILE"
fi
echo "  outdir:   $OUTDIR"

: > "$STATUS_TSV"
i=0
while IFS= read -r f; do
    i=$((i + 1))
    base="$(basename "$f" .mps)"
    name="$base.mps"
    json="$OUTDIR/results/$base.json"
    stderr_file="$OUTDIR/results/$base.stderr"
    run_method="$METHOD"
    run_obj_rel_tol="$OBJ_REL_TOL"

    if [[ "$METHOD_FROM_CLI" -eq 0 ]]; then
        override_method="$(jq -r --arg name "$name" \
            '.problem_overrides[$name].method // empty' "$BASELINE_FILE")"
        if [[ -n "$override_method" ]]; then
            run_method="$override_method"
        fi
    fi
    if [[ "$OBJ_REL_TOL_FROM_CLI" -eq 0 ]]; then
        override_obj_rel_tol="$(jq -r --arg name "$name" \
            '.problem_overrides[$name].obj_rel_tol // empty' "$BASELINE_FILE")"
        if [[ -n "$override_obj_rel_tol" ]]; then
            run_obj_rel_tol="$override_obj_rel_tol"
        fi
    fi

    bench_cmd=("$BENCH_EXEC" --hard-cap "$HARD_CAP_SEC" --method "$run_method")
    if [[ -n "$run_obj_rel_tol" ]]; then
        bench_cmd+=(--obj-rel-tol "$run_obj_rel_tol")
    fi
    bench_cmd+=("$f")

    if [[ -n "$run_obj_rel_tol" ]]; then
        echo "[$i/$total] $name (method=$run_method obj_rel_tol=$run_obj_rel_tol)"
    else
        echo "[$i/$total] $name (method=$run_method)"
    fi
    set +e
    "$TIMEOUT_BIN" -k 5 "$OUTER_TIMEOUT_SEC" \
        "${bench_cmd[@]}" \
        > "$json" 2> "$stderr_file"
    ec=$?
    set -e
    if [[ "$ec" -eq 124 && ! -s "$json" ]]; then
        write_timeout_stub_json "$json" "$name" "$HARD_CAP_SEC" "$OUTER_TIMEOUT_SEC" "$ec"
    fi
    printf "%s\t%s\n" "$name" "$ec" >> "$STATUS_TSV"
done < "$FILES_TXT"

known_status="$OUTDIR/known.status_mismatch.txt"
known_obj="$OUTDIR/known.objective_mismatch.txt"
known_sol="$OUTDIR/known.solution_invalid.txt"
known_timeout="$OUTDIR/known.timeouts.txt"
required_pass="$OUTDIR/required.pass.txt"
required_coverage="$OUTDIR/required.coverage.txt"

jq -r '.known_status_mismatch[]?' "$BASELINE_FILE" | LC_ALL=C sort -u > "$known_status"
jq -r '.known_objective_mismatch[]?' "$BASELINE_FILE" | LC_ALL=C sort -u > "$known_obj"
jq -r '.known_solution_invalid[]?' "$BASELINE_FILE" | LC_ALL=C sort -u > "$known_sol"
jq -r '.known_timeouts[]?' "$BASELINE_FILE" | LC_ALL=C sort -u > "$known_timeout"
jq -r '.required_pass[]?' "$BASELINE_FILE" | LC_ALL=C sort -u > "$required_pass"
jq -r '.required_coverage[]?' "$BASELINE_FILE" | LC_ALL=C sort -u > "$required_coverage"

actual_timeout="$OUTDIR/actual.timeouts.txt"
actual_cmd_fail="$OUTDIR/actual.command_failures.txt"
actual_status="$OUTDIR/actual.status_mismatch.txt"
actual_obj="$OUTDIR/actual.objective_mismatch.txt"
actual_sol="$OUTDIR/actual.solution_invalid.txt"
actual_dense="$OUTDIR/actual.dense_fallback.txt"
solved_jsons="$OUTDIR/solved.jsons.txt"
phase1_no_pivot_ladder_tsv="$OUTDIR/phase1_no_pivot_ladder.tsv"

: > "$actual_timeout"
: > "$actual_cmd_fail"
: > "$actual_status"
: > "$actual_obj"
: > "$actual_sol"
: > "$actual_dense"
: > "$solved_jsons"
printf "problem\tno_progress_events\tretry_defers\tdual_rescue_attempts\tdual_rescue_successes\tdual_rescue_failures\tforced_refactors\tevents_ratio_breakdown\tevents_dir_skip\tevents_pivot_fail\tretry_ratio_breakdown\tretry_dir_skip\tretry_pivot_fail\tdual_attempts_ratio_breakdown\tdual_attempts_dir_skip\tdual_attempts_pivot_fail\tforced_ratio_breakdown\tforced_dir_skip\tforced_pivot_fail\trescue_guard_cooldown_blocks\trescue_guard_fail_cap_forces\n" > "$phase1_no_pivot_ladder_tsv"

while IFS=$'\t' read -r name ec; do
    base="${name%.mps}"
    json="$OUTDIR/results/$base.json"

    if [[ "$ec" -eq 124 ]]; then
        echo "$name" >> "$actual_timeout"
        continue
    fi
    if [[ "$ec" -ne 0 ]]; then
        echo "$name" >> "$actual_cmd_fail"
        continue
    fi
    if [[ ! -s "$json" ]]; then
        echo "$name" >> "$actual_cmd_fail"
        continue
    fi
    echo "$json" >> "$solved_jsons"

    rec="$(jq -r '
        [
          .problem.name,
          .ralph.status,
          .glpk.status,
          (if .validation.objective_match == true then "true" else "false" end),
          (if .validation.solution_valid == true then "true" else "false" end),
          ((.lu.sparse_dense_fallbacks // 0) | tostring),
          ((.refactor.phase1_no_pivot_no_progress_events // .phase_hotspots.phase1.no_pivot_no_progress_events // 0) | tostring),
          ((.refactor.phase1_no_pivot_ladder_retry_defers // .phase_hotspots.phase1.no_pivot_ladder_retry_defers // 0) | tostring),
          ((.refactor.phase1_no_pivot_ladder_dual_rescue_attempts // .phase_hotspots.phase1.no_pivot_ladder_dual_rescue_attempts // 0) | tostring),
          ((.refactor.phase1_no_pivot_ladder_dual_rescue_successes // .phase_hotspots.phase1.no_pivot_ladder_dual_rescue_successes // 0) | tostring),
          ((.refactor.phase1_no_pivot_ladder_dual_rescue_failures // .phase_hotspots.phase1.no_pivot_ladder_dual_rescue_failures // 0) | tostring),
          ((.refactor.phase1_no_pivot_ladder_forced_refactors // .phase_hotspots.phase1.no_pivot_ladder_forced_refactors // 0) | tostring),
          ((.refactor.phase1_no_pivot_events_ratio_breakdown // .phase_hotspots.phase1.no_pivot_events_ratio_breakdown // 0) | tostring),
          ((.refactor.phase1_no_pivot_events_dir_skip // .phase_hotspots.phase1.no_pivot_events_dir_skip // 0) | tostring),
          ((.refactor.phase1_no_pivot_events_pivot_fail // .phase_hotspots.phase1.no_pivot_events_pivot_fail // 0) | tostring),
          ((.refactor.phase1_no_pivot_ladder_retry_ratio_breakdown // .phase_hotspots.phase1.no_pivot_ladder_retry_ratio_breakdown // 0) | tostring),
          ((.refactor.phase1_no_pivot_ladder_retry_dir_skip // .phase_hotspots.phase1.no_pivot_ladder_retry_dir_skip // 0) | tostring),
          ((.refactor.phase1_no_pivot_ladder_retry_pivot_fail // .phase_hotspots.phase1.no_pivot_ladder_retry_pivot_fail // 0) | tostring),
          ((.refactor.phase1_no_pivot_ladder_dual_rescue_attempts_ratio_breakdown // .phase_hotspots.phase1.no_pivot_ladder_dual_rescue_attempts_ratio_breakdown // 0) | tostring),
          ((.refactor.phase1_no_pivot_ladder_dual_rescue_attempts_dir_skip // .phase_hotspots.phase1.no_pivot_ladder_dual_rescue_attempts_dir_skip // 0) | tostring),
          ((.refactor.phase1_no_pivot_ladder_dual_rescue_attempts_pivot_fail // .phase_hotspots.phase1.no_pivot_ladder_dual_rescue_attempts_pivot_fail // 0) | tostring),
          ((.refactor.phase1_no_pivot_ladder_forced_refactors_ratio_breakdown // .phase_hotspots.phase1.no_pivot_ladder_forced_refactors_ratio_breakdown // 0) | tostring),
          ((.refactor.phase1_no_pivot_ladder_forced_refactors_dir_skip // .phase_hotspots.phase1.no_pivot_ladder_forced_refactors_dir_skip // 0) | tostring),
          ((.refactor.phase1_no_pivot_ladder_forced_refactors_pivot_fail // .phase_hotspots.phase1.no_pivot_ladder_forced_refactors_pivot_fail // 0) | tostring),
          ((.refactor.phase1_no_pivot_ladder_rescue_guard_cooldown_blocks // .phase_hotspots.phase1.no_pivot_ladder_rescue_guard_cooldown_blocks // 0) | tostring),
          ((.refactor.phase1_no_pivot_ladder_rescue_guard_fail_cap_forces // .phase_hotspots.phase1.no_pivot_ladder_rescue_guard_fail_cap_forces // 0) | tostring)
        ] | @tsv' "$json" 2>/dev/null || true)"
    if [[ -z "$rec" ]]; then
        echo "$name" >> "$actual_cmd_fail"
        continue
    fi

    IFS=$'\t' read -r prob_name r_status g_status obj_ok sol_ok dense_fb \
        phase1_no_pivot_no_progress_events phase1_no_pivot_ladder_retry_defers \
        phase1_no_pivot_ladder_dual_rescue_attempts phase1_no_pivot_ladder_dual_rescue_successes \
        phase1_no_pivot_ladder_dual_rescue_failures phase1_no_pivot_ladder_forced_refactors \
        phase1_no_pivot_events_ratio_breakdown phase1_no_pivot_events_dir_skip \
        phase1_no_pivot_events_pivot_fail phase1_no_pivot_ladder_retry_ratio_breakdown \
        phase1_no_pivot_ladder_retry_dir_skip phase1_no_pivot_ladder_retry_pivot_fail \
        phase1_no_pivot_ladder_dual_rescue_attempts_ratio_breakdown \
        phase1_no_pivot_ladder_dual_rescue_attempts_dir_skip \
        phase1_no_pivot_ladder_dual_rescue_attempts_pivot_fail \
        phase1_no_pivot_ladder_forced_refactors_ratio_breakdown \
        phase1_no_pivot_ladder_forced_refactors_dir_skip \
        phase1_no_pivot_ladder_forced_refactors_pivot_fail \
        phase1_no_pivot_ladder_rescue_guard_cooldown_blocks \
        phase1_no_pivot_ladder_rescue_guard_fail_cap_forces <<< "$rec"
    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
        "$prob_name" \
        "${phase1_no_pivot_no_progress_events:-0}" \
        "${phase1_no_pivot_ladder_retry_defers:-0}" \
        "${phase1_no_pivot_ladder_dual_rescue_attempts:-0}" \
        "${phase1_no_pivot_ladder_dual_rescue_successes:-0}" \
        "${phase1_no_pivot_ladder_dual_rescue_failures:-0}" \
        "${phase1_no_pivot_ladder_forced_refactors:-0}" \
        "${phase1_no_pivot_events_ratio_breakdown:-0}" \
        "${phase1_no_pivot_events_dir_skip:-0}" \
        "${phase1_no_pivot_events_pivot_fail:-0}" \
        "${phase1_no_pivot_ladder_retry_ratio_breakdown:-0}" \
        "${phase1_no_pivot_ladder_retry_dir_skip:-0}" \
        "${phase1_no_pivot_ladder_retry_pivot_fail:-0}" \
        "${phase1_no_pivot_ladder_dual_rescue_attempts_ratio_breakdown:-0}" \
        "${phase1_no_pivot_ladder_dual_rescue_attempts_dir_skip:-0}" \
        "${phase1_no_pivot_ladder_dual_rescue_attempts_pivot_fail:-0}" \
        "${phase1_no_pivot_ladder_forced_refactors_ratio_breakdown:-0}" \
        "${phase1_no_pivot_ladder_forced_refactors_dir_skip:-0}" \
        "${phase1_no_pivot_ladder_forced_refactors_pivot_fail:-0}" \
        "${phase1_no_pivot_ladder_rescue_guard_cooldown_blocks:-0}" \
        "${phase1_no_pivot_ladder_rescue_guard_fail_cap_forces:-0}" \
        >> "$phase1_no_pivot_ladder_tsv"
    if [[ "$r_status" == "timeout" ]]; then
        echo "$prob_name" >> "$actual_timeout"
        continue
    fi
    if [[ "$r_status" != "$g_status" ]]; then
        echo "$prob_name" >> "$actual_status"
    fi
    if [[ "$obj_ok" != "true" ]]; then
        echo "$prob_name" >> "$actual_obj"
    fi
    if [[ "$sol_ok" != "true" ]]; then
        echo "$prob_name" >> "$actual_sol"
    fi
    if [[ "$dense_fb" -gt 0 ]]; then
        echo "$prob_name" >> "$actual_dense"
    fi
done < "$STATUS_TSV"

LC_ALL=C sort -u "$actual_timeout" -o "$actual_timeout"
LC_ALL=C sort -u "$actual_cmd_fail" -o "$actual_cmd_fail"
LC_ALL=C sort -u "$actual_status" -o "$actual_status"
LC_ALL=C sort -u "$actual_obj" -o "$actual_obj"
LC_ALL=C sort -u "$actual_sol" -o "$actual_sol"
LC_ALL=C sort -u "$actual_dense" -o "$actual_dense"

unexpected_timeout="$OUTDIR/unexpected.timeouts.txt"
unexpected_status="$OUTDIR/unexpected.status_mismatch.txt"
unexpected_obj="$OUTDIR/unexpected.objective_mismatch.txt"
unexpected_sol="$OUTDIR/unexpected.solution_invalid.txt"
missing_required="$OUTDIR/missing.required.txt"
actual_fail_any="$OUTDIR/actual.fail_any.txt"
required_failed="$OUTDIR/required.failed.txt"
missing_allowlist="$OUTDIR/missing.allowlist.txt"
missing_coverage="$OUTDIR/missing.coverage.txt"

comm -23 "$actual_timeout" "$known_timeout" > "$unexpected_timeout"
comm -23 "$actual_status" "$known_status" > "$unexpected_status"
comm -23 "$actual_obj" "$known_obj" > "$unexpected_obj"
comm -23 "$actual_sol" "$known_sol" > "$unexpected_sol"

if [[ -s "$required_pass" ]]; then
    {
        cat "$actual_timeout"
        cat "$actual_cmd_fail"
        cat "$actual_status"
        cat "$actual_obj"
        cat "$actual_sol"
        cat "$actual_dense"
    } | LC_ALL=C sort -u > "$actual_fail_any"
    comm -12 "$required_pass" "$actual_fail_any" > "$required_failed"
else
    : > "$actual_fail_any"
    : > "$required_failed"
fi

if [[ -s "$required_pass" && -z "$FILTER_REGEX" && -z "$ALLOWLIST_FILE" ]]; then
    comm -23 "$required_pass" "$SELECTED_NAMES" > "$missing_required"
else
    : > "$missing_required"
fi

if [[ -s "$required_coverage" && -z "$FILTER_REGEX" && -z "$ALLOWLIST_FILE" ]]; then
    comm -23 "$required_coverage" "$SELECTED_NAMES" > "$missing_coverage"
else
    : > "$missing_coverage"
fi

if [[ -n "$ALLOWLIST_FILE" ]]; then
    comm -23 "$ALLOWLIST_NAMES" "$SELECTED_NAMES" > "$missing_allowlist"
else
    : > "$missing_allowlist"
fi

require_zero_dense="$(jq -r '.require_zero_dense_fallback // true' "$BASELINE_FILE")"

timeout_count="$(wc -l < "$actual_timeout" | tr -d ' ')"
cmd_fail_count="$(wc -l < "$actual_cmd_fail" | tr -d ' ')"
status_count="$(wc -l < "$actual_status" | tr -d ' ')"
obj_count="$(wc -l < "$actual_obj" | tr -d ' ')"
sol_count="$(wc -l < "$actual_sol" | tr -d ' ')"
dense_count="$(wc -l < "$actual_dense" | tr -d ' ')"

unexpected_timeout_count="$(wc -l < "$unexpected_timeout" | tr -d ' ')"
unexpected_status_count="$(wc -l < "$unexpected_status" | tr -d ' ')"
unexpected_obj_count="$(wc -l < "$unexpected_obj" | tr -d ' ')"
unexpected_sol_count="$(wc -l < "$unexpected_sol" | tr -d ' ')"
missing_required_count="$(wc -l < "$missing_required" | tr -d ' ')"
required_failed_count="$(wc -l < "$required_failed" | tr -d ' ')"
missing_allowlist_count="$(wc -l < "$missing_allowlist" | tr -d ' ')"
missing_coverage_count="$(wc -l < "$missing_coverage" | tr -d ' ')"

read -r ladder_no_progress_total ladder_retry_defers_total \
    ladder_dual_rescue_attempts_total ladder_dual_rescue_successes_total \
    ladder_dual_rescue_failures_total ladder_forced_refactors_total \
    ladder_events_ratio_total ladder_events_dir_total ladder_events_pivot_total \
    ladder_retry_ratio_total ladder_retry_dir_total ladder_retry_pivot_total \
    ladder_dual_attempt_ratio_total ladder_dual_attempt_dir_total ladder_dual_attempt_pivot_total \
    ladder_forced_ratio_total ladder_forced_dir_total ladder_forced_pivot_total \
    ladder_guard_cooldown_total ladder_guard_fail_cap_total <<< "$(
    awk -F'\t' '
        NR > 1 {
            no_progress += ($2 + 0);
            retry += ($3 + 0);
            attempts += ($4 + 0);
            successes += ($5 + 0);
            failures += ($6 + 0);
            forced += ($7 + 0);
            events_ratio += ($8 + 0);
            events_dir += ($9 + 0);
            events_pivot += ($10 + 0);
            retry_ratio += ($11 + 0);
            retry_dir += ($12 + 0);
            retry_pivot += ($13 + 0);
            dual_attempt_ratio += ($14 + 0);
            dual_attempt_dir += ($15 + 0);
            dual_attempt_pivot += ($16 + 0);
            forced_ratio += ($17 + 0);
            forced_dir += ($18 + 0);
            forced_pivot += ($19 + 0);
            guard_cooldown += ($20 + 0);
            guard_fail_cap += ($21 + 0);
        }
        END {
            printf "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                   no_progress, retry, attempts, successes, failures, forced,
                   events_ratio, events_dir, events_pivot,
                   retry_ratio, retry_dir, retry_pivot,
                   dual_attempt_ratio, dual_attempt_dir, dual_attempt_pivot,
                   forced_ratio, forced_dir, forced_pivot,
                   guard_cooldown, guard_fail_cap;
        }' "$phase1_no_pivot_ladder_tsv"
)"

ladder_dual_rescue_success_rate="0.0"
if [[ "${ladder_dual_rescue_attempts_total:-0}" -gt 0 ]]; then
    ladder_dual_rescue_success_rate="$(
        awk -v s="$ladder_dual_rescue_successes_total" -v a="$ladder_dual_rescue_attempts_total" \
            'BEGIN { printf "%.1f", (100.0 * s) / a }'
    )"
fi

echo
echo "Summary:"
echo "  total files:            $total"
echo "  timeout files:          $timeout_count"
echo "  command failures:       $cmd_fail_count"
echo "  status mismatches:      $status_count"
echo "  objective mismatches:   $obj_count"
echo "  invalid solutions:      $sol_count"
echo "  dense fallback files:   $dense_count"
echo
echo "Phase1 no-pivot ladder telemetry (aggregate on solved files):"
echo "  no-progress events:     ${ladder_no_progress_total:-0}"
echo "  retry defers:           ${ladder_retry_defers_total:-0}"
echo "  dual rescue attempts:   ${ladder_dual_rescue_attempts_total:-0}"
echo "  dual rescue successes:  ${ladder_dual_rescue_successes_total:-0}"
echo "  dual rescue failures:   ${ladder_dual_rescue_failures_total:-0}"
echo "  forced refactors:       ${ladder_forced_refactors_total:-0}"
echo "  dual rescue success %:  ${ladder_dual_rescue_success_rate}%"
echo "  events by cause:        ratio=${ladder_events_ratio_total:-0} dir_skip=${ladder_events_dir_total:-0} pivot_fail=${ladder_events_pivot_total:-0}"
echo "  retry by cause:         ratio=${ladder_retry_ratio_total:-0} dir_skip=${ladder_retry_dir_total:-0} pivot_fail=${ladder_retry_pivot_total:-0}"
echo "  rescue attempts cause:  ratio=${ladder_dual_attempt_ratio_total:-0} dir_skip=${ladder_dual_attempt_dir_total:-0} pivot_fail=${ladder_dual_attempt_pivot_total:-0}"
echo "  forced refs by cause:   ratio=${ladder_forced_ratio_total:-0} dir_skip=${ladder_forced_dir_total:-0} pivot_fail=${ladder_forced_pivot_total:-0}"
echo "  rescue guard blocks:    cooldown=${ladder_guard_cooldown_total:-0} fail_cap=${ladder_guard_fail_cap_total:-0}"
echo "  per-file ladder TSV:    $phase1_no_pivot_ladder_tsv"

echo
echo "Unexpected vs baseline:"
echo "  new timeouts:           $unexpected_timeout_count"
echo "  new status mismatch:    $unexpected_status_count"
echo "  new objective mismatch: $unexpected_obj_count"
echo "  new invalid solutions:  $unexpected_sol_count"
echo "  missing required-pass:  $missing_required_count"
echo "  failed required-pass:   $required_failed_count"
echo "  missing coverage:       $missing_coverage_count"
echo "  missing allowlist:      $missing_allowlist_count"

gate_fail=0

if [[ "$cmd_fail_count" -gt 0 ]]; then
    echo
    echo "FAIL: command failures (non-timeout):"
    cat "$actual_cmd_fail"
    gate_fail=1
fi

if [[ "$unexpected_timeout_count" -gt 0 ]]; then
    echo
    echo "FAIL: new timeout regressions:"
    cat "$unexpected_timeout"
    gate_fail=1
fi

if [[ "$unexpected_status_count" -gt 0 ]]; then
    echo
    echo "FAIL: new status mismatches:"
    cat "$unexpected_status"
    gate_fail=1
fi

if [[ "$unexpected_obj_count" -gt 0 ]]; then
    echo
    echo "FAIL: new objective mismatches:"
    cat "$unexpected_obj"
    gate_fail=1
fi

if [[ "$unexpected_sol_count" -gt 0 ]]; then
    echo
    echo "FAIL: new invalid solutions:"
    cat "$unexpected_sol"
    gate_fail=1
fi

if [[ "$require_zero_dense" == "true" && "$dense_count" -gt 0 ]]; then
    echo
    echo "FAIL: dense fallback observed in sparse LU path:"
    cat "$actual_dense"
    gate_fail=1
fi

if [[ "$missing_required_count" -gt 0 ]]; then
    echo
    echo "FAIL: required-pass problems missing from full gate selection:"
    cat "$missing_required"
    gate_fail=1
fi

if [[ "$required_failed_count" -gt 0 ]]; then
    echo
    echo "FAIL: required-pass problems failed gate checks:"
    cat "$required_failed"
    gate_fail=1
fi

if [[ "$missing_coverage_count" -gt 0 ]]; then
    echo
    echo "FAIL: required coverage problems missing from selected NETLIB set:"
    cat "$missing_coverage"
    gate_fail=1
fi

if [[ "$missing_allowlist_count" -gt 0 ]]; then
    echo
    echo "FAIL: allowlist problems missing from selected NETLIB set:"
    cat "$missing_allowlist"
    gate_fail=1
fi

if [[ "$gate_fail" -ne 0 ]]; then
    echo
    echo "NETLIB regression gate: FAILED"
    echo "Artifacts: $OUTDIR"
    exit 1
fi

echo
echo "NETLIB regression gate: PASSED"
echo "Artifacts: $OUTDIR"
