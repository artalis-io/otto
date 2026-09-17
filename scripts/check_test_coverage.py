#!/usr/bin/env python3
"""
Every test program must be reached by something CI runs, or be listed here
with a reason.

This exists because the same defect kept recurring: a test file in the tree,
compiling, passing, and named by no target that anything invokes -- so nothing
ran it. It hid an invalid MPS writer (`make test` without test_lp), a broken
MSVC link and an unsanitised PDF parser (`shared/test` without test_pdf2struc),
and 135 passing assertions in ralph (test_refueling, test_benders_master_start,
test_benders_warmstart). Each time the suite reported all green, because what
it did not run it also did not count.

The question this asks is deliberately the real one -- "does any target CI
invokes reach this file" -- not the easier "is it in `make test`". Plenty of
tests here are run by their own target and their own CI job, and a check that
demanded membership of `make test` would flag sixteen of them wrongly.

Method:
  1. read every `make` invocation out of .github/workflows/*.yml
  2. build the module libraries -- `make -n` aborts at the first
     "No rule to make target" on an unbuilt tree and reports far less than the
     truth, which is a trap this check fell into while being written
  3. dry-run each CI target and collect the test binaries it would build or run
  4. compare that against the test programs on disk

Exclusions are not forbidden; they have to be written down. Usage:

    python3 scripts/check_test_coverage.py [--quiet]
"""
import os
import re
import sys
import glob
import shutil
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

MODULES = ['shared', 'ralph', 'arbor', 'velo', 'carta', 'locus',
           'fuelwise', 'nexus', 'surge']

# Tools and scratch programs, not tests.
SCRATCH = re.compile(r'^(debug_|quick_|bench_|lp_only_test$|compare_glpk$|font_compare$)')

# Deliberately not run in CI. Keep the reason with the entry.
ALLOWLIST = {
    'ralph/test_phase1_oracle_glpk':
        'needs GLPK; the gate that uses it is the NETLIB nightly',
    'ralph/test_presolve_beaconfd':
        'diagnostic for a known numerical issue; prints, does not assert',
    'ralph/test_beaconfd_phase1_trace':
        'trace dump for beaconfd, not an assertion suite',
    'ralph/test_benders_debug':
        'scratch harness that builds a 2-station Benders problem by hand',
    'velo/test_osrm_comparison':
        'needs a running OSRM server to compare against',
    'locus/test_pbf_parse':
        'needs a real .osm.pbf, which the repo does not carry',
}

MAKE = 'mingw32-make' if shutil.which('mingw32-make') else 'make'
QUIET = '--quiet' in sys.argv


def log(*a):
    if not QUIET:
        print(*a)


def ci_make_calls():
    """(directory, target) pairs that CI invokes."""
    calls = set()
    for wf in glob.glob('.github/workflows/*.yml'):
        txt = open(wf, encoding='utf-8').read()
        for m in re.finditer(r'\bmake\s+-C\s+([A-Za-z0-9_./-]+)((?:\s+[A-Za-z0-9_.\-]+)*)', txt):
            for t in m.group(2).split():
                if '=' not in t and not t.startswith('-'):
                    calls.add((m.group(1), t))
        for m in re.finditer(r'(?<![-\w])make((?:\s+[A-Za-z0-9_.\-]+)+)', txt):
            for t in m.group(1).split():
                if '=' not in t and not t.startswith('-') and t != '-C':
                    calls.add(('.', t))
    return calls


def test_programs(mod):
    out = []
    for sub in ('tests', 'test'):
        d = os.path.join(mod, sub)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if not name.endswith('.c'):
                continue
            body = open(os.path.join(d, name), encoding='utf-8', errors='replace').read()
            if re.search(r'^\s*int\s+main\s*\(', body, re.M):
                stem = os.path.splitext(name)[0]
                if not SCRATCH.match(stem):
                    out.append(stem)
    return out


def main():
    log('Building module libraries (a dry run on an unbuilt tree reports nothing useful)...')
    for m in MODULES:
        if os.path.isdir(m):
            subprocess.run([MAKE, '-C', m, 'lib'], capture_output=True, timeout=900)

    calls = ci_make_calls()
    log('CI invokes %d distinct (directory, target) pairs.' % len(calls))

    reached = set()
    for d, t in sorted(calls):
        if not os.path.isdir(d):
            continue
        try:
            r = subprocess.run([MAKE, '-C', d, '-n', t],
                               capture_output=True, text=True, timeout=900)
        except Exception:
            continue
        out = (r.stdout or '') + (r.stderr or '')
        reached.update(re.findall(r'tests?/([A-Za-z0-9_]+)\.c', out))
        reached.update(re.findall(r'(?:^|[\s/])(?:\./)?(test_[A-Za-z0-9_]+)(?:\.exe)?\b', out))

    print()
    print('%-10s %7s %9s  %s' % ('module', 'tests', 'unreached', 'reached by no CI target'))
    print('-' * 92)

    failures = []
    stale = set(ALLOWLIST)
    for m in MODULES:
        if not os.path.isdir(m):
            continue
        progs = test_programs(m)
        bad = []
        for p in progs:
            key = '%s/%s' % (m, p)
            if p in reached:
                continue
            if key in ALLOWLIST:
                stale.discard(key)
                continue
            bad.append(p)
        failures += ['%s/%s' % (m, p) for p in bad]
        print('%-10s %7d %9d  %s' % (m, len(progs), len(bad), ' '.join(bad) or '-'))

    print('-' * 92)

    # An allowlist entry for a test that IS reached, or no longer exists, is
    # stale -- it would silence a future regression without anyone noticing.
    stale = {k for k in stale
             if not os.path.exists(os.path.join(*k.split('/', 1)[0:1], 'tests',
                                                k.split('/', 1)[1] + '.c'))
             or k.split('/', 1)[1] in reached}
    if stale:
        print()
        print('Stale allowlist entries (reached by CI, or the file is gone):')
        for k in sorted(stale):
            print('    %s' % k)
        print('Remove them, so the allowlist keeps meaning what it says.')

    if failures or stale:
        print()
        print('FAIL: %d test programs nothing runs, %d stale allowlist entries.'
              % (len(failures), len(stale)))
        if failures:
            print()
            print('Wire each into a target CI invokes, or add it to ALLOWLIST in')
            print('this file with the reason. A test nobody runs is worse than no')
            print('test: it looks like coverage.')
        return 1

    print('OK: every test program is reached by a CI target, or allowlisted with a reason.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
