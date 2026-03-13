/*
 * Surge API Server
 *
 * REST API for VRP/PDPTW solving.
 * Uses Mongoose for HTTP serving.
 *
 * Features:
 * - Rate limiting (per-IP token bucket)
 * - Work queue with backpressure
 * - Socket timeout protection
 * - Structured logging
 * - Distributed tracing
 * - Prometheus metrics
 *
 * Architecture:
 *   Each solve request is fully self-contained: JSON in → SGContext build →
 *   solve → JSON out. No shared mutable state between requests.
 */

#include "surge.h"
#include "sg_api.h"
#include "mongoose.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>

/* Shared library includes */
#include "shared.h"
#include "sh_httpserver.h"
#include "sh_log.h"
#include "sh_trace.h"
#include "sh_metrics.h"
#include "sh_completion.h"
#include "sh_worker_pool.h"
#include "sh_json.h"

/* ============================================================================
 * Configuration
 * ============================================================================ */

typedef struct {
    ShServerConfig server;
    int num_workers;  /* Solve worker threads (0 = auto) */
} SurgeServerConfig;

static SurgeServerConfig s_config;

/* CORS configuration */
static ShCorsConfig s_cors;

/* ============================================================================
 * Global State
 *
 * DESIGN NOTE: Thread Safety
 *
 * Unlike Locus/Carta, Surge has no shared read-only index. Each solve
 * request creates its own SGContext, so there is no shared mutable state
 * between requests. The only shared resources are the work queue and
 * rate limiter, which are internally thread-safe.
 * ============================================================================ */

static volatile sig_atomic_t s_signo = 0;

/* Transport-agnostic API context */
static SGAPIContext *s_api_ctx = NULL;

/* Rate limiter instance */
static ShRateLimiter *s_rate_limiter = NULL;

/* Work queue instance */
static ShWorkQueue *s_work_queue = NULL;

/* Worker pool for solving */
static ShWorkerPool *s_worker_pool = NULL;

/* ============================================================================
 * Solve Work Item
 * ============================================================================ */

typedef struct {
    /* Request body (copied from HTTP request) */
    char *body;
    size_t body_len;

    /* Response (set by worker) */
    char *response_json;
    size_t response_len;
    int status_code;
    char error_msg[128];

    /* Completion signaling */
    ShCompletion completion;
} SolveWorkItem;

static void solve_work_item_init(SolveWorkItem *item) {
    memset(item, 0, sizeof(*item));
    item->status_code = 500;
    sh_completion_init(&item->completion);
}

static void solve_work_item_cleanup(SolveWorkItem *item) {
    sh_completion_cleanup(&item->completion);
    free(item->response_json);
    item->response_json = NULL;
    free(item->body);
    item->body = NULL;
}

static void signal_handler(int signo) {
    s_signo = signo;
}

/* ============================================================================
 * Configuration Loading
 * ============================================================================ */

static void init_surge_defaults(SurgeServerConfig *cfg) {
    sh_args_init(&cfg->server);

    /* Surge-specific defaults */
    cfg->server.port = 8085;
    cfg->server.rate_limit_rps = 10.0;       /* VRP solving is expensive */
    cfg->server.rate_limit_burst = 20.0;
    cfg->server.work_queue_depth = 64;
    cfg->server.work_queue_timeout = 60.0;   /* Solves can take time */

    cfg->num_workers = 0;  /* Auto-detect */
}

static void load_surge_env(SurgeServerConfig *cfg) {
    const char *val;

    sh_args_load_env(&cfg->server, SH_API_SURGE);

    if ((val = getenv("SURGE_NUM_WORKERS"))) {
        cfg->num_workers = sh_parse_int(val, 0, 0, 256);
    }

    /* CORS configuration */
    if ((val = getenv("SURGE_CORS_ORIGINS"))) {
        sh_cors_parse_origins(&s_cors, val);
    }
    if ((val = getenv("SURGE_CORS_METHODS"))) {
        sh_cors_set_methods(&s_cors, val);
    }
    if ((val = getenv("SURGE_CORS_HEADERS"))) {
        sh_cors_set_headers(&s_cors, val);
    }
    if ((val = getenv("SURGE_CORS_CREDENTIALS"))) {
        s_cors.allow_credentials = (sh_parse_int(val, 0, 0, 1) != 0);
    }
}

/* ============================================================================
 * HTTP Response Helpers
 * ============================================================================ */

static const char *get_origin_from_request(struct mg_http_message *hm) {
    struct mg_str *origin_hdr = hm ? mg_http_get_header(hm, "Origin") : NULL;
    if (origin_hdr && origin_hdr->len > 0) {
        static __thread char origin_buf[256];
        size_t len = origin_hdr->len < sizeof(origin_buf) - 1 ?
                     origin_hdr->len : sizeof(origin_buf) - 1;
        memcpy(origin_buf, origin_hdr->buf, len);
        origin_buf[len] = '\0';
        return origin_buf;
    }
    return NULL;
}

static void get_cors_preflight_headers(struct mg_http_message *hm,
                                       char *buf, size_t size) {
    sh_cors_preflight_headers(&s_cors, get_origin_from_request(hm), buf, size);
}

static void send_json_cors(struct mg_connection *c, struct mg_http_message *hm,
                           int status, const char *json) {
    sh_mg_reply_json(c, status, &s_cors, get_origin_from_request(hm), json);
}

static void send_error_cors(struct mg_connection *c, struct mg_http_message *hm,
                            int status, const char *message) {
    sh_mg_reply_error(c, status, &s_cors, get_origin_from_request(hm), message);
}

/* ============================================================================
 * Solve Worker
 * ============================================================================ */

/*
 * Process a solve request.
 * Called by the worker pool on a background thread.
 */
static void process_solve(SolveWorkItem *item) {
    int status_code = 500;
    size_t out_len = 0;

    char *result = sg_api_solve(item->body, item->body_len,
                                &status_code, &out_len);

    if (result) {
        item->response_json = result;
        item->response_len = out_len;
        item->status_code = status_code;
    } else {
        item->status_code = 500;
        snprintf(item->error_msg, sizeof(item->error_msg), "Solver internal error");
    }
}

/*
 * Worker pool callback — dispatches solve work items.
 */
static void solve_worker_callback(ShWorkItem *queue_item, void *ctx) {
    (void)ctx;

    SolveWorkItem *item = (SolveWorkItem *)queue_item->user_ctx;
    if (!item) {
        sh_workqueue_item_free(queue_item);
        return;
    }

    /* Check for expiry or cancellation */
    if (sh_workqueue_item_expired(s_work_queue, queue_item) ||
        sh_completion_is_cancelled(&item->completion)) {
        item->status_code = 504;
        snprintf(item->error_msg, sizeof(item->error_msg), "Request timeout");
        sh_completion_signal(&item->completion);
        sh_workqueue_item_free(queue_item);
        return;
    }

    /* Measure solve time */
    ShMetricsTimer solve_timer = sh_metrics_timer_start();

    process_solve(item);

    sh_metrics_timer_observe(solve_timer, "surge_solve_duration_ms",
                             "endpoint:solve", NULL);

    sh_completion_signal(&item->completion);
    sh_workqueue_item_free(queue_item);
}

/*
 * Submit solve work via work queue and send response.
 */
static int submit_solve_work(struct mg_connection *c, struct mg_http_message *hm,
                             SolveWorkItem *item) {
    ShWorkItem queue_item = {
        .data = NULL,
        .data_len = 0,
        .user_ctx = item
    };

    double pressure;
    if (!sh_workqueue_try_push(s_work_queue, &queue_item, &pressure)) {
        send_error_cors(c, hm, 503, "Server busy, try again later");
        return 0;
    }

    double timeout = s_config.server.work_queue_timeout;
    if (!sh_completion_wait(&item->completion, (int)(timeout * 1000))) {
        sh_completion_cancel(&item->completion);
        send_error_cors(c, hm, 504, "Solve timeout");
        return 0;
    }

    if (item->status_code >= 200 && item->status_code < 300) {
        if (item->response_json && item->response_len > 0) {
            send_json_cors(c, hm, item->status_code, item->response_json);
        } else {
            send_json_cors(c, hm, 200, "{}");
        }
    } else {
        if (item->response_json && item->response_len > 0) {
            send_json_cors(c, hm, item->status_code, item->response_json);
        } else {
            send_error_cors(c, hm, item->status_code,
                            item->error_msg[0] ? item->error_msg : "Internal error");
        }
    }

    return 1;
}

/* ============================================================================
 * Request Handlers
 * ============================================================================ */

static void handle_health(struct mg_connection *c, struct mg_http_message *hm) {
    sh_mg_handle_health(c, &s_cors, get_origin_from_request(hm),
                        "surge", sg_version());
}

static void handle_version(struct mg_connection *c, struct mg_http_message *hm) {
    size_t out_len;
    char *json = sg_api_version(&out_len);
    if (json) {
        send_json_cors(c, hm, 200, json);
        free(json);
    } else {
        send_error_cors(c, hm, 500, "Failed to generate version response");
    }
}

static void handle_solve(struct mg_connection *c, struct mg_http_message *hm) {
    /* Verify POST method */
    if (!mg_match(hm->method, mg_str("POST"), NULL)) {
        send_error_cors(c, hm, 405, "Method not allowed. Use POST.");
        return;
    }

    if (hm->body.len == 0) {
        send_error_cors(c, hm, 400, "Empty request body");
        return;
    }

    /* Use work queue if enabled */
    if (s_work_queue) {
        SolveWorkItem item;
        solve_work_item_init(&item);

        /* Copy body for worker thread */
        item.body = malloc(hm->body.len + 1);
        if (!item.body) {
            solve_work_item_cleanup(&item);
            send_error_cors(c, hm, 500, "Out of memory");
            return;
        }
        memcpy(item.body, hm->body.buf, hm->body.len);
        item.body[hm->body.len] = '\0';
        item.body_len = hm->body.len;

        submit_solve_work(c, hm, &item);
        solve_work_item_cleanup(&item);
        return;
    }

    /* Fallback: direct execution (work queue disabled) */
    int status_code = 500;
    size_t out_len = 0;
    char *result = sg_api_solve(hm->body.buf, hm->body.len,
                                &status_code, &out_len);

    if (result) {
        send_json_cors(c, hm, status_code, result);
        free(result);
    } else {
        send_error_cors(c, hm, 500, "Solver internal error");
    }
}

static void handle_stats(struct mg_connection *c, struct mg_http_message *hm) {
    SGAPIRequest req = { .path = "/api/v1/stats" };
    SGAPIResponse resp = {0};

    if (sg_api_handle(s_api_ctx, &req, &resp) == 0 && resp.body) {
        send_json_cors(c, hm, resp.status_code, resp.body);
    } else {
        send_error_cors(c, hm, 500, "Failed to generate stats response");
    }
    sg_api_response_free(&resp);
}

static void handle_metrics(struct mg_connection *c) {
    sh_mg_handle_metrics(c);
}

/* ============================================================================
 * Request Router
 * ============================================================================ */

static void handle_request(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_ACCEPT) {
        sh_mg_set_write_timeout(c, 5000);
        return;
    }

    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        ShMetricsTimer req_timer = sh_metrics_timer_start();

        /* Extract or generate trace ID */
        sh_trace_from_headers(sh_mg_trace_header_getter, hm);

        /* Rate limiting */
        if (!sh_mg_check_rate_limit(c, s_rate_limiter, &s_cors,
                                    get_origin_from_request(hm))) {
            SH_LOG_WARN("Rate limit exceeded", "status", "429");
            sh_metrics_counter_inc("http_requests_total", 1,
                                   "status:429", "endpoint:ratelimit", NULL);
            sh_trace_clear();
            return;
        }

        /* CORS preflight */
        if (mg_match(hm->method, mg_str("OPTIONS"), NULL)) {
            char cors_headers[512];
            get_cors_preflight_headers(hm, cors_headers, sizeof(cors_headers));
            mg_http_reply(c, 204, cors_headers, "");
            sh_trace_clear();
            return;
        }

        /* Route requests */
        if (mg_match(hm->uri, mg_str("/api/v1/health"), NULL)) {
            handle_health(c, hm);
            sh_metrics_counter_inc("http_requests_total", 1,
                                   "status:200", "endpoint:health", NULL);
        } else if (mg_match(hm->uri, mg_str("/api/v1/version"), NULL)) {
            handle_version(c, hm);
            sh_metrics_counter_inc("http_requests_total", 1,
                                   "status:200", "endpoint:version", NULL);
        } else if (mg_match(hm->uri, mg_str("/api/v1/stats"), NULL)) {
            handle_stats(c, hm);
            sh_metrics_counter_inc("http_requests_total", 1,
                                   "status:200", "endpoint:stats", NULL);
        } else if (mg_match(hm->uri, mg_str("/api/v1/solve"), NULL)) {
            handle_solve(c, hm);
            sh_metrics_timer_observe(req_timer, "http_request_duration_ms",
                                     "endpoint:solve", NULL);
            sh_metrics_counter_inc("http_requests_total", 1,
                                   "status:200", "endpoint:solve", NULL);
        } else if (mg_match(hm->uri, mg_str("/metrics"), NULL)) {
            handle_metrics(c);
        } else {
            send_error_cors(c, hm, 404, "Not found");
            sh_metrics_counter_inc("http_requests_total", 1,
                                   "status:404", "endpoint:unknown", NULL);
        }

        sh_trace_clear();
    }
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void print_usage(const char *prog) {
    printf("Surge VRP Solver Server\n\n");
    printf("Usage: %s [options]\n\n", prog);

    sh_args_usage(prog, NULL);

    printf("Surge-specific options:\n");
    printf("  --workers N          Solve worker threads (default: auto)\n");
    printf("\n");
    printf("Surge-specific environment variables:\n");
    printf("  SURGE_NUM_WORKERS    Worker thread count (0 = auto)\n");
    printf("\n");
    printf("CORS configuration:\n");
    printf("  SURGE_CORS_ORIGINS      Comma-separated allowed origins (empty = allow all)\n");
    printf("  SURGE_CORS_METHODS      Allowed HTTP methods (default: GET, POST, OPTIONS)\n");
    printf("  SURGE_CORS_HEADERS      Allowed request headers\n");
    printf("  SURGE_CORS_CREDENTIALS  Allow credentials (default: 0)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s                           # Start on port 8085\n", prog);
    printf("  %s -p 9000                   # Start on custom port\n", prog);
    printf("  %s --workers 4               # Use 4 solve workers\n", prog);
}

int main(int argc, char *argv[]) {
    /* Initialize logging */
    ShLogConfig log_cfg = SH_LOG_CONFIG_DEFAULT;
    log_cfg.service = "surge";
    log_cfg.version = sg_version();
    sh_log_init(&log_cfg);

    /* Initialize defaults */
    init_surge_defaults(&s_config);
    sh_cors_init(&s_cors);

    /* Load config from environment */
    load_surge_env(&s_config);

    /* Parse command line arguments */
    int arg_index = sh_args_parse(&s_config.server, argc, argv);
    if (arg_index == -2) {
        print_usage(argv[0]);
        return 0;
    }

    /* Parse Surge-specific arguments */
    for (int i = (arg_index > 0 ? arg_index : 1); i < argc; i++) {
        if (strcmp(argv[i], "--workers") == 0) {
            if (++i < argc) s_config.num_workers = sh_parse_int(argv[i], 0, 0, 256);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    /* Initialize API context */
    s_api_ctx = sg_api_create();
    if (!s_api_ctx) {
        fprintf(stderr, "Error: Failed to create API context\n");
        return 1;
    }

    /* Initialize rate limiter */
    if (s_config.server.rate_limit_enabled) {
        s_rate_limiter = sh_ratelimit_create(s_config.server.rate_limit_rps,
                                             s_config.server.rate_limit_burst, 4096);
        if (s_rate_limiter) {
            printf("Rate limit: %.0f RPS, burst %.0f\n",
                   s_config.server.rate_limit_rps, s_config.server.rate_limit_burst);
        } else {
            fprintf(stderr, "Warning: Failed to create rate limiter\n");
        }
    } else {
        printf("Rate limit: disabled\n");
    }

    /* Initialize work queue and worker pool */
    if (s_config.server.work_queue_enabled) {
        s_work_queue = sh_workqueue_create(s_config.server.work_queue_depth,
                                           s_config.server.work_queue_timeout);
        if (s_work_queue) {
            ShWorkerPoolConfig pool_cfg = {
                .queue = s_work_queue,
                .callback = solve_worker_callback,
                .ctx = NULL,
                .poll_timeout_ms = 100
            };
            s_worker_pool = sh_worker_pool_create(s_config.num_workers, &pool_cfg);
            if (s_worker_pool) {
                printf("Work queue: depth %zu, timeout %.1fs, %d workers\n",
                       s_config.server.work_queue_depth,
                       s_config.server.work_queue_timeout,
                       sh_worker_pool_size(s_worker_pool));
            } else {
                fprintf(stderr, "Warning: Failed to create worker pool\n");
                sh_workqueue_free(s_work_queue);
                s_work_queue = NULL;
            }
        } else {
            fprintf(stderr, "Warning: Failed to create work queue\n");
        }
    } else {
        printf("Work queue: disabled\n");
    }

    /* Initialize metrics */
    ShMetricsConfig metrics_cfg = SH_METRICS_CONFIG_DEFAULT;
    metrics_cfg.service = "surge";
    if (sh_metrics_init(&metrics_cfg) == 0) {
        const char *statsd_host = getenv("SH_METRICS_STATSD_HOST");
        if (statsd_host && statsd_host[0]) {
            printf("Metrics: StatsD enabled (%s:%d)\n", statsd_host,
                   metrics_cfg.statsd_port ? metrics_cfg.statsd_port : 8125);
        } else {
            printf("Metrics: Prometheus endpoint at /metrics\n");
        }
    }

    SH_LOG_INFO("Server initializing",
                "port", s_config.server.host);

    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Start HTTP server */
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    char listen_addr[SH_URL_MAX];
    snprintf(listen_addr, sizeof(listen_addr), "http://%s:%d",
             s_config.server.host, s_config.server.port);

    struct mg_connection *conn = mg_http_listen(&mgr, listen_addr,
                                                handle_request, NULL);
    if (!conn) {
        fprintf(stderr, "Error: Failed to listen on %s\n", listen_addr);
        if (s_worker_pool) {
            sh_worker_pool_stop(s_worker_pool);
            sh_worker_pool_join(s_worker_pool);
            sh_worker_pool_free(s_worker_pool);
        }
        sh_workqueue_free(s_work_queue);
        sh_ratelimit_free(s_rate_limiter);
        return 1;
    }

    printf("\nSurge VRP Solver Server v%s\n", sg_version());
    printf("Listening on http://%s:%d\n",
           s_config.server.host, s_config.server.port);
    printf("\nEndpoints:\n");
    printf("  POST /api/v1/solve     - Solve VRP problem\n");
    printf("  GET  /api/v1/health    - Health check\n");
    printf("  GET  /api/v1/version   - Version info\n");
    printf("  GET  /api/v1/stats     - Statistics\n");
    printf("  GET  /metrics          - Prometheus metrics\n");
    printf("\nPress Ctrl+C to stop.\n\n");

    /* Event loop */
    while (s_signo == 0) {
        mg_mgr_poll(&mgr, 100);
    }

    printf("\nShutting down...\n");

    /* Shutdown worker pool first */
    if (s_worker_pool) {
        sh_worker_pool_stop(s_worker_pool);
        sh_worker_pool_join(s_worker_pool);
    }

    /* Print work queue stats */
    if (s_work_queue) {
        ShWorkQueueStats wq_stats;
        sh_workqueue_stats(s_work_queue, &wq_stats);
        printf("Work queue: %lu pushed, %lu popped, %lu dropped, %lu expired\n",
               (unsigned long)wq_stats.total_pushed,
               (unsigned long)wq_stats.total_popped,
               (unsigned long)wq_stats.total_dropped,
               (unsigned long)wq_stats.total_expired);
    }

    /* Print rate limiter stats */
    if (s_rate_limiter) {
        ShRateLimitStats rl_stats;
        sh_ratelimit_stats(s_rate_limiter, &rl_stats);
        printf("Rate limiter: %lu allowed, %lu denied\n",
               (unsigned long)rl_stats.requests_allowed,
               (unsigned long)rl_stats.requests_denied);
    }

    /* Cleanup */
    mg_mgr_free(&mgr);
    sh_worker_pool_free(s_worker_pool);
    sh_workqueue_free(s_work_queue);
    sh_ratelimit_free(s_rate_limiter);
    sg_api_free(s_api_ctx);

    SH_LOG_INFO("Server shutdown complete");
    sh_metrics_shutdown();
    sh_log_shutdown();

    return 0;
}
