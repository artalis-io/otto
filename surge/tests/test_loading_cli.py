#!/usr/bin/env python3
"""Native HGS loading input/output contract; entirely synthetic fixtures."""
import copy
import json
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
INVALID_CASES=0

def fixture():
    return {
        'config':{'max_iterations':100,'max_time_seconds':2,'population':True,
                  'population_threads':2,'population_generations':2,'seed':42},
        'depots':[{'x':0,'y':0}],
        'vehicles':[{'start_depot_id':0,'end_depot_id':0,'capacity':[10]} for _ in range(2)],
        'tasks':[{'type':'delivery','x':i+1,'y':1,'tw_early':0,'tw_late':100000,'demand':[-1]} for i in range(2)],
        'requests':[{'delivery_task_id':i} for i in range(2)],
        'loading':{'version':1,'mode':'hard','node_budget':1000,
            'vehicles':[{'vehicle_id':i,'access_model':'side_free','slots':[{'lane':0,'depth':0}],
                'max_layers':2,'clearance_mm':2400,'max_stack_weight_g':10000,'payload_g':100000,
                'allowed_top':[[0]]} for i in range(2)],
            'requests':[{'request_id':i,'units':[{'height_mm':1000,'weight_g':500,'stack_class':0,
                'max_weight_above_g':1000,'max_units_above':0}]} for i in range(2)]}}

def solve(data,valid=True):
    with tempfile.TemporaryDirectory(prefix='otto-loading-cli-') as directory:
        p=Path(directory)/'request.json';p.write_text(json.dumps(data))
        result=subprocess.run([str(ROOT/'surge_solve'),str(p)],capture_output=True,text=True,timeout=30)
    if not valid:
        global INVALID_CASES
        INVALID_CASES+=1
        assert result.returncode!=0,(result.stdout,result.stderr)
        return None
    assert result.returncode==0,result.stderr
    value=json.loads(result.stdout);assert value['status']=='OK',value
    if 'loading' in data:
        for r in value['loading']['rounds']:
            assert r['outcome']=='feasible' and r['reason']=='witness',r
            assert len({(p['request_id'],p['unit_index']) for p in r['placements']})==len(r['placements'])
    else:assert 'loading' not in value
    return value

def main():
    base=fixture();value=solve(base)
    assert value['stats']['unassigned']==0 and value['stats']['vehicles_used']==2
    stacked=copy.deepcopy(base)
    for request in stacked['loading']['requests']:request['units'][0]['max_units_above']=1
    value=solve(stacked);assert value['stats']['vehicles_used']==1
    assert len(value['loading']['rounds'][0]['placements'])==2
    legacy=copy.deepcopy(base);del legacy['loading']
    value=solve(legacy);assert value['stats']['vehicles_used']==1
    tiny=copy.deepcopy(base);tiny['loading']['node_budget']=1
    value=solve(tiny);assert value['stats']['unassigned']==2
    assert len(value['loading']['unassigned_checks'])==2
    assert all(r['unknown_vehicles']==2 and r['infeasible_vehicles']==0 for r in value['loading']['unassigned_checks'])
    trips=copy.deepcopy(base);trips['vehicles']=trips['vehicles'][:1];trips['vehicles'][0]['max_trips']=2
    trips['loading']['vehicles']=trips['loading']['vehicles'][:1]
    value=solve(trips);assert value['stats']['unassigned']==0
    assert len(value['loading']['rounds'])==2
    for path,bad in [('version',2),('version',True),('node_budget',-1),('node_budget',1.5),('node_budget',100001),('mode','soft')]:
        value=copy.deepcopy(base);value['loading'][path]=bad;solve(value,False)
    for bad in [None,True,'1',-1,0.5,4294967296,9007199254740992]:
        value=copy.deepcopy(base);value['loading']['requests'][0]['units'][0]['height_mm']=bad;solve(value,False)
    for key in ['height_mm','weight_g','max_weight_above_g','max_units_above','stack_class']:
        value=copy.deepcopy(base);del value['loading']['requests'][0]['units'][0][key];solve(value,False)
    value=copy.deepcopy(base);value['loading']['vehicles'][1]['vehicle_id']=0;solve(value,False)
    value=copy.deepcopy(base);value['loading']['requests'][1]['request_id']=0;solve(value,False)
    value=copy.deepcopy(base);value['loading']['requests'].pop();solve(value,False)
    value=copy.deepcopy(base);value['loading']['vehicles'][0]['access_model']='guessed';solve(value,False)
    print(f'loading native CLI: HGS, stacking, legacy, UNKNOWN, reloads and {INVALID_CASES} invalid-input cases passed')

if __name__=='__main__':main()
