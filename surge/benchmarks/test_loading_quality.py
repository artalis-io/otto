#!/usr/bin/env python3
"""Public, bounded benchmark-harness regression tests (no private fixtures)."""
import copy
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

from loading_quality import execute, fixture, adversarial_fixture
from loading_reference import check_solution, exact_routing, valid_placement, exhaustive_pack
from loading_assess import assess

DRIVER = Path(os.environ.get('SURGE_BENCH_DRIVER',
    str(Path(__file__).resolve().parents[1] / 'bench_loading_driver')))


class ReferenceTests(unittest.TestCase):
    def test_interrupted_runner_kills_and_reaps_its_own_child(self):
        child = Mock(pid=12345, returncode=None)
        with tempfile.TemporaryDirectory() as path:
            with patch('loading_quality.subprocess.Popen', return_value=child), \
                 patch('loading_quality.os.wait4', side_effect=[KeyboardInterrupt(), (12345, 9, None)]) as wait, \
                 patch('loading_quality.os.killpg') as kill:
                with self.assertRaises(KeyboardInterrupt):
                    execute(Path('/not-executed'), {}, Path(path), 30)
                kill.assert_called_once()
                self.assertEqual(kill.call_args.args[0], 12345)
                self.assertEqual(wait.call_args.args, (12345, 0))

    def test_quality_gates_do_not_call_lower_service_savings(self):
        limits = json.loads(Path(__file__).with_name('loading_thresholds.json').read_bytes())
        report = {'host': {}, 'runs': [], 'complete': True, 'planned_runs': 6}
        for seed in (1, 2, 3):
            for variant in ('baseline', 'current'):
                report['runs'].append({'case': 'example', 'budget': 'wall', 'seed': seed,
                    'variant': variant, 'valid': True, 'served': 6, 'iterations_per_second': 100,
                    'stats': {'vehicles_used': 2, 'total_cost': 100, 'total_distance': 100}})
        self.assertTrue(assess(report, limits)['passed'])
        report['complete'] = False
        self.assertFalse(assess(report, limits)['passed'])
        report['complete'] = True
        report['runs'][1]['served'] = 5
        report['runs'][1]['stats']['total_distance'] = 1
        result = assess(report, limits)
        self.assertFalse(result['passed'])
        self.assertFalse(result['comparisons'][0]['distance_cost_comparable'])
        self.assertNotIn('distance_ratio', result['comparisons'][0])

    def test_missing_seed_and_invalid_result_fail_review(self):
        limits = json.loads(Path(__file__).with_name('loading_thresholds.json').read_bytes())
        report = {'host': {}, 'runs': [], 'complete': True, 'planned_runs': 6}
        for seed in (1, 2, 3):
            for variant in ('baseline', 'current'):
                report['runs'].append({'case': 'example', 'budget': 'iterations', 'seed': seed,
                    'variant': variant, 'valid': True, 'served': 6, 'iterations_per_second': 100,
                    'stats': {'vehicles_used': 2, 'total_cost': 100, 'total_distance': 100}})
        self.assertTrue(assess(report, limits)['passed'])
        invalid = copy.deepcopy(report)
        invalid['runs'][2]['valid'] = False
        self.assertFalse(assess(invalid, limits)['passed'])
        missing = copy.deepcopy(report)
        missing['runs'].pop()
        result = assess(missing, limits)
        self.assertFalse(result['passed'])
        self.assertFalse(result['comparisons'][0]['checks']['complete_pairs'])

    def test_reference_known_line_and_physical_split(self):
        model = fixture(6, True)
        for i, task in enumerate(model['tasks']):
            task.update(x=i + 1, y=0)
        self.assertEqual(exact_routing(model), {'served': 6, 'vehicles': 2, 'distance': 16})
        del model['loading']
        self.assertEqual(exact_routing(model), {'served': 6, 'vehicles': 1, 'distance': 12})

    def test_reference_refuses_unsupported_problem(self):
        with self.assertRaises(ValueError):
            exact_routing(fixture(9))
        model = fixture(6)
        model['vehicles'][0]['max_trips'] = 2
        with self.assertRaises(ValueError):
            exact_routing(model)
        model = fixture(6)
        model['depots'][0]['x'] = 5
        with self.assertRaises(ValueError):
            exact_routing(model)
        model = fixture(6)
        model['loading']['vehicles'][1]['clearance_mm'] = 1
        with self.assertRaises(ValueError):
            exact_routing(model)

    def test_nonbinding_is_guaranteed_by_scalar_capacity(self):
        model = fixture()
        for vehicle, physical in zip(model['vehicles'], model['loading']['vehicles']):
            self.assertEqual(vehicle['capacity'][0], len(physical['slots']))
            self.assertEqual(physical['max_layers'], 1)
            self.assertGreaterEqual(physical['payload_g'], vehicle['capacity'][0] * 500)

    def test_binding_reference_has_the_same_feasible_sets(self):
        for access in ('side_free', 'rear_lifo'):
            physical = fixture(6, True, access)
            scalar = copy.deepcopy(physical)
            del scalar['loading']
            for vehicle in scalar['vehicles']:
                vehicle['capacity'] = [4]
            self.assertEqual(exact_routing(physical), exact_routing(scalar))
            geometry = physical['loading']['vehicles'][0]
            for count in range(1, 6):
                units = [{**physical['loading']['requests'][i]['units'][0], 'stop': i}
                         for i in range(count)]
                self.assertEqual(exhaustive_pack(geometry, units), count <= 4)

    def test_mixed_and_nonmonotone_bridge_references(self):
        for bridge in (False, True):
            model = adversarial_fixture(bridge)
            reference = exact_routing(model)
            self.assertEqual((reference['served'], reference['vehicles']), (6, 2))
        model = adversarial_fixture(True)
        units = [{**r['units'][0], 'stop': i} for i, r in enumerate(model['loading']['requests'][:3])]
        geometry = model['loading']['vehicles'][0]
        self.assertFalse(exhaustive_pack(geometry, [units[0], units[2]]))
        self.assertTrue(exhaustive_pack(geometry, units))

    def test_certificate_rejects_each_physical_violation(self):
        model = fixture(2, True)
        vehicle = model['loading']['vehicles'][0]
        units = [{**model['loading']['requests'][i]['units'][0], 'stop': i} for i in range(2)]
        positions = [{'slot': 0, 'layer': 0}, {'slot': 0, 'layer': 1}]
        self.assertTrue(valid_placement(vehicle, units, positions))
        for field in ('payload_g', 'max_stack_weight_g', 'clearance_mm'):
            changed = copy.deepcopy(vehicle)
            changed[field] = 1
            self.assertFalse(valid_placement(changed, units, positions), field)
        for field in ('max_units_above', 'max_weight_above_g'):
            changed = copy.deepcopy(units)
            changed[0][field] = 0
            self.assertFalse(valid_placement(vehicle, changed, positions), field)
        changed = copy.deepcopy(vehicle)
        changed['allowed_top'] = [[]]
        self.assertFalse(valid_placement(changed, units, positions))
        changed['access_model'] = 'rear_lifo'
        changed['allowed_top'] = [[0]]
        self.assertFalse(valid_placement(changed, units, positions))
        self.assertFalse(valid_placement(vehicle, units, [positions[0], positions[0]]))
        self.assertFalse(valid_placement(vehicle, units, [positions[0]]))
        self.assertFalse(valid_placement(vehicle, units,
            [{'slot': 0, 'layer': 1}, {'slot': 1, 'layer': 1}]))
        rear = copy.deepcopy(vehicle)
        rear['access_model'] = 'rear_lifo'
        self.assertFalse(valid_placement(rear, units,
            [{'slot': 1, 'layer': 0}, {'slot': 0, 'layer': 0}]))


@unittest.skipUnless(DRIVER.exists(), 'build make -C surge bench_loading_driver')
class NativeTests(unittest.TestCase):
    def model(self, binding=True, access='side_free'):
        model = fixture(6, binding, access)
        model['config'].update(population=True, population_threads=2,
                               population_generations=3, max_iterations=60,
                               max_time_seconds=0, seed=42)
        for i, task in enumerate(model['tasks']):
            task.update(x=i + 1, y=0)
        return model

    def test_exact_quality_and_independent_replay(self):
        for access in ('side_free', 'rear_lifo'):
            with tempfile.TemporaryDirectory() as path:
                model = self.model(access=access)
                measured = execute(DRIVER, model, Path(path), 30, synthetic=True)
                self.assertTrue(measured['valid'], measured)
                optimum = exact_routing(model)
                self.assertEqual(measured['served'], optimum['served'])
                self.assertEqual(measured['stats']['vehicles_used'], optimum['vehicles'])
                self.assertAlmostEqual(measured['stats']['total_distance'], optimum['distance'])
                solution = json.loads((Path(path) / 'output.json').read_bytes())['solution']
                solution['loading']['rounds'][0]['placements'].pop()
                with self.assertRaises(ValueError):
                    check_solution(model, solution)

    def test_hgs_quality_for_mixed_and_nonmonotone_classes(self):
        limits = json.loads(Path(__file__).with_name('loading_thresholds.json').read_bytes())
        for bridge in (False, True):
            model = adversarial_fixture(bridge)
            model['config'].update(population=True, population_threads=2,
                                   population_generations=3, max_iterations=300,
                                   max_time_seconds=0, seed=42)
            optimum = exact_routing(model)
            with tempfile.TemporaryDirectory() as path:
                measured = execute(DRIVER, model, Path(path), 30, synthetic=True)
                self.assertTrue(measured['valid'], measured)
                self.assertEqual(measured['served'], optimum['served'])
                self.assertEqual(measured['stats']['vehicles_used'], optimum['vehicles'])
                self.assertLessEqual(measured['stats']['total_distance'], optimum['distance'] *
                    (1 + limits['exact_maximum_distance_gap_fraction']) + 1e-7)

    def test_reassigned_reordered_warm_start_is_repaired(self):
        model = self.model(access='rear_lifo')
        # A planner supplies all six on one truck: scalar-feasible, physically
        # impossible (four cells). Reversed manual order is not a certificate.
        model['initial_routes'] = [{'vehicle_id': 2, 'request_ids': [5, 4, 3, 2, 1, 0]}]
        model['committed_requests'] = list(range(6))
        model['frozen_requests'] = [5]
        with tempfile.TemporaryDirectory() as path:
            measured = execute(DRIVER, model, Path(path), 30, synthetic=True)
            self.assertTrue(measured['valid'], measured)
            self.assertEqual(measured['served'], 6)
            self.assertEqual(measured['stats']['vehicles_used'], 2)
            solution = json.loads((Path(path) / 'output.json').read_bytes())['solution']
            route = next(r for r in solution['routes'] if any(s['request_id'] == 5 for s in r['stops']))
            self.assertEqual(route['vehicle_id'], 2)

    def test_reload_rounds_preserve_independent_capacity(self):
        model = self.model()
        model['vehicles'] = model['vehicles'][:1]
        model['vehicles'][0]['max_trips'] = 2
        model['loading']['vehicles'] = model['loading']['vehicles'][:1]
        with tempfile.TemporaryDirectory() as path:
            measured = execute(DRIVER, model, Path(path), 30, synthetic=True)
            self.assertTrue(measured['valid'], measured)
            self.assertEqual(measured['served'], 6)
            self.assertEqual(measured['stats']['vehicles_used'], 1)
            self.assertEqual(measured['trips'], 2)

    def test_cancellation_is_bounded(self):
        import subprocess
        for mode in ('single', 'parallel', 'population'):
            for hard in (False, True):
                with self.subTest(mode=mode, hard=hard), tempfile.TemporaryDirectory() as path:
                    model = fixture(66)
                    if not hard:
                        del model['loading']
                    model['config'].update(population=True, population_threads=8,
                        population_generations=3, max_iterations=2000000000,
                        max_time_seconds=0, segment_size=1000000, seed=42)
                    request = Path(path) / 'request.json'
                    request.write_text(json.dumps(model))
                    result = subprocess.run([str(DRIVER), str(request), '100', mode],
                        capture_output=True, text=True, timeout=10)
                    self.assertIn(result.returncode, (0, 1), result.stderr)
                    output = json.loads(result.stdout)
                    measured = output['benchmark']
                    self.assertTrue(measured['cancel_requested'])
                    # Separate measured ARM64 runs enforce the stricter 2s target.
                    self.assertLess(measured['cancel_latency_ms'], 5000)
                    if result.returncode == 0:
                        self.assertTrue(measured['hard_validated'])
                        check_solution(model, output['solution'])
                    else:
                        self.assertFalse(output['solution'].get('routes'))

    def test_watchdog_is_not_a_pass(self):
        model = self.model()
        model['config'].update(max_iterations=2000000000, max_time_seconds=30)
        with tempfile.TemporaryDirectory() as path:
            measured = execute(DRIVER, model, Path(path), 0.001)
            self.assertTrue(measured['timeout'])
            self.assertFalse(measured['valid'])


if __name__ == '__main__':
    unittest.main()
