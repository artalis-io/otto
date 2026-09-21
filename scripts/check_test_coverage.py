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

It then asks the same question once per platform, because "something runs it"
and "Windows runs it" are different claims and only the first was ever
checked. The gap that prompted this: ralph's Benders, edge-case, optim,
regression and transport suites, and shared's pdf2struc suite, run on Linux
and nowhere else -- and the original check was satisfied, because it has no
concept of a runner at all.

Comparing test programs rather than target names is the whole trick. At the
level of names, 22 of 40 test targets look Linux-only; 8 of those are
top-level aliases (`make test-carta`) that Windows invokes as
`make -C carta test`, and several more are reached through a shell script
rather than a target. A name-level check would be mostly false positives and
would deserve to be switched off. Dry-running to the binaries resolves all of
those, because two spellings of the same work reach the same programs.

Method:
  1. read every `make` invocation out of .github/workflows/*.yml, along with
     the platform of the job that makes it
  2. build the module libraries -- `make -n` aborts at the first
     "No rule to make target" on an unbuilt tree and reports far less than the
     truth, which is a trap this check fell into while being written
  3. dry-run each CI target and collect the test binaries it would build or run
  4. compare that against the test programs on disk

Required platforms are Linux and Windows. macOS is reported but does not
fail the run: macOS Core is deliberately a thinner job -- seven module `test`
targets and the API servers built but not exercised -- and holding it to
parity would mean a long allowlist that mostly says "on purpose".

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

# Platforms a test program is expected to run on. macOS is collected and
# shown, but a gap there is not a failure -- see the note above.
REQUIRED = ('linux', 'windows')
ADVISORY = ('macos',)

# Programs that run on some platforms and deliberately not on others. Same
# rule as ALLOWLIST: the reason lives with the entry, and an entry that no
# longer applies fails the run.
PLATFORM_ALLOWLIST = {}

LF = chr(10)
MAKE = 'mingw32-make' if shutil.which('mingw32-make') else 'make'
QUIET = '--quiet' in sys.argv


def log(*a):
    if not QUIET:
        print(*a)


def platform_of(runs_on, matrix_os):
    """Normalise a runs-on value to linux / windows / macos."""
    r = runs_on.lower()
    if 'matrix.os' in r:
        return sorted({platform_of(o, []) for o in matrix_os} - {None})
    for key, name in (('ubuntu', 'linux'), ('windows', 'windows'),
                      ('macos', 'macos'), ('mac-', 'macos')):
        if key in r:
            return [name]
    return []


def ci_invocations():
    """{(directory, target): {platforms}} for everything CI invokes.

    Jobs are walked in order so each `make` line can be attributed to the
    runner of the job it sits in. Without that attribution the check cannot
    tell "nothing runs this" from "only Linux runs this", which is the
    distinction it exists for.
    """
    calls = {}
    for wf in sorted(glob.glob('.github/workflows/*.yml')):
        lines = open(wf, encoding='utf-8').read().split('\n')

        # Resolve `runs-on: ${{ matrix.os }}` from the job's own matrix.
        job_os = {}
        job = None
        for line in lines:
            m = re.match(r'^  ([A-Za-z0-9_-]+):\s*$', line)
            if m:
                job = m.group(1)
            m = re.match(r'^\s*os:\s*\[(.+)\]', line)
            if m and job:
                job_os[job] = [x.strip().strip('"\'') for x in m.group(1).split(',')]

        # Matrix entries that name a directory and a target, per job.
        #
        # A job whose step is
        #     make -C ${{ matrix.suite.dir }} ${{ matrix.suite.target }}
        # spells no target anywhere the scan below can read, so without this
        # every such job is invisible. That had really happened: nine module
        # suites were being sanitized and this file could see only the six api
        # ones, whose paths are written out literally.
        #
        # The test-c matrix is unaffected either way -- its entries carry the
        # whole command (`test: make -C ralph test`), which the scan reads as
        # ordinary text.
        job_matrix = {}
        job = None
        entry = {}
        for line in lines:
            m = re.match(r'^  ([A-Za-z0-9_-]+):\s*$', line)
            if m:
                job, entry = m.group(1), {}
                continue
            if re.match(r'^\s*-\s', line):
                entry = {}          # a new list item starts a new entry
            m = re.match(r'^\s*(?:-\s+)?(target|dir):\s*([A-Za-z0-9_./-]+)\s*$', line)
            if m and job:
                entry[m.group(1)] = m.group(2)
                if 'dir' in entry and 'target' in entry:
                    job_matrix.setdefault(job, []).append((entry['dir'], entry['target']))
                    entry = {}

        job = None
        plats = []
        for line in lines:
            m = re.match(r'^  ([A-Za-z0-9_-]+):\s*$', line)
            if m:
                job, plats = m.group(1), []
            m = re.match(r'^\s*runs-on:\s*(.+)$', line)
            if m:
                plats = platform_of(m.group(1).strip(), job_os.get(job, []))
            if not plats:
                continue

            # make -C ${{ matrix.X.dir }} ${{ matrix.X.target }}
            if re.search(r'\bmake\s+-C\s+\$\{\{\s*matrix\.[A-Za-z0-9_]+\.dir\s*\}\}'
                         r'\s+\$\{\{\s*matrix\.[A-Za-z0-9_]+\.target\s*\}\}', line):
                for _d, _t in job_matrix.get(job, []):
                    calls.setdefault((_d, _t), set()).update(plats)

            for mm in re.finditer(
                    r'\bmake\s+-C\s+([A-Za-z0-9_./-]+)((?:\s+[A-Za-z0-9_.\-]+)*)', line):
                for t in mm.group(2).split():
                    if '=' not in t and not t.startswith('-'):
                        calls.setdefault((mm.group(1), t), set()).update(plats)
            for mm in re.finditer(r'(?<![-\w])make((?:\s+[A-Za-z0-9_.\-]+)+)', line):
                for t in mm.group(1).split():
                    if '=' not in t and not t.startswith('-') and t != '-C':
                        calls.setdefault(('.', t), set()).update(plats)

            # Suites reached by a script rather than a target. Without this the
            # six API e2e suites look Linux-only, because Windows runs them as
            # `bash carta/api/test_api.sh` and nothing here was reading that.
            for mm in re.finditer(r'\bbash\s+([A-Za-z0-9_./-]+\.sh)', line):
                calls.setdefault(('script', mm.group(1)), set()).update(plats)
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

    calls = ci_invocations()
    log('CI invokes %d distinct (directory, target) pairs.' % len(calls))

    # program -> platforms that reach it. The union across every spelling is
    # the point: `make test-carta` on Linux and `make -C carta test` on
    # Windows are the same work, and dry-running both to their binaries is
    # what makes them comparable.
    where = {}
    for (d, t), plats in sorted(calls.items()):
        if d == 'script':
            if not os.path.isfile(t):
                continue
            try:
                out = open(t, encoding='utf-8', errors='replace').read()
            except OSError:
                continue
        else:
            if not os.path.isdir(d):
                continue
            try:
                r = subprocess.run([MAKE, '-C', d, '-n', t],
                                   capture_output=True, text=True, timeout=900)
            except Exception:
                continue
            out = (r.stdout or '') + (r.stderr or '')
        # Drop deletion lines before reading program names out of the recipe.
        # A target like `test-asan: clean test` dry-runs its clean first, and
        # clean's `rm -f` names every test binary in the module -- which would
        # otherwise read as 'CI runs all of them'. That is the opposite of what
        # this file is for, and it is how the check first reported a suite as
        # covered when the only thing touching it was its own removal.
        out = LF.join(l for l in out.split(LF)
                      if not re.match(r'^\s*(rm|del)\b', l))

        progs = set(re.findall(r'tests?/([A-Za-z0-9_]+)\.c', out))
        progs |= set(re.findall(
            r'(?:^|[\s/])(?:\./)?(test_[A-Za-z0-9_]+)(?:\.exe)?\b', out))
        for prog in progs:
            where.setdefault(prog, set()).update(plats)

    reached = set(where)

    print()
    print('%-10s %6s %10s %9s %8s' %
          ('module', 'tests', 'unreached', 'no-windows', 'no-macos'))
    print('-' * 92)

    failures = []
    gaps = []
    advisory = []
    stale = set(ALLOWLIST)
    stale_plat = set(PLATFORM_ALLOWLIST)
    for m in MODULES:
        if not os.path.isdir(m):
            continue
        progs = test_programs(m)
        bad, missing_win, missing_mac = [], [], []
        for prog in progs:
            key = '%s/%s' % (m, prog)
            plats = where.get(prog, set())
            if not plats:
                if key in ALLOWLIST:
                    stale.discard(key)
                else:
                    bad.append(prog)
                continue
            if key in PLATFORM_ALLOWLIST:
                stale_plat.discard(key)
                continue
            for want in REQUIRED:
                if want not in plats:
                    missing_win.append(prog)
                    gaps.append('%s: runs on %s, not %s'
                                % (key, '+'.join(sorted(plats)), want))
                    break
            for want in ADVISORY:
                if want not in plats:
                    missing_mac.append(prog)
                    advisory.append(key)
        failures += ['%s/%s' % (m, x) for x in bad]
        print('%-10s %6d %10d %9d %8d'
              % (m, len(progs), len(bad), len(missing_win), len(missing_mac)))

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

    if advisory:
        print()
        print('Not run on macOS (%d; advisory, does not fail the run):'
              % len(advisory))
        print('    %s' % ' '.join(sorted(advisory)[:12]))
        if len(advisory) > 12:
            print('    ... and %d more' % (len(advisory) - 12))

    if stale_plat:
        print()
        print('Stale PLATFORM_ALLOWLIST entries (the gap is closed, or the')
        print('file is gone):')
        for k in sorted(stale_plat):
            print('    %-44s %s' % (k, PLATFORM_ALLOWLIST[k]))

    if gaps:
        print()
        print('Run on some platforms and not others:')
        for g in sorted(gaps):
            print('    %s' % g)
        print()
        print('Add the suite to that platform\'s job, or list it in')
        print('PLATFORM_ALLOWLIST with the reason. "Something runs it" and')
        print('"Windows runs it" are different claims.')

    if failures or stale or gaps or stale_plat:
        print()
        print('FAIL: %d test programs nothing runs, %d with a platform gap, '
              '%d stale allowlist entries.'
              % (len(failures), len(gaps), len(stale) + len(stale_plat)))
        if failures:
            print()
            print('Wire each into a target CI invokes, or add it to ALLOWLIST in')
            print('this file with the reason. A test nobody runs is worse than no')
            print('test: it looks like coverage.')
        return 1

    print('OK: every test program is reached by a CI target on every required '
          'platform, or allowlisted with a reason.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
