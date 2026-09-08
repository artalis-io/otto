/*
 * sh_keelasync.h - The Keel async protocol, owned once.
 *
 * Five OTTO servers hand-rolled the same suspend/pool/resume dance. It is
 * subtle in ways that are not obvious from Keel's examples, and both real
 * bugs found during the Keel migration lived in it: a SEGV from
 * suspending inside middleware, and a hang from a no-op on_resume. This file
 * is that protocol, written once, so a handler never has to know it exists.
 *
 * A handler is a plain synchronous ShApiHandler. Whether it runs on the event
 * loop or on a pool worker is decided here, not by the handler.
 *
 * NOT part of libshared.a -- it needs Keel headers, so API servers compile it
 * directly, the same way they compile sh_keelserver.c:
 *
 *   $(CC) $(CFLAGS) -I../shared/include -I../vendor/keel/include \
 *         ../shared/src/sh_keelasync.c
 *
 * See docs/roadmaps/transport.md.
 */
#ifndef SH_KEELASYNC_H
#define SH_KEELASYNC_H

#include <stdint.h>
#include <stddef.h>

#include <keel/http_request.h>
#include <keel/http_response.h>
#include <keel/http_server.h>
#include <keel/thread_pool.h>

#include "sh_api.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ShCorsConfig;

/*
 * Queue counters, for a /api/v1/stats endpoint. Every field is touched only
 * on the event loop thread (dispatch, done_fn and on_deadline all run there),
 * so plain integers are correct without atomics.
 */
typedef struct {
    uint64_t pushed;    /* accepted onto the pool */
    uint64_t popped;    /* completed (done_fn ran) */
    uint64_t dropped;   /* rejected, queue full -> 503 */
    uint64_t expired;   /* deadline passed before completion -> 504 */
} ShKeelAsyncStats;

/*
 * Everything the protocol needs. Fill once at startup and reuse.
 */
typedef struct {
    KlHttpServer *server;               /* required */
    KlThreadPool *pool;                 /* NULL: run handlers inline */
    const struct ShCorsConfig *cors;    /* NULL: no CORS headers */
    double        timeout_s;            /* <= 0: no deadline */
    ShKeelAsyncStats *stats;            /* NULL: no counting */
} ShKeelAsync;

/*
 * Run `handler` for one request and reply.
 *
 * `api_req` is marshalled by the caller (it knows which Keel route matched);
 * its strings are COPIED before anything is submitted, because they belong to
 * the connection and the worker outlives it.
 *
 * With a pool configured the request is suspended and the handler runs on a
 * worker; without one it runs inline on the event loop. Either way the caller
 * is done once this returns -- the response is written here.
 *
 * Backpressure and timeouts are handled: a full queue replies 503, an exceeded
 * deadline replies 504, and a dropped connection is detected so nothing is
 * written to a dead conn.
 */
void sh_keel_async_dispatch(const ShKeelAsync *cfg,
                            KlHttpRequest *req,
                            KlHttpResponse *res,
                            ShApiHandler handler,
                            void *handler_ctx,
                            const ShApiRequest *api_req);

#ifdef __cplusplus
}
#endif

#endif /* SH_KEELASYNC_H */
