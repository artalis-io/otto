#!/usr/bin/env python3
"""Verify V1/V2 assessment fails closed without timing noisy CI hardware."""
import copy
import json
from pathlib import Path
import unittest

from axle_nonbinding import review

LIMITS = json.loads(Path(__file__).with_name('loading_thresholds.json').read_bytes())


class AxleReviewTests(unittest.TestCase):
    def report(self):
        return dict(host={}, complete=True, planned_runs=6, runs=[
            dict(variant=variant, seed=seed, budget='wall', valid=True, served=66,
                 iterations_per_second=100, stats=dict(vehicles_used=6, total_distance=100, total_cost=100))
            for seed in (42, 31415, 2718) for variant in ('v1', 'v2')])

    def test_complete_equivalent_pairs_pass_unchanged_limits(self):
        result = review(self.report(), LIMITS)
        self.assertTrue(result['passed'])
        self.assertEqual(result['comparisons'][0]['comparison'], 'equivalent')
        self.assertEqual(result['limits'], LIMITS)

    def test_lower_service_cannot_be_called_distance_or_cost_savings(self):
        report = self.report()
        report['runs'][1].update(served=65, stats=dict(vehicles_used=5,total_distance=1,total_cost=1))
        result = review(report, LIMITS)
        self.assertFalse(result['passed'])
        pair = result['comparisons'][0]
        self.assertFalse(pair['distance_cost_comparable'])
        self.assertNotIn('distance_ratio', pair)
        self.assertNotIn('cost_ratio', pair)

    def test_incomplete_or_invalid_runs_fail(self):
        base = self.report()
        for mutation in ('missing', 'incomplete', 'invalid', 'few-seeds'):
            report = copy.deepcopy(base)
            if mutation == 'missing': report['runs'].pop()
            if mutation == 'incomplete': report['complete'] = False
            if mutation == 'invalid': report['runs'][1]['valid'] = False
            if mutation == 'few-seeds': report['runs'] = report['runs'][:4]; report['planned_runs'] = 4
            with self.subTest(mutation=mutation): self.assertFalse(review(report, LIMITS)['passed'])

    def test_duplicate_seed_or_unknown_variant_is_rejected(self):
        report = self.report();report['runs'].append(copy.deepcopy(report['runs'][0]))
        with self.assertRaises(ValueError): review(report, LIMITS)
        report = self.report();report['runs'][0]['variant'] = 'other'
        with self.assertRaises(ValueError): review(report, LIMITS)

    def test_plain_cli_or_incomplete_native_output_is_not_measurement_evidence(self):
        report = self.report()
        report['runs'][0] = dict(variant='v1', seed=42, budget='wall', valid=False)
        result = review(report, LIMITS)
        self.assertFalse(result['passed'])
        self.assertIn('bench_loading_driver', result['error'])

    def test_throughput_boundary_is_enforced(self):
        for ratio, passed in [(0.75, True), (0.74, False), (0, False)]:
            report = self.report()
            for row in report['runs']:
                if row['variant'] == 'v2': row['iterations_per_second'] *= ratio
            self.assertEqual(review(report, LIMITS)['passed'], passed)

    def test_vehicle_distance_and_cost_regressions_are_rejected(self):
        for metric, value in [('vehicles_used',7), ('total_distance',106), ('total_cost',106)]:
            report = self.report()
            for row in report['runs']:
                if row['variant'] == 'v2': row['stats'][metric] = value
            with self.subTest(metric=metric): self.assertFalse(review(report, LIMITS)['passed'])


if __name__ == '__main__': unittest.main()
