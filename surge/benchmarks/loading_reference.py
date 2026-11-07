"""Independent synthetic-only checks: no native checker/search helpers.

The exact reference supports <= 8 homogeneous delivery orders with unit scalar
demand, one depot, no reloads, and unconstrained time windows. It intentionally
rejects richer inputs rather than pretending to solve the production problem.
"""
import itertools
import math
from functools import lru_cache


def valid_placement(vehicle, units, placements):
    if len(units) != len(placements):
        return False
    positions = [(p['slot'], p['layer']) for p in placements]
    if len(set(positions)) != len(positions):
        return False
    if sum(u['weight_g'] for u in units) > vehicle['payload_g']:
        return False
    for slot, layer in positions:
        if not 0 <= slot < len(vehicle['slots']) or not 0 <= layer < vehicle['max_layers']:
            return False
    for slot in range(len(vehicle['slots'])):
        members = [(u, p['layer']) for u, p in zip(units, placements) if p['slot'] == slot]
        if sorted(layer for _, layer in members) != list(range(len(members))):
            return False
        if sum(u['height_mm'] for u, _ in members) > vehicle['clearance_mm']:
            return False
        if sum(u['weight_g'] for u, _ in members) > vehicle['max_stack_weight_g']:
            return False
    for unit, position in zip(units, placements):
        above = [(u, p) for u, p in zip(units, placements)
                 if p['slot'] == position['slot'] and p['layer'] > position['layer']]
        if sum(u['weight_g'] for u, _ in above) > unit['max_weight_above_g']:
            return False
        if len(above) > unit['max_units_above']:
            return False
        for other, op in zip(units, placements):
            if op['slot'] == position['slot'] and op['layer'] == position['layer'] + 1:
                if other['stack_class'] not in vehicle['allowed_top'][unit['stack_class']]:
                    return False
            if vehicle['access_model'] == 'rear_lifo':
                if op['slot'] == position['slot'] and op['layer'] > position['layer']:
                    if other['stop'] > unit['stop']:
                        return False
                near, far = vehicle['slots'][position['slot']], vehicle['slots'][op['slot']]
                if near['lane'] == far['lane'] and near['depth'] < far['depth']:
                    if unit['stop'] > other['stop']:
                        return False
    return True


def exhaustive_pack(vehicle, units):
    if len(units) > 8:
        raise ValueError('reference restricted to eight units')
    cells = [(s, l) for s in range(len(vehicle['slots'])) for l in range(vehicle['max_layers'])]
    if len(units) > len(cells):
        return False
    for cells_used in itertools.permutations(cells, len(units)):
        if valid_placement(vehicle, units, [{'slot': s, 'layer': l} for s, l in cells_used]):
            return True
    return False


def check_solution(model, solution):
    """Validate identities, coverage, rounds and every physical witness."""
    seen = set()
    rounds = {}
    distance = 0.0
    for route in solution['routes']:
        vehicle = route['vehicle_id']
        previous = (0, 0)
        previous_trip = 0
        for stop in route['stops']:
            request = stop['request_id']
            if request in seen or not 0 <= request < len(model['requests']):
                raise ValueError('duplicate/unknown served identity')
            seen.add(request)
            if stop['task_id'] != model['requests'][request]['delivery_task_id']:
                raise ValueError('task/request mismatch')
            task = model['tasks'][stop['task_id']]
            trip = stop['trip_index']
            if trip != previous_trip:
                distance += math.dist(previous, (0, 0))
                previous = (0, 0)
            point = (task['x'], task['y'])
            distance += math.dist(previous, point)
            previous, previous_trip = point, trip
            rounds.setdefault((vehicle, trip), []).append(request)
        distance += math.dist(previous, (0, 0))
    unassigned = [r['request_id'] if isinstance(r, dict) else r for r in solution['unassigned']]
    if len(set(unassigned)) != len(unassigned) or seen.intersection(unassigned):
        raise ValueError('duplicate served/unassigned identity')
    if seen.union(unassigned) != set(range(len(model['requests']))):
        raise ValueError('incomplete coverage')
    stats = solution['stats']
    if stats['unassigned'] != len(unassigned) or stats['vehicles_used'] != len(solution['routes']):
        raise ValueError('inconsistent coverage statistics')
    if not math.isclose(distance, stats['total_distance'], rel_tol=1e-8, abs_tol=1e-5):
        raise ValueError('distance differs from independent geometry replay')
    for (vehicle, _), requests in rounds.items():
        capacity = model['vehicles'][vehicle]['capacity'][0]
        if len(requests) > capacity:
            raise ValueError('scalar capacity violation')
    if 'loading' not in model:
        return
    loading = model['loading']
    evidence = solution['loading']['rounds']
    if len(evidence) != len(rounds):
        raise ValueError('missing/duplicate round certificate')
    remaining = set(rounds)
    for proof in evidence:
        key = (proof['vehicle_id'], proof['trip_index'])
        if key not in remaining or proof['outcome'] != 'feasible':
            raise ValueError('invalid round certificate')
        remaining.remove(key)
        identities, units = [], []
        for ordinal, request in enumerate(rounds[key]):
            for index, unit in enumerate(loading['requests'][request]['units']):
                identities.append((request, index))
                units.append({**unit, 'stop': ordinal})
        supplied = {(p['request_id'], p['unit_index']): p for p in proof['placements']}
        if len(supplied) != len(proof['placements']) or set(supplied) != set(identities):
            raise ValueError('missing/duplicate physical unit')
        if not valid_placement(loading['vehicles'][key[0]], units, [supplied[i] for i in identities]):
            raise ValueError('independent physical certificate rejection')


def exact_routing(model):
    n = len(model['requests'])
    if not 1 <= n <= 8 or model['depots'] != [{'x': 0, 'y': 0}]:
        raise ValueError('small homogeneous synthetic reference only')
    if set(model) - {'config', 'depots', 'vehicles', 'tasks', 'requests', 'loading'}:
        raise ValueError('extra model constraints are outside the reference domain')
    if model['config'].get('lexicographic_objective') is not True:
        raise ValueError('reference objective is vehicles then distance')
    if any(v != model['vehicles'][0] for v in model['vehicles']):
        raise ValueError('heterogeneous fleet not supported')
    if any(set(v) != {'start_depot_id', 'end_depot_id', 'capacity'} for v in model['vehicles']):
        raise ValueError('reference requires single-trip simple vehicles')
    if 'travel' in model or 'initial_routes' in model:
        raise ValueError('reference requires Euclidean unconstrained routing')
    if 'loading' in model:
        physical = model['loading']
        geometries = [{k: v for k, v in vehicle.items() if k != 'vehicle_id'}
                      for vehicle in physical['vehicles']]
        if len(geometries) != len(model['vehicles']) or any(g != geometries[0] for g in geometries):
            raise ValueError('reference requires homogeneous physical geometry')
        if ([v['vehicle_id'] for v in physical['vehicles']] != list(range(len(model['vehicles']))) or
            [r['request_id'] for r in physical['requests']] != list(range(n))):
            raise ValueError('reference requires canonical fixture identities')
    points = []
    for request in model['requests']:
        if set(request) != {'delivery_task_id'}:
            raise ValueError('reference requires ordinary deliveries')
        task = model['tasks'][request['delivery_task_id']]
        if (set(task) != {'type', 'x', 'y', 'demand', 'tw_early', 'tw_late'} or
            task['type'] != 'delivery' or task['demand'] != [-1] or
            task['tw_early'] != 0 or task['tw_late'] != 1000000):
            raise ValueError('reference requires unit demand and wide windows')
        points.append((task['x'], task['y']))
    routes = {}
    for mask in range(1, 1 << n):
        members = [i for i in range(n) if mask >> i & 1]
        if len(members) > model['vehicles'][0]['capacity'][0]:
            continue
        best = math.inf
        for order in itertools.permutations(members):
            if 'loading' in model:
                units = [{**unit, 'stop': ordinal} for ordinal, request in enumerate(order)
                         for unit in model['loading']['requests'][request]['units']]
                if not exhaustive_pack(model['loading']['vehicles'][0], units):
                    continue
            path = [(0, 0)] + [points[i] for i in order] + [(0, 0)]
            best = min(best, sum(math.dist(a, b) for a, b in zip(path, path[1:])))
        if math.isfinite(best):
            routes[mask] = best

    @lru_cache(None)
    def partition(mask):
        if not mask:
            return (0, 0.0)
        best = (n + 1, math.inf)
        subset = mask
        while subset:
            if subset & (mask & -mask) and subset in routes:
                count, distance = partition(mask ^ subset)
                best = min(best, (count + 1, distance + routes[subset]))
            subset = (subset - 1) & mask
        return best
    vehicles, distance = partition((1 << n) - 1)
    if vehicles > len(model['vehicles']):
        raise ValueError('reference requires sufficient fleet for full coverage')
    return {'served': n, 'vehicles': vehicles, 'distance': distance}
