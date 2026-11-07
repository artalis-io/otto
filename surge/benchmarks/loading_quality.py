#!/usr/bin/env python3
"""Paired HGS experiments. Private inputs/results must live outside the checkout.

No production services, network calls, third-party Python packages or solver
defaults are involved. Each process runs alone; AB/BA order alternates by seed.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import signal
import subprocess
import sys
import time
from loading_reference import check_solution, exact_routing


def digest(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()


def atomic_json(path, value):
    tmp = path.with_suffix(path.suffix + '.partial')
    with tmp.open('wb') as stream:
        stream.write(encoded(value) + b'\n')
        stream.flush()
        os.fsync(stream.fileno())
    tmp.replace(path)


def fixture(count=66, binding=False, access='side_free'):
    rng = random.Random(20261107)
    points = [(round(rng.uniform(-50, 50), 4), round(rng.uniform(-50, 50), 4))
              for _ in range(count)]
    capacity = 11 if count > 8 else (6 if binding else 4)
    vehicles = (20 if binding else 12) if count > 8 else 4
    model = {
        'config': {'lexicographic_objective': True, 'hard_capacity': True,
                   'hard_time_windows': True, 'hard_max_duration': True},
        'depots': [{'x': 0, 'y': 0}],
        'vehicles': [{'start_depot_id': 0, 'end_depot_id': 0,
                      'capacity': [capacity]} for _ in range(vehicles)],
        'tasks': [{'type': 'delivery', 'x': x, 'y': y, 'demand': [-1],
                   'tw_early': 0, 'tw_late': 1000000} for x, y in points],
        'requests': [{'delivery_task_id': i} for i in range(count)],
    }
    # Facts are synthetic, explicit and integral. Nonbinding: scalar capacity
    # already limits every route to this many units, each with its own slot.
    slots = 2 if binding else capacity
    model['loading'] = {
        'version': 1, 'mode': 'hard', 'node_budget': 10000,
        'vehicles': [{'vehicle_id': i, 'access_model': access,
                      'slots': [{'lane': 0, 'depth': j} for j in range(slots)],
                      'max_layers': 2 if binding else 1, 'clearance_mm': 2200,
                      'max_stack_weight_g': 2000, 'payload_g': 20000,
                      'allowed_top': [[0]]} for i in range(vehicles)],
        'requests': [{'request_id': i, 'units': [{'height_mm': 1000,
                       'weight_g': 500, 'stack_class': 0,
                       'max_weight_above_g': 500, 'max_units_above': 1}]}
                     for i in range(count)]}
    return model


def adversarial_fixture(bridge=False):
    model = fixture(6, True, 'rear_lifo' if bridge else 'side_free')
    classes = [0, 1, 0, 0, 1, 0] if bridge else [0, 1, 0, 1, 0, 1]
    points = [(-20, 0), (0, 5), (-10, 0), (10, 0), (0, -5), (20, 0)]
    for i, task in enumerate(model['tasks']):
        task['x'], task['y'] = points[i]
    for vehicle in model['loading']['vehicles']:
        vehicle.update(slots=[{'lane': 0, 'depth': j} for j in range(1 if bridge else 2)],
                       max_layers=3 if bridge else 2, clearance_mm=2400,
                       max_stack_weight_g=5000, allowed_top=[[1], [0] if bridge else []])
    for i, request in enumerate(model['loading']['requests']):
        request['units'][0].update(stack_class=classes[i], height_mm=700 if bridge else 1000,
                                  weight_g=500, max_weight_above_g=2000,
                                  max_units_above=2 if bridge else 1)
    return model


def execute(binary, request, directory, timeout, cancel_ms=None, synthetic=False):
    """wait4 supplies this child's CPU/peak RSS, not cumulative child maxima."""
    if not hasattr(os, 'wait4'):
        raise RuntimeError('measurement requires native POSIX wait4 (Linux/macOS)')
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    atomic_json(directory / 'request.json', request)
    command = [str(binary), str(directory / 'request.json')]
    if cancel_ms is not None:
        command.append(str(cancel_ms))
    begin = time.monotonic()
    timed_out = False
    with (directory / 'output.json').open('wb') as stdout, (directory / 'stderr.log').open('wb') as stderr:
        child = subprocess.Popen(command, stdout=stdout, stderr=stderr, start_new_session=True)
        try:
            while True:
                pid, status, usage = os.wait4(child.pid, os.WNOHANG)
                if pid:
                    child.returncode = os.waitstatus_to_exitcode(status)
                    break
                if time.monotonic() - begin > timeout and not timed_out:
                    timed_out = True
                    try:
                        os.killpg(child.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass  # Child exited after wait4; still reap it below.
                time.sleep(0.02)
        finally:
            if child.returncode is None:
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                _, status, _ = os.wait4(child.pid, 0)
                child.returncode = os.waitstatus_to_exitcode(status)
    result = {'wall_seconds': time.monotonic() - begin, 'returncode': child.returncode,
              'timeout': timed_out, 'cpu_seconds': usage.ru_utime + usage.ru_stime,
              'peak_rss_mib': usage.ru_maxrss / (1048576 if sys.platform == 'darwin' else 1024)}
    try:
        output = json.loads((directory / 'output.json').read_bytes())
        result.update(output['benchmark'])
        solution = output['solution']
        rounds = solution.get('loading', {}).get('rounds', [])
        result['published_unknown_rounds'] = sum(r['outcome'] == 'unknown' for r in rounds)
        result['certificate_nodes'] = sum(r['nodes'] for r in rounds)
        result['singleton_unknown_vehicles'] = sum(r['unknown_vehicles'] for r in
            solution.get('loading', {}).get('unassigned_checks', []))
        if synthetic:
            check_solution(request, solution)
            result['independent_replay'] = True
        result['status'] = solution['status']
        result['stats'] = solution['stats']
        result['trips'] = sum(1 + max((s['trip_index'] for s in r['stops']), default=-1)
                              for r in solution['routes'])
        result['requests'] = len(request['requests'])
        result['served'] = result['requests'] - solution['stats']['unassigned']
        result['iterations_per_second'] = solution['stats']['iterations'] / result['wall_seconds']
        result['valid'] = (not timed_out and child.returncode == 0 and
                           output['benchmark']['hard_validated'] and
                           solution['status'] in ('OK', 'LIMIT'))
    except (ValueError, KeyError, TypeError):
        result['valid'] = False
    return result


def run(args):
    os.umask(0o077)
    output = args.output.resolve()
    # Refuse storing potentially private request/solution copies in Git.
    ancestor = output
    while not ancestor.exists():
        ancestor = ancestor.parent
    if subprocess.run(['git', '-C', str(ancestor), 'rev-parse', '--is-inside-work-tree'],
                      capture_output=True).returncode == 0:
        raise ValueError('output must be outside a Git checkout')
    output.mkdir(parents=True, mode=0o700, exist_ok=True)
    binaries = {'baseline': args.baseline.resolve(), 'current': args.current.resolve()}
    identities = {name: digest(path.read_bytes()) for name, path in binaries.items()}
    runner_identity = digest(Path(__file__).read_bytes())
    cases = []
    references = {}
    if not args.external_only:
        definitions = [('binding', True, 'side_free'), ('rear', True, 'rear_lifo')]
        if args.adversarial_small:
            definitions = [('mixed', True, 'side_free'), ('bridge', True, 'rear_lifo')]
        elif not args.equivalent_only:
            definitions.insert(0, ('nonbinding', False, 'side_free'))
        for name, binding, access in definitions:
            model = adversarial_fixture(name == 'bridge') if args.adversarial_small else fixture(args.orders, binding, access)
            legacy = copy.deepcopy(model)
            del legacy['loading']
            if args.equivalent_only:
                name += '-equivalent'
                # These public units are identical, with two slots/two layers.
                # Every ordered subset <= 4 can be loaded, including rear LIFO.
                # Thus scalar capacity four defines EXACTLY the same feasible
                # routes, while the hard input retains capacity six/eleven.
                for vehicle in legacy['vehicles']:
                    vehicle['capacity'] = [4]
                variants = [('baseline', legacy), ('current', model)]
            else:
                variants = [('baseline', legacy), ('current', legacy), ('hard', model)]
            cases.append((name, variants))
            if args.orders <= 8 or args.adversarial_small:
                references[name] = {'legacy': exact_routing(legacy), 'hard': exact_routing(model)}
    synthetic_cases = {case for case, _ in cases}
    for item in args.input:
        name, filename = item.split('=', 1)
        if not name.replace('-', '').replace('_', '').isalnum():
            raise ValueError('case name must be alphanumeric')
        if name in ('nonbinding', 'binding', 'rear', 'binding-equivalent', 'rear-equivalent', 'mixed', 'bridge'):
            raise ValueError('external name is reserved for synthetic references')
        model = json.loads(Path(filename).read_bytes())
        if 'loading' in model:
            raise ValueError('external parity inputs must have loading absent')
        cases.append((name, [('baseline', model), ('current', model)]))
    rows = []
    planned = sum(len(variants) for _, variants in cases) * len(args.seeds) * len(args.budgets)
    if not planned:
        raise ValueError('at least one benchmark case is required')
    if len({case for case, _ in cases}) != len(cases):
        raise ValueError('duplicate case name')
    if len(set(args.seeds)) != len(args.seeds) or len(set(args.budgets)) != len(args.budgets):
        raise ValueError('duplicate seed/budget')
    for case, variants in cases:
        for seed_index, seed in enumerate(args.seeds):
            for budget in args.budgets:
                ordered = variants if seed_index % 2 == 0 else list(reversed(variants))
                for variant, original in ordered:
                    model = copy.deepcopy(original)
                    model.setdefault('config', {}).update({
                        'population': True, 'population_threads': args.threads,
                        'population_generations': 3, 'seed': seed,
                        'max_iterations': args.iterations if budget == 'iterations' else 2000000000,
                        'max_time_seconds': 0 if budget == 'iterations' else args.seconds})
                    label = 'current' if variant == 'hard' else variant
                    key = {'case': case, 'variant': variant, 'budget': budget, 'seed': seed,
                           'binary_sha256': identities[label], 'input_sha256': digest(encoded(model)),
                           'runner_sha256': runner_identity,
                           'reference_sha256': digest(Path(__file__).with_name('loading_reference.py').read_bytes())}
                    directory = output / digest(encoded(key))[:24]
                    metadata = directory / 'measurement.json'
                    if metadata.exists():
                        row = json.loads(metadata.read_bytes())
                        if any(row.get(k) != v for k, v in key.items()):
                            raise ValueError('resume identity mismatch')
                        if case in synthetic_cases:
                            check_solution(model, json.loads((directory / 'output.json').read_bytes())['solution'])
                    else:
                        print(json.dumps({'starting': key}), flush=True)
                        row = {**key, **execute(binaries[label], model, directory, args.watchdog,
                                              synthetic=case in synthetic_cases),
                               'loading_enabled': 'loading' in model}
                        atomic_json(metadata, row)
                    rows.append(row)
                    atomic_json(output / 'report.json', {'schema_version': 1,
                        'planned_runs': planned, 'complete': len(rows) == planned,
                        'host': {'architecture': platform.machine(), 'system': platform.system(),
                                 'cpus': os.cpu_count()},
                        'source_revisions': {'baseline': args.baseline_revision, 'current': args.current_revision},
                        'runner_sha256': runner_identity,
                        'binary_sha256': identities,
                        'settings': {'threads': args.threads, 'seconds': args.seconds,
                                     'iterations_per_worker': args.iterations,
                                     'generations': 3, 'seeds': args.seeds},
                        'exact_references': references, 'runs': rows})
                    print(json.dumps({'finished': key, 'valid': row['valid'],
                                      'wall_seconds': row['wall_seconds']}), flush=True)
                    if not row['valid']:
                        raise RuntimeError('invalid/incomplete benchmark; inspect private artifacts')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--current', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--input', action='append', default=[], metavar='NAME=PATH')
    parser.add_argument('--external-only', action='store_true')
    parser.add_argument('--equivalent-only', action='store_true')
    parser.add_argument('--adversarial-small', action='store_true', help='six-order mixed-class and rear-LIFO support-bridge exact cases')
    parser.add_argument('--baseline-revision', default='unspecified')
    parser.add_argument('--current-revision', default='unspecified')
    parser.add_argument('--seconds', type=int, default=30)
    parser.add_argument('--iterations', type=int, default=300)
    parser.add_argument('--threads', type=int, choices=range(1, 9), default=8)
    parser.add_argument('--orders', type=int, default=66)
    parser.add_argument('--watchdog', type=int, default=900)
    parser.add_argument('--seeds', type=int, nargs='+', default=[42, 31415, 2718])
    parser.add_argument('--budgets', choices=['wall', 'iterations'], nargs='+', default=['wall', 'iterations'])
    args = parser.parse_args()
    if sum((args.external_only, args.equivalent_only, args.adversarial_small)) > 1:
        parser.error('external-only, equivalent-only and adversarial-small are mutually exclusive')
    if args.seconds < 3 or args.iterations < 3 or args.watchdog <= args.seconds or not 1 <= args.orders <= 128:
        parser.error('seconds/iterations >= 3, watchdog > seconds, orders 1..128 required')
    def interrupted(_signum, _frame):
        raise KeyboardInterrupt('benchmark interrupted')
    signal.signal(signal.SIGTERM, interrupted)
    return run(args)


if __name__ == '__main__':
    raise SystemExit(main())
