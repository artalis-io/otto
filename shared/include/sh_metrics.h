/*
 * sh_metrics.h - Application Metrics Library
 *
 * Provides counters, gauges, and histograms with export to:
 * - StatsD (UDP push to Datadog/Graphite/InfluxDB)
 * - Prometheus (HTTP pull endpoint in text format)
 *
 * Usage:
 *   // Initialize
 *   ShMetricsConfig cfg = SH_METRICS_CONFIG_DEFAULT;
 *   cfg.statsd_host = "localhost";
 *   cfg.statsd_port = 8125;
 *   cfg.service = "carta-tile-server";
 *   sh_metrics_init(&cfg);
 *
 *   // Record metrics
 *   sh_metrics_counter_inc("http_requests_total", 1, "method:GET", "status:200");
 *   sh_metrics_gauge_set("active_connections", 42, NULL);
 *   sh_metrics_histogram_observe("http_request_duration_ms", 23.5, "endpoint:/tiles");
 *
 *   // Get Prometheus output
 *   char *prom = sh_metrics_prometheus_output();
 *   // ... serve at /metrics endpoint ...
 *   free(prom);
 *
 *   // Cleanup
 *   sh_metrics_shutdown();
 *
 * Environment Variables:
 *   SH_METRICS_STATSD_HOST   - StatsD host (default: disabled)
 *   SH_METRICS_STATSD_PORT   - StatsD port (default: 8125)
 *   SH_METRICS_PREFIX        - Metric name prefix
 */

#ifndef SH_METRICS_H
#define SH_METRICS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration
 * ============================================================================ */

typedef struct {
    const char *service;      /* Service name (used as metric prefix) */
    const char *statsd_host;  /* StatsD host (NULL to disable) */
    int statsd_port;          /* StatsD port (default: 8125) */
    int flush_interval_ms;    /* Auto-flush interval (default: 1000) */
    int max_metrics;          /* Max unique metrics to track (default: 1000) */
} ShMetricsConfig;

#define SH_METRICS_CONFIG_DEFAULT { \
    .service = NULL, \
    .statsd_host = NULL, \
    .statsd_port = 8125, \
    .flush_interval_ms = 1000, \
    .max_metrics = 1000 \
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

/*
 * Initialize the metrics system.
 * If config is NULL, reads from environment variables.
 */
int sh_metrics_init(const ShMetricsConfig *config);

/*
 * Shutdown and flush all pending metrics.
 */
void sh_metrics_shutdown(void);

/* ============================================================================
 * Counter Metrics
 * ============================================================================ */

/*
 * Increment a counter by the given value.
 * Tags are optional key:value pairs (NULL-terminated).
 *
 * Example:
 *   sh_metrics_counter_inc("requests_total", 1, "method:GET", "status:200", NULL);
 */
void sh_metrics_counter_inc(const char *name, int64_t value, ...);

/*
 * Increment counter with tag array (for programmatic use).
 */
void sh_metrics_counter_inc_tags(const char *name, int64_t value,
                                  const char **tags, int num_tags);

/* ============================================================================
 * Gauge Metrics
 * ============================================================================ */

/*
 * Set a gauge to an absolute value.
 *
 * Example:
 *   sh_metrics_gauge_set("active_connections", 42, NULL);
 */
void sh_metrics_gauge_set(const char *name, double value, ...);

/*
 * Increment/decrement a gauge.
 */
void sh_metrics_gauge_inc(const char *name, double delta, ...);

/*
 * Set gauge with tag array.
 */
void sh_metrics_gauge_set_tags(const char *name, double value,
                                const char **tags, int num_tags);

/* ============================================================================
 * Histogram Metrics
 * ============================================================================ */

/*
 * Record an observation in a histogram.
 * Automatically tracks count, sum, and bucketed distribution.
 *
 * Example:
 *   sh_metrics_histogram_observe("request_duration_ms", 23.5, "endpoint:/api", NULL);
 */
void sh_metrics_histogram_observe(const char *name, double value, ...);

/*
 * Observe with tag array.
 */
void sh_metrics_histogram_observe_tags(const char *name, double value,
                                        const char **tags, int num_tags);

/* ============================================================================
 * Timing Helpers
 * ============================================================================ */

/*
 * Start a timer for measuring duration.
 * Returns an opaque handle.
 */
typedef struct { double start; } ShMetricsTimer;

ShMetricsTimer sh_metrics_timer_start(void);

/*
 * Record elapsed time to a histogram.
 */
void sh_metrics_timer_observe(ShMetricsTimer timer, const char *name, ...);

/* ============================================================================
 * Export Functions
 * ============================================================================ */

/*
 * Get all metrics in Prometheus text format.
 * Returns malloc'd string that caller must free.
 * Returns NULL on error.
 */
char *sh_metrics_prometheus_output(void);

/*
 * Flush all pending metrics to StatsD.
 * Called automatically on interval, but can be called manually.
 */
void sh_metrics_flush_statsd(void);

/* ============================================================================
 * Common HTTP Metrics (Pre-defined)
 * ============================================================================ */

/*
 * Record an HTTP request.
 * Automatically updates:
 *   - http_requests_total (counter)
 *   - http_request_duration_ms (histogram)
 *   - http_request_size_bytes (histogram)
 *   - http_response_size_bytes (histogram)
 */
void sh_metrics_http_request(const char *method, const char *path,
                              int status_code, double duration_ms,
                              size_t request_bytes, size_t response_bytes);

/*
 * Record current active connections.
 */
void sh_metrics_http_connections(int active);

/*
 * Record work queue metrics.
 */
void sh_metrics_workqueue(int depth, int capacity, int dropped, int expired);

/*
 * Record rate limiter metrics.
 */
void sh_metrics_ratelimit(int allowed, int denied, int active_entries);

/* ============================================================================
 * Internal Metric Storage (for advanced use)
 * ============================================================================ */

typedef enum {
    SH_METRIC_COUNTER,
    SH_METRIC_GAUGE,
    SH_METRIC_HISTOGRAM
} ShMetricType;

typedef struct {
    char name[128];
    char tags[256];
    ShMetricType type;
    union {
        int64_t counter;
        double gauge;
        struct {
            double sum;
            uint64_t count;
            uint64_t buckets[16];  /* Exponential buckets */
        } histogram;
    } value;
} ShMetric;

/*
 * Get a metric by name and tags (for testing/inspection).
 * Returns NULL if not found.
 */
const ShMetric *sh_metrics_get(const char *name, const char *tags);

/*
 * Get total number of unique metrics.
 */
int sh_metrics_count(void);

#ifdef __cplusplus
}
#endif

#endif /* SH_METRICS_H */
