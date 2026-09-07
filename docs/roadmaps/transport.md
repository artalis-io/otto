# Transport Interface + Platform Abstraction Layer

**Status:** Phase 1 in progress
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

    /* Streaming escape hatch -- see "Layer 3". NULL for a unary response. */
    int  (*stream_fn)(void *stream_ctx, ShApiStream *out);
    void  *stream_ctx;
    void (*stream_free)(void *stream_ctx);
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

No endpoint uses this yet. **Partially built** — the interface and the in-process
transport are done; the Keel side is not:

| piece | state |
|---|---|
| `ShApiResponse.stream_fn` / `_ctx` / `_free` | built (Phase 1) |
| `ShApiStream`, `sh_api_stream_send()`, `sh_api_stream_closed()` | built |
| `sh_transport_direct` collecting stream | built and tested |
| `sh_keelasync` streaming via `kl_http_sse_*` | **not built** — declines with 501 |

Building the interface half early cut both ways. It keeps `ShApiResponse` stable, so
adding streaming later is not a struct change — but it also shipped a promise the Keel
transport did not keep. Until the Keel side exists, `sh_keel_async_dispatch()` replies
501 to a streaming response and calls `stream_free`, rather than sending the handler's
own status with an error payload and leaking `stream_ctx`.

The hatch lives **in the response, not in the vtable** — it is data, not a vtable method,
so it cannot become the crack that lets Keel's model back in.

```c
int sh_api_stream_send  (ShApiStream*, const char *event,
                         const void *data, size_t len);
int sh_api_stream_closed(const ShApiStream*);   /* peer went away */
```

When `stream_fn != NULL` the transport ignores `body`/`body_len`, emits the status line
and `content_type`, then calls `stream_fn` on a worker thread with a live handle. The
handler owns the response until `stream_fn` returns; `stream_free` is always called
afterwards.

`stream_fn` runs on a pool thread and **may block**. That is what push requires: SSE
events fire when they occur, not when a puller asks. A pull-based producer callback
would be simpler but cannot express progress events, and progress reporting on long
solves is the realistic first consumer (solves of 10+ minutes are observed today).

- `sh_transport_keel` implements this on `kl_http_sse_begin` / `_event` / `_end`.
- `sh_transport_direct` implements it by collecting into a buffer, so streaming handlers
  are testable without sockets.

**Out of scope: websockets.** Full duplex needs a read side and a different lifetime.
Stretching this shape to cover it would be the mistake this design exists to avoid.

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

**Phase 4 requires a Windows CI job.** CI paths nobody exercises decay silently —
`build-wasm` sat behind a red `test-c` for months without running. A PAL with no Windows
job is the same trap with more surface.

## Invariant to enforce in CI

A test that links every module's handler against `sh_transport_direct` **with no Keel
headers on the include path**. If that stops compiling, the abstraction has leaked, and
it will be caught immediately rather than at the next transport migration.
