/*
 * FuelWise REST API Server
 *
 * A lightweight HTTP API for fuel optimization using mongoose.
 * Features rate limiting, work queue for CPU-intensive operations,
 * and configurable through CLI args and environment variables.
 *
 * Uses the transport-agnostic fw_api_handle() for core processing.
 *
 * Endpoints:
 *   GET  /api/v1/health         - Health check (bypasses queue)
 *   GET  /api/v1/stats          - Server statistics (bypasses queue)
 *   POST /api/v1/filter         - Filter stations to route
 *   POST /api/v1/solve          - Solve refueling problem
 *   POST /api/v1/optimize       - Full optimization pipeline
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <stdint.h>
#include "mongoose.h"
#include "fuelwise.h"
#include "fw_api.h"  /* Transport-agnostic API handler */
#include "shared.h"  /* For sh_ratelimit, sh_workqueue, sh_args */
#include "sh_httpserver.h"  /* For sh_mg_set_write_timeout */
#include "sh_completion.h"  /* For ShCompletion */
#include "sh_worker_pool.h" /* For ShWorkerPool */
#include "sh_log.h"
#include "sh_trace.h"
#include "sh_metrics.h"

/* ============================================================================
 * Configuration
 * ============================================================================ */

/* Global state */
static volatile sig_atomic_t s_signo = 0;

/* Rate limiter instance */
static ShRateLimiter *s_rate_limiter = NULL;

/* Work queue instance */
static ShWorkQueue *s_work_queue = NULL;

/* Server configuration (from sh_args) */
static ShServerConfig s_config;

/* CORS configuration */
static ShCorsConfig s_cors;

/* Work item for CPU-intensive operations */
typedef struct {
    char *request_path;      /* Copy of request path (owned) */
    char *request_body;      /* Copy of request body (owned) */
    size_t request_len;

    /* Response buffer (set by worker) */
    char *response_data;
    size_t response_size;
    int status_code;

    /* Completion signaling (uses shared library) */
    ShCompletion completion;
} SolveWorkItem;

/* Worker pool (uses shared library) */
static ShWorkerPool *s_worker_pool = NULL;

/* Signal handler */
static void signal_handler(int signo) {
    s_signo = signo;
}

/* JSON parsing and request handling moved to fw_api.c (transport-agnostic) */

/* ============================================================================
 * Response Building
 * ============================================================================ */

/* Build error response - uses shared helper */
static void send_error(struct mg_connection *c, int status, const char *message) {
    sh_mg_reply_error(c, status, &s_cors, NULL, message);
}

/* Build success response with JSON body - uses shared helper */
static void send_json(struct mg_connection *c, const char *json) {
    sh_mg_reply_json(c, 200, &s_cors, NULL, json);
}

/* Build response with custom status code and JSON body - uses shared helper */
static void send_json_status(struct mg_connection *c, int status, const char *json) {
    sh_mg_reply_json(c, status, &s_cors, NULL, json);
}

/* Core processing functions moved to fw_api.c (transport-agnostic) */

/* ============================================================================
 * Worker Pool Callback
 * ============================================================================ */

static void worker_callback(ShWorkItem *item, void *ctx) {
    (void)ctx;

    /* Check if item expired */
    if (sh_workqueue_item_expired(s_work_queue, item)) {
        sh_workqueue_item_free(item);
        return;
    }

    /* Get the work item */
    SolveWorkItem *work = (SolveWorkItem *)item->user_ctx;
    if (!work) {
        sh_workqueue_item_free(item);
        return;
    }

    /* Check if HTTP handler already timed out and cancelled */
    if (sh_completion_is_cancelled(&work->completion)) {
        /* Clean up the cancelled work item */
        free(work->request_path);
        free(work->request_body);
        sh_completion_cleanup(&work->completion);
        free(work);
        sh_workqueue_item_free(item);
        return;
    }

    /* Build API request and call transport-agnostic handler */
    FWAPIRequest req = {
        .path = work->request_path,
        .query = NULL,
        .body = work->request_body,
        .body_len = work->request_len,
        .host = NULL
    };

    FWAPIResponse resp;
    fw_api_handle(NULL, &req, &resp);

    /* Store result and signal completion */
    work->response_data = resp.body;
    work->response_size = resp.body_len;
    work->status_code = resp.status_code;
    sh_completion_signal(&work->completion);

    sh_workqueue_item_free(item);
}

/* ============================================================================
 * API Handlers
 * ============================================================================ */

/* GET /api/v1/health - bypasses work queue, uses shared helper */
static void handle_health(struct mg_connection *c, struct mg_http_message *hm) {
    (void)hm;
    sh_mg_handle_health(c, &s_cors, NULL, "fuelwise-api", fw_version());
}

/* GET /api/v1/stats - bypasses work queue */
static void handle_stats(struct mg_connection *c, struct mg_http_message *hm) {
    (void)hm;
    char response[2048];
    size_t pos = 0;
    int n;

    n = snprintf(response + pos, sizeof(response) - pos,
        "{\n"
        "  \"service\": \"fuelwise-api\",\n"
        "  \"version\": \"%s\",\n",
        fw_version());
    if (n > 0 && (size_t)n < sizeof(response) - pos) pos += (size_t)n;

    /* Work queue stats */
    if (s_work_queue) {
        ShWorkQueueStats wq_stats;
        sh_workqueue_stats(s_work_queue, &wq_stats);
        n = snprintf(response + pos, sizeof(response) - pos,
            "  \"work_queue\": {\n"
            "    \"enabled\": true,\n"
            "    \"depth\": %zu,\n"
            "    \"capacity\": %zu,\n"
            "    \"pushed\": %llu,\n"
            "    \"popped\": %llu,\n"
            "    \"dropped\": %llu,\n"
            "    \"expired\": %llu,\n"
            "    \"timeout_sec\": %.1f\n"
            "  },\n",
            wq_stats.current_depth,
            wq_stats.max_capacity,
            (unsigned long long)wq_stats.total_pushed,
            (unsigned long long)wq_stats.total_popped,
            (unsigned long long)wq_stats.total_dropped,
            (unsigned long long)wq_stats.total_expired,
            wq_stats.timeout_sec);
    } else {
        n = snprintf(response + pos, sizeof(response) - pos,
            "  \"work_queue\": {\n"
            "    \"enabled\": false\n"
            "  },\n");
    }
    if (n > 0 && (size_t)n < sizeof(response) - pos) pos += (size_t)n;

    /* Rate limit stats */
    if (s_rate_limiter) {
        ShRateLimitStats rl_stats;
        sh_ratelimit_stats(s_rate_limiter, &rl_stats);
        n = snprintf(response + pos, sizeof(response) - pos,
            "  \"rate_limit\": {\n"
            "    \"enabled\": true,\n"
            "    \"rps\": %.1f,\n"
            "    \"burst\": %.1f,\n"
            "    \"allowed\": %llu,\n"
            "    \"denied\": %llu,\n"
            "    \"active_entries\": %zu,\n"
            "    \"evictions\": %zu\n"
            "  }\n",
            s_config.rate_limit_rps,
            s_config.rate_limit_burst,
            (unsigned long long)rl_stats.requests_allowed,
            (unsigned long long)rl_stats.requests_denied,
            rl_stats.active_entries,
            rl_stats.evictions);
    } else {
        n = snprintf(response + pos, sizeof(response) - pos,
            "  \"rate_limit\": {\n"
            "    \"enabled\": false\n"
            "  }\n");
    }
    if (n > 0 && (size_t)n < sizeof(response) - pos) pos += (size_t)n;

    snprintf(response + pos, sizeof(response) - pos, "}\n");
    send_json(c, response);
}

/* Generic handler that uses work queue */
static void handle_via_queue(struct mg_connection *c, struct mg_http_message *hm, const char *path) {
    if (!s_work_queue) {
        /* Work queue disabled - process synchronously using transport-agnostic handler */
        FWAPIRequest req = {
            .path = path,
            .query = NULL,
            .body = hm->body.buf,
            .body_len = hm->body.len,
            .host = NULL
        };

        FWAPIResponse resp;
        fw_api_handle(NULL, &req, &resp);

        if (resp.body) {
            send_json_status(c, resp.status_code, resp.body);
            fw_api_response_free(&resp);
        } else {
            send_error(c, 500, "Processing failed");
        }
        return;
    }

    /* Create work item */
    SolveWorkItem *work = calloc(1, sizeof(SolveWorkItem));
    if (!work) {
        send_error(c, 500, "Memory allocation failed");
        return;
    }

    /* Copy path */
    work->request_path = strdup(path);
    if (!work->request_path) {
        free(work);
        send_error(c, 500, "Memory allocation failed");
        return;
    }

    /* Copy body */
    work->request_body = malloc(hm->body.len + 1);
    if (!work->request_body) {
        free(work->request_path);
        free(work);
        send_error(c, 500, "Memory allocation failed");
        return;
    }
    memcpy(work->request_body, hm->body.buf, hm->body.len);
    work->request_body[hm->body.len] = '\0';
    work->request_len = hm->body.len;

    sh_completion_init(&work->completion);

    /* Push to work queue */
    ShWorkItem item = {
        .data = NULL,  /* We manage our own data */
        .data_len = 0,
        .user_ctx = work
    };

    double pressure = 0.0;
    if (!sh_workqueue_try_push(s_work_queue, &item, &pressure)) {
        /* Queue full - backpressure */
        sh_completion_cleanup(&work->completion);
        free(work->request_body);
        free(work);
        send_error(c, 503, "Service unavailable - queue full");
        return;
    }

    /* Wait for completion with timeout */
    int timeout_ms = (int)(s_config.work_queue_timeout * 1000);
    if (!sh_completion_wait(&work->completion, timeout_ms)) {
        /* Timeout - mark item as cancelled so worker can skip if not started */
        sh_completion_cancel(&work->completion);
        send_error(c, 504, "Gateway timeout");
        /* Note: work (including request_path and request_body) will be cleaned up when worker processes it */
        return;
    }

    /* Send response */
    if (work->response_data) {
        send_json_status(c, work->status_code, work->response_data);
        free(work->response_data);
    } else {
        send_error(c, 500, "Processing failed");
    }

    /* Cleanup */
    sh_completion_cleanup(&work->completion);
    free(work->request_path);
    free(work->request_body);
    free(work);
}

/* POST /api/v1/solve */
static void handle_solve(struct mg_connection *c, struct mg_http_message *hm) {
    handle_via_queue(c, hm, "/api/v1/solve");
}

/* POST /api/v1/filter */
static void handle_filter(struct mg_connection *c, struct mg_http_message *hm) {
    handle_via_queue(c, hm, "/api/v1/filter");
}

/* POST /api/v1/optimize */
static void handle_optimize(struct mg_connection *c, struct mg_http_message *hm) {
    handle_via_queue(c, hm, "/api/v1/optimize");
}

/* GET /metrics - Prometheus metrics endpoint, uses shared helper */
static void handle_metrics(struct mg_connection *c) {
    sh_mg_handle_metrics(c);
}

/* OPTIONS handler for CORS preflight */
static void handle_options(struct mg_connection *c) {
    char cors_headers[512];
    sh_cors_preflight_headers(&s_cors, NULL, cors_headers, sizeof(cors_headers));
    mg_http_reply(c, 204, cors_headers, "");
}

/* ============================================================================
 * Main Event Handler
 * ============================================================================ */

static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    /* Set socket write timeout on new connections to protect against slow clients */
    if (ev == MG_EV_ACCEPT) {
        sh_mg_set_write_timeout(c, 5000);  /* 5 second write timeout */
        return;
    }

    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        /* Start request timing */
        ShMetricsTimer req_timer = sh_metrics_timer_start();

        /* Extract or generate trace ID */
        sh_trace_from_headers(sh_mg_trace_header_getter, hm);

        /* Handle CORS preflight - no rate limiting */
        if (mg_match(hm->method, mg_str("OPTIONS"), NULL)) {
            handle_options(c);
            sh_trace_clear();
            return;
        }

        /* Health, stats, and metrics bypass rate limiting and work queue */
        if (mg_match(hm->uri, mg_str("/api/v1/health"), NULL)) {
            handle_health(c, hm);
            sh_metrics_counter_inc("http_requests_total", 1,
                "endpoint", "health", "service", "fuelwise", NULL);
            sh_metrics_timer_observe(req_timer, "http_request_duration_ms",
                "endpoint", "health", "service", "fuelwise", NULL);
            sh_trace_clear();
            return;
        }
        if (mg_match(hm->uri, mg_str("/api/v1/stats"), NULL)) {
            handle_stats(c, hm);
            sh_metrics_counter_inc("http_requests_total", 1,
                "endpoint", "stats", "service", "fuelwise", NULL);
            sh_metrics_timer_observe(req_timer, "http_request_duration_ms",
                "endpoint", "stats", "service", "fuelwise", NULL);
            sh_trace_clear();
            return;
        }
        if (mg_match(hm->uri, mg_str("/metrics"), NULL)) {
            handle_metrics(c);
            sh_trace_clear();
            return;
        }

        /* Check rate limit for all other endpoints - uses shared helper */
        if (!sh_mg_check_rate_limit(c, s_rate_limiter, &s_cors, NULL)) {
            sh_metrics_counter_inc("http_requests_total", 1,
                "endpoint", "rate_limited", "status", "429", NULL);
            sh_trace_clear();
            return;
        }

        /* Route requests */
        const char *endpoint = "unknown";
        if (mg_match(hm->uri, mg_str("/api/v1/solve"), NULL)) {
            endpoint = "solve";
            if (mg_match(hm->method, mg_str("POST"), NULL)) {
                handle_solve(c, hm);
            } else {
                send_error(c, 405, "Method not allowed");
            }
        } else if (mg_match(hm->uri, mg_str("/api/v1/optimize"), NULL)) {
            endpoint = "optimize";
            if (mg_match(hm->method, mg_str("POST"), NULL)) {
                handle_optimize(c, hm);
            } else {
                send_error(c, 405, "Method not allowed");
            }
        } else if (mg_match(hm->uri, mg_str("/api/v1/filter"), NULL)) {
            endpoint = "filter";
            if (mg_match(hm->method, mg_str("POST"), NULL)) {
                handle_filter(c, hm);
            } else {
                send_error(c, 405, "Method not allowed");
            }
        } else {
            endpoint = "not_found";
            send_error(c, 404, "Not found");
        }

        /* Record metrics */
        sh_metrics_counter_inc("http_requests_total", 1,
            "endpoint", endpoint, "service", "fuelwise", NULL);
        sh_metrics_timer_observe(req_timer, "http_request_duration_ms",
            "endpoint", endpoint, "service", "fuelwise", NULL);

        /* Clear trace context */
        sh_trace_clear();
    }
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void print_usage(const char *prog) {
    sh_args_usage(prog,
        "<no-data-file>\n\n"
        "FuelWise API server for fuel optimization.\n\n"
        "Example:\n"
        "  %s -p 8080                    # Start on port 8080\n"
        "  %s --rate-limit-off           # Disable rate limiting\n"
        "  %s --queue-off                # Disable work queue\n"
    );
}

int main(int argc, char *argv[]) {
    /* Initialize config with defaults */
    sh_args_init(&s_config);
    sh_cors_init(&s_cors);

    /* FuelWise-specific defaults */
    s_config.port = 8080;
    s_config.rate_limit_rps = 10.0;
    s_config.rate_limit_burst = 50.0;
    s_config.work_queue_depth = 100;
    s_config.work_queue_timeout = 10.0;
    s_config.worker_threads = 4;

    /* Load from environment first */
    sh_args_load_env(&s_config, SH_API_FUELWISE);

    /* Parse command line (overrides env) */
    int first_arg = sh_args_parse(&s_config, argc, argv);
    if (first_arg < 0) {
        print_usage(argv[0]);
        return 1;
    }

    /* Check for help flag */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    /* Set up signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Initialize logging */
    ShLogConfig log_cfg = SH_LOG_CONFIG_DEFAULT;
    log_cfg.service = "fuelwise";
    log_cfg.version = fw_version();
    sh_log_init(&log_cfg);

    /* Initialize metrics */
    ShMetricsConfig metrics_cfg = SH_METRICS_CONFIG_DEFAULT;
    metrics_cfg.service = "fuelwise";
    sh_metrics_init(&metrics_cfg);

    SH_LOG_INFO("Starting fuelwise API server", "version", fw_version(), NULL);

    /* Initialize rate limiter */
    if (s_config.rate_limit_enabled) {
        s_rate_limiter = sh_ratelimit_create(
            s_config.rate_limit_rps,
            s_config.rate_limit_burst,
            s_config.rate_limit_buckets > 0 ? s_config.rate_limit_buckets : 4096
        );
        if (!s_rate_limiter) {
            fprintf(stderr, "Error: Failed to create rate limiter\n");
            return 1;
        }
    }

    /* Initialize work queue and workers */
    if (s_config.work_queue_enabled) {
        s_work_queue = sh_workqueue_create(
            s_config.work_queue_depth,
            s_config.work_queue_timeout
        );
        if (!s_work_queue) {
            fprintf(stderr, "Error: Failed to create work queue\n");
            sh_ratelimit_free(s_rate_limiter);
            return 1;
        }

        /* Start worker pool */
        int num_workers = s_config.worker_threads > 0 ? s_config.worker_threads : 0;
        ShWorkerPoolConfig pool_cfg = {
            .queue = s_work_queue,
            .callback = worker_callback,
            .ctx = NULL
        };
        s_worker_pool = sh_worker_pool_create(num_workers, &pool_cfg);
        if (!s_worker_pool) {
            fprintf(stderr, "Error: Failed to create worker pool\n");
            sh_workqueue_free(s_work_queue);
            sh_ratelimit_free(s_rate_limiter);
            return 1;
        }
    }

    /* Initialize mongoose */
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    /* Build listen address */
    char listen_addr[SH_URL_MAX];
    snprintf(listen_addr, sizeof(listen_addr), "http://%s:%d",
        s_config.host[0] ? s_config.host : "0.0.0.0", s_config.port);

    /* Start listening */
    struct mg_connection *c = mg_http_listen(&mgr, listen_addr, ev_handler, NULL);
    if (c == NULL) {
        fprintf(stderr, "Error: Cannot listen on %s\n", listen_addr);
        goto cleanup;
    }

    /* Print startup message */
    printf("FuelWise API Server v%s\n", fw_version());
    printf("Listening on http://%s:%d\n",
        s_config.host[0] ? s_config.host : "0.0.0.0", s_config.port);
    printf("\n");
    printf("Configuration:\n");
    printf("  Rate limiting: %s", s_config.rate_limit_enabled ? "enabled" : "disabled");
    if (s_config.rate_limit_enabled) {
        printf(" (%.1f RPS, burst %.0f)", s_config.rate_limit_rps, s_config.rate_limit_burst);
    }
    printf("\n");
    printf("  Work queue: %s", s_config.work_queue_enabled ? "enabled" : "disabled");
    if (s_config.work_queue_enabled && s_worker_pool) {
        printf(" (depth %zu, timeout %.1fs, %d workers)",
            s_config.work_queue_depth, s_config.work_queue_timeout,
            sh_worker_pool_size(s_worker_pool));
    }
    printf("\n");
    printf("\n");
    printf("Endpoints:\n");
    printf("  GET  /api/v1/health    - Health check\n");
    printf("  GET  /api/v1/stats     - Server statistics\n");
    printf("  POST /api/v1/solve     - Solve refueling problem\n");
    printf("  POST /api/v1/filter    - Filter stations to route\n");
    printf("  POST /api/v1/optimize  - Full optimization pipeline\n");
    printf("\nPress Ctrl+C to stop.\n\n");

    /* Event loop */
    while (s_signo == 0) {
        mg_mgr_poll(&mgr, 1000);
    }

    printf("\nShutting down...\n");

cleanup:
    /* Shutdown worker pool */
    if (s_worker_pool) {
        sh_worker_pool_stop(s_worker_pool);
        sh_worker_pool_join(s_worker_pool);
        sh_worker_pool_free(s_worker_pool);
    }

    /* Cleanup */
    mg_mgr_free(&mgr);
    sh_workqueue_free(s_work_queue);
    sh_ratelimit_free(s_rate_limiter);

    return 0;
}
