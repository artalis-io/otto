/*
 * sh_capacity.c - Capacity Planning and Queuing Theory Utilities
 *
 * Implements M/M/c queue analysis for server configuration.
 */

#include "sh_capacity.h"
#include <stdio.h>
#include <math.h>

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
