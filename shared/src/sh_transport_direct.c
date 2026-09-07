/*
 * sh_transport_direct.c - In-process transport.
 *
 * The degenerate driver: no sockets, no event loop, no threads. It exists so
 * a module's handler can be exercised without a server, and so CI can prove
 * that handlers compile and run with no Keel headers on the include path.
 * If this file ever needs a transport-library header, the abstraction has
 * leaked. See docs/roadmaps/transport.md.
 */
#include "sh_transport.h"
#include "sh_transport_internal.h"

#include <stdlib.h>
#include <string.h>

/*
 * A producer that keeps answering MORE without finishing would spin here
 * forever. Cap it: any real producer reaches DONE in far fewer rounds, and
 * a runaway one is a bug worth surfacing as a failed stream.
 */
#define SH_DIRECT_STREAM_MAX_SPINS 100000UL

/* ============================================================================
 * Collecting stream
 *
 * A streaming handler writing to the direct transport has its output gathered
 * into a buffer, so the caller sees one unary response no matter which shape
 * the handler chose. That is what makes streaming handlers testable.
 * ============================================================================ */

typedef struct {
    ShApiStream base;       /* must stay first */
    uint8_t    *buf;
    size_t      len;
    size_t      cap;
    int         failed;     /* sticky: an allocation failed */
} DirectStream;

static int direct_stream_reserve(DirectStream *ds, size_t extra)
{
    size_t need;
    size_t cap;
    uint8_t *p;

    /* `extra` derives from a length the handler supplied. Left unchecked,
     * ds->len + extra + 1 wraps, `need` comes out small, and this function
     * reports success without growing the buffer -- after which the caller's
     * memcpy writes past the end. Check before adding, not after. */
    if (extra > (size_t)-1 - ds->len - 1) return -1;

    need = ds->len + extra + 1;          /* +1 for the trailing NUL */

    if (need <= ds->cap) return 0;

    cap = ds->cap ? ds->cap : 256;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) return -1;   /* overflow guard */
        cap *= 2;
    }

    p = (uint8_t *)realloc(ds->buf, cap);
    if (!p) return -1;

    ds->buf = p;
    ds->cap = cap;
    return 0;
}

static int direct_stream_send(ShApiStream *s, const char *event,
                              const void *data, size_t len)
{
    DirectStream *ds = (DirectStream *)s;
    size_t elen;

    if (!ds || ds->failed) return -1;

    /* Event names are recorded as an "event: <name>\n" prefix so a test can
     * assert on them. Transports with no notion of named events drop them;
     * here they are part of what the caller is checking. */
    if (event && *event) {
        elen = strlen(event);
        if (elen > (size_t)-1 - 8) { ds->failed = 1; return -1; }
        if (direct_stream_reserve(ds, elen + 8) != 0) { ds->failed = 1; return -1; }
        memcpy(ds->buf + ds->len, "event: ", 7); ds->len += 7;
        memcpy(ds->buf + ds->len, event, elen);  ds->len += elen;
        ds->buf[ds->len++] = '\n';
    }

    if (data && len) {
        if (direct_stream_reserve(ds, len) != 0) { ds->failed = 1; return -1; }
        memcpy(ds->buf + ds->len, data, len);
        ds->len += len;
    }

    ds->buf[ds->len] = '\0';
    return 0;
}

static int direct_stream_closed(const ShApiStream *s)
{
    const DirectStream *ds = (const DirectStream *)s;
    /* A collecting stream has no peer to hang up; it is only "closed" once a
     * write has failed and further sends would be pointless. */
    return ds ? ds->failed : 1;
}

static const ShApiStreamVTable direct_stream_vt = {
    direct_stream_send,
    direct_stream_closed
};

/* ============================================================================
 * Driver
 * ============================================================================ */

int sh_transport_direct_call(ShApiHandler handler, void *ctx,
                             const ShApiRequest *req, ShApiResponse *resp)
{
    int rc;

    if (!handler || !req || !resp) return -1;

    memset(resp, 0, sizeof(*resp));
    rc = handler(ctx, req, resp);
    if (rc != 0) return rc;

    if (!resp->stream_fn) return 0;   /* unary: nothing more to do */

    /* Streaming: drive the producer to completion and fold the result into the
     * unary fields, so every caller sees the same shape.
     *
     * The producer must not block (see sh_api.h), so calling it repeatedly in
     * a tight loop is exactly what it expects. The spin cap guards against a
     * producer that answers MORE forever without finishing: here that would
     * hang the caller, and on the Keel transport it would hang the event loop,
     * so it is treated as a producer error rather than trusted. */
    {
        DirectStream ds;
        ShApiStreamStatus srv = SH_API_STREAM_MORE;
        unsigned long spins = 0;

        memset(&ds, 0, sizeof(ds));
        ds.base.vt = &direct_stream_vt;

        while (srv == SH_API_STREAM_MORE && !ds.failed) {
            if (++spins > SH_DIRECT_STREAM_MAX_SPINS) {
                ds.failed = 1;
                break;
            }
            srv = resp->stream_fn(resp->stream_ctx, &ds.base);
        }

        if (resp->stream_free) resp->stream_free(resp->stream_ctx);
        resp->stream_fn = NULL;
        resp->stream_ctx = NULL;
        resp->stream_free = NULL;

        if (srv != SH_API_STREAM_DONE || ds.failed) {
            free(ds.buf);
            free(resp->body);
            resp->body = NULL;
            resp->body_len = 0;
            return -1;
        }

        free(resp->body);           /* ignored for streaming responses */
        resp->body = ds.buf;        /* ownership moves to the response */
        resp->body_len = ds.len;
    }

    return 0;
}

/*
 * There is nothing to listen on, so serve() is a no-op that reports success:
 * an embedded caller has already got the request and drives the handler with
 * sh_transport_direct_call(). Returning an error here would make the direct
 * transport unusable as a drop-in in a ShTransport-typed slot.
 */
static int direct_serve(const ShServeConfig *cfg, ShApiHandler handler, void *ctx)
{
    (void)cfg; (void)ctx;
    return handler ? 0 : -1;
}

static void direct_stop(void *server)
{
    (void)server;
}

const ShTransport sh_transport_direct = {
    "direct",
    direct_serve,
    direct_stop
};
