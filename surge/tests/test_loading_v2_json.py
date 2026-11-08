#!/usr/bin/env python3
"""Real CLI/API parity, exact evidence replay and fail-closed malformed input."""
import copy
import json
from pathlib import Path
import subprocess
import tempfile
import sys
import random
from loading_v2_reference import verify

ROOT = Path(__file__).resolve().parents[2]
CLI = ROOT / 'surge/surge_solve'
BASE = json.loads((ROOT / 'docs/roadmaps/loading-v2-examples/axles.request.json').read_text())
count = 0

def run(request, success=True, status=None):
    global count
    with tempfile.NamedTemporaryFile(mode='w', suffix='.json') as f:
        f.write(request if isinstance(request, str) else json.dumps(request))
        f.flush()
        result = subprocess.run([str(CLI), f.name], capture_output=True, text=True, timeout=15)
    data = json.loads(result.stdout)
    assert (result.returncode == 0) == success, (result.returncode, data, result.stderr)
    if status:
        assert data['status'] == status, data
    if not success:
        assert 'routes' not in data and 'loading' not in data, data
    if success:
        verify(request, data)
    count += 1
    return data

actual = run(BASE, status='OK')
assert actual['stats']['unassigned'] == 0
loading = actual['loading']
round_ = loading['rounds'][0]
assert round_['axle_states'][1]['front_min_g'] == {'numerator': '12975000000', 'denominator': 4000}
assert round_['stops'][0]['departure_sec'] == 60
assert len(round_['operations']) == 2

# Compare native output with the independently calculated design witness.
expected = json.loads((ROOT / 'docs/roadmaps/loading-v2-examples/axles.result-loading.json').read_text())
a = copy.deepcopy(loading); b = copy.deepcopy(expected)
a['rounds'][0].pop('nodes'); b['rounds'][0].pop('nodes')
assert a == b, (a, b)

fixed = copy.deepcopy(BASE)
fixed['plan'] = [{'vehicle_id': 0, 'task_ids': [0]}]
fixed['loading_witnesses'] = loading
assert run(fixed, status='OK')['loading'] == loading
without = copy.deepcopy(fixed); without.pop('loading_witnesses')
run(without, status='OK')

# Replay works without a search budget; FIND cannot manufacture a certificate.
zero = copy.deepcopy(fixed)
zero['loading']['node_budget'] = zero['loading_witnesses']['node_budget'] = 0
zero['loading_witnesses']['rounds'][0]['nodes'] = 0
run(zero, status='OK')
zero.pop('loading_witnesses')
run(zero, False, 'LIMIT')

for bad_plan in ([{'vehicle_id': 0.5, 'task_ids': [0]}],
                 [{'vehicle_id': 0, 'task_ids': [0.5]}],
                 [{'vehicle_id': 0, 'task_ids': ['0']}],
                 [{'vehicle_id': 0, 'task_ids': [0], 'unrecognized': 1}],
                 [{'vehicle_id': 0, 'task_ids': [0, 0]}]):
    bad = copy.deepcopy(fixed); bad['plan'] = bad_plan
    run(bad, False, 'INVALID_ARG')

# Forged exact reactions, placement, action identity and schedule are rejected.
for mutation in ('reaction', 'placement', 'kind', 'stop', 'time', 'missing', 'duplicate'):
    bad = copy.deepcopy(fixed); r = bad['loading_witnesses']['rounds'][0]
    if mutation == 'reaction': r['axle_states'][1]['front_min_g']['numerator'] = '12975000001'
    if mutation == 'placement': r['placements'][0]['slot'] = 1
    if mutation == 'kind': r['operations'][0]['kind'] = 'deliver'
    if mutation == 'stop': r['operations'][1]['stop_index'] = None
    if mutation == 'time': r['stops'][0]['departure_sec'] += 1
    if mutation == 'missing': r['operations'].pop()
    if mutation == 'duplicate': bad['loading_witnesses']['rounds'].append(copy.deepcopy(r))
    run(bad, False)

for mutation in ('unknown', 'negative', 'fraction', 'missing_cg', 'bad_hash', 'null_mass', 'wrong_slot_count', 'unknown_cap', 'dup_cap', 'unselected', 'literal_zero', 'bad_profile'):
    bad = copy.deepcopy(BASE); l = bad['loading']; v = l['vehicles'][0]; u = l['requests'][0]['units'][0]
    if mutation == 'unknown': l['guess_me'] = 1
    if mutation == 'negative': u['weight_g'] = -1
    if mutation == 'fraction': v['axles']['wheelbase_mm'] = 4000.5
    if mutation == 'missing_cg': u.pop('cg_offset_mm')
    if mutation == 'bad_hash': v['axles']['profile_sha256'] = 'x' * 64
    if mutation == 'null_mass': v['axles']['front_tare_g'] = None
    if mutation == 'wrong_slot_count': v['axles']['slot_centres_mm'].append(3000)
    if mutation == 'unknown_cap': l['required_capabilities'].append('magic')
    if mutation == 'dup_cap': l['required_capabilities'].append('stacking')
    if mutation == 'unselected': l['required_capabilities'] = ['stacking']
    if mutation == 'literal_zero': u['height_mm'] = 0
    if mutation == 'bad_profile': v['axles']['deck_min_mm'] = 999999
    run(bad, False, 'INVALID_ARG')

raw = json.dumps(BASE).replace('"node_budget": 10000', '"node_budget": 0, "node_budget": 10000')
run(raw, False, 'INVALID_ARG')
raw = json.dumps(BASE).replace('"weight_g": 500000', '"weight_g\\u0000junk": 500000')
run(raw, False, 'INVALID_ARG')

future = copy.deepcopy(BASE)
future['loading']['version'] = 3
run(future, False, 'NOT_IMPLEMENTED')

unsupported = copy.deepcopy(BASE)
unsupported['loading']['required_capabilities'].append('bounded_reshuffling')
unsupported['loading']['frontier_budget'] = 256
run(unsupported, False, 'NOT_IMPLEMENTED')

# Capability subset stacking-only is explicit, with no hidden axle requirements.
stacking = copy.deepcopy(BASE)
stacking['loading']['required_capabilities'] = ['stacking']
stacking['loading']['vehicles'][0].pop('axles')
stacking['loading']['requests'][0]['units'][0].pop('cg_offset_mm')
r = run(stacking, status='OK')
assert r['loading']['rounds'][0]['axle_states'] == []

# Literal zero payload is restrictive rather than interpreted as unlimited.
zero_payload = copy.deepcopy(BASE)
zero_payload['loading']['vehicles'][0]['payload_g'] = 0
r = run(zero_payload, status='OK')
assert r['stats']['unassigned'] == 1 and not r['routes']
assert r['loading']['unassigned_checks'][0]['infeasible_vehicles'] == 1

# Every reload round has its own witness; zero-unit stops retain their timing.
multi = copy.deepcopy(BASE)
multi['tasks'].extend([copy.deepcopy(multi['tasks'][0]), copy.deepcopy(multi['tasks'][0])])
multi['tasks'][1]['demand'] = [-1]
multi['requests'].extend([{'delivery_task_id': 1}, {'delivery_task_id': 2}])
u = copy.deepcopy(multi['loading']['requests'][0]['units'])
multi['loading']['requests'].extend([{'request_id': 1, 'units': []}, {'request_id': 2, 'units': u}])
multi['vehicles'][0].update(max_trips=2, trip_reload_seconds=30)
multi['plan'] = [{'vehicle_id': 0, 'task_ids': [0, 1, 2], 'trip_starts': [0, 0, 1]}]
r = run(multi, status='OK')
rs = r['loading']['rounds']
assert len(rs) == 2 and len(rs[0]['stops']) == 2 and len(rs[1]['stops']) == 1
assert rs[0]['stops'][1]['task_id'] == 1 and rs[0]['stops'][1]['departure_sec'] == 120
assert rs[1]['trip_index'] == 1 and rs[1]['stops'][0]['departure_sec'] == 210
multi['loading_witnesses'] = r['loading']
run(multi, status='OK')
multi['loading_witnesses']['rounds'][1]['trip_index'] = 0
run(multi, False, 'INVALID_ARG')

# Stop count is independent of physical-unit count; do not truncate after 128.
long = copy.deepcopy(BASE)
long['tasks'] = [dict(long['tasks'][0], demand=[-1], service_seconds=0) for _ in range(130)]
long['requests'] = [{'delivery_task_id': i} for i in range(130)]
long['loading']['requests'] = [{'request_id': i, 'units': []} for i in range(130)]
long['plan'] = [{'vehicle_id': 0, 'task_ids': list(range(130))}]
r = run(long, status='OK')
assert len(r['loading']['rounds'][0]['stops']) == 130
assert r['loading']['rounds'][0]['operations'] == []
assert len(r['loading']['rounds'][0]['axle_states']) == 1
long['loading_witnesses'] = r['loading']
run(long, status='OK')

# Exercise varied actual HGS routes against an independent full operation/state
# replay. All physical facts here are synthetic; no customer defaults inferred.
sys.path.insert(0, str(ROOT / 'surge/benchmarks'))
from loading_quality import fixture, adversarial_fixture
rng = random.Random(5619)
for case in range(12):
    model = adversarial_fixture(bool(case % 2)) if case < 4 else fixture(6, False, 'rear_lifo' if case % 2 else 'side_free')
    model['config'].update(max_iterations=40, max_time_seconds=1, population=True,
                           population_threads=2, population_generations=2, seed=case + 41)
    l = model['loading']; l.update(version=2, required_capabilities=['stacking', 'axle_loads'])
    for v in l['vehicles']:
        centres = [1000 + 1200 * n for n in range(len(v['slots']))]
        if v['access_model'] == 'rear_lifo': centres.reverse()
        v['axles'] = dict(model='rigid_two_axle_v1', profile_sha256='e' * 64,
            wheelbase_mm=8000, footprint_length_mm=1200, deck_min_mm=0, deck_max_mm=8000,
            slot_centres_mm=centres, front_tare_g=dict(min=3000000, max=3000100),
            rear_tare_g=dict(min=2000000, max=2000100), front_limits_g=dict(min=0, max=6000000),
            rear_limits_g=dict(min=0, max=6500000), max_gross_g=12000000,
            min_front_fraction=dict(numerator=1, denominator=5))
    for order in l['requests']:
        for u in order['units']:
            u['cg_offset_mm'] = dict(min=-rng.randrange(51), max=rng.randrange(51))
    result = run(model, status='OK')
    assert result['stats']['unassigned'] == 0

caps = json.loads(subprocess.check_output([str(CLI), '--capabilities']))
assert caps['implemented_capabilities'] == ['stacking', 'axle_loads'] and caps['handling_models'] == []
print(f'loading v2 JSON: {count} real native solve/replay/failure cases passed')
