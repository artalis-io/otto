/*
 * sh_api.h - Transport-agnostic API request/response types.
 *
 * Every OTTO module exposes the same handler shape: given a request, fill in a
 * response. These types are that shape, shared, so a module's core logic is
 * written once and driven by any transport (HTTP, WASM, in-process, embedded).
 *
 * Deliberately free of transport headers: this header pulls in no Keel, no
 * sockets, no platform types. See docs/roadmaps/transport.md.
 */
#ifndef SH_API_H
#define SH_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Request
 * ============================================================================ */

/*
 * An inbound request.
 *
 * All strings are borrowed and valid only for the duration of the handler
 * call. A transport that runs the handler on another thread MUST copy them
 * first -- they are typically owned by a connection that can be recycled.
 */
typedef struct {
    const char *method;     /* "GET", "POST", ... (NULL ok) */
    const char *path;       /* URI path, e.g. "/api/v1/solve" (required) */
    const char *query;      /* query string without '?' (NULL ok) */
    const char *host;       /* Host header, for URL generation (NULL ok) */
    const char *body;       /* request body (NULL ok) */
    size_t      body_len;   /* body length in bytes */
} ShApiRequest;

/* ============================================================================
 * Response
 * ============================================================================ */

/*
 * A live streaming response. Opaque; owned by the transport.
 * Only meaningful inside a ShApiResponse::stream_fn callback.
 */
typedef struct ShApiStream ShApiStream;

/* What a producer wants to happen next. */
typedef enum {
    SH_API_STREAM_DONE  =  0,   /* finished; transport closes the stream */
    SH_API_STREAM_MORE  =  1,   /* more to send; transport calls again */
    SH_API_STREAM_ERROR = -1    /* give up; transport aborts the stream */
} ShApiStreamStatus;

/*
 * Emit the next slice of a streaming response.
 *
 * ============================ MUST NOT BLOCK =============================
 *
 * This runs on the transport's EVENT LOOP THREAD, which is also serving every
 * other connection. A producer that blocks -- on I/O, on a lock, on a
 * condition variable, or on a long computation -- stalls the entire server.
 *
 * There is no worker-thread variant, and this is not an oversight. Keel's
 * streaming writes go straight to the connection fd, or into a drain buffer
 * that the event loop flushes, with no locking anywhere in that path
 * (kl_stream_write in keel/src/protocols/http/http_response.c). Emitting from
 * a pool worker would race the loop on the connection. An earlier draft of
 * this interface did specify a blocking worker-side producer; it could not be
 * implemented against any real transport. See docs/roadmaps/transport.md.
 *
 * So: do the expensive work somewhere else and leave a result for this
 * function to pick up. A long solve should publish progress into a shared
 * value that the producer reads and forwards; it must not compute here.
 *
 * =========================================================================
 *
 * Emit zero or more events with sh_api_stream_send(), then return MORE to be
 * called again, DONE when finished, or ERROR to abort. Returning MORE without
 * having sent anything is a valid way to say "nothing yet, ask me later".
 */
typedef ShApiStreamStatus (*ShApiStreamFn)(void *stream_ctx, ShApiStream *out);

/*
 * An outbound response.
 *
 * Unary (the common case): set status_code, content_type and body/body_len.
 * Ownership of `body` passes to the transport, which frees it.
 *
 * Streaming: set stream_fn instead. The transport then ignores body/body_len,
 * emits the status line and content_type, and drives stream_fn until it
 * returns DONE or ERROR. stream_free, if set, is always called afterwards --
 * including when the transport declines to stream at all.
 */
typedef struct {
    int         status_code;    /* HTTP-style status: 200, 400, 404, 500, ... */
    const char *content_type;   /* MIME type; static string, never freed */
    uint8_t    *body;           /* heap-allocated; transport takes ownership */
    size_t      body_len;

    /* Streaming. NULL for a unary response. stream_fn MUST NOT BLOCK. */
    ShApiStreamFn stream_fn;
    void         *stream_ctx;
    void        (*stream_free)(void *stream_ctx);
} ShApiResponse;

/*
 * Handle one request.
 *
 * `ctx` is the module's own context, opaque to the transport. Returns 0 when
 * the response has been filled in (including error responses -- a 404 is a
 * successful handling), and non-zero only on an internal failure that leaves
 * the response unusable.
 *
 * Handlers are synchronous and self-contained. Whether they run on an event
 * loop thread or a worker pool is the transport's decision, not theirs.
 */
typedef int (*ShApiHandler)(void *ctx,
                            const ShApiRequest *req,
                            ShApiResponse *resp);

/* ============================================================================
 * Helpers
 * ============================================================================ */

/* Free a response body and zero the struct. Safe on NULL and on a response
 * whose body was never set. Does not call stream_free -- the transport owns
 * that lifetime. */
void sh_api_response_free(ShApiResponse *resp);

/* Fill `resp` with a copy of `body`. Returns 0 on success, -1 on allocation
 * failure (in which case resp is left as a 500 with no body). `content_type`
 * must be a static string. */
int sh_api_response_set(ShApiResponse *resp, int status,
                        const char *content_type,
                        const void *body, size_t body_len);

/* Convenience: {"error":"<msg>"} as application/json. `msg` is escaped for
 * the characters JSON requires. Returns 0 on success, -1 on allocation
 * failure. */
int sh_api_response_error(ShApiResponse *resp, int status, const char *msg);

/* Write one chunk/event to a live stream. `event` names an SSE event type and
 * may be NULL for an unnamed event; transports without a notion of event names
 * ignore it. Returns 0 on success, -1 if the stream is closed or errored. */
int sh_api_stream_send(ShApiStream *stream, const char *event,
                       const void *data, size_t len);

/* Non-zero once the peer has gone away and further sends are pointless. */
int sh_api_stream_closed(const ShApiStream *stream);

#ifdef __cplusplus
}
#endif

#endif /* SH_API_H */
