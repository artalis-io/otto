# TRE — vendored POSIX regular expressions

The regex engine Nexus compiles validation rules and transform patterns with,
on **every** platform.

## Why this is here

Two reasons, and the second is the important one.

**Windows has no `<regex.h>`.** It is a POSIX header; MSVC and the Windows SDK
do not ship it. `nx_validate.c` and `nx_xform.c` both need it, so before this
existed Nexus did not build on Windows at all.

**A rule has to mean one thing.** Validation patterns and transform match rules
come from customer-authored config, not from our source tree. If the engine
behind them changed with the host — glibc on the Linux workers, Apple's on a
developer's laptop, something else in a container — then the same rule could
accept a load on one machine and reject it on another, and the pipeline's
verdict would depend on where it ran. Engines really do differ at the edges:
leftmost-longest handling in alternation, what a capture group holds after a
repeat matched empty, how bounded repeats interact with backtracking.

So this engine is used unconditionally, not as a Windows fallback. The build
does not consult the platform. `nexus/tests/test_regex_differential.c` runs it
against the system engine wherever there is one and asserts they agree, down to
capture offsets — which is what turns "should be the same" into something CI
checks.

## Provenance

| | |
|---|---|
| Upstream | [musl libc](https://musl.libc.org/) — `src/regex/` |
| Original author | Ville Laurikari, as part of [TRE](https://laurikari.net/tre/) |
| Licence | 2-clause BSD (TRE), MIT (musl's own contributions). See `LICENSE`. |
| Fetched | 2026-09-10, from musl's then-current tree |
| Files taken | `regcomp.c`, `regexec.c`, `tre-mem.c`, `tre.h` |

`regerror.c` was **not** taken. musl's version includes `locale_impl.h` to run
each message through musl's message catalogue, and that is the only place in the
whole engine that reaches into libc internals. Ours is a plain table of the same
English strings — see below.

TRE is a full-featured engine upstream, with approximate matching and wide-char
APIs. musl carries the subset that implements POSIX `regcomp`/`regexec`, which
is all Nexus wants, and that subset is what is here.

## Layout

| File | What it is |
|---|---|
| `tre_regex.h` | **The public header.** The only one callers include. |
| `tre_posix_compat.h` | Internal. Maps upstream's POSIX spellings onto our prefixed ones. |
| `tre.h` | Upstream internal header, plus the shims listed below. |
| `regcomp.c`, `regexec.c`, `tre-mem.c` | Upstream, near-untouched. |
| `regerror.c` | Ours. Replaces musl's locale-dependent one. |

## Namespacing

Everything exported is prefixed: `tre_regcomp`, `tre_regexec`, `tre_regfree`,
`tre_regerror`, `tre_regex_t`, `tre_regmatch_t`, `TRE_REG_EXTENDED` and so on.

This is not tidiness. If the engine exported the bare POSIX names, then on Linux
and macOS *which engine you actually got* would depend on include order and link
order — and it would resolve silently, differently in different translation
units. Prefixing makes that impossible to get wrong.

Upstream's `.c` files still say `regcomp`, `regex_t` and `REG_EXTENDED`
throughout. Rewriting thousands of lines would make every future upstream diff
unreadable, so `tre_posix_compat.h` maps one spelling onto the other and the
engine bodies stay comparable with musl's. The renames live in that header
rather than in build flags on purpose: compiled without them, the engine would
define the bare POSIX symbols and displace the platform's regex at link time.

## Local changes

Kept deliberately small, so re-vendoring is a re-apply rather than a merge.

**`tre.h`**

- Shims for three names musl gets from its own internal headers: `hidden` (a
  visibility annotation, needs no definition standalone), `CHARCLASS_NAME_MAX`
  and `RE_DUP_MAX` (POSIX `<limits.h>` values that only musl and glibc define).
- `#include <regex.h>` → `#include "tre_posix_compat.h"`, so the platform's
  header can never be picked up instead.
- `TRE_REGEX_T_FIELD` renamed from `__opaque` to `opaque` — a leading double
  underscore is reserved for the implementation, and we are not one.
- **`ALIGN` casts through `uintptr_t` instead of `long`.** On Windows LLP64
  `long` is 32 bits, so the original truncated the pointer.

**`regexec.c`, `tre-mem.c`**

- **`ALIGN(p, long)` → `ALIGN(p, void *)`** at all five call sites. This is a
  real bug fix, not a portability shuffle: the macro aligns a scratch buffer that
  is then carved into structures holding pointers, and `sizeof(long)` is 4 on
  Win64 against 8 on LP64. Upstream got 8-byte alignment on every platform musl
  supports and 4 on ours. `void *` reproduces musl's behaviour exactly wherever
  musl runs, and fixes it here.

**`regcomp.c`, `regexec.c`**

- `restrict` → `TRE_RESTRICT`. MSVC has `__restrict` but rejects C99 `restrict`,
  and rejects it especially in the array-parameter form `T p[restrict]`. It is an
  aliasing hint, so it costs optimisation and nothing else.
- The `<regex.h>` include, as above.

**`tre_regex.h` (ours)**

- `tre_regoff_t` is `ptrdiff_t`, not `long`. Same LLP64 reason: an offset into a
  string should be pointer-sized, and `long` would cap it at 2 GB on Windows.
- `tre_regex_t` drops musl's padding. musl pads its `regex_t` out to glibc's
  layout for ABI compatibility; nothing here is ABI-compatible with anything by
  design, and the engine only ever touches `re_nsub` and the opaque pointer.

## Extensions beyond POSIX, and the one that changed behaviour

TRE and GNU both add escapes POSIX does not define, and they mostly agree:
`\w`, `\s`, `\S`, `\b` and `\<` behave identically in both.

`\d` and `\D` do not. **TRE has them; GNU does not.** Under glibc, `\d` is
just a literal `d`.

That matters because `nexus/CLAUDE.md` documents a transform pattern
`"^(\d{4}) (.+)$"` and means four digits by it. Under the platform engine on
Linux that pattern matched four literal `d` characters, so the documented example
never did what it said. Vendoring makes the documented meaning the real one, on
every platform.

It is a behaviour change on Linux, in the direction of the documentation, and
`test_regex_differential.c` pins it. It is *not* in the differential corpus:
comparing `\d` against the system engine would only assert that the two differ,
which they do and always will.

## Updating

1. Take fresh `regcomp.c`, `regexec.c`, `tre-mem.c`, `tre.h` from musl.
2. Re-apply the list above. It is short and every item says why.
3. Leave `regerror.c`, `tre_regex.h` and `tre_posix_compat.h` alone — they are
   ours, not upstream's.
4. Run `make -C nexus test`. `test_regex_differential` is the one that matters:
   on Linux and macOS it compares this engine against the platform's over a
   corpus of patterns and inputs and asserts identical match results *and*
   identical capture offsets.

## Using it

```c
#include "tre_regex.h"

tre_regex_t re;
tre_regmatch_t m[3];

if (tre_regcomp(&re, "^([0-9]{4}) (.+)$", TRE_REG_EXTENDED) != TRE_REG_OK)
    return -1;                       /* do NOT tre_regfree() a failed compile */

if (tre_regexec(&re, line, 3, m, 0) == TRE_REG_OK) {
    /* m[0] is the whole match; m[n] is group n.
       A group that did not participate reports rm_so == -1. */
    size_t len = (size_t)(m[1].rm_eo - m[1].rm_so);
    memcpy(postcode, line + m[1].rm_so, len);
}

tre_regfree(&re);
```

Compile with `TRE_REG_NOSUB` when you only need match/no-match, as
`nx_validate.c` does — it skips the capture bookkeeping entirely.

## Gotchas

- **Do not call `tre_regfree()` on a failed `tre_regcomp()`.** Nothing was
  allocated, and the struct is uninitialised.
- **These are byte offsets, not character offsets.** UTF-8 input works, but
  `rm_so`/`rm_eo` land on bytes; slicing mid-sequence gives invalid UTF-8.
- **The engine is built without `-Werror`**, like miniz. Upstream's warnings are
  upstream's, and `nexus/Makefile` compiles it through `$(CC_SYSINC)` so they do
  not count against Nexus's own warning bar.
- **`RE_DUP_MAX` is 255 here.** A pattern such as `x{1,300}` is rejected with
  `TRE_REG_BADBR`, which is POSIX-conforming but stricter than some engines.
