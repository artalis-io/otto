#!/usr/bin/env python3
"""Synthetic multi-position physics, metamorphic laws and native JSON round trips.

An exact corner-enumeration oracle, not a vehicle calibration. No production
checker or search routine computes expected reactions. All inputs are public.
"""
import copy
import ctypes
from fractions import Fraction
import itertools
import json
from pathlib import Path
import random
import subprocess
import tempfile

from test_axle_oracle import load_library
from loading_v2_reference import verify

ROOT = Path(__file__).resolve().parents[2]
BASE = json.loads((ROOT / 'docs/roadmaps/loading-v2-examples/axles.request.json').read_text())
SEED = 20261110


def interval(low, high=None):
    return {'min': low, 'max': low if high is None else high}


def profile(centres, length=4000):
    return dict(model='rigid_two_axle_v1', profile_sha256='a' * 64,
        wheelbase_mm=length, footprint_length_mm=1000, deck_min_mm=-2000,
        deck_max_mm=length+2000, slot_centres_mm=centres,
        front_tare_g=interval(3000000), rear_tare_g=interval(2000000),
        front_limits_g=interval(0, 50000000), rear_limits_g=interval(0, 50000000),
        max_gross_g=50000000, min_front_fraction=None)


def unit(weight, slot=0, cg=(0, 0), active=1):
    return dict(weight_g=weight, slot=slot, cg_offset_mm=interval(*cg), active=active)


def corners(p, units):
    """Enumerate independent tare and per-unit CG endpoints using Fractions."""
    onboard = [u for u in units if u['active']]
    mass = sum(u['weight_g'] for u in onboard)
    endpoints = [sorted(set(u['cg_offset_mm'].values())) for u in onboard]
    for tf, tr in itertools.product(p['front_tare_g'].values(), p['rear_tare_g'].values()):
        for offsets in itertools.product(*endpoints):
            rear = Fraction(tr) + sum(Fraction(u['weight_g'] *
                (p['slot_centres_mm'][u['slot']] + x), p['wheelbase_mm'])
                for u, x in zip(onboard, offsets))
            gross = tf + tr + mass
            yield Fraction(gross) - rear, rear, gross


def oracle(p, units):
    values = list(corners(p, units))
    front, rear, gross = zip(*values)
    share = p['min_front_fraction']
    feasible = (min(front) >= p['front_limits_g']['min'] and max(front) <= p['front_limits_g']['max']
        and min(rear) >= p['rear_limits_g']['min'] and max(rear) <= p['rear_limits_g']['max']
        and max(gross) <= p['max_gross_g'] and
        (share is None or all(f >= Fraction(share['numerator'], share['denominator']) * g for f, _, g in values)))
    bounds = [min(front), max(front), min(rear), max(rear)]
    scaled = [b * p['wheelbase_mm'] for b in bounds]
    assert all(b.denominator == 1 for b in scaled)
    return (0 if feasible else 3), [int(b) for b in scaled] + [p['wheelbase_mm'], min(gross), max(gross)]


def state_checks():
    checks = 0
    with tempfile.TemporaryDirectory(prefix='surge-multislot-physics-') as directory:
        lib = load_library(directory)
        native = lib.sg_axle_multislot_probe
        native.restype = ctypes.c_int
        native.argtypes = [ctypes.POINTER(ctypes.c_int64), ctypes.c_uint32, ctypes.POINTER(ctypes.c_int64)]

        def check(label, p, units):
            nonlocal checks
            fraction = p['min_front_fraction']
            row = [p['wheelbase_mm'], p['deck_min_mm'], p['deck_max_mm'], p['footprint_length_mm'],
                len(p['slot_centres_mm']), *p['front_tare_g'].values(), *p['rear_tare_g'].values(),
                *p['front_limits_g'].values(), *p['rear_limits_g'].values(), p['max_gross_g'],
                int(fraction is not None), fraction['numerator'] if fraction else 0,
                fraction['denominator'] if fraction else 0, len(units), *p['slot_centres_mm']]
            for u in units:
                row += [u['weight_g'], *u['cg_offset_mm'].values(), u['slot'], u['active']]
            out = (ctypes.c_int64 * 7)()
            status = native((ctypes.c_int64 * len(row))(*row), len(row), out)
            expected = oracle(p, units)
            assert (status, list(out)) == expected, (SEED, label, p, units, status, list(out), expected)
            checks += 1
            return status, list(out)

        rng = random.Random(SEED)
        for case in range(192):
            length = rng.choice([4000, 5501, 7813, 65537])
            p = profile([rng.randint(500, length-500) for _ in range(rng.randint(2, 6))], length)
            p['front_tare_g'] = interval(2900000, 3100000)
            p['rear_tare_g'] = interval(1900000, 2100000)
            units = [unit(rng.randrange(2000001), rng.randrange(len(p['slot_centres_mm'])),
                          (-rng.randrange(101), rng.randrange(101)), rng.randrange(2)) for _ in range(rng.randint(1, 5))]
            original = check((case, 'base'), p, units)
            assert original[0] == 0  # balanced geometry and deliberately loose limits
            shuffled = copy.deepcopy(units); rng.shuffle(shuffled)
            assert check((case, 'permutation'), p, shuffled) == original
            assert check((case, 'zero-mass'), p, units + [unit(0)]) == original
            split = copy.deepcopy(units)
            first = split.pop(0); a = copy.deepcopy(first); b = copy.deepcopy(first)
            a['weight_g'] //= 2; b['weight_g'] -= a['weight_g']
            assert check((case, 'split-mass'), p, split + [a, b]) == original
            shifted = copy.deepcopy(p); shifted['slot_centres_mm'] = [x+50 for x in p['slot_centres_mm']]
            shifted['deck_min_mm'] += 50; shifted['deck_max_mm'] += 50
            shifted_units = copy.deepcopy(units)
            for u in shifted_units:
                u['cg_offset_mm'] = {k: v-50 for k, v in u['cg_offset_mm'].items()}
            assert check((case, 'same-global-CG'), shifted, shifted_units) == original
            reflected = copy.deepcopy(p)
            reflected['slot_centres_mm'] = [length-x for x in p['slot_centres_mm']]
            reflected['deck_min_mm'], reflected['deck_max_mm'] = length-p['deck_max_mm'], length-p['deck_min_mm']
            for field in ['tare_g', 'limits_g']:
                reflected['front_'+field], reflected['rear_'+field] = p['rear_'+field], p['front_'+field]
            reflected_units = copy.deepcopy(units)
            for u in reflected_units:
                u['cg_offset_mm'] = interval(-u['cg_offset_mm']['max'], -u['cg_offset_mm']['min'])
            expected = original[1][2:4] + original[1][0:2] + original[1][4:]
            assert check((case, 'front-rear-reflection'), reflected, reflected_units) == (original[0], expected)
            stretched = copy.deepcopy(p); stretched_units = copy.deepcopy(units)
            for k in ['wheelbase_mm', 'footprint_length_mm', 'deck_min_mm', 'deck_max_mm']: stretched[k] *= 2
            stretched['slot_centres_mm'] = [2*x for x in p['slot_centres_mm']]
            for u in stretched_units: u['cg_offset_mm'] = {k: 2*v for k, v in u['cg_offset_mm'].items()}
            assert check((case, 'length-scale'), stretched, stretched_units) == (0, [2*x for x in original[1][:5]] + original[1][5:])
            heavier = copy.deepcopy(p); heavier_units = copy.deepcopy(units)
            for k in ['front_tare_g', 'rear_tare_g', 'front_limits_g', 'rear_limits_g']:
                heavier[k] = {key: 2*v for key, v in p[k].items()}
            heavier['max_gross_g'] *= 2
            for u in heavier_units: u['weight_g'] *= 2
            expected = [2*x for x in original[1][:4]] + [original[1][4]] + [2*x for x in original[1][5:]]
            assert check((case, 'mass-scale'), heavier, heavier_units) == (0, expected)
            # Tighten each independent bound just beyond the exact extremum.
            for axle, index in [('front', 1), ('rear', 3)]:
                tight = copy.deepcopy(p)
                maximum = Fraction(original[1][index], length)
                tight[axle+'_limits_g']['max'] = (maximum.numerator-1)//maximum.denominator
                assert check((case, axle+'-upper'), tight, units)[0] == 3
            tight = copy.deepcopy(p); tight['max_gross_g'] = original[1][6]-1
            assert check((case, 'gross-upper'), tight, units)[0] == 3
            minimum_share = min(f/Fraction(g) for f, _, g in corners(p, units))
            q = 1000000; lower = int(minimum_share*q)
            for numerator, wanted in [(lower, 0), (lower+1, 3)]:
                tight = copy.deepcopy(p); tight['min_front_fraction'] = dict(numerator=numerator, denominator=q)
                assert check((case, 'front-share', numerator), tight, units)[0] == wanted

        # Mixed signed moments cancel beyond float's exact integer range.
        # This is deliberately gross-infeasible; bounds must still be exact.
        p = profile([-900000, 900000], 1000000)
        p.update(deck_min_mm=-1000000, deck_max_mm=1000000, max_gross_g=1000000000)
        units = [unit(1000000000, i % 2) for i in range(128)]
        check('128-unit-signed-cancellation', p, units)
        units.reverse(); check('128-unit-permuted', p, units)
    return checks


def native_model(p, groups, order=None):
    model = copy.deepcopy(BASE)
    model['config'].update(population=True, population_threads=2, population_generations=3)
    model['vehicles'][0]['capacity'] = [50000]
    model['tasks'] = [dict(model['tasks'][0], demand=[-max(1, sum(u['weight_g'] for u in group)/1000)]) for group in groups]
    model['requests'] = [{'delivery_task_id': i} for i in range(len(groups))]
    vehicle = model['loading']['vehicles'][0]
    vehicle.update(slots=[{'lane': i, 'depth': 0} for i in range(len(p['slot_centres_mm']))],
        max_layers=1, clearance_mm=2000, max_stack_weight_g=50000000, payload_g=50000000, axles=p)
    model['loading']['requests'] = [{'request_id': i, 'units': [dict(height_mm=1000,
        weight_g=u['weight_g'], stack_class=0, max_weight_above_g=0, max_units_above=0,
        cg_offset_mm=u['cg_offset_mm']) for u in group]} for i, group in enumerate(groups)]
    model['plan'] = [{'vehicle_id': 0, 'task_ids': order if order is not None else list(range(len(groups)))}]
    return model


def named_cases():
    cases = []
    def add(name, p, groups, expected, full=None, order=None):
        cases.append((name, native_model(p, groups, order), expected, full))
    add('empty cargo, tare only', profile([2000]), [[]], True, (3000000, 3000000, 2000000, 2000000, 5000000, 5000000))
    add('balanced full load', profile([1000, 3000]), [[unit(1000000)], [unit(1000000)]], True,
        (4000000, 4000000, 3000000, 3000000, 7000000, 7000000))
    add('rear-biased full load', profile([3000]), [[unit(2000000)]], True,
        (3500000, 3500000, 3500000, 3500000, 7000000, 7000000))
    add('front overhang', profile([-1000]), [[unit(1000000)]], True,
        (4250000, 4250000, 1750000, 1750000, 6000000, 6000000))
    add('rear overhang', profile([5000]), [[unit(1000000)]], True,
        (2750000, 2750000, 3250000, 3250000, 6000000, 6000000))
    p = profile([2000]); p['front_tare_g']=interval(2900000,3100000); p['rear_tare_g']=interval(1900000,2100000)
    add('independent tare and CG uncertainty', p, [[unit(500000, cg=(-50,50))]], True,
        (3143750,3356250,2143750,2356250,5300000,5700000))
    p=profile([2000],4001)
    add('non-integral gram reactions', p, [[unit(1)]], True,
        (3000000+Fraction(2001,4001),)*2+(2000000+Fraction(2000,4001),)*2+(5000001,5000001))
    p=copy.deepcopy(p);p['front_limits_g']['max']=3000000
    add('sub-gram overload is not rounded away', p, [[unit(1)]], False)
    for limit, good in [(3256250,True),(3250000,False)]:
        p=profile([2000]);p['front_limits_g']['max']=limit
        add('CG endpoint versus nominal-only '+str(limit),p,[[unit(500000,cg=(-50,50))]],good)
    for limit, good in [(7000000,True),(6999999,False)]:
        p=profile([1000,3000]);p['max_gross_g']=limit
        add('gross equality versus one gram '+str(limit),p,[[unit(1000000)],[unit(1000000)]],good)
    for numerator, good in [(500000,True),(500001,False)]:
        p=profile([3000]);p['min_front_fraction']=dict(numerator=numerator,denominator=1000000)
        add('loaded front-share boundary '+str(numerator),p,[[unit(2000000)]],good)
    for minimum, good in [(1750000,True),(1750001,False)]:
        p=profile([-1000]);p['rear_limits_g']['min']=minimum
        add('rear minimum boundary '+str(minimum),p,[[unit(1000000)]],good)
    p=profile([2000]);p['front_limits_g']['max']=2999999
    add('empty tare itself violates front limit',p,[[]],False)
    p=profile([2000]);p.update(front_tare_g=interval(0),rear_tare_g=interval(0),front_limits_g=interval(0),rear_limits_g=interval(0),max_gross_g=0,min_front_fraction=dict(numerator=1,denominator=1))
    add('literal zero empty limits',p,[[]],True,(0,0,0,0,0,0))
    p=profile([-2000,5000]);p['deck_min_mm']=-3000
    p['front_limits_g']['max']=6000000;p['rear_limits_g']['max']=6500000
    add('full safe but first delivery removes counterweight',copy.deepcopy(p),[[unit(1000000)],[unit(4000000)]],False)
    add('safe reversed delivery sequence',copy.deepcopy(p),[[unit(1000000)],[unit(4000000)]],True,
        (3500000,3500000,6500000,6500000,10000000,10000000),order=[1,0])
    add('same-stop removal must deliver heavy unit first',p,[[unit(1000000),unit(4000000)]],True,
        (3500000,3500000,6500000,6500000,10000000,10000000))
    return cases


def json_checks():
    count = 0
    def run(name, model, good):
        nonlocal count
        with tempfile.NamedTemporaryFile('w',suffix='.json') as file:
            json.dump(model,file);file.flush()
            result=subprocess.run([str(ROOT/'surge/surge_solve'),file.name],capture_output=True,text=True,timeout=20)
        response=json.loads(result.stdout)
        assert (result.returncode==0)==good,(name,response,result.stderr)
        if good:
            assert response['status']=='OK' and response['stats']['unassigned']==0,(name,response)
            verify(model,response)
        else:
            assert response['status']=='INFEASIBLE' and 'routes' not in response and 'loading' not in response,(name,response)
        count+=1
        return response
    for name, model, good, full in named_cases():
        response=run(name,model,good)
        if not good:continue
        proof=response['loading']['rounds'][0]
        if full is not None:
            n=sum(len(r['units']) for r in model['loading']['requests']);state=proof['axle_states'][n]
            actual=tuple(Fraction(int(state[k]['numerator']),state[k]['denominator']) for k in
                ['front_min_g','front_max_g','rear_min_g','rear_max_g'])+(state['gross_min_g'],state['gross_max_g'])
            assert actual==full,(name,actual,full)
        if name.startswith('same-stop'):
            assert [op['unit_index'] for op in proof['operations'] if op['kind']=='deliver']==[1,0]
        model['loading_witnesses']=response['loading']
        run(name+' exact public witness replay',model,True)
    return count


if __name__=='__main__':
    print(f'multi-position exact physics: {state_checks()} seeded/metamorphic state checks passed (seed {SEED})')
    print(f'named synthetic native physics: {json_checks()} solve/replay/failure checks passed')
