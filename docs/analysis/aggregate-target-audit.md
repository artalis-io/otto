# Aggregate target audit

An audit of every Makefile target that claims to cover a set of things, asking
in each case whether it still covers that set.

The question came out of a run of defects that all shared one shape: a target
whose membership had drifted from what its name said, with the uncovered part
quietly broken.

| where | what was missing | what it hid |
|---|---|---|
| `make test` | `test_lp` | `ralph_lp_write_mps()` emitting invalid MPS |
| `make wasm` | 3 modules | three modules that no longer compiled to wasm |
| `shared/test` | `test_pdf2struc` | a broken MSVC link, and the PDF parser excluded from every sanitizer run |
| `.PHONY` | 15 targets | nothing yet |
| fuzz harnesses | build rules that worked | five harnesses that could not run at all |

In every case the suite reported green. What a target does not run, it also
does not count.

## Method

Four checks, each mechanical.

1. **Test reachability.** Every `make` invocation in `.github/workflows/*.yml`
   was extracted, dry-run, and the test binaries it would build or run
   collected. That union was compared against every `.c` file under `tests/`
   with a `main()`.
2. **Clean completeness.** Snapshot the filesystem, build everything, `make
   clean`, snapshot again. Anything left is something `clean` does not know
   about — the mechanism behind a stale object being linked into a later build.
3. **`.PHONY` completeness.** Every target that names no file, checked against
   the `.PHONY` declarations.
4. **Top-level aggregates.** `all`, `test`, `wasm`, `clean`, `api`, `test-api`
   against the modules that exist on disk.

### One methodological trap, recorded because it nearly invalidated the whole audit

`make -n` **aborts at the first "No rule to make target"** on a tree that has
not been built. On a clean checkout the dry run stops early and reports a small
fraction of what the target actually does. The first pass of this audit
produced 58 "unrun" tests, then 19, then 13, then 10 — each number an artefact
of a different extraction bug, not a finding. Three separate claims had to be
retracted after checking them by hand.

Build first, then dry-run. The checked-in script does this and says why.

## Findings

### 1. Three ralph test suites that nothing ran — fixed

| suite | size | assertions |
|---|---|---|
| `test_refueling` | 3406 lines | 105 |
| `test_benders_warmstart` | 124 lines | 17 |
| `test_benders_master_start` | 119 lines | 13 |

All three build and pass, and did so before this audit — they were simply never
invoked. **135 assertions**, about 100ms for all three.

`test_refueling` was not mentioned anywhere in `ralph/Makefile`, in any target.
The other two are the more interesting case: both already had `_SOURCES` and
`_EXEC` variables *and* build rules, and both were already listed in `clean`.
Only the line that runs them was missing. Everything about the Makefile looked
like they were wired up.

### 2. 41 undeclared phony targets across 17 Makefiles — fixed

Mostly `help`. `ralph/Makefile` accounted for 15, including `test-lp` — the
target whose omission from `make test` started this whole line of enquiry.

An undeclared phony target is skipped the moment a file or directory of that
name appears. `make test` in a tree containing a directory called `test` does
nothing, reports success, and is indistinguishable from a passing run.

### 3. Six test programs legitimately not run — allowlisted

Not defects, but now written down rather than merely absent:

| program | why |
|---|---|
| `ralph/test_phase1_oracle_glpk` | needs GLPK; the gate that uses it is the NETLIB nightly |
| `ralph/test_presolve_beaconfd` | diagnostic for a known numerical issue; prints, does not assert |
| `ralph/test_beaconfd_phase1_trace` | trace dump, not an assertion suite |
| `ralph/test_benders_debug` | scratch harness for a hand-built 2-station problem |
| `velo/test_osrm_comparison` | needs a running OSRM server |
| `locus/test_pbf_parse` | needs a real `.osm.pbf`, which the repo does not carry |

A further 30 `debug_*`, `quick_*` and `bench_*` programs in ralph are scratch
tools rather than tests and are excluded by prefix.

## Clean bills of health

Recorded because a negative result is worth as much as a positive one here, and
because the next person to wonder should not have to re-derive them.

- **`clean` is complete in all nine modules.** 367 artefacts created across a
  full build and test run; zero left behind.
- **Top-level `all`, `test`, `wasm` and `clean` cover every module** that has a
  Makefile, a test directory, or a `wasm/` directory respectively. The `wasm`
  gap fixed earlier has held.
- **`api` and `test-api` cover all six servers.** An earlier note that
  `test-api` covered three of six is out of date; it was fixed in between.
- **shared, carta, nexus, surge, arbor and fuelwise have no unreached tests.**
  The gaps were confined to ralph, plus one each in velo and locus, both
  legitimate.

## The check that replaces this audit

`scripts/check_test_coverage.py`, run by the `Every Test Is Run` CI job.

It asks the real question — "does any target CI invokes reach this file" —
rather than the easier "is it in `make test`". Sixteen tests here are run by
their own target and their own CI job; a check demanding membership of `make
test` would flag all sixteen wrongly, and a check that is wrong sixteen times
gets ignored.

Exclusions stay possible, but they have to be written down with a reason. The
script also flags **stale** allowlist entries — an entry for a test that is now
reached, or whose file is gone, would silence a future regression. That check
earned itself immediately: it caught two entries in the first draft of its own
allowlist that were not needed.

Verified in both directions: it passes on the tree as it stands, and
unwiring `test_refueling` again makes it fail with exit 1.

## Not covered

- `clayshards/`, `site/` and the six `api/` Makefiles were checked for
  `.PHONY` only, not for test reachability. They have no `tests/` directories
  of the shape this audit looked for; the api suites are shell scripts driven
  from CI, which is a different check.
- Whether the tests that *are* run assert anything useful. This audit only
  establishes that they run. Three of the four defects that motivated it were
  found by reading the code, not by the suite.
- wasm targets beyond membership: that each module's wasm build still produces
  a loadable module is checked by `Build WASM`, not here.
