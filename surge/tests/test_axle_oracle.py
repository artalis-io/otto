"""Independent exact-rational axle oracle; public synthetic inputs only."""
import ctypes
from fractions import Fraction
import itertools
import os
from pathlib import Path
import random
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def load_library(directory):
    """Shared test-only field bridge; never linked into production archives."""
    if os.name == 'nt':
        raise RuntimeError('Fraction bridge requires a POSIX compiler; native C tests remain portable.')
    library = Path(directory) / 'oracle.so'
    compiler = shlex.split(os.environ.get('CC', 'cc'))
    shared = '-dynamiclib' if sys.platform == 'darwin' else '-shared'
    subprocess.run(compiler + ['-std=c11', '-O1', shared, '-fPIC', '-Wall', '-Wextra', '-Werror',
        '-I'+str(ROOT/'include'), str(ROOT/'tests/axle_oracle_probe.c'), str(ROOT/'src/sg_axle.c'),
        str(ROOT/'src/sg_loading.c'), '-o', str(library)], check=True)
    return ctypes.CDLL(str(library))

def oracle(row):
    L, deck_min, deck_max, footprint, x, tf0, tf1, tr0, tr1, f0, f1, r0, r1, gross, has, p, q, n = row[:18]
    del deck_min, deck_max, footprint
    units = [row[18+4*i:22+4*i] for i in range(n)]
    onboard = [u for u in units if u[3]]
    mass = sum(u[0] for u in onboard)
    fronts, rears, totals, shares = [], [], [], []
    # Enumerate every independent physical endpoint combination, rather than
    # calling production extrema or fraction helpers. Fractions never round.
    for tf, tr in itertools.product([tf0, tf1], [tr0, tr1]):
        corners = list(itertools.product(*[sorted({u[1],u[2]}) for u in onboard])) if len(onboard)<=6 else [tuple(u[1] for u in onboard),tuple(u[2] for u in onboard)]
        # For the numeric-boundary-only large case, every reaction and the
        # front-share linear inequality is monotone in all nonnegative-mass
        # unit offsets. All-min/all-max therefore suffice without 2^128 states.
        for offsets in corners:
            rear = Fraction(tr) + sum(Fraction(u[0]*(x+offset),L) for u,offset in zip(onboard,offsets))
            total = tf + tr + mass
            front = total-rear
            fronts.append(front);rears.append(rear);totals.append(total)
            shares.append(front >= Fraction(p,q)*total if has else True)
    accepted = min(fronts)>=f0 and max(fronts)<=f1 and min(rears)>=r0 and max(rears)<=r1 and max(totals)<=gross and all(shares)
    bounds = [min(fronts)*L,max(fronts)*L,min(rears)*L,max(rears)*L,L,min(totals),max(totals)]
    assert all(v.denominator == 1 for v in bounds if isinstance(v,Fraction))
    return 0 if accepted else 3, [int(v) for v in bounds]  # existing SGStatus enum

def main():
    if os.name == 'nt':
        raise SystemExit('Fraction bridge requires a POSIX compiler; native C tests remain portable.')
    with tempfile.TemporaryDirectory(prefix='surge-axle-oracle-') as directory:
        lib=load_library(directory);probe=lib.sg_axle_oracle_probe
        probe.argtypes=[ctypes.POINTER(ctypes.c_int64),ctypes.c_uint32,ctypes.POINTER(ctypes.c_int64)];probe.restype=ctypes.c_int
        rng=random.Random(20261108);count=0
        def check(row):
            nonlocal count
            wanted_status,wanted_bounds=oracle(row)
            source=(ctypes.c_int64*len(row))(*row);out=(ctypes.c_int64*7)()
            status=probe(source,len(row),out)
            assert (status,list(out))==(wanted_status,wanted_bounds),(row,status,list(out),wanted_status,wanted_bounds)
            count+=1
        for i in range(4096):
            n=rng.randrange(7);L=rng.randint(1,1000000);x=rng.randint(-900000,900000)
            tf=sorted([rng.randint(0,1000000000) for _ in range(2)]);tr=sorted([rng.randint(0,1000000000) for _ in range(2)])
            f=sorted([rng.randint(0,1000000000) for _ in range(2)]);r=sorted([rng.randint(0,1000000000) for _ in range(2)])
            q=rng.randint(1,1000000);p=rng.randint(0,q)
            row=[L,-1000000,1000000,1000,x,*tf,*tr,*f,*r,rng.randint(0,1000000000),1,p,q,n]
            for _ in range(n):
                low,high=sorted([rng.randint(-500,500) for _ in range(2)])
                row += [rng.randint(0,1000000000),low,high,rng.randrange(2)]
            check(row)
        # Dense near-boundary front-share comparisons: most random heavily
        # loaded profiles fail other limits before reaching the ratio check.
        for L in [1,4000,1000000]:
            for tf,tr in [(0,0),(1,2),(3000000,2000000),(999999999,1)]:
                for q in [1,3,5,999983,1000000]:
                    ratio=Fraction(tf,tf+tr) if tf+tr else Fraction(0)
                    centre=int(ratio*q)
                    for p in sorted({max(0,centre-1),centre,min(q,centre+1),q}):
                        check([L,-1000000,1000000,1000,0,tf,tf,tr,tr,0,1000000000,0,1000000000,1000000000,1,p,q,0])
        for n in [1,8,128]:
            check([1000000,-1000000,1000000,1000,-999500,0,0,0,0,0,1000000000,0,1000000000,1000000000,1,1,1000000,n]+[1000000000,-500,500,1]*n)
        print(f'axle exact Fraction oracle: {count} state/boundary cases passed')

if __name__=='__main__':main()
