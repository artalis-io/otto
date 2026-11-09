"""Independent exact replay of the supported rigid-two-axle V2 JSON witness.

No native checker, packing search, floating reaction arithmetic or shared native
helpers. Ordinary route feasibility is tested by the existing separate suite.
"""
from fractions import Fraction


def verify(request, solution):
    facts = request['loading']
    vehicles = {v['vehicle_id']: v for v in facts['vehicles']}
    orders = {r['request_id']: r['units'] for r in facts['requests']}
    route_rounds = {}
    for route in solution['routes']:
        for stop in route['stops']:
            route_rounds.setdefault((route['vehicle_id'], stop['trip_index']), []).append(stop)
    proofs = solution['loading']['rounds']
    assert len(proofs) == len(route_rounds)
    seen = set()
    for proof in proofs:
        key = proof['vehicle_id'], proof['trip_index']
        assert key in route_rounds and key not in seen
        seen.add(key)
        stops = route_rounds[key]
        vehicle = vehicles[key[0]]
        units = {(s['request_id'], i): dict(u, stop=n) for n, s in enumerate(stops)
                 for i, u in enumerate(orders[s['request_id']])}
        places = {(p['request_id'], p['unit_index']): p for p in proof['placements']}
        assert len(places) == len(proof['placements']) and set(places) == set(units)
        assert proof['outcome'] == 'feasible' and proof['reason'] == 'witness'
        assert len(proof['operations']) == 2 * len(units)
        assert not proof['extra_moves'] and not proof['extra_service_sec'] and not proof['extra_fee_units']
        assert len(proof['stops']) == len(stops)
        for ordinal, (timing, stop) in enumerate(zip(proof['stops'], stops)):
            assert timing['task_id'] == stop['task_id'] and timing['stop_index'] == ordinal
            assert timing['arrival_sec'] == stop['arrival']
            assert timing['service_start_sec'] == stop['service_start']
            assert timing['departure_sec'] == stop['departure']
            assert timing['base_service_sec'] == request['tasks'][stop['task_id']].get('service_seconds', 0)
            assert not timing['extra_service_sec'] and not timing['extra_moves'] and not timing['extra_fee_units']
        active, loaded, delivered = set(), set(), set()
        states = []

        def state(step):
            assert len({(places[i]['slot'], places[i]['layer']) for i in active}) == len(active)
            assert sum(units[i]['weight_g'] for i in active) <= vehicle['payload_g']
            for slot in range(len(vehicle['slots'])):
                stack = sorted((i for i in active if places[i]['slot'] == slot), key=lambda i: places[i]['layer'])
                assert [places[i]['layer'] for i in stack] == list(range(len(stack)))
                assert len(stack) <= vehicle['max_layers']
                assert sum(units[i]['height_mm'] for i in stack) <= vehicle['clearance_mm']
                assert sum(units[i]['weight_g'] for i in stack) <= vehicle['max_stack_weight_g']
                for n, i in enumerate(stack):
                    above = stack[n+1:]
                    assert len(above) <= units[i]['max_units_above']
                    assert sum(units[j]['weight_g'] for j in above) <= units[i]['max_weight_above_g']
                    if above:
                        assert units[above[0]]['stack_class'] in vehicle['allowed_top'][units[i]['stack_class']]
            if 'axles' not in vehicle:
                return
            a = vehicle['axles']; length = a['wheelbase_mm']
            mass = sum(units[i]['weight_g'] for i in active)
            moment_min = sum(units[i]['weight_g'] * (a['slot_centres_mm'][places[i]['slot']] + units[i]['cg_offset_mm']['min']) for i in active)
            moment_max = sum(units[i]['weight_g'] * (a['slot_centres_mm'][places[i]['slot']] + units[i]['cg_offset_mm']['max']) for i in active)
            fmin = a['front_tare_g']['min'] + mass - Fraction(moment_max, length)
            fmax = a['front_tare_g']['max'] + mass - Fraction(moment_min, length)
            rmin = a['rear_tare_g']['min'] + Fraction(moment_min, length)
            rmax = a['rear_tare_g']['max'] + Fraction(moment_max, length)
            gmin = a['front_tare_g']['min'] + a['rear_tare_g']['min'] + mass
            gmax = a['front_tare_g']['max'] + a['rear_tare_g']['max'] + mass
            assert a['front_limits_g']['min'] <= fmin <= fmax <= a['front_limits_g']['max']
            assert a['rear_limits_g']['min'] <= rmin <= rmax <= a['rear_limits_g']['max']
            assert gmax <= a['max_gross_g']
            fraction = a['min_front_fraction']
            if fraction:
                gross = a['front_tare_g']['min'] + a['rear_tare_g']['max'] + mass
                assert fmin >= Fraction(fraction['numerator'], fraction['denominator']) * gross
            states.append({'step': step, 'front_min_g': fmin, 'front_max_g': fmax,
                           'rear_min_g': rmin, 'rear_max_g': rmax,
                           'gross_min_g': gmin, 'gross_max_g': gmax})

        state(0)
        for step, op in enumerate(proof['operations']):
            identity = op['request_id'], op['unit_index']
            assert identity in units
            place = places[identity]; slot = op['slot']; layer = op['layer']
            assert (slot, layer) == (place['slot'], place['layer'])
            assert 0 <= slot < len(vehicle['slots']) and 0 <= layer < vehicle['max_layers']
            assert not any(places[i]['slot'] == slot and places[i]['layer'] > layer for i in active)
            if vehicle['access_model'] == 'rear_lifo':
                target = vehicle['slots'][slot]
                assert not any(vehicle['slots'][places[i]['slot']]['lane'] == target['lane'] and
                               vehicle['slots'][places[i]['slot']]['depth'] < target['depth'] for i in active if i != identity)
            if step < len(units):
                assert op['kind'] == 'load' and op['stop_index'] is None and identity not in loaded
                loaded.add(identity); active.add(identity)
            else:
                assert op['kind'] == 'deliver' and identity in active and identity not in delivered
                assert op['stop_index'] == units[identity]['stop']
                assert not any(units[i]['stop'] < op['stop_index'] for i in active)
                active.remove(identity); delivered.add(identity)
            state(step + 1)
        assert not active and loaded == delivered == set(units)
        assert len(states) == len(proof['axle_states'])
        for expected, actual in zip(states, proof['axle_states']):
            assert expected.keys() == actual.keys()
            for field, value in expected.items():
                if isinstance(value, Fraction):
                    rational = actual[field]
                    assert isinstance(rational['numerator'], str)
                    assert rational['denominator'] == vehicle['axles']['wheelbase_mm']
                    assert value == Fraction(int(rational['numerator']), rational['denominator'])
                else:
                    assert actual[field] == value
