# Transport Interface + Platform Abstraction Layer

**Status:** Phases 1-5 done. All six modules hold their handler in their library.
**Scope:** `shared/` transport interface, per-module API types, `sh_pal.h`

## Motivation

Every OTTO module already exposes the same handler shape:

```c
int ralph_api_handle(RalphAPIContext*, const RalphAPIRequest*, RalphAPIResponse*);
int ct_api_handle   (CTAPIContext*,    const CTAPIRequest*,    CTAPIResponse*);
int vl_api_handle   (VLAPIContext*,    const VLAPIRequest*,    VLAPIResponse*);
int lc_api_handle   (LCAPIContext*,    const LCAPIRequest*,    LCAPIResponse*);
int fw_api_handle   (FWAPIContext*,    const FWAPIRequest*,    FWAPIResponse*);
int sg_api_handle   (SGAPIContext*,    const SGAPIRequest*,    SGAPIResponse*);
```

The six `Response` structs are identical — `{int status_code; const char *content_type;
body; size_t body_len;}` — differing only in `uint8_t*` vs `char*`. The six `Request`
structs are each a subset of one six-field union: `{method, path, query, body, body_len,
host}`.

The abstraction is not missing. It is duplicated six times, and validated by six
independent implementations. This work deletes five copies rather than inventing
something new.

The migration to Keel v3 confirmed the cost of not having it: the expensive part
was not swapping HTTP libraries, it was that each server hand-rolled the same async
ownership protocol, and two of the migration's real bugs came from that duplication
(the Carta SEGV from suspending inside middleware, and the `on_resume` hang).

## Layer 1: Shared API types

`shared/include/sh_api.h`:

```c
typedef struct {
    const char *method;     /* "GET", "POST", ... (NULL ok) */
    const char *path;       /* URI path (required) */
    const char *query;      /* query string without '?' (NULL ok) */
    const char *host;       /* Host header, for URL generation (NULL ok) */
    const char *body;       /* request body (NULL ok) */
    size_t      body_len;
} ShApiRequest;

typedef struct ShApiStream ShApiStream;   /* opaque, transport-owned */

typedef struct {
    int         status_code;
    const char *content_type;   /* static string, not freed */
    uint8_t    *body;           /* heap; ownership passes to the transport */
    size_t      body_len;

    /* Streaming -- see "Layer 3". NULL for a unary response.
     * stream_fn MUST NOT BLOCK: it runs on the event loop thread. */
    ShApiStreamFn stream_fn;
    void         *stream_ctx;
    void        (*stream_free)(void *stream_ctx);
} ShApiResponse;

typedef int (*ShApiHandler)(void *ctx, const ShApiRequest*, ShApiResponse*);
```

Routing stays **inside** the module handler, which is what `ralph_api_handle()` already
does: it switches on `req->path`. The transport never learns about routes. This is what
keeps the transport interface at two methods.

## Layer 2: Transport vtable

`shared/include/sh_transport.h`:

```c
typedef struct {
    const char *name;
    int  (*serve)(const ShServeConfig*, ShApiHandler, void *ctx);
    void (*stop) (void *server);
} ShTransport;

extern const ShTransport sh_transport_keel;    /* HTTP via Keel */
extern const ShTransport sh_transport_direct;  /* in-process, for tests */
```

**Hard constraint: the vtable exposes `serve` and `stop` and nothing else.**

The moment `suspend`, `complete`, `route` or `middleware` appear in it, Keel's model has
been encoded as *the* model, which is lock-in with an extra layer of indirection. If a
future transport genuinely cannot be driven through `serve`/`stop`, that is a signal to
revisit the design, not to widen the vtable.

### Async is owned by the transport, once

`sh_transport_keel` absorbs the entire protocol:

```
kl_async_suspend
  -> snapshot the request
  -> kl_thread_pool_submit
       -> worker runs the plain synchronous ShApiHandler
  -> done_fn: copy body into KlHttpResponse, kl_async_complete
  -> on_resume: kl_http_request_send_response(req)
  -> cancel_fn: free when the pool drops the item at shutdown
  -> `detached` flag for on_cancel / on_deadline
```

Modules stop seeing `KlAsyncOp` entirely. Whether a handler runs on the event loop or on
a pool worker is a transport decision, not a handler concern.

**Correctness requirement:** the transport MUST copy `path`, `query`, `body` and `host`
into the async context before submitting. Those strings are borrowed from the Keel
connection and the handler runs on another thread. This is the same class of defect as
the Carta SEGV; it is written down here so it is not rediscovered.

## Layer 3: Streaming escape hatch

**Built.** The hatch lives **in the response, not in the vtable** -- it is data,
not a vtable method, so it cannot become the crack that lets Keel's model back in.

```c
typedef enum {
    SH_API_STREAM_DONE  =  0,   /* finished; transport closes the stream */
    SH_API_STREAM_MORE  =  1,   /* more to send; transport calls again */
    SH_API_STREAM_ERROR = -1    /* give up; transport aborts */
} ShApiStreamStatus;

typedef ShApiStreamStatus (*ShApiStreamFn)(void *stream_ctx, ShApiStream *out);

int sh_api_stream_send  (ShApiStream*, const char *event,
                         const void *data, size_t len);
int sh_api_stream_closed(const ShApiStream*);
```

### The producer must not block

`stream_fn` runs on the transport's **event loop thread** and must not block.
The transport drives it in a loop until it answers `DONE` or `ERROR`; a spin cap
catches a producer that answers `MORE` forever, which would otherwise wedge the
loop and every other connection with it.

This corrects an earlier version of this design, which specified a producer
running on a pool worker that *may block*. **That could not be implemented.**
Keel's `kl_stream_write()` writes straight to `res->conn_fd`, or into a drain
buffer the event loop flushes, with no locking anywhere in that path
(`keel/src/protocols/http/http_response.c`). Emitting from a worker would race
the loop on the connection -- the same defect class as the Carta SEGV. Keel also
exposes no post-to-loop or wakeup API; the thread pool's worker-to-loop pipe is
private, and `kl_async_complete()` is one-shot per work item.

The consequence for callers: do the expensive work elsewhere and leave a result
for the producer to pick up. A long solve publishes progress into a shared value
that the producer reads and forwards; it must not compute inside `stream_fn`.

The earlier design was written from the producer's nature (SSE is push, so a
pull model felt wrong) without checking the transport's threading constraints.
It shipped as a public struct field in Phase 1 with only the in-process
transport behind it, which is why nothing caught it: `sh_transport_direct`
writes into a buffer with no connection and no loop, so its tests passed and
proved nothing.

### Implementations

| transport | how |
|---|---|
| `sh_httpasync` | `kl_http_sse_begin` / `_event` / `_end`; backpressure via `KlDrain` (bounded, 1 MiB, flushed by the loop) |
| `sh_transport_direct` | collects events into a buffer, so streaming handlers are testable without sockets |

`stream_free`, if set, is always called afterwards -- including when a transport
declines to stream at all.

**Out of scope: websockets.** Full duplex needs a read side and a different
lifetime. Stretching this shape to cover it would be the mistake this design
exists to avoid.

## Layer 4: `sh_pal.h`

Native Windows is a supported target. Keel is **not** the right implementation for this
layer, for two reasons.

**The overlap is nearly empty.** Keel's public platform surface is `net.h`, `socket.h`,
`sockaddr.h`, `resolver.h`, plus `kl_monotonic_ms` and `thread_pool.h`. Its `clock.h`
contains exactly one function, and it ships a `freestanding.h` for UEFI, so the minimal
surface is deliberate. What OTTO's core needs, Keel does not export:

| area | call sites | in Keel? | Windows mapping |
|---|---|---|---|
| mutex / cond / rwlock / once / TLS | ~150 | no | `SRWLOCK`, `CONDITION_VARIABLE`, `INIT_ONCE`, `FlsAlloc` |
| read-only file mapping | 7 files (velo, carta, locus) | no | `CreateFileMapping` + `MapViewOfFile` |
| monotonic + wall clock | 15 | partly | `QueryPerformanceCounter`, `GetSystemTimeAsFileTime` |
| calendar (`gmtime_r`, `localtime_r`) | shimmed | no | `gmtime_s`, `localtime_s` |
| CPU count (`sysconf`) | 3 | no | `GetSystemInfo` |
| env (`setenv`, `unsetenv`) | tests/bench | no | `_putenv_s` -- built |
| temp directory | tests/bench | no | `GetTempPathA` -- built |
| sockets / addresses / DNS | 0 in core | yes | only servers need it; they already use Keel |

**It would invert the layering.** `sh_httpserver.c` is deliberately excluded from
`libshared.a` so core does not depend on Keel. Making Keel the PAL would put a transport
vendor underneath Ralph and Shared, and drag it into WASM builds that have no server.

### Header strategy

`sh_pal.h` must not pull `windows.h` into every translation unit. Defining
`ShMutex` as `struct { CRITICAL_SECTION cs; }` would do exactly that, dragging in
`min`/`max` macros and much else that breaks C code. Use opaque fixed-size storage with
a `_Static_assert` on size and alignment in the `.c` file.

## Phases

| phase | work | rationale |
|---|---|---|
| 1 | `sh_api.h`, `sh_transport_direct`, migrate Ralph | Ralph is already synchronous — proves the interface end to end with zero async risk |
| 2 | `sh_transport_keel` with async ownership; migrate fuelwise, surge, locus, velo, carta | fuelwise is the simplest async server; carta last, it has two suspend sites |
| 3 | streaming escape hatch | build when a real consumer exists |
| 4 | `sh_pal.h`: threading + clock, then file mapping | biggest win and lowest risk first |
| 5 | extract Velo, Carta and Locus handlers into their libraries | the CI invariant cannot cover them until the handler leaves the Keel TU |

**Phase 4 requires a Windows CI job.** CI paths nobody exercises decay silently —
`build-wasm` sat behind a red `test-c` for months without running. A PAL with no Windows
job is the same trap with more surface.

### What widening that job found

The job started narrow: core suites plus a *build* of the six servers, with
FuelWise and Surge running only `test-transport`. Widening those two to their
full `test` targets found three more defects in the FuelWise bench harness --
none of which `test-transport` could have caught, because it never compiled
that code:

| defect | why it is a Windows defect |
|---|---|
| `setenv` / `unsetenv` in `test_validator.c` | POSIX-only; the CRT spells it `_putenv_s`. Now `sh_pal_setenv` / `sh_pal_unsetenv`. |
| `fuelwise/bench/Makefile` missing `-lbcrypt -lws2_32` | the same hole as #63, in a second Makefile nobody had linked on Windows |
| `glpsol` invoked with `'single quotes'` | `popen()` goes through `cmd.exe`, which does not treat `'...'` as quoting and looks for a program named `'glpsol'` |
| `mkstemp("/tmp/fw_bench_XXXXXX")` | a mingw binary is a native Windows program, so `/tmp/x` means `C:\tmp\x`. Now `sh_pal_temp_dir()`. |

The `cmd.exe` one has a second layer worth writing down: double quotes *are*
quoting in cmd, but when the command line begins with one, cmd strips the first
and the last quote of the whole line. A line quoting three arguments therefore
needs an extra outer pair, which is what `fw_glpk.c` now emits under `_WIN32`.

`glpk` is installed in the job so the FuelWise regression harness can run its
objective comparisons against `glpsol` on Windows too. That is a check on
Ralph's numerics under a different libm and compiler, not merely on whether the
code compiles.

**Still Linux-only:** the API HTTP suites. They drive a live server with `curl`
and want a downloaded PBF, which is a bigger CI surface than this job needs; the
Windows job builds all six servers, which is what was actually unknown.

### Two Windows jobs, not one

Widening the job took it to ten minutes, which is too long to sit in front of
every push. The split that followed is worth recording because the obvious
guess was wrong.

The suspicion was the FuelWise bench harness -- it is the part that shells out
to an external solver, so it *looks* expensive. The step timings from the run
said otherwise:

| step | time |
|---|---|
| Build and test Surge | 338s |
| Build and test Ralph | 66s |
| Build and test FuelWise | 65s |
| Build the API servers | 54s |
| toolchain setup + checkout | 49s |
| everything else | 48s |

Surge was more than half the job, and inside it `test_surge` alone is ~297s:
it is an 18k-line suite that actually solves. The other four Surge binaries
together run in under 1.5 seconds. The bench harness was ~40s.

So the cut is by *measured* cost rather than by which step sounds heavy:

- **Windows Core** -- the PAL, every library suite, FuelWise's transport and
  unit suites, Surge's transport test, and a build of all six servers. Roughly
  three minutes, and it no longer installs `glpk`, because only the bench
  harness needs `glpsol`.
- **Windows Suites** -- the full Surge suite and the FuelWise bench regression.

They run in parallel, so the Windows coverage still completes in about the time
Surge takes, but Core answers in three minutes.

`fuelwise/Makefile` grew `test-unit` and `test-bench` for this; `test-regression`
is now the two together, so `make -C fuelwise test` is unchanged and Linux CI
did not move.

### What the split itself found

Splitting turned three Ralph tests red -- `test_lp.c`'s LP and MPS round-trips
-- that had passed in the single job an hour earlier. Nothing about Ralph had
changed. The tests wrote to literal `/tmp/test_write.lp` and `/tmp/roundtrip.mps`,
which for a mingw binary means `C:\tmp\...`, and on the runner that directory
existed *only because `shared/tests/test_fs.c` had created it earlier in the
same job* as a side effect of its own `sh_mkdirs` test.

So the single job had been green by accident: one suite was silently providing
a directory another suite depended on. Ordering them into separate jobs removed
the accident, which is the correct outcome and the reason to write it down --
a job split is supposed to be behaviour-preserving, and when it is not, the
thing it broke was already broken.

All twelve literal `/tmp` paths across six Ralph test files now go through
`ralph/tests/test_tmp.h`, a two-line helper over `sh_pal_temp_dir()`. The files
land in `%TEMP%` on Windows and `$TMPDIR` or `/tmp` on POSIX. `test_fs.c` keeps
its literal path -- it creates its own tree, so it is correct on both platforms
-- but now carries a comment saying not to lean on the side effect.

## Invariant to enforce in CI

A test that links every module's handler against `sh_transport_direct` **with no Keel
headers on the include path**. If that stops compiling, the abstraction has leaked, and
it will be caught immediately rather than at the next transport migration.

### Status: 6 of 6

| module | handler lives in | invariant test |
|---|---|---|
| Ralph | `ralph/src/ralph_api.c` | `make -C ralph test-transport` |
| FuelWise | `fuelwise/src/fw_api.c` | `make -C fuelwise test-transport` |
| Surge | `surge/src/sg_api.c` | `make -C surge test-transport` |
| Velo | `velo/src/vl_api.c` | `make -C velo test-transport` |
| Carta | `carta/src/ct_api.c` | `make -C carta test-transport` |
| Locus | `locus/src/lc_api.c` | `make -C locus test-transport` |

Every module's handler is now a plain `ShApiHandler` in its library, and every
module has a test that links it against `sh_transport_direct` with no Keel
headers on the include path. `make -C <module> test` runs it, so both the Linux
and Windows jobs pick all six up with no workflow change.

Every module's WASM bridge calls that same handler too -- Ralph, FuelWise and
Surge already did; Carta, Locus and Velo now do. There is one implementation of
each API, and the demo really is the product.

### Phase 5: what the extraction actually found

The phase was written up as "a real refactor of three servers, not a
test-writing exercise". That was right, but for the wrong reason. The work was
not moving a handler across a file boundary. It was that **each of the three
modules had shipped two or three implementations of its own public API**, and
they had drifted.

**Velo.** Three copies of three endpoints, and the WASM one did not merely
differ in style -- it spoke a different API than the one Velo documents:

| | server | library | WASM |
|---|---|---|---|
| route parameters | `from=lat,lon` | `from=lat,lon` | `from_lat`, `from_lon`, `to_lat`, `to_lon` |
| `geometry` | encoded polyline | encoded polyline | raw `[[lat,lon],...]` array |
| no route | 404 | 404 | **422** |
| `meta` object | yes | yes | **absent** |
| geometry default | on | **off** | **off** |
| POST missing `from` | **silently (0,0)** | 400 | n/a |

`site/api-config.json` describes "Google Polyline encoded geometry", and
`vl_api_handle`'s own `@query` annotations document `from`/`to` -- so the
browser demo was contradicting the docs generated from the same header. The JS
wrapper is the only client that ever sent the four-parameter form, and it now
sends `from=`/`to=` like everyone else; the demo form keeps its four numeric
inputs and joins them.

The server's POST path also validated `from` only *if it was present*
(`if (from_str && parse_coord(...))`), so a body with no `from` routed from
(0, 0) rather than answering 400. The library was already strict; that is the
version that survived.

**Locus.** Also three copies, and the library's carried a heap overflow:
`lc_api_search()` accumulated `snprintf`'s would-have-written return into an
unclamped `offset` against a fixed 64 KB allocation. Reachable with a hundred
long-named results; aborts under `-D_FORTIFY_SOURCE=2`. Unreachable in
production only because nothing called the function. Its `atof()`-based
coordinate parsing also turned `lat=north` into `0.0`, which passes a
`-90..90` range check.

**Carta.** Two copies rather than three -- its WASM demo already called the
library -- but the same shape of drift: the server's ASCII endpoint parsed
`charset` and then discarded it (`(void)ascii_opts`), so the browser honoured
`charset=braille` and the HTTP server silently served `extended`.

### What stayed in the servers

Not everything in `api/src/main.c` was duplication. Each server kept the parts
that are genuinely a server's job, wrapped around the library handler:

| module | wrapper | keeps |
|---|---|---|
| Carta | `carta_cached_handler()` | PNG/MVT response cache, adaptive capacity |
| Locus | `locus_metered_handler()` | adaptive capacity |
| Velo | `velo_metered_handler()` | adaptive capacity |

The adaptive-capacity feedback retunes *that server's* rate limiter; a WASM
build has no rate limiter to retune. Locus and Velo also keep a server-side
`/api/v1/stats` that reports rate limiter, work queue and adaptive counters,
which is a different thing from the library's index/graph statistics rather
than a second implementation of it.

### One shared addition

`sh_query_get_str_decoded()` percent-decodes a query value (`%XX` and `+`),
passing a malformed `%` through as a literal rather than truncating. Only the
Locus WASM copy decoded `?q=`, so `?q=Monte%20Carlo` searched the HTTP API for
the literal string. Decoding in the shared handler is what let the three
collapse without the demo losing behaviour.
