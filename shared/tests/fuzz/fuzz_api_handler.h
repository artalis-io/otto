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
 * A module whose context needs building -- a graph, an index, a PBF --
 * defines FUZZ_API_CREATE() to return a prepared one. Building it per
 * execution would dominate the run, so the three that need it build once
 * in a file-scope helper and hand back the same pointer, which is why
 * FUZZ_API_FREE is a no-op there.
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

/*
 * Optional: path prefixes whose tail comes from the input.
 *
 * Most handlers dispatch on a fixed path and take their parameters from the
 * query string or the body, so the harness picks a path from FUZZ_API_ROUTES
 * and fuzzes the other two. Carta does not: /tiles/{z}/{x}/{y}.{ext} carries
 * its parameters in the path itself, parsed by hand, and fuzzing only the
 * query there would leave the one parser that matters untouched.
 *
 * A module lists such prefixes -- comma-terminated, since the list is empty
 * for everyone else -- and the harness appends the input to one of them and
 * sends no query and no body.
 */
#ifndef FUZZ_API_PATH_PREFIXES
#define FUZZ_API_PATH_PREFIXES /* none; the array holds just its terminator */
#endif
static const char *const fuzz_api_path_prefixes[] = {
    FUZZ_API_PATH_PREFIXES NULL
};
#define FUZZ_API_NPREFIXES                          \
    ((int)(sizeof(fuzz_api_path_prefixes)           \
           / sizeof(fuzz_api_path_prefixes[0])) - 1)

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
    char *query;
    char *path_buf;
    const char *path;
    int sel;
    const char *method;
    int is_query;
    int rc;

    /* Two control bytes pick the route and method; the rest is the input. A
     * fuzzer discovers the split on its own, and keeping it at the front means
     * a mutation of the input does not also change the endpoint. */
    if (size < 2) return 0;
    if (size > 4 * 1024 * 1024) return 0;

    method = fuzz_api_methods[data[1] % FUZZ_API_NMETHODS];
    sel    = data[0] % (FUZZ_API_NROUTES + FUZZ_API_NPREFIXES);

    data += 2;
    size -= 2;

    body     = NULL;
    query    = NULL;
    path_buf = NULL;

    if (sel >= FUZZ_API_NROUTES) {
        /*
         * A prefix route: the input is the rest of the request target. It is
         * cut at the first NUL, because a path is a C string by the time a
         * handler sees it -- the same truncation a real server would do.
         *
         * Then it is split at the first '?', exactly as a server splits a
         * request target into path and query. That is not a detail worth
         * skipping: carta's .txt tile route reads req->query as well as the
         * coordinates in its path, so a harness that only filled one of them
         * would leave the other unreached. The fuzzer finds the '?' itself.
         */
        const char *prefix = fuzz_api_path_prefixes[sel - FUZZ_API_NROUTES];
        size_t plen = strlen(prefix);
        char *qmark;

        path_buf = (char *)malloc(plen + size + 1);
        if (!path_buf) return 0;
        memcpy(path_buf, prefix, plen);
        if (size) memcpy(path_buf + plen, data, size);
        path_buf[plen + size] = '\0';
        path = path_buf;

        qmark = strchr(path_buf, '?');
        if (qmark) {
            *qmark = '\0';                  /* path ends here ... */
            query  = qmark + 1;             /* ... and the query follows */
        }

        size = 0;   /* nothing left over for the body */
    } else {
        path = fuzz_api_routes[sel];
    }

    /*
     * Where the input goes follows the method, because that is where it comes
     * from in reality: a GET carries its parameters in the query string and a
     * POST carries them in the body. Putting the bytes in the wrong one would
     * leave half of each handler unreached -- velo, locus and carta take query
     * strings, surge, ralph and fuelwise take JSON bodies, and several accept
     * both.
     *
     * The body is a separate allocation of exactly the input length, so a read
     * one byte past the end is a heap overflow ASan can see rather than a read
     * into whatever the fuzzer's own buffer holds. Handlers take
     * (pointer, length) there and must not assume a terminator.
     *
     * A query string is a C string by contract, so that one is terminated --
     * but it is allocated to fit exactly, so an overrun past the NUL is still
     * a heap error rather than a walk into the fuzzer's buffer.
     */
    is_query = (strcmp(method, "GET") == 0 || strcmp(method, "DELETE") == 0);

    if (path_buf) {
        is_query = 0;           /* the input went into the path */
    } else if (is_query) {
        query = (char *)malloc(size + 1);
        if (!query) return 0;
        if (size) memcpy(query, data, size);
        query[size] = '\0';
        body = NULL;
    } else {
        body = (char *)malloc(size ? size : 1);
        if (!body) return 0;
        if (size) memcpy(body, data, size);
    }

    ctx = FUZZ_API_CREATE();
    if (!ctx) {
        free(body);
        if (!path_buf) free(query);
        free(path_buf);
        return 0;
    }

    memset(&req, 0, sizeof(req));
    req.method   = method;
    req.path     = path;
    req.query    = query;
    req.host     = "fuzz.invalid";
    req.body     = (!is_query && size) ? body : NULL;
    req.body_len = is_query ? 0 : size;

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
    /* In prefix mode the query points into path_buf rather than owning
     * anything, so only the allocation itself is released. */
    if (!path_buf) free(query);
    free(path_buf);
    return 0;
}

#endif /* FUZZ_API_HANDLER_H */
