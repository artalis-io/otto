#!/usr/bin/env python3
"""Apply predeclared review gates to paired benchmark reports, without retuning."""
import argparse
import json
from pathlib import Path
import statistics


def distribution(values):
    return {'min': min(values), 'median': statistics.median(values), 'max': max(values)}


def assess(report, limits):
    rows = report['runs']
    groups = {}
    for row in rows:
        key = (row['case'], row['budget'])
        variant = groups.setdefault(key, {}).setdefault(row['variant'], {})
        if row['seed'] in variant:
            raise ValueError('duplicate seed in comparison group')
        variant[row['seed']] = row
    comparisons = []
    for (case, budget), variants in sorted(groups.items()):
        choices = [('baseline', 'current', 'disabled')]
        if case == 'nonbinding':
            choices.append(('current', 'hard', 'nonbinding'))
        for left, right, purpose in choices:
            before, after = variants.get(left, {}), variants.get(right, {})
            seeds = sorted(set(before) & set(after))
            checks = {'complete_pairs': bool(before) and set(before) == set(after),
                      'minimum_seeds': len(seeds) >= limits['minimum_paired_seeds'],
                      'valid_results': all(r['valid'] for r in list(before.values()) + list(after.values()))}
            result = {'case': case, 'budget': budget, 'comparison': purpose, 'seeds': seeds, 'checks': checks}
            if not seeds:
                result['passed'] = False
                comparisons.append(result)
                continue
            served = [after[s]['served'] - before[s]['served'] for s in seeds]
            vehicles = [after[s]['stats']['vehicles_used'] - before[s]['stats']['vehicles_used'] for s in seeds]
            result['served_delta'] = distribution(served)
            result['vehicle_delta'] = distribution(vehicles)
            checks['no_service_loss'] = min(served) >= -limits['maximum_paired_served_loss']
            checks['vehicle_quality'] = statistics.median(vehicles) <= limits['maximum_median_vehicle_increase']
            # A shorter route serving fewer orders is NOT an efficiency gain.
            same_service = all(value == 0 for value in served)
            result['distance_cost_comparable'] = same_service
            for metric in ('total_distance', 'total_cost'):
                ratios = []
                for s in seeds:
                    a, b = before[s]['stats'][metric], after[s]['stats'][metric]
                    if a <= 0:
                        if b != a:
                            ratios = []
                            break
                        ratios.append(1.0)
                    else:
                        ratios.append(b / a)
                field = 'distance' if metric == 'total_distance' else 'cost'
                if same_service and len(ratios) == len(seeds):
                    result[field + '_ratio'] = distribution(ratios)
                    checks[field + '_quality'] = statistics.median(ratios) <= 1 + limits[
                        'maximum_median_' + field + '_increase_fraction']
                else:
                    checks[field + '_quality'] = False
            throughput = [after[s]['iterations_per_second'] / before[s]['iterations_per_second']
                          for s in seeds if before[s]['iterations_per_second'] > 0]
            if len(throughput) == len(seeds):
                result['throughput_ratio'] = distribution(throughput)
                checks['throughput'] = statistics.median(throughput) >= limits[
                    purpose + '_minimum_median_throughput_ratio']
            else:
                checks['throughput'] = False
            result['passed'] = all(checks.values())
            comparisons.append(result)
    exact = []
    for row in rows:
        refs = report.get('exact_references', {}).get(row['case'])
        if not refs:
            continue
        reference = refs['hard' if row['variant'] == 'hard' else 'legacy']
        record = {k: row[k] for k in ('case', 'variant', 'budget', 'seed')}
        record['vehicle_gap'] = row['stats']['vehicles_used'] - reference['vehicles']
        record['distance_gap_fraction'] = row['stats']['total_distance'] / reference['distance'] - 1
        record['passed'] = (row['valid'] and row['served'] == reference['served'] and
            record['vehicle_gap'] <= limits['exact_maximum_vehicle_gap'] and
            record['distance_gap_fraction'] <= limits['exact_maximum_distance_gap_fraction'])
        exact.append(record)
    complete = report.get('complete') is True and report.get('planned_runs') == len(rows)
    return {'schema_version': 1, 'limits': limits, 'host': report['host'],
            'complete': complete,
            'comparisons': comparisons, 'exact': exact,
            'passed': complete and bool(comparisons) and all(r['passed'] for r in comparisons + exact)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--limits', type=Path, default=Path(__file__).with_name('loading_thresholds.json'))
    args = parser.parse_args()
    result = assess(json.loads(args.report.read_bytes()), json.loads(args.limits.read_bytes()))
    print(json.dumps(result, indent=2, allow_nan=False))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
