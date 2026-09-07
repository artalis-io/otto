/*
 * sh_keelasync.c - The Keel async protocol, owned once.
 *
 * OWNERSHIP / LIFETIME
 *   The call context is freed in exactly one place: done_fn when the item
 *   ran, or cancel_fn when the pool dropped it at shutdown before starting.
 *   on_cancel and on_deadline never free -- work_fn may still be running on a
 *   worker -- they only set `detached`, a plain int touched only on the event
 *   loop thread.
 *
 * See docs/roadmaps/transport.md.
 */
#include "sh_keelasync.h"
#include "sh_keelserver.h"

#include <keel/async.h>
#include <keel/clock.h>

#include <stdlib.h>
#include <string.h>

typedef struct {
    KlAsyncOp op;                   /* must stay first-ish; recovered via offsetof */

    const ShKeelAsync *cfg;
    KlHttpRequest *req;             /* lives in the conn; valid while suspended */

    /* Owned copies of the request. The originals belong to the connection,
     * which the worker outlives. */
    char *path;
    char *query;
    char *host;
    char *body;
    size_t body_len;
    const char *method;             /* static string from Keel, not owned */

    ShApiHandler handler;
    void *handler_ctx;

    ShApiResponse resp;             /* filled by the handler on the worker */
    int handler_rc;

    int detached;                   /* conn died, or we already replied 504 */
} KeelCall;

/* ------------------------------------------------------------------------ */

static void call_free(KeelCall *c)
{
    if (!c) return;
    sh_api_response_free(&c->resp);
    free(c->path);
    free(c->query);
    free(c->host);
    free(c->body);
    free(c);
}

/* strdup that tolerates NULL and reports failure distinguishably. */
static int dup_opt(char **dst, const char *src)
{
    size_t n;
    if (!src) { *dst = NULL; return 0; }
    n = strlen(src);
    *dst = (char *)malloc(n + 1);
    if (!*dst) return -1;
    memcpy(*dst, src, n + 1);
    return 0;
}

static void reply_from(const ShKeelAsync *cfg, KlHttpResponse *res,
                       const ShApiResponse *r)
{
    if (r->body && r->body_len > 0) {
        sh_kl_reply_body(res, r->status_code,
                         r->content_type ? r->content_type : "application/json",
                         cfg->cors, NULL,
                         (const char *)r->body, r->body_len);
    } else {
        sh_kl_reply_error(res, r->status_code ? r->status_code : 500,
                          cfg->cors, NULL, "Processing failed");
    }
}

/* ------------------------------------------------------------------------
 * Pool callbacks
 * ------------------------------------------------------------------------ */

/* Worker thread. Touches only this context -- no Keel state, no globals. */
static void call_work_fn(void *user_data)
{
    KeelCall *c = (KeelCall *)user_data;
    ShApiRequest req;

    memset(&req, 0, sizeof(req));
    req.method   = c->method;
    req.path     = c->path;
    req.query    = c->query;
    req.host     = c->host;
    req.body     = c->body;
    req.body_len = c->body_len;

    memset(&c->resp, 0, sizeof(c->resp));
    c->handler_rc = c->handler(c->handler_ctx, &req, &c->resp);
}

/* Event loop thread: write the response and resume the connection. */
static void call_done_fn(void *user_data)
{
    KeelCall *c = (KeelCall *)user_data;

    if (c->cfg->stats) c->cfg->stats->popped++;

    /* Connection gone, or on_deadline already replied. Nothing to write. */
    if (c->detached) {
        call_free(c);
        return;
    }

    {
        KlHttpResponse *res = kl_http_conn_response(c->op.conn);
        if (c->handler_rc != 0) {
            sh_kl_reply_error(res, 500, c->cfg->cors, NULL, "Processing failed");
        } else {
            reply_from(c->cfg, res, &c->resp);
        }
    }

    kl_async_complete(c->cfg->server, &c->op);
    call_free(c);
}

/* Pool shutdown dropped the item before it started; no worker will touch it. */
static void call_cancel_fn(void *user_data)
{
    call_free((KeelCall *)user_data);
}

/* ------------------------------------------------------------------------
 * Async op callbacks
 * ------------------------------------------------------------------------ */

/*
 * Declare the send.
 *
 * kl_async_complete() re-arms the fd but leaves the connection SUSPENDED
 * unless on_resume says what happens next; without this the response is never
 * written and the client hangs. Keel's examples/thread_pool.c and
 * examples/async_thread_pool.c leave this a no-op and hang for exactly that
 * reason -- tests/smoke_iouring_async.c is the correct reference, and
 * kl_http_request_send_response() is its public equivalent.
 */
static void call_on_resume(KlAsyncOp *op, void *ud)
{
    KeelCall *c = (KeelCall *)((char *)op - offsetof(KeelCall, op));
    (void)ud;
    kl_http_request_send_response(c->req);
}

/* Connection died while suspended. The worker may still be running, so this
 * must not free anything -- done_fn will, once work_fn returns. */
static void call_on_cancel(KlAsyncOp *op, void *ud)
{
    KeelCall *c = (KeelCall *)((char *)op - offsetof(KeelCall, op));
    (void)ud;
    c->detached = 1;
}

/* Deadline exceeded: reply 504 now, let done_fn free the context later. */
static void call_on_deadline(KlAsyncOp *op, void *ud)
{
    KeelCall *c = (KeelCall *)((char *)op - offsetof(KeelCall, op));
    (void)ud;

    if (c->detached) return;
    c->detached = 1;
    if (c->cfg->stats) c->cfg->stats->expired++;

    sh_kl_reply_error(kl_http_conn_response(op->conn), 504,
                      c->cfg->cors, NULL, "Gateway timeout");
    kl_async_complete(c->cfg->server, op);
}

/* ------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------ */

void sh_keel_async_dispatch(const ShKeelAsync *cfg,
                            KlHttpRequest *req,
                            KlHttpResponse *res,
                            ShApiHandler handler,
                            void *handler_ctx,
                            const ShApiRequest *api_req)
{
    KeelCall *c;

    if (!cfg || !cfg->server || !req || !res || !handler || !api_req) {
        if (res) kl_http_response_status(res, 500);
        return;
    }

    /* No pool: run inline on the event loop. Cheap endpoints (health, stats)
     * take this path deliberately so they never queue behind a solve. */
    if (!cfg->pool) {
        ShApiResponse r;
        memset(&r, 0, sizeof(r));
        if (handler(handler_ctx, api_req, &r) != 0) {
            sh_kl_reply_error(res, 500, cfg->cors, NULL, "Processing failed");
        } else {
            reply_from(cfg, res, &r);
        }
        sh_api_response_free(&r);
        return;
    }

    c = (KeelCall *)calloc(1, sizeof(*c));
    if (!c) {
        sh_kl_reply_error(res, 500, cfg->cors, NULL, "Memory allocation failed");
        return;
    }

    /* Copy every borrowed string before the worker can outlive the conn. This
     * is the requirement that the Carta SEGV came from ignoring. */
    if (dup_opt(&c->path,  api_req->path)  != 0 ||
        dup_opt(&c->query, api_req->query) != 0 ||
        dup_opt(&c->host,  api_req->host)  != 0) {
        call_free(c);
        sh_kl_reply_error(res, 500, cfg->cors, NULL, "Memory allocation failed");
        return;
    }
    if (api_req->body && api_req->body_len > 0) {
        if (api_req->body_len == (size_t)-1) {   /* would wrap the +1 below */
            call_free(c);
            sh_kl_reply_error(res, 400, cfg->cors, NULL, "Body too large");
            return;
        }
        c->body = (char *)malloc(api_req->body_len + 1);
        if (!c->body) {
            call_free(c);
            sh_kl_reply_error(res, 500, cfg->cors, NULL, "Memory allocation failed");
            return;
        }
        memcpy(c->body, api_req->body, api_req->body_len);
        c->body[api_req->body_len] = '\0';
        c->body_len = api_req->body_len;
    }

    c->cfg = cfg;
    c->req = req;
    c->method = api_req->method;   /* static ("GET"/"POST"), safe to borrow */
    c->handler = handler;
    c->handler_ctx = handler_ctx;
    c->handler_rc = -1;
    c->resp.status_code = 500;

    c->op.on_resume   = call_on_resume;
    c->op.on_cancel   = call_on_cancel;
    c->op.on_deadline = call_on_deadline;
    if (cfg->timeout_s > 0.0) {
        c->op.deadline_ms = kl_monotonic_ms() +
                            (uint64_t)(cfg->timeout_s * 1000.0);
    }

    if (kl_async_suspend(cfg->server, kl_http_request_conn(req), &c->op) < 0) {
        call_free(c);
        sh_kl_reply_error(res, 500, cfg->cors, NULL, "Failed to suspend request");
        return;
    }

    {
        KlWorkItem item;
        memset(&item, 0, sizeof(item));
        item.work_fn   = call_work_fn;
        item.done_fn   = call_done_fn;
        item.cancel_fn = call_cancel_fn;
        item.user_data = c;

        if (kl_thread_pool_submit(cfg->pool, &item) < 0) {
            /* Queue full: backpressure. The op is suspended, so complete it
             * before freeing -- and mark detached so nothing else replies. */
            if (cfg->stats) cfg->stats->dropped++;
            c->detached = 1;
            sh_kl_reply_error(res, 503, cfg->cors, NULL,
                              "Service unavailable - queue full");
            kl_async_complete(cfg->server, &c->op);
            call_free(c);
            return;
        }
    }

    if (cfg->stats) cfg->stats->pushed++;
}
