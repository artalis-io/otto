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

## Deep pass: shared/ and the API servers

The mechanical sweep above said where to look next: `shared/` links into all six
servers, so a defect there has the widest reach. Of its 46k lines, 28k are
generated font tables, leaving ~18k of logic -- and the file that matters most
is `sh_json.c`, because all six servers parse request bodies with it.

### Findings

**`sh_json` accepted numbers it could not represent.** `strtod` saturates to
`HUGE_VAL` on overflow, so `1e999` -- or any integer past ~1.8e308, with no
exponent in sight -- parsed as a valid JSON number and came back as infinity
with `SH_JSON_OK`. Verified by harness before fixing: `{"x": 1e999}` yielded
`inf`, a 309-digit integer yielded `inf`.

An infinity is worse than a rejection because it survives the usual validation
shape. A range check written as two comparisons passes it in one direction, and
fails *both* for a NaN. The codebase already knew this where someone had been
bitten: `sh_parse_coord()` rejects "inf/NaN from malformed input like 1e1000"
and `lc_api.c` checks `isnan` with a comment explaining that "a NaN fails every
comparison". `parse_number` now rejects non-finite results, which closes the
class at the parser instead of at each consumer. Finite values are untouched,
`DBL_MAX` included.

**`ralph/src/ralph_api.c` cast a request body's double straight to `int`.**

    int parsed_timeout = (int)sh_json_as_double(sh_json_get(root, "timeout_ms"), 0.0);

`(int)` on a value outside `int`'s range is undefined, and `{"timeout_ms":
1e999}` is a POST body away. `sh_json_as_int()` exists and clamps; this bypassed
it. In practice the damage was bounded -- the result is range-checked on the next
line, and on x86 the cast yields something that fails `> 0` -- but undefined is
undefined. It uses `sh_json_as_int` now. Grepping the pattern found exactly one
other class of instance, all in nexus tests parsing their own output.

### Checked and sound

Worth recording so the next pass does not redo it:

- **Recursion depth.** `parse_value` -> `parse_array`/`parse_object` ->
  `parse_value` is bounded by `SH_JSON_MAX_DEPTH` (64), checked in both
  recursive functions. Deeply nested input cannot exhaust the stack. The
  unbalanced `p->depth--` on error paths does not matter: the parse aborts.
- **Two-pass string decoding.** `parse_string` counts the output length, then
  allocates, then writes. Divergence between passes would be a heap overflow, so
  each case was compared: both call the same `encode_utf8`, which cannot return
  0 for any reachable code point (a bare `\uXXXX` is at most 0xFFFF, a surrogate
  pair at most 0x10FFFF), and pass 1 rejects the control characters and invalid
  escapes that pass 2 does not re-check.
- **`parse_hex4`** bounds-checks against the input length before reading 4 digits.
- **`sh_json_as_int`** clamps at both ends, so an infinity cannot reach the cast;
  NaN would slip through the comparisons but cannot be produced by
  `parse_number`, which only accepts JSON number grammar.
- **Coordinate paths are hardened.** `sh_parse_coord` rejects non-finite values
  and range-checks lat/lon; locus checks `isnan` explicitly. velo and carta have
  no such checks in their handlers because they go through `sh_parse_coord`.
- **Request limits.** All six servers rate-limit. Keel bounds bodies at 1 MB and
  headers at 8 KB by default, which is what carta and locus rely on.

### sh_pdf2struc

The largest parser in shared and the least exercised. Two defects, both
reachable from a crafted file, both confirmed with a sanitizer rather than
argued from the code.

**PNG predictor: signed overflow on an attacker-chosen width.** `Columns` comes
straight from the stream's `DecodeParms` and was unbounded, so
`int stride = row_bytes + 1` overflowed for anything near `INT_MAX`. Confirmed
with clang's signed-integer-overflow sanitizer on a 477-byte file:

    sh_pdf2struc.c:688:28: runtime error: signed integer overflow:
    2147483647 + 1 cannot be represented in type 'int'

`columns` is now bounded by the decompressed length -- a row cannot be larger
than the data it describes, and `len` is capped at `PDF_MAX_DECOMPRESS`, which
keeps the stride arithmetic well inside `int`.

**xref stream: negative `/W` field widths.** `entry_size = w0 + w1 + w2` was
checked against (0, 20], but the three read loops each run `max(0, w)` times. A
negative width lowers the sum without lowering the bytes consumed: `/W [-5 10
10]` sums to 15, passes the bound, then reads 20 -- so `pos` advances five bytes
past what the guard verified, per entry. Each width is now rejected on its own.

The over-read lands inside the decompression buffer in the common case, because
that buffer is sized at ten times the compressed length, which is why ASan stays
quiet on an ordinary file. It is not bounded by anything structural: a payload
compressing at better than 10:1 puts `actual` against `decomp_cap` and the
overshoot leaves the allocation. Fixed rather than argued down.

**Neither the sanitizer job nor MSVC was looking at this file.**
`shared/test-asan` is `clean test`, and the `test` target's list of binaries did
not include `test_pdf2struc` -- so the parser with the largest untrusted attack surface in
shared/ was the one the sanitizer never ran. It is in the list now, which is
also what makes the predictor regression test worth keeping: without a
sanitizer that test cannot fail, since the overflow does not change the result
this compiler produces.

Adding it immediately turned the Windows MSVC job red, which is the other half
of the same gap. `$(TEST_PDF2STRUC_BIN)`'s link rule still spelled its inputs
`-L. -lsh_pdf2struc -lshared`, from before `mk/toolchain.mk` existed. `cl`
ignores those with a D9002 warning per flag and then fails with six unresolved
externals, so that suite had never built under MSVC -- it was only ever reached
by the Linux `test-pdf2struc` job. It uses `$(call link_lib,...)` like every
other test binary in the file now, and passes 30/30 under both toolchains.

### The remaining shared parsers, and what watches them

Before reading any of them, the question of what already watches these files:

| parser | lines | unit test | ASan/UBSan | fuzzed |
|---|---|---|---|---|
| sh_json | 1199 | yes | yes | no |
| sh_pdf2struc | 1487 | yes | yes | no |
| sh_csv | 568 | yes | yes | no |
| sh_xml | 428 | yes | yes | no |
| **sh_inflate** | 124 | **none** | **none** | no |
| **sh_protobuf** | 264 | **none** | **none** | no |

OTTO does have fuzzers -- `carta/tests/fuzz/fuzz_pbf.c`,
`nexus/tests/fuzz/{fuzz_csv,fuzz_pdf,fuzz_xlsx}.c`, `surge/fuzz/fuzz_json_api.c`
-- with `fuzz` targets in those three Makefiles. **No CI job runs any of them**,
and none targets a shared primitive directly: they drive the module wrappers
(`nx_csv_parse`, `ct_pbf_*`) rather than `sh_csv` or `sh_protobuf`. `shared/`
has no fuzz target at all.

**sh_protobuf: the length check could wrap.** `sh_pb_skip_field` validated a
length-delimited field with `(size_t)(n + field_len) > len`, which is unsigned
64-bit addition: a length near 2^64 wraps to something small and passes. The
first reading of this was that it failed closed, because `(int)field_len` would
then come out negative -- a test written to that assumption failed against the
unfixed source, which is the more useful answer. The check now compares against
the space that actually remains and bounds the return to `INT_MAX`.

**sh_inflate: a byte count stored in a status enum.**
`tinfl_decompress_mem_to_mem` returns the number of bytes written, or
`(size_t)-1`. That was assigned to a `tinfl_status` and tested as
`status == TINFL_STATUS_DONE || (int)status >= 0`, which works only because a
count is non-negative and the failure value is -1. It now compares against
`TINFL_DECOMPRESS_MEM_TO_MEM_FAILED`. The `size_t` to `mz_uint32` narrowing of
the buffer lengths is also rejected rather than silently truncated -- a
truncation there describes a smaller buffer than the caller passed, so it
reports a short result as success rather than corrupting memory.

Both files now have tests -- 19 and 13 -- and are in the `test` target, so
`test-asan` covers them.

**sh_csv and sh_xml were read and are sound.** The CSV quoted-field decoder
uses the same two-pass count-then-write shape as sh_json, where divergence
would be a heap overflow; the two loops are structurally identical, so the
write cannot outrun the count. An over-long field returns NULL without
advancing the read position, but the caller sets `eof` rather than retrying, so
it cannot spin. XML entity decoding is in-place and every entity shrinks
(`&quot;` to one byte), so the write index can never pass the read index.

One contract gap rather than a defect: `sh_xml_decode_entities` is public and
writes a NUL at the decoded length, which equals `len` when the input has no
entities -- so it needs `len + 1` bytes, and the header only said "must be
writable". The internal caller allocates `len + 1`; the header now says so.

## What this sweep did not cover

Everything semantic. Use-after-free, double-free, leak tracing, null-deref
paths, resource lifetimes and the Keel hardening checklist are per-module work
and were not attempted. `sh_json.c` has now had the deep pass described above, and
the six servers' request paths were traced for numeric validation. `sh_pdf2struc` has now had the pass
described above, though it was targeted at the highest-risk routines -- the
predictor, the xref stream decoders, the decompression sizing -- rather than
read end to end; its text-extraction half (1.7k lines) was not examined at all.
`sh_csv`, `sh_xml`, `sh_inflate` and `sh_protobuf`
have had the pass described above. What remains unread is
`sh_pdf2struc_text.c` -- 1.7k lines of text extraction and table assembly,
reached only after a document has already parsed -- and the question of whether
any of these parsers deserves a fuzz target, given that the ones OTTO has are
run by nothing.
