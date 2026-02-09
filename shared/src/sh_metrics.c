/*
 * sh_metrics.c - Application Metrics Library Implementation
 */

#include "sh_metrics.h"
#include "sh_args.h"  /* For sh_parse_int */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>

/* ============================================================================
 * Configuration and State
 *
 * DESIGN NOTE: Metrics collection is an intentional singleton. Aggregating
 * metrics requires a central registry, and multiple instances would fragment
 * data. This is an accepted exception to the "no static state in libraries"
 * rule.
 *
 * Thread safety: All access to metrics protected by s_mutex.
 * ============================================================================ */

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static ShMetricsConfig s_config = SH_METRICS_CONFIG_DEFAULT;
static int s_initialized = 0;

/* Metric storage */
static ShMetric *s_metrics = NULL;
static int s_num_metrics = 0;
static int s_max_metrics = 1000;

/* StatsD socket */
static int s_statsd_socket = -1;
static struct sockaddr_in s_statsd_addr;

/* Histogram bucket boundaries (exponential: 1, 2, 5, 10, 20, 50, ...) */
static const double HISTOGRAM_BUCKETS[] = {
    1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 1e9
};
#define NUM_BUCKETS 16

/* ============================================================================
 * StatsD Connection
 * ============================================================================ */

static int connect_statsd(void) {
    if (!s_config.statsd_host) return -1;

    s_statsd_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_statsd_socket < 0) return -1;

    memset(&s_statsd_addr, 0, sizeof(s_statsd_addr));
    s_statsd_addr.sin_family = AF_INET;
    s_statsd_addr.sin_port = htons(s_config.statsd_port);

    /* Resolve hostname using thread-safe getaddrinfo */
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(s_config.statsd_host, NULL, &hints, &result) == 0 && result) {
        struct sockaddr_in *addr = (struct sockaddr_in *)result->ai_addr;
        memcpy(&s_statsd_addr.sin_addr, &addr->sin_addr, sizeof(s_statsd_addr.sin_addr));
        freeaddrinfo(result);
    } else {
        /* Fallback: try parsing as IP address */
        if (inet_pton(AF_INET, s_config.statsd_host, &s_statsd_addr.sin_addr) != 1) {
            close(s_statsd_socket);
            s_statsd_socket = -1;
            return -1;
        }
    }

    return 0;
}

static void send_statsd(const char *metric) {
    if (s_statsd_socket < 0 || !metric) return;

    sendto(s_statsd_socket, metric, strlen(metric), 0,
           (struct sockaddr *)&s_statsd_addr, sizeof(s_statsd_addr));
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

int sh_metrics_init(const ShMetricsConfig *config) {
    pthread_mutex_lock(&s_mutex);

    if (config) {
        s_config = *config;
    } else {
        s_config = (ShMetricsConfig)SH_METRICS_CONFIG_DEFAULT;
    }

    /* Override from environment */
    const char *env_host = getenv("SH_METRICS_STATSD_HOST");
    if (env_host && env_host[0]) {
        s_config.statsd_host = env_host;
    }

    const char *env_port = getenv("SH_METRICS_STATSD_PORT");
    if (env_port) {
        s_config.statsd_port = sh_parse_int(env_port, 8125, 1, 65535);
    }

    /* Allocate metric storage */
    s_max_metrics = s_config.max_metrics > 0 ? s_config.max_metrics : 1000;
    s_metrics = calloc(s_max_metrics, sizeof(ShMetric));
    if (!s_metrics) {
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }
    s_num_metrics = 0;

    /* Connect to StatsD if configured */
    if (s_config.statsd_host) {
        connect_statsd();
    }

    s_initialized = 1;
    pthread_mutex_unlock(&s_mutex);
    return 0;
}

void sh_metrics_shutdown(void) {
    pthread_mutex_lock(&s_mutex);

    /* Flush pending metrics */
    if (s_statsd_socket >= 0) {
        close(s_statsd_socket);
        s_statsd_socket = -1;
    }

    free(s_metrics);
    s_metrics = NULL;
    s_num_metrics = 0;
    s_initialized = 0;

    pthread_mutex_unlock(&s_mutex);
}

/* ============================================================================
 * Tag Formatting
 * ============================================================================ */

static void format_tags(char *buf, size_t len, va_list args) {
    buf[0] = '\0';
    size_t pos = 0;

    const char *tag;
    while ((tag = va_arg(args, const char *)) != NULL) {
        if (pos > 0 && pos < len - 1) {
            buf[pos++] = ',';
        }
        size_t tag_len = strlen(tag);
        if (pos + tag_len < len - 1) {
            memcpy(buf + pos, tag, tag_len);
            pos += tag_len;
        }
    }
    buf[pos] = '\0';
}

static void format_tags_array(char *buf, size_t len,
                               const char **tags, int num_tags) {
    buf[0] = '\0';
    size_t pos = 0;

    for (int i = 0; i < num_tags && tags[i]; i++) {
        if (pos > 0 && pos < len - 1) {
            buf[pos++] = ',';
        }
        size_t tag_len = strlen(tags[i]);
        if (pos + tag_len < len - 1) {
            memcpy(buf + pos, tags[i], tag_len);
            pos += tag_len;
        }
    }
    buf[pos] = '\0';
}

/* ============================================================================
 * Metric Lookup/Creation
 * ============================================================================ */

static ShMetric *find_or_create_metric(const char *name, const char *tags,
                                        ShMetricType type) {
    if (!s_initialized || !s_metrics) return NULL;

    /* Look for existing metric */
    for (int i = 0; i < s_num_metrics; i++) {
        if (s_metrics[i].type == type &&
            strcmp(s_metrics[i].name, name) == 0 &&
            strcmp(s_metrics[i].tags, tags) == 0) {
            return &s_metrics[i];
        }
    }

    /* Create new metric */
    if (s_num_metrics >= s_max_metrics) {
        return NULL;  /* At capacity */
    }

    ShMetric *m = &s_metrics[s_num_metrics++];
    memset(m, 0, sizeof(*m));
    strncpy(m->name, name, sizeof(m->name) - 1);
    m->name[sizeof(m->name) - 1] = '\0';
    strncpy(m->tags, tags ? tags : "", sizeof(m->tags) - 1);
    m->tags[sizeof(m->tags) - 1] = '\0';
    m->type = type;

    return m;
}

/* ============================================================================
 * Counter Implementation
 * ============================================================================ */

void sh_metrics_counter_inc_tags(const char *name, int64_t value,
                                  const char **tags, int num_tags) {
    if (!name) return;

    char tag_str[256];
    format_tags_array(tag_str, sizeof(tag_str), tags, num_tags);

    pthread_mutex_lock(&s_mutex);

    ShMetric *m = find_or_create_metric(name, tag_str, SH_METRIC_COUNTER);
    if (m) {
        m->value.counter += value;
    }

    /* Send to StatsD */
    if (s_statsd_socket >= 0) {
        char statsd_msg[512];
        if (tag_str[0]) {
            snprintf(statsd_msg, sizeof(statsd_msg), "%s%s%s:%lld|c|#%s",
                     s_config.service ? s_config.service : "",
                     s_config.service ? "." : "",
                     name, (long long)value, tag_str);
        } else {
            snprintf(statsd_msg, sizeof(statsd_msg), "%s%s%s:%lld|c",
                     s_config.service ? s_config.service : "",
                     s_config.service ? "." : "",
                     name, (long long)value);
        }
        send_statsd(statsd_msg);
    }

    pthread_mutex_unlock(&s_mutex);
}

void sh_metrics_counter_inc(const char *name, int64_t value, ...) {
    va_list args;
    va_start(args, value);

    char tag_str[256];
    format_tags(tag_str, sizeof(tag_str), args);
    va_end(args);

    const char *tags[] = { tag_str[0] ? tag_str : NULL };
    int num_tags = tag_str[0] ? 1 : 0;

    /* Re-parse tags properly */
    va_start(args, value);
    const char *tag_array[32];
    int tag_count = 0;
    const char *tag;
    while ((tag = va_arg(args, const char *)) != NULL && tag_count < 32) {
        tag_array[tag_count++] = tag;
    }
    va_end(args);

    sh_metrics_counter_inc_tags(name, value, tag_array, tag_count);
}

/* ============================================================================
 * Gauge Implementation
 * ============================================================================ */

void sh_metrics_gauge_set_tags(const char *name, double value,
                                const char **tags, int num_tags) {
    if (!name) return;

    char tag_str[256];
    format_tags_array(tag_str, sizeof(tag_str), tags, num_tags);

    pthread_mutex_lock(&s_mutex);

    ShMetric *m = find_or_create_metric(name, tag_str, SH_METRIC_GAUGE);
    if (m) {
        m->value.gauge = value;
    }

    /* Send to StatsD */
    if (s_statsd_socket >= 0) {
        char statsd_msg[512];
        if (tag_str[0]) {
            snprintf(statsd_msg, sizeof(statsd_msg), "%s%s%s:%g|g|#%s",
                     s_config.service ? s_config.service : "",
                     s_config.service ? "." : "",
                     name, value, tag_str);
        } else {
            snprintf(statsd_msg, sizeof(statsd_msg), "%s%s%s:%g|g",
                     s_config.service ? s_config.service : "",
                     s_config.service ? "." : "",
                     name, value);
        }
        send_statsd(statsd_msg);
    }

    pthread_mutex_unlock(&s_mutex);
}

void sh_metrics_gauge_set(const char *name, double value, ...) {
    va_list args;
    va_start(args, value);
    const char *tag_array[32];
    int tag_count = 0;
    const char *tag;
    while ((tag = va_arg(args, const char *)) != NULL && tag_count < 32) {
        tag_array[tag_count++] = tag;
    }
    va_end(args);

    sh_metrics_gauge_set_tags(name, value, tag_array, tag_count);
}

void sh_metrics_gauge_inc(const char *name, double delta, ...) {
    if (!name) return;

    va_list args;
    va_start(args, delta);
    char tag_str[256];
    format_tags(tag_str, sizeof(tag_str), args);
    va_end(args);

    pthread_mutex_lock(&s_mutex);

    ShMetric *m = find_or_create_metric(name, tag_str, SH_METRIC_GAUGE);
    if (m) {
        m->value.gauge += delta;
    }

    pthread_mutex_unlock(&s_mutex);
}

/* ============================================================================
 * Histogram Implementation
 * ============================================================================ */

void sh_metrics_histogram_observe_tags(const char *name, double value,
                                        const char **tags, int num_tags) {
    if (!name) return;

    char tag_str[256];
    format_tags_array(tag_str, sizeof(tag_str), tags, num_tags);

    pthread_mutex_lock(&s_mutex);

    ShMetric *m = find_or_create_metric(name, tag_str, SH_METRIC_HISTOGRAM);
    if (m) {
        m->value.histogram.count++;
        m->value.histogram.sum += value;

        /* Update bucket counts */
        for (int i = 0; i < NUM_BUCKETS; i++) {
            if (value <= HISTOGRAM_BUCKETS[i]) {
                m->value.histogram.buckets[i]++;
                break;
            }
        }
    }

    /* Send to StatsD as timing/histogram */
    if (s_statsd_socket >= 0) {
        char statsd_msg[512];
        if (tag_str[0]) {
            snprintf(statsd_msg, sizeof(statsd_msg), "%s%s%s:%g|h|#%s",
                     s_config.service ? s_config.service : "",
                     s_config.service ? "." : "",
                     name, value, tag_str);
        } else {
            snprintf(statsd_msg, sizeof(statsd_msg), "%s%s%s:%g|h",
                     s_config.service ? s_config.service : "",
                     s_config.service ? "." : "",
                     name, value);
        }
        send_statsd(statsd_msg);
    }

    pthread_mutex_unlock(&s_mutex);
}

void sh_metrics_histogram_observe(const char *name, double value, ...) {
    va_list args;
    va_start(args, value);
    const char *tag_array[32];
    int tag_count = 0;
    const char *tag;
    while ((tag = va_arg(args, const char *)) != NULL && tag_count < 32) {
        tag_array[tag_count++] = tag;
    }
    va_end(args);

    sh_metrics_histogram_observe_tags(name, value, tag_array, tag_count);
}

/* ============================================================================
 * Timer Implementation
 * ============================================================================ */

ShMetricsTimer sh_metrics_timer_start(void) {
    ShMetricsTimer timer;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    timer.start = ts.tv_sec + ts.tv_nsec / 1e9;
    return timer;
}

void sh_metrics_timer_observe(ShMetricsTimer timer, const char *name, ...) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;
    double duration_ms = (now - timer.start) * 1000;

    va_list args;
    va_start(args, name);
    const char *tag_array[32];
    int tag_count = 0;
    const char *tag;
    while ((tag = va_arg(args, const char *)) != NULL && tag_count < 32) {
        tag_array[tag_count++] = tag;
    }
    va_end(args);

    sh_metrics_histogram_observe_tags(name, duration_ms, tag_array, tag_count);
}

/* ============================================================================
 * Prometheus Export
 * ============================================================================ */

char *sh_metrics_prometheus_output(void) {
    pthread_mutex_lock(&s_mutex);

    if (!s_initialized || !s_metrics) {
        pthread_mutex_unlock(&s_mutex);
        return NULL;
    }

    /* Estimate output size */
    size_t buf_size = s_num_metrics * 512 + 1024;
    char *output = malloc(buf_size);
    if (!output) {
        pthread_mutex_unlock(&s_mutex);
        return NULL;
    }

    size_t pos = 0;

    for (int i = 0; i < s_num_metrics; i++) {
        ShMetric *m = &s_metrics[i];

        /* Format metric name with optional prefix */
        char full_name[256];
        if (s_config.service) {
            snprintf(full_name, sizeof(full_name), "%s_%s", s_config.service, m->name);
        } else {
            strncpy(full_name, m->name, sizeof(full_name) - 1);
            full_name[sizeof(full_name) - 1] = '\0';
        }

        /* Replace dots and hyphens with underscores for Prometheus */
        for (char *p = full_name; *p; p++) {
            if (*p == '.' || *p == '-') *p = '_';
        }

        /* Format labels */
        char labels[512] = "";
        if (m->tags[0]) {
            /* Convert tag:value format to label="value" format */
            char *tag_copy = strdup(m->tags);
            char *label_pos = labels;
            char *remaining = NULL;
            int first = 1;

            for (char *tok = strtok_r(tag_copy, ",", &remaining);
                 tok;
                 tok = strtok_r(NULL, ",", &remaining)) {
                char *colon = strchr(tok, ':');
                if (colon) {
                    *colon = '\0';
                    label_pos += snprintf(label_pos, sizeof(labels) - (label_pos - labels),
                                          "%s%s=\"%s\"",
                                          first ? "" : ",", tok, colon + 1);
                    first = 0;
                }
            }
            free(tag_copy);
        }

        switch (m->type) {
            case SH_METRIC_COUNTER:
                pos += snprintf(output + pos, buf_size - pos,
                                "# TYPE %s counter\n%s%s%s%s %lld\n",
                                full_name, full_name,
                                labels[0] ? "{" : "", labels, labels[0] ? "}" : "",
                                (long long)m->value.counter);
                break;

            case SH_METRIC_GAUGE:
                pos += snprintf(output + pos, buf_size - pos,
                                "# TYPE %s gauge\n%s%s%s%s %g\n",
                                full_name, full_name,
                                labels[0] ? "{" : "", labels, labels[0] ? "}" : "",
                                m->value.gauge);
                break;

            case SH_METRIC_HISTOGRAM:
                pos += snprintf(output + pos, buf_size - pos,
                                "# TYPE %s histogram\n", full_name);

                /* Output bucket counts */
                uint64_t cumulative = 0;
                for (int b = 0; b < NUM_BUCKETS; b++) {
                    cumulative += m->value.histogram.buckets[b];
                    pos += snprintf(output + pos, buf_size - pos,
                                    "%s_bucket{%s%sle=\"%g\"} %llu\n",
                                    full_name,
                                    labels, labels[0] ? "," : "",
                                    HISTOGRAM_BUCKETS[b],
                                    (unsigned long long)cumulative);
                }

                /* Output count and sum */
                pos += snprintf(output + pos, buf_size - pos,
                                "%s_count%s%s%s %llu\n",
                                full_name,
                                labels[0] ? "{" : "", labels, labels[0] ? "}" : "",
                                (unsigned long long)m->value.histogram.count);
                pos += snprintf(output + pos, buf_size - pos,
                                "%s_sum%s%s%s %g\n",
                                full_name,
                                labels[0] ? "{" : "", labels, labels[0] ? "}" : "",
                                m->value.histogram.sum);
                break;
        }
    }

    pthread_mutex_unlock(&s_mutex);
    return output;
}

void sh_metrics_flush_statsd(void) {
    /* Currently metrics are sent immediately, but this could batch them */
}

/* ============================================================================
 * Inspection Functions
 * ============================================================================ */

const ShMetric *sh_metrics_get(const char *name, const char *tags) {
    if (!s_initialized || !s_metrics || !name) return NULL;

    pthread_mutex_lock(&s_mutex);

    for (int i = 0; i < s_num_metrics; i++) {
        if (strcmp(s_metrics[i].name, name) == 0 &&
            strcmp(s_metrics[i].tags, tags ? tags : "") == 0) {
            pthread_mutex_unlock(&s_mutex);
            return &s_metrics[i];
        }
    }

    pthread_mutex_unlock(&s_mutex);
    return NULL;
}

int sh_metrics_count(void) {
    return s_num_metrics;
}

/* ============================================================================
 * Common HTTP Metrics
 * ============================================================================ */

void sh_metrics_http_request(const char *method, const char *path,
                              int status_code, double duration_ms,
                              size_t request_bytes, size_t response_bytes) {
    char status_str[8];
    snprintf(status_str, sizeof(status_str), "%d", status_code);

    char status_class[8];
    snprintf(status_class, sizeof(status_class), "%dxx", status_code / 100);

    sh_metrics_counter_inc("http_requests_total", 1,
                           method ? method : "UNKNOWN",
                           status_class,
                           NULL);

    if (duration_ms >= 0) {
        sh_metrics_histogram_observe("http_request_duration_ms", duration_ms,
                                     method ? method : "UNKNOWN",
                                     NULL);
    }

    if (request_bytes > 0) {
        sh_metrics_histogram_observe("http_request_size_bytes", (double)request_bytes,
                                     NULL);
    }

    if (response_bytes > 0) {
        sh_metrics_histogram_observe("http_response_size_bytes", (double)response_bytes,
                                     NULL);
    }
}

void sh_metrics_http_connections(int active) {
    sh_metrics_gauge_set("http_active_connections", (double)active, NULL);
}

void sh_metrics_workqueue(int depth, int capacity, int dropped, int expired) {
    sh_metrics_gauge_set("workqueue_depth", (double)depth, NULL);
    sh_metrics_gauge_set("workqueue_capacity", (double)capacity, NULL);
    sh_metrics_counter_inc("workqueue_dropped_total", dropped, NULL);
    sh_metrics_counter_inc("workqueue_expired_total", expired, NULL);
}

void sh_metrics_ratelimit(int allowed, int denied, int active_entries) {
    sh_metrics_counter_inc("ratelimit_allowed_total", allowed, NULL);
    sh_metrics_counter_inc("ratelimit_denied_total", denied, NULL);
    sh_metrics_gauge_set("ratelimit_active_entries", (double)active_entries, NULL);
}
