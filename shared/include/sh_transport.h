/*
 * sh_transport.h - Transport drivers for OTTO API handlers.
 *
 * A transport takes a ShApiHandler and runs it: over HTTP, in-process, or
 * anywhere else. The interface is deliberately two methods wide.
 *
 * DO NOT ADD METHODS. In particular, do not add suspend/complete/route/
 * middleware. Those are Keel's model; putting them here would encode one
 * library's design as the interface, which is lock-in with an extra layer of
 * indirection. If a transport genuinely cannot be driven through serve/stop,
 * that is a reason to revisit the design -- not to widen the vtable.
 *
 * See docs/roadmaps/transport.md.
 */
#ifndef SH_TRANSPORT_H
#define SH_TRANSPORT_H

#include <stddef.h>
#include "sh_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Transport configuration.
 *
 * Fields a given transport does not understand are ignored: sh_transport_direct
 * has no port and no threads. Zero-initialise and set what matters.
 */
typedef struct {
    const char *host;           /* bind address, e.g. "127.0.0.1" (NULL = any) */
    int         port;           /* TCP port; ignored by in-process transports */
    int         threads;        /* worker pool size; 0 = transport default */
    size_t      max_body_bytes; /* request body cap; 0 = transport default */
    const char *service_name;   /* for logs and health payloads (NULL ok) */
} ShServeConfig;

/*
 * A transport driver.
 *
 * serve() runs until stopped and returns 0 on a clean shutdown, non-zero on a
 * startup or runtime failure. stop() may be called from another thread or a
 * signal handler; passing NULL is a no-op.
 */
typedef struct {
    const char *name;
    int  (*serve)(const ShServeConfig *cfg, ShApiHandler handler, void *ctx);
    void (*stop) (void *server);
} ShTransport;

/*
 * In-process transport.
 *
 * serve() does not listen on anything: it is the degenerate driver used by
 * tests and by embedded callers that already have the request in hand. Use
 * sh_transport_direct_call() to invoke a handler directly.
 *
 * This transport links without any Keel headers on the include path, which is
 * the invariant that proves the abstraction has not leaked.
 */
extern const ShTransport sh_transport_direct;

/*
 * Invoke a handler once, synchronously.
 *
 * Returns whatever the handler returns. If the handler produced a streaming
 * response, the stream is collected into resp->body and resp->body_len, and
 * stream_free is called before returning -- so callers see a unary response
 * regardless of how the handler chose to answer.
 */
int sh_transport_direct_call(ShApiHandler handler, void *ctx,
                             const ShApiRequest *req, ShApiResponse *resp);

#ifdef __cplusplus
}
#endif

#endif /* SH_TRANSPORT_H */
