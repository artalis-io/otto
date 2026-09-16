# C Audit -- codebase sweep

`/c-audit`'s mechanical checks run across all ten C modules (~437k lines,
vendor excluded). Recorded because the interesting part is how few of the raw
hits survived checking: the counts a grep produces and the findings that stand
up are different numbers, and repeating the sweep is only cheap if the next
person knows which is which.

## Method

The skill is module-scoped (`/c-audit <module>`) and its deep categories --
use-after-free, double-free, leak tracing, resource lifetimes, the Keel API
hardening list -- need per-module review. What ran here is the pattern set:
banned functions, unsafe parsing, allocation overflow shape, security-model
compliance, and mutable file-scope state. Benchmarks, tests and `vendor/` are
excluded from the library counts, which is what separates a real hit from noise.

## Result

| Category | Raw hits | Stood up | Notes |
|---|---|---|---|
| `system`/`popen`/`exec*` | 6 | 0 | five are benchmark harnesses shelling to glpsol/OSRM |
| `dlopen` | 0 | 0 | |
| `strcpy`/`strcat`/`sprintf`/`gets`/`alloca` | 13 | **0** | all in benchmarks or the vendored netlib `emps.c` |
| `atoi`/`atol`/`atof` | 31 | **0** | the one `src/` hit is a comment explaining why `atof` is not used |
| `malloc(a * b)` shape | 389 | 1 | 386 are the `n * sizeof(T)` idiom; 3 multiply two variables, 2 of those are guarded |
| Mutable file-scope state | 85 | 0 | `shared/` has 11; the server-path ones are a log mutex and terminal state |

**Zero banned string functions and zero unsafe integer parsing in library
code.** That is the headline, and it is worth writing down because the raw
counts (13 and 31) suggest the opposite.

### The one finding

`ensure_scratch_capacity()` in `carta/src/ct_multipolygon.c` lacked two guards
that `sh_pool_ensure_capacity()` and `sh_pqueue_push()` both apply before the
identical `realloc`:

- the doubling loop `while (new_cap < needed) new_cap *= 2;` had no overflow
  guard, and a wrap to 0 never reaches `needed`, so it would not terminate
- the product `new_cap * elem_size` was not checked against `SIZE_MAX`

Severity is lower than the shape suggests and the entry should say so: `needed`
is `num_outer + 1`, incremented once per relation member, so reaching either
condition wants a member count near `SIZE_MAX` and memory exhaustion gets there
first. It is a consistency gap against the codebase's own standard rather than
a reachable overflow. Fixed.

### Not findings, checked rather than assumed

- **`execvp` in `ralph/src/lp_external_oop.c`** is library code that spawns a
  process, which the skill flags. It uses the argv-array form, so there is no
  shell and no injection path, and it sits behind the opt-in
  `RALPH_LP_EXTERNAL_PROVIDER_*` selection. Deliberate architecture.
- **carta and locus set no `max_body_size`.** Keel's documented defaults are
  bounded -- 1 MB body, 8 KB headers -- so they are covered; the other four
  servers set explicit values. All six rate-limit.

### Housekeeping

`ralph/benchmarks/.emps/emps.c` was tracked and byte-identical to
`ralph/benchmarks/emps.c`. It is not a duplicate source file: `.emps/` is a
cache `download_netlib.sh` curls `emps.c` into from netlib.org and builds
there, so the tracked copy was being overwritten on every run. The directory is
now ignored, and the script builds the vendored `emps.c` instead of downloading
it -- which also takes netlib.org off the critical path for building the NETLIB
corpus. The vendored file was byte-identical to what that host serves, so the
binary is unchanged.

## What this sweep did not cover

Everything semantic. Use-after-free, double-free, leak tracing, null-deref
paths, resource lifetimes and the Keel hardening checklist are per-module work
and were not attempted. If they are picked up, `shared/` is the place to start
-- it links into all six servers, so anything there has the widest blast radius
-- followed by the six `api/` servers, which are the only code that touches
untrusted input.
