/*
 * fuzz_api_handler.h - shared body for the *_api_handle() fuzzers
 *
 * The six API servers turned out to be thin marshalling wrappers: every one
 * of them builds an ShApiRequest and calls a single library entry point, and
 * that entry point is where a request body becomes a model. See
 * docs/analysis/api-server-read.md and docs/analysis/api-handler-read.md.
 *
 * Those entry points are ordinary C functions taking (ctx, req, resp), so a
 * fuzzer needs no socket, no event loop and no server lifecycle -- which is
 * what makes this cheap enough to be worth having. What it costs is that the
 * transport is not covered; Keel's own parsing is fuzzed upstream, not here.
 *
 * A module includes this after defining:
 *
 *   FUZZ_API_CTX_TYPE     the context type          (e.g. SGAPIContext)
 *   FUZZ_API_CREATE()     make one                  (e.g. sg_api_create())
 *   FUZZ_API_FREE(c)      destroy one               (e.g. sg_api_free(c))
 *   FUZZ_API_HANDLE(c,q,s) call it                  (e.g. sg_api_handle(...))
 *   FUZZ_API_ROUTES       a brace list of path strings
 *
 * What it looks for, beyond a crash: a handler that claims success must leave
 * a response the transport can actually send. The transports read status_code,
 * body and body_len without re-validating them, so a body pointer with no
 * length, or a length with no pointer, is a bug here even though nothing
 * dereferences it inside the handler.
 */
#ifndef FUZZ_API_HANDLER_H
#define FUZZ_API_HANDLER_H

#include "sh_api.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *const fuzz_api_routes[] = FUZZ_API_ROUTES;
#define FUZZ_API_NROUTES \
    ((int)(sizeof(fuzz_api_routes) / sizeof(fuzz_api_routes[0])))

/* Methods worth trying: the handlers route on these, and a POST body arriving
 * at a GET-only path is exactly the kind of mismatch worth exercising. */
static const char *const fuzz_api_methods[] = { "POST", "GET", "PUT", "DELETE" };
#define FUZZ_API_NMETHODS \
    ((int)(sizeof(fuzz_api_methods) / sizeof(fuzz_api_methods[0])))

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_API_CTX_TYPE *ctx;
    ShApiRequest req;
    ShApiResponse resp;
    char *body;
    const char *path;
    const char *method;
    int rc;

    /* Two control bytes pick the route and method; the rest is the body. A
     * fuzzer discovers the split on its own, and keeping it at the front means
     * a mutation of the body does not also change the endpoint. */
    if (size < 2) return 0;
    if (size > 4 * 1024 * 1024) return 0;

    path   = fuzz_api_routes[data[0] % FUZZ_API_NROUTES];
    method = fuzz_api_methods[data[1] % FUZZ_API_NMETHODS];

    data += 2;
    size -= 2;

    /* A separate allocation of exactly the body length, so a read one byte
     * past the end is a heap overflow ASan can see rather than a read into
     * whatever the fuzzer's own buffer happens to hold. The handlers take
     * (pointer, length) and must not assume a terminator. */
    body = (char *)malloc(size ? size : 1);
    if (!body) return 0;
    if (size) memcpy(body, data, size);

    ctx = FUZZ_API_CREATE();
    if (!ctx) { free(body); return 0; }

    memset(&req, 0, sizeof(req));
    req.method   = method;
    req.path     = path;
    req.query    = NULL;
    req.host     = "fuzz.invalid";
    req.body     = size ? body : NULL;
    req.body_len = size;

    memset(&resp, 0, sizeof(resp));
    rc = FUZZ_API_HANDLE(ctx, &req, &resp);

    if (rc == 0) {
        /*
         * The contract in sh_api.h: returning 0 means the response is filled
         * in and usable, including for error statuses. The transports send it
         * without re-checking, so an inconsistency here reaches the wire.
         */
        if (resp.status_code < 100 || resp.status_code > 599) abort();

        if (resp.body_len > 0 && !resp.body) abort();   /* length without data */
        if (resp.body && resp.body_len == 0) {
            /* A body pointer with no length is allowed only for the streaming
             * form, which carries a callback instead. */
            if (!resp.stream_fn) abort();
        }

        /* Touch the whole body, so a length longer than the allocation is a
         * heap overflow here rather than in a transport later. */
        if (resp.body && resp.body_len > 0) {
            volatile unsigned char sink = 0;
            size_t i;
            for (i = 0; i < resp.body_len; i++) {
                sink = (unsigned char)((const unsigned char *)resp.body)[i];
            }
            (void)sink;
        }
    }

    sh_api_response_free(&resp);
    FUZZ_API_FREE(ctx);
    free(body);
    return 0;
}

#endif /* FUZZ_API_HANDLER_H */
