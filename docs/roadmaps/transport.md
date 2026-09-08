# Transport Interface + Platform Abstraction Layer

**Status:** Phases 1-4 done. Phase 5 done for Carta; Velo and Locus remain.
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

The Mongoose to Keel migration confirmed the cost of not having it: the expensive part
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
| `sh_keelasync` | `kl_http_sse_begin` / `_event` / `_end`; backpressure via `KlDrain` (bounded, 1 MiB, flushed by the loop) |
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
| env (`setenv`, `unsetenv`) | tests/bench | no | `_putenv` |
| sockets / addresses / DNS | 0 in core | yes | only servers need it; they already use Keel |

**It would invert the layering.** `sh_keelserver.c` is deliberately excluded from
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

## Invariant to enforce in CI

A test that links every module's handler against `sh_transport_direct` **with no Keel
headers on the include path**. If that stops compiling, the abstraction has leaked, and
it will be caught immediately rather than at the next transport migration.

### Status: 4 of 6

| module | handler lives in | invariant test |
|---|---|---|
| Ralph | `ralph/src/ralph_api.c` | `make -C ralph test-transport` |
| FuelWise | `fuelwise/src/fw_api.c` | `make -C fuelwise test-transport` |
| Surge | `surge/src/sg_api.c` | `make -C surge test-transport` |
| Carta | `carta/src/ct_api.c` | `make -C carta test-transport` |
| Velo | **`velo/api/src/main.c`** | none possible yet |
| Locus | **`locus/api/src/main.c`** | none possible yet |

Velo and Locus have no library-side `ShApiHandler` to link against at all: their
`src/*_api.c` is a context and lifecycle layer with zero `ShApi` references, and
the handler itself lives in `api/src/main.c` next to 25-44 Keel references.
Phase 2 gave all six the shared request/response *types*, but only Ralph,
FuelWise and Surge ended up with a handler the direct transport could call.

So for those two the invariant is not merely unenforced -- it is currently
unenforceable, and "no Keel on the include path" is trivially false for the only
translation unit that has a handler.

Closing it means extracting the handler out of `api/src/main.c` into the library,
as `ralph_api.c` already does, leaving `main.c` as the thin Keel wrapper the
manifesto describes. That is a real refactor of two servers (1276 and 1096 line
files), not a test-writing exercise.

### Phase 5 progress

**Carta: done.** `ct_api_handle()` is now a plain `ShApiHandler` in
`carta/src/ct_api.c`, and `carta/api/src/main.c` lost 238 lines to it.

What that deleted was not plumbing, it was a *second implementation*. The server
had its own routing, its own render dispatch (`process_png_render`,
`process_mvt_render`, `process_ascii_render`, `RenderCtx`) and its own error
bodies, while the WASM demo went through `ct_api_handle`. Same product surface,
two code paths, and they had already drifted: the server's ASCII endpoint parsed
`charset` and then discarded it (`(void)ascii_opts`, with a comment admitting the
options were parsed a second time inside the library), and capped `width` at 256
where the library's documented range is 20-400. The browser demo supported a
charset the HTTP server silently ignored. That is exactly the split "the demo IS
the product" exists to rule out.

`main.c` keeps the two things that really are the server's job and not the
library's, wrapped around the handler as `carta_cached_handler()`:

- the PNG/MVT response cache, which is per-process state a WASM build has no use for
- the adaptive-capacity feedback that retunes *this server's* rate limiter

Both were previously interleaved with the render code they now merely wrap.

`CTAPIRequest` and `CTAPIResponse` are gone; `ct_api_handle()` takes
`ShApiRequest`/`ShApiResponse` and its `ctx` is `void*` so the signature matches
`ShApiHandler` exactly and the compiler checks it. `carta/wasm/src/ct_wasm_api.c`
follows the same types, so the browser demo and the HTTP server now run the same
function.

Two things surfaced on the way and are fixed here rather than left as traps:

- error bodies are now `{"error":"..."}` via `sh_api_response_error()`, matching
  Ralph, FuelWise and Surge, where Carta used to return bare text
- `ct_api.h` could not be included on its own -- it named `ct_types.h` as the
  home of `CTLODConfig`, which lives in `ct_lod.h`. Nothing caught it because
  every existing includer pulled in `carta.h` first.

**Velo and Locus: not started.** Velo is the larger of the two but its handler is
the more mechanical extraction; Locus has the smaller `main.c`.
