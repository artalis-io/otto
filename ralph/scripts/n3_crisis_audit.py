#!/usr/bin/env python3
"""N3: Crisis telemetry audit — analyze which Phase 1 mechanisms fire per NETLIB problem.

Usage:
    make -C ralph test-netlib-gate   # produces results/ dir
    python3 scripts/n3_crisis_audit.py /tmp/netlib-regression-gate-*/results/

Reads per-problem JSON telemetry from the NETLIB gate and reports which crisis
mechanisms fire, how often, and on how many problems. Helps identify dead-weight
mechanisms that can be pruned.
"""

import json
import os
import sys
import glob

# Crisis mechanism groups — each is a set of telemetry counter prefixes
# that represent one logical mechanism
MECHANISMS = {
    "dir_stabilize": [
        "dir_stabilize_skip",
        "dir_stabilize_cooldown",
        "dir_stabilize_force",
        "dir_stabilize_ratio",
    ],
    "no_pivot_ladder": [
        "no_pivot_ladder_retry",
        "no_pivot_ladder_dual_rescue",
        "no_pivot_ladder_forced_refactor",
    ],
    "failed_stabilize": [
        "failed_stabilize_site",
        "failed_stabilize_retry",
    ],
    "force_extreme": [
        "force_extreme_followup",
        "force_extreme_tiny_theta",
        "force_extreme_bound_flip",
    ],
    "shadow_guard": [
        "shadow_guard_followup",
    ],
    "window_pressure": [
        "window_pressure_events",
        "window_pressure_force_pivot",
    ],
    "force_pivot": [
        "force_pivot_budget",
        "force_pivot_mode",
    ],
    "dir_escape": [
        "dir_escape",
    ],
    "stagnation_escape": [
        "stagnation_escape",
    ],
    "ratio_breakdown": [
        "ratio_breakdown_retry",
        "ratio_breakdown_escalation",
    ],
    "no_pivot_force": [
        "no_pivot_force_refactor",
        "no_pivot_force_pending",
    ],
}

def find_mechanism_counters(data, mechanism_prefixes):
    """Sum all counters matching any prefix for a mechanism."""
    total = 0

    def scan(obj, path=""):
        nonlocal total
        if isinstance(obj, dict):
            for k, v in obj.items():
                new_path = f"{path}.{k}" if path else k
                if isinstance(v, int) and v > 0:
                    for prefix in mechanism_prefixes:
                        if prefix in k:
                            total += v
                            break
                scan(v, new_path)

    scan(data)
    return total


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <results_dir>")
        print(f"  e.g.: {sys.argv[0]} /tmp/netlib-regression-gate-*/results/")
        sys.exit(1)

    results_dir = sys.argv[1]
    files = sorted(glob.glob(os.path.join(results_dir, "*.json")))
    if not files:
        print(f"No JSON files found in {results_dir}")
        sys.exit(1)

    # Per-mechanism: list of (problem, count) where count > 0
    mechanism_data = {name: [] for name in MECHANISMS}

    for f in files:
        name = os.path.basename(f).replace(".json", "")
        try:
            with open(f) as fh:
                data = json.load(fh)
        except Exception:
            continue

        for mech_name, prefixes in MECHANISMS.items():
            count = find_mechanism_counters(data, prefixes)
            if count > 0:
                mechanism_data[mech_name].append((name, count))

    total_problems = len(files)

    # Sort mechanisms by number of problems that trigger them
    sorted_mechs = sorted(
        mechanism_data.items(),
        key=lambda x: -len(x[1])
    )

    print(f"N3 Crisis Telemetry Audit — {total_problems} NETLIB problems\n")
    print(f"{'Mechanism':25s} {'Problems':>9s} {'%':>6s} {'TotalEvents':>12s} {'Classification':>16s}")
    print("-" * 72)

    for mech_name, problem_list in sorted_mechs:
        n_problems = len(problem_list)
        pct = 100.0 * n_problems / total_problems if total_problems > 0 else 0
        total_events = int(sum(c for _, c in problem_list))

        if pct >= 50:
            classification = "CORE"
        elif pct >= 10:
            classification = "ADAPTIVE"
        elif pct > 0:
            classification = "EDGE-CASE"
        else:
            classification = "DEAD"

        print(f"{mech_name:25s} {n_problems:9d} {pct:5.1f}% {total_events:12d} {classification:>16s}")

    # Detail: per-mechanism top-5 heaviest problems
    print(f"\n{'='*72}")
    print("Per-mechanism top-5 heaviest problems:\n")
    for mech_name, problem_list in sorted_mechs:
        if not problem_list:
            continue
        top5 = sorted(problem_list, key=lambda x: -x[1])[:5]
        print(f"  {mech_name}:")
        for prob, count in top5:
            print(f"    {prob:25s} {count:8d} events")
        print()


if __name__ == "__main__":
    main()
