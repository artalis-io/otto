#!/usr/bin/env bash
set -euo pipefail

bench="./ralph-benchmark"
if [[ "${1:-}" == "--bench" ]]; then
  bench="${2:?missing benchmark path}"
fi

solved_canaries=(25fv47 bnl1 bnl2 maros nesm)
fit2p_art_limit="${PHASE1_FIT2P_ART_LIMIT:-9000}"
fit2p_artmax_limit="${PHASE1_FIT2P_ARTMAX_LIMIT:-200}"

tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/phase1-canaries.XXXXXX")"
trap 'rm -rf "$tmpdir"' EXIT

run_case() {
  local problem="$1"
  local time_mult="$2"
  local out="$tmpdir/${problem}.json"

  OMP_NUM_THREADS=1 "$bench" \
    --netlib "$problem" \
    --hard-cap 10 \
    --time-mult "$time_mult" \
    --no-adaptive-fallback > "$out"
  printf '%s\n' "$out"
}

check_solved() {
  local problem="$1"
  local out
  out="$(run_case "$problem" 1)"
  jq -e --arg problem "$problem" '
    .ralph.status == "optimal" and
    .validation.solution_valid == true and
    .validation.objective_match == true and
    .ralph.phase1_artificial_sum == 0 and
    .lu.sparse_numeric_last_failure_reason == "none"
  ' "$out" >/dev/null
  jq -c --arg problem "$problem" '{
    problem:$problem,
    status:.ralph.status,
    valid:.validation.solution_valid,
    objective_match:.validation.objective_match,
    time_ms:.ralph.time_ms,
    iterations:.ralph.iterations,
    artificial_sum:.ralph.phase1_artificial_sum,
    artificial_basic:.ralph.phase1_artificial_basic,
    sparse_numeric_failure:.lu.sparse_numeric_last_failure_reason,
    cleanup:{attempts:.phase_hotspots.phase1.cleanup_attempts,accepted:.phase_hotspots.phase1.cleanup_accepted,rejected:.phase_hotspots.phase1.cleanup_rejected,probe_rejects:.phase_hotspots.phase1.cleanup_candidate_probe_rejects},
    progress_window:{refactors:.phase_hotspots.phase1.progress_window_refactors,cleanups:.phase_hotspots.phase1.progress_window_cleanups,perturbs:.phase_hotspots.phase1.progress_window_perturbs}
  }' "$out"
}

check_fit2p() {
  local out
  out="$(run_case fit2p 10)"
  jq -e \
    --argjson art_limit "$fit2p_art_limit" \
    --argjson artmax_limit "$fit2p_artmax_limit" '
    (.ralph.status == "optimal" or .ralph.status == "timeout") and
    (.ralph.status != "optimal" or (.validation.solution_valid == true and .validation.objective_match == true)) and
    .ralph.phase1_artificial_sum <= $art_limit and
    .ralph.phase1_artificial_max <= $artmax_limit and
    .lu.sparse_numeric_last_failure_reason == "none"
  ' "$out" >/dev/null
  jq -c '{
    problem:"fit2p",
    status:.ralph.status,
    valid:.validation.solution_valid,
    objective_match:.validation.objective_match,
    time_ms:.ralph.time_ms,
    iterations:.ralph.iterations,
    artificial_sum:.ralph.phase1_artificial_sum,
    artificial_max:.ralph.phase1_artificial_max,
    artificial_basic:.ralph.phase1_artificial_basic,
    sparse_numeric_failure:.lu.sparse_numeric_last_failure_reason,
    cleanup:{attempts:.phase_hotspots.phase1.cleanup_attempts,accepted:.phase_hotspots.phase1.cleanup_accepted,rejected:.phase_hotspots.phase1.cleanup_rejected,probe_rejects:.phase_hotspots.phase1.cleanup_candidate_probe_rejects},
    progress_window:{refactors:.phase_hotspots.phase1.progress_window_refactors,cleanups:.phase_hotspots.phase1.progress_window_cleanups,perturbs:.phase_hotspots.phase1.progress_window_perturbs}
  }' "$out"
}

for problem in "${solved_canaries[@]}"; do
  check_solved "$problem"
done
check_fit2p
