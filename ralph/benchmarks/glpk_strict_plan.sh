#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
RALPH_DIR="$ROOT_DIR/ralph"
STATE_FILE="$ROOT_DIR/docs/roadmaps/ralph_glpk_strict_state.json"

now_utc() {
    date -u +"%Y-%m-%dT%H:%M:%SZ"
}

phase_name() {
    case "${1:-}" in
        1) echo "pivot_quality_loop" ;;
        2) echo "working_lp_semantics" ;;
        3) echo "perturbation_state_machine" ;;
        *) return 1 ;;
    esac
}

usage() {
    cat <<'EOF'
Usage: glpk_strict_plan.sh <command> [options]

Commands:
  init
      Initialize phase-state tracking file if missing.

  status
      Print current phase-tracker status and next actionable phase.

  next
      Print only the next actionable phase.

  start <phase>
      Mark phase (1..3) as in_progress. Enforces ordered execution.

  complete <phase> --artifact <dir> [--full-gate]
      Validate a NETLIB gate artifact and mark phase completed.
      --full-gate additionally requires missing.required + missing.coverage == 0.

  validate-artifact --artifact <dir> [--full-gate]
      Validate a NETLIB gate artifact only (no state mutation).

  run-phase <phase> [--small-only]
      Ordered runner:
      - test-simplex-policy
      - test-lp-policy-glpk-compat
      - test-lu-markowitz
      - test-netlib-gate-small
      - test-netlib-gate (unless --small-only)
      If all checks pass, marks phase completed.

Examples:
  bash ralph/benchmarks/glpk_strict_plan.sh init
  bash ralph/benchmarks/glpk_strict_plan.sh status
  bash ralph/benchmarks/glpk_strict_plan.sh run-phase 1
EOF
}

init_state_if_missing() {
    if [[ -f "$STATE_FILE" ]]; then
        return 0
    fi
    mkdir -p "$(dirname "$STATE_FILE")"
    local ts
    ts="$(now_utc)"
    cat > "$STATE_FILE" <<EOF
{
  "plan": "GLPK strict parity execution",
  "created_at": "$ts",
  "updated_at": "$ts",
  "current_phase": 0,
  "phases": {
    "1": {
      "name": "pivot_quality_loop",
      "status": "pending",
      "started_at": null,
      "completed_at": null,
      "last_artifact": null,
      "last_validation": null
    },
    "2": {
      "name": "working_lp_semantics",
      "status": "pending",
      "started_at": null,
      "completed_at": null,
      "last_artifact": null,
      "last_validation": null
    },
    "3": {
      "name": "perturbation_state_machine",
      "status": "pending",
      "started_at": null,
      "completed_at": null,
      "last_artifact": null,
      "last_validation": null
    }
  }
}
EOF
}

require_jq() {
    if ! command -v jq >/dev/null 2>&1; then
        echo "ERROR: jq is required." >&2
        exit 2
    fi
}

count_lines() {
    local f="$1"
    if [[ -f "$f" ]]; then
        wc -l < "$f" | tr -d ' '
    else
        echo 0
    fi
}

validate_artifact_dir() {
    local artifact="$1"
    local require_full="$2"

    if [[ ! -d "$artifact" ]]; then
        echo "ERROR: artifact directory not found: $artifact" >&2
        return 2
    fi

    local cmd_fail timeout unexpected_timeout unexpected_status unexpected_obj unexpected_sol dense req_failed miss_req miss_cov
    cmd_fail="$(count_lines "$artifact/actual.command_failures.txt")"
    timeout="$(count_lines "$artifact/unexpected.timeouts.txt")"
    unexpected_timeout="$timeout"
    unexpected_status="$(count_lines "$artifact/unexpected.status_mismatch.txt")"
    unexpected_obj="$(count_lines "$artifact/unexpected.objective_mismatch.txt")"
    unexpected_sol="$(count_lines "$artifact/unexpected.solution_invalid.txt")"
    dense="$(count_lines "$artifact/actual.dense_fallback.txt")"
    req_failed="$(count_lines "$artifact/required.failed.txt")"
    miss_req="$(count_lines "$artifact/missing.required.txt")"
    miss_cov="$(count_lines "$artifact/missing.coverage.txt")"

    echo "Artifact check:"
    echo "  artifact: $artifact"
    echo "  command_failures: $cmd_fail"
    echo "  unexpected_timeouts: $unexpected_timeout"
    echo "  unexpected_status_mismatch: $unexpected_status"
    echo "  unexpected_objective_mismatch: $unexpected_obj"
    echo "  unexpected_solution_invalid: $unexpected_sol"
    echo "  dense_fallback_files: $dense"
    echo "  required_failed: $req_failed"
    if [[ "$require_full" -eq 1 ]]; then
        echo "  missing_required: $miss_req"
        echo "  missing_coverage: $miss_cov"
    fi

    if [[ "$cmd_fail" -ne 0 || "$unexpected_timeout" -ne 0 || "$unexpected_status" -ne 0 || "$unexpected_obj" -ne 0 || "$unexpected_sol" -ne 0 || "$dense" -ne 0 || "$req_failed" -ne 0 ]]; then
        return 1
    fi
    if [[ "$require_full" -eq 1 ]]; then
        if [[ "$miss_req" -ne 0 || "$miss_cov" -ne 0 ]]; then
            return 1
        fi
    fi
    return 0
}

ensure_order_startable() {
    local phase="$1"
    local prev="$((phase - 1))"
    if [[ "$phase" -gt 1 ]]; then
        local prev_status
        prev_status="$(jq -r --arg p "$prev" '.phases[$p].status' "$STATE_FILE")"
        if [[ "$prev_status" != "completed" ]]; then
            echo "ERROR: phase $phase cannot start before phase $prev is completed (current: $prev_status)." >&2
            return 1
        fi
    fi
    return 0
}

set_phase_status() {
    local phase="$1"
    local status="$2"
    local artifact="$3"
    local validation="$4"
    local ts
    ts="$(now_utc)"

    local tmp
    tmp="$(mktemp)"
    jq \
        --arg p "$phase" \
        --arg s "$status" \
        --arg t "$ts" \
        --arg a "$artifact" \
        --arg v "$validation" \
        '
        .updated_at = $t
        | .current_phase = ($p|tonumber)
        | .phases[$p].status = $s
        | .phases[$p].last_artifact = (if $a == "" then .phases[$p].last_artifact else $a end)
        | .phases[$p].last_validation = (if $v == "" then .phases[$p].last_validation else $v end)
        | (if $s == "in_progress" then .phases[$p].started_at = $t else . end)
        | (if $s == "completed" then .phases[$p].completed_at = $t else . end)
        ' "$STATE_FILE" > "$tmp"
    mv "$tmp" "$STATE_FILE"
}

print_status() {
    jq -r '
      . as $s
      | "State: " + $s.plan,
        "File: " + input_filename,
        ("Updated: " + $s.updated_at),
        "",
        "Phases:",
        (
          $s.phases
          | to_entries
          | sort_by(.key|tonumber)
          | .[]
          | "  " + .key + " - " + .value.name + " [" + .value.status + "]"
            + (if .value.completed_at then " completed_at=" + .value.completed_at else "" end)
            + (if .value.last_artifact then " artifact=" + .value.last_artifact else "" end)
        ),
        "",
        (
          "Next: phase " +
          (
            (
              ($s.phases | to_entries | sort_by(.key|tonumber) | map(select(.value.status != "completed")) | .[0].key)
              // "done"
            ) | tostring
          )
        )
    ' "$STATE_FILE"
}

print_next() {
    local next
    next="$(jq -r '((.phases | to_entries | sort_by(.key|tonumber) | map(select(.value.status != "completed")) | .[0].key) // "done")' "$STATE_FILE")"
    if [[ "$next" == "done" ]]; then
        echo "All phases completed."
    else
        local name
        name="$(jq -r --arg p "$next" '.phases[$p].name' "$STATE_FILE")"
        echo "Next phase: $next ($name)"
    fi
}

extract_outdir_from_log() {
    local log_file="$1"
    awk '/^[[:space:]]*outdir:[[:space:]]*/ {print $2}' "$log_file" | tail -n 1
}

run_phase() {
    local phase="$1"
    local small_only="$2"
    local phase_token
    phase_token="$(phase_name "$phase")"
    ensure_order_startable "$phase"

    set_phase_status "$phase" "in_progress" "" ""
    echo "Running phase $phase ($phase_token)"

    make -C "$RALPH_DIR" test-simplex-policy
    make -C "$RALPH_DIR" test-lp-policy-glpk-compat
    make -C "$RALPH_DIR" test-lu-markowitz

    local stamp small_log small_artifact
    stamp="$(date +%Y%m%d-%H%M%S)"
    small_log="/tmp/ralph_glpk_phase${phase}_small_${stamp}.log"
    (
        cd "$RALPH_DIR"
        make test-netlib-gate-small
    ) 2>&1 | tee "$small_log"
    small_artifact="$(extract_outdir_from_log "$small_log")"
    if [[ -z "$small_artifact" ]]; then
        echo "ERROR: could not detect small-gate artifact from $small_log" >&2
        exit 1
    fi
    validate_artifact_dir "$small_artifact" 0

    if [[ "$small_only" -eq 1 ]]; then
        set_phase_status "$phase" "in_progress" "$small_artifact" "small_gate_pass"
        echo "Phase $phase small gate passed (full gate skipped by --small-only)."
        return 0
    fi

    local full_log full_artifact
    full_log="/tmp/ralph_glpk_phase${phase}_full_${stamp}.log"
    (
        cd "$RALPH_DIR"
        make test-netlib-gate
    ) 2>&1 | tee "$full_log"
    full_artifact="$(extract_outdir_from_log "$full_log")"
    if [[ -z "$full_artifact" ]]; then
        echo "ERROR: could not detect full-gate artifact from $full_log" >&2
        exit 1
    fi
    validate_artifact_dir "$full_artifact" 1

    set_phase_status "$phase" "completed" "$full_artifact" "full_gate_pass"
    echo "Phase $phase completed."
}

main() {
    require_jq
    init_state_if_missing

    local cmd="${1:-}"
    if [[ -z "$cmd" ]]; then
        usage
        exit 2
    fi
    shift || true

    case "$cmd" in
        init)
            echo "Initialized: $STATE_FILE"
            ;;
        status)
            print_status
            ;;
        next)
            print_next
            ;;
        start)
            local phase="${1:-}"
            if [[ -z "$phase" ]]; then
                echo "ERROR: missing phase for start." >&2
                usage
                exit 2
            fi
            phase_name "$phase" >/dev/null
            ensure_order_startable "$phase"
            set_phase_status "$phase" "in_progress" "" ""
            echo "Phase $phase started."
            ;;
        validate-artifact)
            local artifact=""
            local full_gate=0
            while [[ $# -gt 0 ]]; do
                case "$1" in
                    --artifact)
                        artifact="$2"
                        shift 2
                        ;;
                    --full-gate)
                        full_gate=1
                        shift
                        ;;
                    *)
                        echo "ERROR: unknown option for validate-artifact: $1" >&2
                        exit 2
                        ;;
                esac
            done
            if [[ -z "$artifact" ]]; then
                echo "ERROR: --artifact is required." >&2
                exit 2
            fi
            validate_artifact_dir "$artifact" "$full_gate"
            echo "Artifact validation passed."
            ;;
        complete)
            local phase="${1:-}"
            shift || true
            if [[ -z "$phase" ]]; then
                echo "ERROR: missing phase for complete." >&2
                usage
                exit 2
            fi
            phase_name "$phase" >/dev/null
            ensure_order_startable "$phase"
            local artifact=""
            local full_gate=0
            while [[ $# -gt 0 ]]; do
                case "$1" in
                    --artifact)
                        artifact="$2"
                        shift 2
                        ;;
                    --full-gate)
                        full_gate=1
                        shift
                        ;;
                    *)
                        echo "ERROR: unknown option for complete: $1" >&2
                        exit 2
                        ;;
                esac
            done
            if [[ -z "$artifact" ]]; then
                echo "ERROR: --artifact is required." >&2
                exit 2
            fi
            validate_artifact_dir "$artifact" "$full_gate"
            set_phase_status "$phase" "completed" "$artifact" "$( [[ "$full_gate" -eq 1 ]] && echo full_gate_pass || echo gate_pass )"
            echo "Phase $phase marked completed."
            ;;
        run-phase)
            local phase="${1:-}"
            shift || true
            if [[ -z "$phase" ]]; then
                echo "ERROR: missing phase for run-phase." >&2
                usage
                exit 2
            fi
            phase_name "$phase" >/dev/null
            local small_only=0
            while [[ $# -gt 0 ]]; do
                case "$1" in
                    --small-only)
                        small_only=1
                        shift
                        ;;
                    *)
                        echo "ERROR: unknown option for run-phase: $1" >&2
                        exit 2
                        ;;
                esac
            done
            run_phase "$phase" "$small_only"
            ;;
        *)
            echo "ERROR: unknown command: $cmd" >&2
            usage
            exit 2
            ;;
    esac
}

main "$@"
