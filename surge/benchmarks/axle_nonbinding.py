#!/usr/bin/env python3
"""Paired V1/V2 HGS screen on provably equivalent nonbinding public facts.

Measurements use the existing runner/resource collection; this is not a customer
calibration or a substitute for the release thresholds and long quiet-host runs.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import platform
import statistics
import sys
from loading_quality import fixture, execute, atomic_json
from loading_assess import assess
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tests'))
from loading_v2_reference import verify


def axle_model(model):
    out = copy.deepcopy(model)
    out['loading'].update(version=2, required_capabilities=['stacking', 'axle_loads'])
    # One floor slot per scalar-feasible unit, all footprints inside the
    # wheelbase. Tare=5t and cargo<=5.5kg: limits cannot bind any such route.
    for v in out['loading']['vehicles']:
        n = len(v['slots'])
        v['axles'] = dict(model='rigid_two_axle_v1', profile_sha256='a' * 64,
            wheelbase_mm=16000, footprint_length_mm=1200, deck_min_mm=0, deck_max_mm=16000,
            slot_centres_mm=[1000 + 1200 * i for i in range(n)],
            front_tare_g=dict(min=3000000, max=3000000), rear_tare_g=dict(min=2000000, max=2000000),
            front_limits_g=dict(min=0, max=6000000), rear_limits_g=dict(min=0, max=6500000),
            max_gross_g=12000000, min_front_fraction=None)
    for r in out['loading']['requests']:
        for u in r['units']:
            u['cg_offset_mm'] = dict(min=-50, max=50)
    return out


def review(report, limits):
    """Reuse the established equivalent-feasible-set release gates."""
    rows = []
    for row in report['runs']:
        if row['variant'] not in ('v1', 'v2'):
            raise ValueError('unknown axle comparison variant')
        rows.append(dict(row, case='axle-nonbinding-equivalent',
                         variant='baseline' if row['variant'] == 'v1' else 'current'))
    if any(not row.get('valid') for row in rows):
        return dict(passed=False, comparisons=[], error='invalid native benchmark output; use bench_loading_driver, not surge_solve')
    return assess(dict(runs=rows, host=report['host'], complete=report['complete'],
                       planned_runs=report['planned_runs']), limits)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=Path, required=True, help='native bench_loading_driver adapter (not surge_solve)')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--seconds', type=int, default=3)
    p.add_argument('--iterations', type=int, default=100)
    p.add_argument('--seeds', type=int, nargs='+', default=[42, 31415, 2718])
    p.add_argument('--orders', type=int, default=66)
    p.add_argument('--budgets', nargs='+', choices=['wall', 'iterations'], default=['wall', 'iterations'])
    args = p.parse_args()
    if len(set(args.seeds)) != len(args.seeds) or len(set(args.budgets)) != len(args.budgets):
        p.error('duplicate seeds/budgets would overwrite evidence')
    if args.orders not in (6, 66) or not 1 <= args.seconds <= 600 or not 1 <= args.iterations <= 100000:
        p.error('supported public sizes are 6/66; positive bounded budgets required')
    from subprocess import run
    ancestor = args.output.resolve()
    while not ancestor.exists(): ancestor = ancestor.parent
    if run(['git', '-C', str(ancestor), 'rev-parse', '--is-inside-work-tree'], capture_output=True).returncode == 0:
        p.error('measurements must stay outside Git')
    import os
    os.umask(0o077)
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve()
    report = dict(binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                  runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), runs=[], complete=False,
                  host=dict(architecture=platform.machine(), system=platform.system()))
    planned = len(args.seeds) * len(args.budgets) * 2
    report['planned_runs'] = planned
    for budget in args.budgets:
        for ordinal, seed in enumerate(args.seeds):
            v1 = fixture(args.orders, False)
            v1['config'].update(population=True, population_threads=2, population_generations=3,
                max_iterations=args.iterations, max_time_seconds=args.seconds if budget == 'wall' else 0, seed=seed)
            models = [('v1', v1), ('v2', axle_model(v1))]
            if ordinal % 2: models.reverse()
            for name, model in models:
                directory = output / f'{budget}-{seed}-{name}'
                result = execute(binary, model, directory, 180, synthetic=True)
                if name == 'v2' and result['valid']:
                    verify(model, json.loads((directory / 'output.json').read_text())['solution'])
                row = dict(variant=name, seed=seed, budget=budget, **result)
                report['runs'].append(row)
                atomic_json(output / 'report.json', report)
                print(json.dumps(row), flush=True)
    report['complete'] = len(report['runs']) == planned
    comparisons = []
    for budget in args.budgets:
        paired = []
        for seed in args.seeds:
            a, b = (next(r for r in report['runs'] if r['budget']==budget and r['seed']==seed and r['variant']==v) for v in ('v1','v2'))
            paired.append(dict(valid=a['valid'] and b['valid'], served_delta=b.get('served', -1)-a.get('served', -1),
                throughput_ratio=b.get('iterations_per_second', 0)/a['iterations_per_second'] if a.get('iterations_per_second') else 0,
                vehicle_delta=b.get('stats', {}).get('vehicles_used', 0)-a.get('stats', {}).get('vehicles_used', 0),
                distance_ratio=b.get('stats', {}).get('total_distance', 0)/a['stats']['total_distance'] if a.get('stats', {}).get('total_distance') else None))
        comparisons.append(dict(budget=budget, pairs=paired, median_throughput_ratio=statistics.median(r['throughput_ratio'] for r in paired)))
    report['comparisons'] = comparisons
    limits = json.loads(Path(__file__).with_name('loading_thresholds.json').read_bytes())
    report['review'] = review(report, limits)
    atomic_json(output / 'report.json', report)
    if not report['review']['passed']: raise SystemExit(1)


if __name__ == '__main__':
    main()
