"""Exhaustive independent placements/removal orders vs native joint search."""
import ctypes
from fractions import Fraction
import itertools
import os
import random
import tempfile
from test_axle_oracle import load_library

def feasible(row,placements,order,cg=None):
    L,_,_,_,x0,tf0,tf1,tr0,tr1,f0,f1,r0,r1,gross,has,p,q,n,x1,rear,rule0,rule1=row[:22]
    units=[row[22+6*i:28+6*i] for i in range(n)]
    cg = cg if cg is not None else [(0, 0)] * n
    if len(set(placements))!=n:return False
    for slot in range(2):
        ids=sorted([i for i in range(n) if placements[i][0]==slot],key=lambda i:placements[i][1])
        if [placements[i][1] for i in ids]!=list(range(len(ids))):return False
        if len(ids)>2 or sum(units[i][5] for i in ids)>2400 or sum(units[i][0] for i in ids)>12000000:return False
        for at,i in enumerate(ids):
            above=ids[at+1:]
            if len(above)>units[i][3] or sum(units[j][0] for j in above)>units[i][4]:return False
            if above and not ([rule0,rule1][units[i][2]] & (1<<units[above[0]][2])):return False
    if sum(u[0] for u in units)>12000000:return False
    active=set(range(n));centres=[x0,x1]
    def axles():
        mass=sum(units[i][0] for i in active)
        for tf,tr in itertools.product([tf0,tf1],[tr0,tr1]):
            for offsets in itertools.product(*[sorted(set(cg[i])) for i in active]):
                moment=sum(units[i][0]*(centres[placements[i][0]]+offset) for i,offset in zip(active,offsets))
                back=Fraction(tr)+Fraction(moment,L);total=tf+tr+mass;front=total-back
                if not (f0<=front<=f1 and r0<=back<=r1 and total<=gross):return False
                if has and front<Fraction(p,q)*total:return False
        return True
    if not axles():return False
    for i in order:
        if i not in active:return False
        if any(units[j][1]<units[i][1] for j in active):return False
        slot,layer=placements[i]
        if any(placements[j][0]==slot and placements[j][1]>layer for j in active):return False
        # rear slot 1 is nearer the door than slot 0 in these fixtures.
        if rear and slot==0 and any(placements[j][0]==1 for j in active):return False
        active.remove(i)
        if not axles():return False
    return not active

def main():
    if os.name=='nt':raise SystemExit('Use portable C tests; oracle bridge requires POSIX compiler.')
    with tempfile.TemporaryDirectory(prefix='surge-axle-search-oracle-') as directory:
        lib=load_library(directory)
        for name in ['sg_axle_joint_probe','sg_axle_joint_cg_probe']:
            probe=getattr(lib,name);probe.restype=ctypes.c_int;probe.argtypes=[ctypes.POINTER(ctypes.c_int64),ctypes.c_uint32,ctypes.POINTER(ctypes.c_int64)]
        rng=random.Random(8128);positives=0
        uncertain_positives=0
        for case in range(768):
            n=rng.randrange(4);q=rng.choice([1,3,5,1000000]);p=rng.randrange(q+1)
            row=[4000,-3000,6000,1200,-2000,3000000,3000000,2000000,2000000,0,rng.randint(6000000,10000000),0,rng.randint(6000000,10000000),12000000,1,p,q,n,5000,rng.randrange(2),rng.randrange(4),rng.randrange(4)]
            for i in range(n):row += [rng.choice([0,500000,1000000,4000000]),rng.randrange(3),rng.randrange(2),rng.randrange(3),rng.randrange(9)*1000000,rng.choice([1000,1500])]
            # Preserve the original 512 seeded cases exactly; add independent
            # CG corners and tare uncertainty only after that original corpus.
            cg=[(-rng.randrange(601),rng.randrange(601)) for _ in range(n)] if case>=512 else None
            if cg is not None:row[5:9]=[2900000,3100000,1900000,2100000]
            possible=False
            for placement in itertools.product(list(itertools.product(range(2),range(2))),repeat=n):
                for order in itertools.permutations(range(n)):
                    if feasible(row,placement,order,cg):possible=True;break
                if possible:break
            encoded=row if cg is None else row[:22]+[x for i in range(n) for x in row[22+6*i:28+6*i]+list(cg[i])]
            probe=lib.sg_axle_joint_probe if cg is None else lib.sg_axle_joint_cg_probe
            source=(ctypes.c_int64*len(encoded))(*encoded);out=(ctypes.c_int64*(2+3*n))()
            status=probe(source,len(encoded),out)
            assert status==0,(8128,case,row,cg,status)
            assert out[0] in [0,1],('unexpected UNKNOWN',8128,case,row,cg,list(out))
            assert (out[0]==0)==possible,(8128,case,row,cg,list(out),possible)
            if possible:
                placement=[(out[2+2*i],out[3+2*i]) for i in range(n)];order=list(out[2+2*n:2+3*n]);assert feasible(row,placement,order,cg),(case,row,cg,list(out));positives+=1
                if cg is not None:uncertain_positives+=1
        assert 0<uncertain_positives<256, 'CG uncertainty must cover feasible and infeasible cases'
        print(f'joint axle/stack/unloading exact oracle: 768 cases passed ({positives} feasible, {768-positives} infeasible; 256 uncertain-CG cases)')
if __name__=='__main__':main()
