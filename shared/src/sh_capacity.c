/*
 * sh_capacity.c - Capacity Planning and Queuing Theory Utilities
 *
 * Implements M/M/c queue analysis for server configuration.
 */

#include "sh_capacity.h"
#include "sh_pal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/*
 * Calculate service rate (requests per second per worker).
 */
static double service_rate(double avg_response_ms) {
    if (avg_response_ms <= 0) return 0;
    return 1000.0 / avg_response_ms;
}

/*
 * Estimate p99 from average using exponential distribution assumption.
 * For exponential: p99 = avg * ln(100) ≈ avg * 4.6
 * In practice, tile serving is more predictable, so we use ~3x.
 */
static double estimate_p99(double avg_ms) {
    return avg_ms * 3.0;
}

/*
 * Calculate Erlang-C probability of waiting (for M/M/c queue).
 * This is the probability that an arriving request must wait.
 *
 * P(wait) = C(c, a) / (C(c, a) + (1 - rho) * sum_{k=0}^{c-1} a^k/k!)
 *
 * Where:
 *   c = number of servers (workers)
 *   a = lambda/mu = traffic intensity (arrival rate / service rate)
 *   rho = a/c = utilization
 *   C(c, a) = (a^c / c!) * (c / (c - a))
 *
 * For simplicity, we use an approximation for high utilization:
 * P(wait) ≈ rho^c for moderate utilization
 */
static double erlang_c_wait_probability(int c, double rho) {
    if (c <= 0 || rho <= 0) return 0;
    if (rho >= 1.0) return 1.0;  /* Unstable queue */

    /* Simplified approximation */
    double prob = pow(rho, c);

    /* Adjust for multi-server effect */
    prob = prob / (1.0 + (1.0 - rho) * (double)c);

    return fmin(prob, 1.0);
}

/*
 * Calculate expected wait time in M/M/c queue.
 *
 * E[W] = P(wait) * (1 / (c * mu * (1 - rho)))
 *
 * Where:
 *   P(wait) = Erlang-C probability
 *   c = number of servers
 *   mu = service rate
 *   rho = utilization
 */
static double expected_wait_time_ms(int c, double mu, double rho) {
    if (c <= 0 || mu <= 0 || rho <= 0) return 0;
    if (rho >= 1.0) return INFINITY;

    double p_wait = erlang_c_wait_probability(c, rho);
    double avg_wait = p_wait / ((double)c * mu * (1.0 - rho));

    return avg_wait * 1000.0;  /* Convert to ms */
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int sh_capacity_calculate(ShCapacityParams *params, const ShCapacityInput *input) {
    if (!params || !input) return 0;

    /* Validate input */
    if (input->avg_response_ms <= 0) return 0;
    if (input->num_workers <= 0) return 0;
    if (input->target_utilization <= 0 || input->target_utilization > 1.0) return 0;

    /* Extract inputs with defaults */
    double avg_ms = input->avg_response_ms;
    double p99_ms = input->p99_response_ms > 0
                    ? input->p99_response_ms
                    : estimate_p99(avg_ms);
    int c = input->num_workers;
    double rho = input->target_utilization;
    double client_timeout_ms = input->client_timeout_ms > 0
                               ? input->client_timeout_ms
                               : 10000.0;  /* Default 10s */
    int burst_tiles = input->burst_tiles > 0 ? input->burst_tiles : 25;

    /* Calculate service rate (requests/sec/worker) */
    double mu = service_rate(avg_ms);

    /* Calculate maximum throughput at target utilization */
    /* max_throughput = c * mu * rho */
    double max_throughput = (double)c * mu * rho;

    /* Calculate expected wait time at target utilization */
    double wait_ms = expected_wait_time_ms(c, mu, rho);

    /* ========================================================================
     * Rate Limiter Settings
     * ========================================================================
     *
     * Rate limit per IP: We want to allow a single client to use a fair share
     * of capacity. With expected_clients=0, assume 10-20 concurrent clients.
     *
     * rate_limit_rps = max_throughput / estimated_clients
     *
     * Burst: Allow enough tokens for initial map view + some margin.
     * burst = max(burst_tiles * 2, rate_limit * 2)
     */
    int estimated_clients = input->expected_clients > 0
                            ? input->expected_clients
                            : 10;

    params->rate_limit_rps = max_throughput / (double)estimated_clients;

    /* Ensure minimum rate limit of 1 RPS */
    if (params->rate_limit_rps < 1.0) {
        params->rate_limit_rps = 1.0;
    }

    /* Burst should handle initial map view */
    double burst_from_tiles = (double)burst_tiles * 2.0;
    double burst_from_rate = params->rate_limit_rps * 2.0;
    params->rate_limit_burst = fmax(burst_from_tiles, burst_from_rate);

    /* Ensure burst is at least equal to burst_tiles */
    if (params->rate_limit_burst < (double)burst_tiles) {
        params->rate_limit_burst = (double)burst_tiles;
    }

    /* ========================================================================
     * Work Queue Settings
     * ========================================================================
     *
     * Queue depth: Size the queue to drain within client timeout.
     *
     * Little's Law: L = λ × W
     *   L = average items in system
     *   λ = arrival rate
     *   W = average time in system
     *
     * Max queue depth should allow oldest item to be processed before timeout:
     *   queue_depth = c × (timeout - p99_response) / avg_response
     *
     * This ensures even the oldest queued request can be served before timeout.
     */
    double usable_wait_time_ms = client_timeout_ms - p99_ms;
    if (usable_wait_time_ms < avg_ms) {
        usable_wait_time_ms = avg_ms;  /* Minimum: can process at least 1 */
    }

    /* Items that can be processed in usable_wait_time */
    double items_processable = (double)c * usable_wait_time_ms / avg_ms;

    params->queue_depth = (size_t)fmax(items_processable, (double)c);

    /* Ensure queue depth is reasonable (not too small, not too large) */
    if (params->queue_depth < (size_t)c) {
        params->queue_depth = (size_t)c;
    }
    if (params->queue_depth > 10000) {
        params->queue_depth = 10000;  /* Cap at 10k for sanity */
    }

    /* Queue timeout: Set to client timeout minus expected processing time */
    double timeout_sec = (client_timeout_ms - avg_ms) / 1000.0;
    if (timeout_sec < 1.0) {
        timeout_sec = 1.0;  /* Minimum 1 second */
    }
    params->queue_timeout_sec = timeout_sec;

    /* ========================================================================
     * Derived Metrics
     * ======================================================================== */
    params->max_throughput_rps = max_throughput;
    params->expected_wait_ms = wait_ms;

    /* Headroom factor: how much spare capacity at target utilization */
    params->headroom_factor = 1.0 / rho;

    return 1;  /* Success */
}

int sh_capacity_report(const ShCapacityParams *params, char *buf, size_t buf_size) {
    if (!params || !buf || buf_size == 0) return 0;

    return snprintf(buf, buf_size,
        "Capacity Planning Report\n"
        "========================\n"
        "\n"
        "Rate Limiter (per IP):\n"
        "  Rate limit:     %.1f requests/sec\n"
        "  Burst capacity: %.0f tokens\n"
        "\n"
        "Work Queue:\n"
        "  Queue depth:    %zu requests\n"
        "  Timeout:        %.1f seconds\n"
        "\n"
        "Performance:\n"
        "  Max throughput: %.1f requests/sec\n"
        "  Expected wait:  %.1f ms\n"
        "  Headroom:       %.0f%%\n",
        params->rate_limit_rps,
        params->rate_limit_burst,
        params->queue_depth,
        params->queue_timeout_sec,
        params->max_throughput_rps,
        params->expected_wait_ms,
        (params->headroom_factor - 1.0) * 100.0);
}

int sh_capacity_validate(size_t current_queue_depth,
                         double current_timeout_sec,
                         double current_rate_limit_rps,
                         double measured_response_ms,
                         int num_workers,
                         char *warnings,
                         size_t warnings_size) {
    if (!warnings || warnings_size == 0) return 0;

    warnings[0] = '\0';
    int warning_count = 0;
    size_t offset = 0;

    /* Validate inputs */
    if (measured_response_ms <= 0 || num_workers <= 0) {
        offset += snprintf(warnings + offset, warnings_size - offset,
            "- Invalid input: response_ms=%g, workers=%d\n",
            measured_response_ms, num_workers);
        return 1;
    }

    double mu = service_rate(measured_response_ms);
    double max_throughput = (double)num_workers * mu;

    /* Check 1: Queue too deep (requests will timeout anyway) */
    double max_wait_sec = (double)current_queue_depth * measured_response_ms / 1000.0 / (double)num_workers;
    if (max_wait_sec > current_timeout_sec) {
        offset += snprintf(warnings + offset, warnings_size - offset,
            "- Queue too deep: %zu items would take %.1fs to drain, "
            "but timeout is %.1fs\n",
            current_queue_depth, max_wait_sec, current_timeout_sec);
        warning_count++;
    }

    /* Check 2: Timeout too short (less than 2x response time) */
    double min_timeout = measured_response_ms * 2.0 / 1000.0;
    if (current_timeout_sec < min_timeout) {
        offset += snprintf(warnings + offset, warnings_size - offset,
            "- Timeout too short: %.1fs < 2x response time (%.1fs)\n",
            current_timeout_sec, min_timeout);
        warning_count++;
    }

    /* Check 3: Rate limit too high (would overload system) */
    if (current_rate_limit_rps > max_throughput * 0.8) {
        offset += snprintf(warnings + offset, warnings_size - offset,
            "- Rate limit too high: %.1f RPS > 80%% of max throughput (%.1f RPS)\n",
            current_rate_limit_rps, max_throughput * 0.8);
        warning_count++;
    }

    /* Check 4: Rate limit too low (wastes capacity) */
    if (current_rate_limit_rps < max_throughput * 0.1) {
        offset += snprintf(warnings + offset, warnings_size - offset,
            "- Rate limit very low: %.1f RPS < 10%% of max throughput (%.1f RPS)\n",
            current_rate_limit_rps, max_throughput);
        warning_count++;
    }

    /* Check 5: Queue too small (less than worker count) */
    if (current_queue_depth < (size_t)num_workers) {
        offset += snprintf(warnings + offset, warnings_size - offset,
            "- Queue too small: %zu < worker count (%d)\n",
            current_queue_depth, num_workers);
        warning_count++;
    }

    return warning_count;
}

/* ============================================================================
 * Adaptive Capacity Tracker Implementation
 * ============================================================================ */

/*
 * Internal structure for adaptive tracker.
 */
struct ShAdaptiveTracker {
    /* Configuration (immutable after creation) */
    ShAdaptiveConfig config;

    /* Sample buffer (circular) */
    double *samples;             /* Sample buffer */
    size_t sample_head;          /* Next write position */
    size_t sample_count;         /* Samples in buffer (up to window_size) */

    /* Statistics */
    double ema_ms;               /* Exponential moving average */
    double sum_ms;               /* Running sum for average */
    double min_ms;               /* Minimum observed */
    double max_ms;               /* Maximum observed */
    uint64_t total_samples;      /* Total samples ever recorded */
    uint64_t last_recalc_sample; /* Sample count at last recalculation */
    uint64_t recalc_count;       /* Number of recalculations */

    /* Current calculated parameters */
    ShCapacityParams current_params;
    int params_valid;            /* 1 if current_params is valid */

    /* Callback */
    ShAdaptiveCallback callback;
    void *callback_user_data;

    /* Thread safety */
    ShMutex mutex;
};

/*
 * Comparison function for qsort (ascending order).
 */
static int compare_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

/*
 * Calculate percentile from sorted array.
 */
static double percentile(const double *sorted, size_t count, double p) {
    if (count == 0) return 0;
    if (count == 1) return sorted[0];

    double index = p * (double)(count - 1);
    size_t lower = (size_t)index;
    size_t upper = lower + 1;
    if (upper >= count) upper = count - 1;

    double frac = index - (double)lower;
    return sorted[lower] * (1.0 - frac) + sorted[upper] * frac;
}

void sh_adaptive_config_init(ShAdaptiveConfig *config) {
    if (!config) return;

    memset(config, 0, sizeof(*config));

    /* Reasonable defaults */
    config->num_workers = 4;
    config->target_utilization = 0.7;
    config->client_timeout_ms = 10000.0;
    config->burst_tiles = 25;

    config->window_size = SH_ADAPTIVE_DEFAULT_WINDOW;
    config->recalc_interval = SH_ADAPTIVE_DEFAULT_INTERVAL;
    config->ema_alpha = SH_ADAPTIVE_DEFAULT_ALPHA;
    config->max_sample_ms = SH_ADAPTIVE_MAX_SAMPLE_MS;

    /* No bounds by default */
    config->min_rate_limit_rps = 0;
    config->max_rate_limit_rps = 0;
    config->min_queue_depth = 0;
    config->max_queue_depth = 0;
}

ShAdaptiveTracker *sh_adaptive_create(const ShAdaptiveConfig *config) {
    ShAdaptiveTracker *tracker = calloc(1, sizeof(ShAdaptiveTracker));
    if (!tracker) return NULL;

    /* Copy or initialize config */
    if (config) {
        tracker->config = *config;
    } else {
        sh_adaptive_config_init(&tracker->config);
    }

    /* Validate config */
    if (tracker->config.window_size == 0) {
        tracker->config.window_size = SH_ADAPTIVE_DEFAULT_WINDOW;
    }
    if (tracker->config.recalc_interval == 0) {
        tracker->config.recalc_interval = SH_ADAPTIVE_DEFAULT_INTERVAL;
    }
    if (tracker->config.ema_alpha <= 0 || tracker->config.ema_alpha > 1.0) {
        tracker->config.ema_alpha = SH_ADAPTIVE_DEFAULT_ALPHA;
    }
    if (tracker->config.max_sample_ms <= 0) {
        tracker->config.max_sample_ms = SH_ADAPTIVE_MAX_SAMPLE_MS;
    }

    /* Allocate sample buffer */
    tracker->samples = calloc(tracker->config.window_size, sizeof(double));
    if (!tracker->samples) {
        free(tracker);
        return NULL;
    }

    /* Initialize statistics */
    tracker->min_ms = INFINITY;
    tracker->max_ms = 0;

    /* Initialize mutex */
    if (sh_mutex_init(&tracker->mutex) != 0) {
        free(tracker->samples);
        free(tracker);
        return NULL;
    }

    return tracker;
}

void sh_adaptive_free(ShAdaptiveTracker *tracker) {
    if (!tracker) return;

    sh_mutex_destroy(&tracker->mutex);
    free(tracker->samples);
    free(tracker);
}

void sh_adaptive_record(ShAdaptiveTracker *tracker, double response_ms) {
    if (!tracker) return;

    /* Cap sample at max */
    if (response_ms > tracker->config.max_sample_ms) {
        response_ms = tracker->config.max_sample_ms;
    }
    if (response_ms < 0) {
        response_ms = 0;
    }

    sh_mutex_lock(&tracker->mutex);

    /* Update EMA */
    if (tracker->total_samples == 0) {
        tracker->ema_ms = response_ms;
    } else {
        tracker->ema_ms = tracker->config.ema_alpha * response_ms
                        + (1.0 - tracker->config.ema_alpha) * tracker->ema_ms;
    }

    /* Update min/max */
    if (response_ms < tracker->min_ms) tracker->min_ms = response_ms;
    if (response_ms > tracker->max_ms) tracker->max_ms = response_ms;

    /* Add to circular buffer */
    if (tracker->sample_count < tracker->config.window_size) {
        /* Buffer not full yet - just append */
        tracker->samples[tracker->sample_count] = response_ms;
        tracker->sample_count++;
        tracker->sum_ms += response_ms;
    } else {
        /* Buffer full - replace oldest */
        tracker->sum_ms -= tracker->samples[tracker->sample_head];
        tracker->sum_ms += response_ms;
        tracker->samples[tracker->sample_head] = response_ms;
    }

    tracker->sample_head = (tracker->sample_head + 1) % tracker->config.window_size;
    tracker->total_samples++;

    sh_mutex_unlock(&tracker->mutex);
}

/*
 * Internal recalculation (must hold mutex).
 */
static int adaptive_recalc_locked(ShAdaptiveTracker *tracker, ShCapacityParams *params) {
    if (tracker->sample_count == 0) {
        return 0;
    }

    /* Copy samples for sorting - use calloc for overflow protection */
    double *sorted = calloc(tracker->sample_count, sizeof(double));
    if (!sorted) return 0;

    memcpy(sorted, tracker->samples, tracker->sample_count * sizeof(double));
    qsort(sorted, tracker->sample_count, sizeof(double), compare_double);

    /* Calculate percentiles */
    double p50 = percentile(sorted, tracker->sample_count, 0.50);
    double p90 = percentile(sorted, tracker->sample_count, 0.90);
    double p99 = percentile(sorted, tracker->sample_count, 0.99);
    double avg = tracker->sum_ms / (double)tracker->sample_count;

    free(sorted);

    /* Build input for capacity calculation - use P50 for rate limit sizing */
    ShCapacityInput input = {
        .avg_response_ms = p50,  /* Use median for stability */
        .p99_response_ms = p99,  /* Use actual P99 */
        .num_workers = tracker->config.num_workers,
        .target_utilization = tracker->config.target_utilization,
        .client_timeout_ms = tracker->config.client_timeout_ms,
        .burst_tiles = tracker->config.burst_tiles,
        .expected_clients = 10   /* Default assumption */
    };

    ShCapacityParams new_params;
    if (!sh_capacity_calculate(&new_params, &input)) {
        return 0;
    }

    /* Apply bounds */
    if (tracker->config.min_rate_limit_rps > 0 &&
        new_params.rate_limit_rps < tracker->config.min_rate_limit_rps) {
        new_params.rate_limit_rps = tracker->config.min_rate_limit_rps;
    }
    if (tracker->config.max_rate_limit_rps > 0 &&
        new_params.rate_limit_rps > tracker->config.max_rate_limit_rps) {
        new_params.rate_limit_rps = tracker->config.max_rate_limit_rps;
    }
    if (tracker->config.min_queue_depth > 0 &&
        new_params.queue_depth < tracker->config.min_queue_depth) {
        new_params.queue_depth = tracker->config.min_queue_depth;
    }
    if (tracker->config.max_queue_depth > 0 &&
        new_params.queue_depth > tracker->config.max_queue_depth) {
        new_params.queue_depth = tracker->config.max_queue_depth;
    }

    /* Store new params */
    tracker->current_params = new_params;
    tracker->params_valid = 1;
    tracker->recalc_count++;
    tracker->last_recalc_sample = tracker->total_samples;

    if (params) {
        *params = new_params;
    }

    /* Invoke callback if set */
    if (tracker->callback) {
        ShAdaptiveStats stats = {
            .p50_ms = p50,
            .p90_ms = p90,
            .p99_ms = p99,
            .avg_ms = avg,
            .ema_ms = tracker->ema_ms,
            .min_ms = tracker->min_ms,
            .max_ms = tracker->max_ms,
            .sample_count = tracker->total_samples,
            .recalc_count = tracker->recalc_count
        };
        tracker->callback(&new_params, &stats, tracker->callback_user_data);
    }

    return 1;
}

int sh_adaptive_update(ShAdaptiveTracker *tracker, ShCapacityParams *params) {
    if (!tracker) return 0;

    sh_mutex_lock(&tracker->mutex);

    /* Check if it's time to recalculate */
    uint64_t samples_since_recalc = tracker->total_samples - tracker->last_recalc_sample;
    if (samples_since_recalc < (uint64_t)tracker->config.recalc_interval) {
        sh_mutex_unlock(&tracker->mutex);
        return 0;
    }

    int result = adaptive_recalc_locked(tracker, params);

    sh_mutex_unlock(&tracker->mutex);
    return result;
}

int sh_adaptive_recalculate(ShAdaptiveTracker *tracker, ShCapacityParams *params) {
    if (!tracker) return 0;

    sh_mutex_lock(&tracker->mutex);
    int result = adaptive_recalc_locked(tracker, params);
    sh_mutex_unlock(&tracker->mutex);

    return result;
}

void sh_adaptive_stats(ShAdaptiveTracker *tracker, ShAdaptiveStats *stats) {
    if (!tracker || !stats) return;

    memset(stats, 0, sizeof(*stats));

    sh_mutex_lock(&tracker->mutex);

    if (tracker->sample_count > 0) {
        /* Copy and sort for percentiles - use calloc for overflow protection */
        double *sorted = calloc(tracker->sample_count, sizeof(double));
        if (sorted) {
            memcpy(sorted, tracker->samples, tracker->sample_count * sizeof(double));
            qsort(sorted, tracker->sample_count, sizeof(double), compare_double);

            stats->p50_ms = percentile(sorted, tracker->sample_count, 0.50);
            stats->p90_ms = percentile(sorted, tracker->sample_count, 0.90);
            stats->p99_ms = percentile(sorted, tracker->sample_count, 0.99);

            free(sorted);
        }

        stats->avg_ms = tracker->sum_ms / (double)tracker->sample_count;
    }

    stats->ema_ms = tracker->ema_ms;
    stats->min_ms = tracker->min_ms == INFINITY ? 0 : tracker->min_ms;
    stats->max_ms = tracker->max_ms;
    stats->sample_count = tracker->total_samples;
    stats->recalc_count = tracker->recalc_count;

    sh_mutex_unlock(&tracker->mutex);
}

void sh_adaptive_set_callback(ShAdaptiveTracker *tracker,
                              ShAdaptiveCallback callback,
                              void *user_data) {
    if (!tracker) return;

    sh_mutex_lock(&tracker->mutex);
    tracker->callback = callback;
    tracker->callback_user_data = user_data;
    sh_mutex_unlock(&tracker->mutex);
}

int sh_adaptive_get_params(ShAdaptiveTracker *tracker, ShCapacityParams *params) {
    if (!tracker || !params) return 0;

    sh_mutex_lock(&tracker->mutex);

    int valid = tracker->params_valid;
    if (valid) {
        *params = tracker->current_params;
    }

    sh_mutex_unlock(&tracker->mutex);
    return valid;
}
