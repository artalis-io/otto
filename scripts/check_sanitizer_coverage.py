#!/usr/bin/env python3
"""
Every test program CI runs must be run by something built with a sanitizer,
or be listed here with a reason.

The companion check, check_test_coverage.py, asks whether anything runs a
test at all. This asks the next question, which turned out to be a different
one: is it ever run with the instrumentation that finds the bugs tests do not
assert about.

Both answers were "yes" for Ralph's solver core, and it still leaked on every
re-solve. 80,000 lines of simplex, LU, branch-and-bound, cuts and Benders had
full suite coverage, ran on three platforms, and had never been compiled with
AddressSanitizer by anything. The CI matrix ran Ralph *Transport* -- the HTTP
layer -- and the name was close enough that it read as covered. When the core
was finally built with sanitizers it produced five leaks, a use-after-free in
a neighbouring module, and a UB memcpy older than all of them.

The near-misses are what make this worth gating rather than auditing:

  - `make -C <mod>/api test-asan` runs for six modules and looks like module
    coverage. It is not: the api Makefiles link libralph and libshared as
    already built, so only main.c and the HTTP helpers are instrumented. The
    module interiors went in prebuilt.
  - Windows cannot substitute. LeakSanitizer is Linux-only and MSVC has
    neither LSan nor UBSan, so every Windows sanitizer job is structurally
    incapable of reporting a leak.
  - A module can define DEBUG_CFLAGS with -fsanitize, and have nothing that
    uses them to run the suite. Five modules were in exactly that state.

So the question is asked against ground truth rather than names: dry-run each
target CI invokes, look at the compile lines it would actually issue, and ask
whether they carry -fsanitize. A target called `test-asan` that quietly stopped
passing the flag would pass a name check and fail this one.

Method, sharing check_test_coverage's parser so the two cannot drift:
  1. read every make invocation out of .github/workflows/*.yml
  2. dry-run each one; record the test programs it reaches, and whether its
     compile lines carry a sanitizer flag
  3. a program is covered if at least one *sanitized* invocation reaches it
  4. programs nothing runs at all are not reported here -- that is the other
     check's failure, and reporting it twice would only obscure which gate to
     fix

Exclusions are not forbidden; they have to be written down. Usage:

    python3 scripts/check_sanitizer_coverage.py [--quiet]
"""
import os
import re
import sys
import subprocess

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_test_coverage as ctc   # noqa: E402  (also chdirs to the repo root)

LF = chr(10)
QUIET = '--quiet' in sys.argv

# -fsanitize=... on GNU/Clang, /fsanitize=address on MSVC.
SANITIZE = re.compile(r'[-/]fsanitize[=\s]')

# Test programs CI runs but deliberately never sanitizes. Keep the reason with
# the entry; an entry that no longer applies fails the run, the same rule the
# companion check uses.
ALLOWLIST = {}


def log(*a):
    if not QUIET:
        print(*a)


def dry_run(d, t):
    """(text of the recipe, whether it compiles with a sanitizer)."""
    try:
        r = subprocess.run([ctc.MAKE, '-C', d, '-n', t],
                           capture_output=True, text=True, timeout=900)
    except Exception:
        return '', False
    out = (r.stdout or '') + (r.stderr or '')
    # Deletion lines name every binary in the module; see the companion check.
    out = LF.join(l for l in out.split(LF) if not re.match(r'^\s*(rm|del)\b', l))
    return out, bool(SANITIZE.search(out))


def programs_in(out):
    progs = set(re.findall(r'tests?/([A-Za-z0-9_]+)\.c', out))
    progs |= set(re.findall(
        r'(?:^|[\s/])(?:\./)?(test_[A-Za-z0-9_]+)(?:\.exe)?\b', out))
    return progs


def main():
    log('Building module libraries (a dry run on an unbuilt tree reports '
        'nothing useful)...')
    for m in ctc.MODULES:
        if os.path.isdir(m):
            subprocess.run([ctc.MAKE, '-C', m, 'lib'],
                           capture_output=True, timeout=900)

    calls = ctc.ci_invocations()
    log('CI invokes %d distinct (directory, target) pairs.' % len(calls))

    reached = set()          # programs any CI target reaches
    sanitized = set()        # programs a sanitized CI target reaches
    san_targets = []
    for (d, t) in sorted(calls):
        if d == 'script' or not os.path.isdir(d):
            continue
        out, is_san = dry_run(d, t)
        progs = programs_in(out)
        reached |= progs
        if is_san:
            san_targets.append('%s %s' % (d, t))
            sanitized |= progs

    log('%d of them build with a sanitizer: %s'
        % (len(san_targets), ', '.join(san_targets)))

    print()
    print('%-10s %6s %9s %9s' % ('module', 'tests', 'sanitized', 'gap'))
    print('-' * 92)

    gaps = []
    stale = set(ALLOWLIST)
    for m in ctc.MODULES:
        if not os.path.isdir(m):
            continue
        progs = ctc.test_programs(m)
        covered, missing = 0, []
        for prog in progs:
            key = '%s/%s' % (m, prog)
            if prog not in reached:
                continue          # the companion check's business, not ours
            if prog in sanitized:
                covered += 1
            elif key in ALLOWLIST:
                stale.discard(key)
            else:
                missing.append(prog)
                gaps.append(key)
        print('%-10s %6d %9d %9d' % (m, len(progs), covered, len(missing)))
        if missing and not QUIET:
            print('    ' + ' '.join(sorted(missing)))
    print('-' * 92)

    if gaps:
        print()
        print('Run by CI, never under a sanitizer (%d):' % len(gaps))
        print('    ' + ' '.join(sorted(gaps)))
        print()
        print('Add the suite to a sanitizer target the CI matrix runs, or list')
        print('it in ALLOWLIST with the reason. A suite that only ever runs')
        print('uninstrumented reports the bugs it asserts about and no others.')

    if stale:
        print()
        print('Stale ALLOWLIST entries (%d) -- these are sanitized now, so the'
              % len(stale))
        print('exemption is obsolete; delete it:')
        print('    ' + ' '.join(sorted(stale)))

    print()
    if gaps or stale:
        print('FAIL: %d test programs never sanitized, %d stale allowlist entries.'
              % (len(gaps), len(stale)))
        return 1
    print('OK: every test program CI runs is also run under a sanitizer, or is '
          'allowlisted with a reason.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
