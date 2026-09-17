# Reading the six API servers

A line-by-line read of the six `api/src/main.c` files — 4,802 lines — looking
for defects in the code closest to untrusted network input.

It found almost nothing, and the reason is worth more than the finding.

## What was read

| server | lines | handlers |
|---|---|---|
| carta | 1,580 | 8 |
| velo | 932 | 4 |
| locus | 787 | 7 |
| surge | 517 | 5 |
| fuelwise | 507 | 7 |
| ralph | 479 | 4 |

Every construct that carries risk in request-handling code was read in full:
tile-parameter parsing, static-file serving, query-string copying, request
marshalling, async dispatch, and body limits.

## The one change

`parse_tile_uri()` in carta called `isalnum(*next)` where `next` is a `char *`.
`isalnum` is undefined for a negative argument other than `EOF`, and plain
`char` is signed on the targets here.

Tested rather than assumed: Keel **rejects a raw byte above 127 in the request
line** (the connection is closed before the handler runs) and does **not**
percent-decode the path, so `%C3%A9` arrives literally and stops the scan at
the `%`. Nothing currently reaches this with the high bit set.

So it is latent, not reachable — but that is Keel's behaviour rather than this
function's contract, MSVC's `isalnum` asserts on a negative argument in the
debug build CI now runs, and the cast costs nothing.

## What was checked and found sound

Recorded so the next reader does not redo it:

- **Tile coordinates.** `z`/`x`/`y` come from `strtol` with no bounds in the
  parser, but all three handlers validate identically — `z` against the
  configured range and a hard `z > 30`, then `x`/`y` against `1 << z` before
  any use. The one call site that skips validation only reaches
  `ct_cache_get()`, which bounds `z` itself and otherwise just computes a hash
  key; unvalidated `x`/`y` produce a key that fails to match.
- **Static file serving.** `serve_static_file()` truncates the path to its
  buffer *before* the `strstr(rel, "..")` check, and builds the filesystem path
  from that same truncated string — so the string that is checked is the string
  that is opened, and truncation cannot smuggle a traversal past the check.
- **Query-string copying.** carta, locus and velo share one pattern:
  `qlen = min(query_len, sizeof(buf) - 1)`, copy, terminate at `qlen`. Correct
  in all three. ralph's `slice_to_buf()` clamps the same way.
- **Async lifetime.** `submit_render_work()` puts stack-local `path` and
  `query` into an `ShApiRequest` and hands it to a worker pool. That is the
  classic use-after-return in this design, and `sh_http_async_dispatch()`
  copies every borrowed string before the worker can run — with a comment
  naming the Carta SEGV that came from getting it wrong previously.
- **Body limits.** The four servers with POST routes set an explicit
  `max_body_size` (velo 1 MB, ralph 4 MB, fuelwise 16 MB, surge 32 MB). carta
  and locus have no POST routes and correctly set none.
- **No arithmetic on request-derived values** anywhere in the six files.

## Why the read found so little, and where the risk actually is

The servers are thin. `docs/MANIFESTO.md` calls for a transport-agnostic
design where HTTP is a wrapper over pure C functions, and these six honour it:
each handler marshals a `ShApiRequest` and calls one library entry point.

The parsing of untrusted input is behind that call:

| entry point | file | lines in the file |
|---|---|---|
| `sg_api_handle` | `surge/src/sg_api.c` | 2,247 |
| `fw_api_handle` | `fuelwise/src/fw_api.c` | 811 |
| `vl_api_handle` | `velo/src/vl_api.c` | 632 |
| `lc_api_handle` | `locus/src/lc_api.c` | 540 |
| `ralph_api_handle` | `ralph/src/ralph_api.c` | 527 |
| `ct_api_handle` | `carta/src/ct_api.c` | — |

**5,800+ lines, more than the `main.c` files that were read**, and it is where
a request body is turned into a model. The single defect found in this area
previously — `ralph_api.c` reading a timeout through `sh_json_as_double`, so a
NaN in the request became the solver's time limit — was in `ralph_api.c`, not
in `ralph/api/src/main.c`.

The scoping was therefore wrong: "read the API servers" pointed at the
transport, and the transport is the thin part. The `*_api.c` files are the
next read, and are the larger one.

## Not covered

- The `*_api.c` request handlers above.
- Keel itself. Its request-line parser was probed for high-byte handling and
  rejects them, but it was not read.
- Concurrency. The async paths were read for lifetime, not for races; ThreadSanitizer
  on these servers is a separate exercise from the ASan/UBSan coverage added
  alongside this.
