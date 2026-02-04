/*
 * sh_capacity.h - Capacity Planning and Queuing Theory Utilities
 *
 * Provides functions to calculate optimal server configuration parameters
 * based on queuing theory (Little's Law, M/M/c queue analysis).
 *
 * Use Case:
 *   Given measured response times and hardware specs, calculate optimal
 *   values for rate limits, queue depths, and timeouts.
 *
 * Example:
 *   ShCapacityParams params;
 *   sh_capacity_calculate(&params, &(ShCapacityInput){
 *       .avg_response_ms = 75,        // Measured average response time
 *       .num_workers = 8,             // Number of worker threads
 *       .target_utilization = 0.7,    // 70% utilization target
 *       .client_timeout_ms = 10000,   // Client gives up after 10s
 *       .burst_tiles = 25             // Tiles in initial map view
 *   });
 *   // params now contains recommended values
 */

#ifndef SH_CAPACITY_H
#define SH_CAPACITY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

/*
 * Input parameters for capacity calculation.
 */
typedef struct {
    /* Performance characteristics */
    double avg_response_ms;      /* Average response time in milliseconds */
    double p99_response_ms;      /* 99th percentile response time (0 = estimate from avg) */

    /* Server configuration */
    int num_workers;             /* Number of worker threads processing requests */

    /* Operational targets */
    double target_utilization;   /* Target utilization (0.0-1.0, recommend 0.6-0.8) */
    double client_timeout_ms;    /* Client timeout in milliseconds (0 = use default 10s) */

    /* Traffic patterns */
    int burst_tiles;             /* Tiles loaded in initial map view (for burst sizing) */
    int expected_clients;        /* Expected concurrent clients (0 = estimate) */
} ShCapacityInput;

/*
 * Calculated capacity parameters with explanations.
 */
typedef struct {
    /* Rate Limiter Settings */
    double rate_limit_rps;       /* Requests per second per IP */
    double rate_limit_burst;     /* Burst capacity (tokens) */

    /* Work Queue Settings */
    size_t queue_depth;          /* Maximum pending requests */
    double queue_timeout_sec;    /* Request timeout in seconds */

    /* Derived Metrics */
    double max_throughput_rps;   /* Maximum sustainable throughput */
    double expected_wait_ms;     /* Expected wait time in queue at target utilization */
    double headroom_factor;      /* Safety margin (1.0 = no margin) */
} ShCapacityParams;

/* ============================================================================
 * API
 * ============================================================================ */

/*
 * Calculate optimal capacity parameters using queuing theory.
 *
 * Uses M/M/c queue model (Erlang-C) assumptions:
 * - Poisson arrival process (random request arrivals)
 * - Exponential service times (reasonable approximation for tile rendering)
 * - c parallel workers (render threads)
 *
 * Key formulas:
 * - Service rate μ = 1000 / avg_response_ms (requests/sec/worker)
 * - Max throughput = c × μ × target_utilization
 * - Queue depth = c × (timeout / response_time) to drain within timeout
 * - Burst = max(burst_tiles × 2, throughput × 2) for map load + margin
 *
 * @param params   Output: calculated parameters
 * @param input    Input: performance characteristics and targets
 *
 * @return 1 on success, 0 on invalid input
 */
int sh_capacity_calculate(ShCapacityParams *params, const ShCapacityInput *input);

/*
 * Print capacity parameters as human-readable report.
 *
 * @param params   Calculated parameters
 * @param buf      Output buffer
 * @param buf_size Buffer size
 * @return Number of characters written (excluding null terminator)
 */
int sh_capacity_report(const ShCapacityParams *params, char *buf, size_t buf_size);

/*
 * Validate current configuration against measured performance.
 *
 * Checks if the current settings are reasonable given actual performance:
 * - Queue not too deep (requests would timeout anyway)
 * - Timeout not too short (drops valid requests)
 * - Rate limit not too high (overloads system)
 *
 * @param current_queue_depth    Current queue depth setting
 * @param current_timeout_sec    Current timeout setting
 * @param current_rate_limit_rps Current rate limit setting
 * @param measured_response_ms   Measured average response time
 * @param num_workers            Number of workers
 * @param warnings               Output: warning messages (null-terminated)
 * @param warnings_size          Size of warnings buffer
 *
 * @return Number of warnings (0 = configuration looks good)
 */
int sh_capacity_validate(size_t current_queue_depth,
                         double current_timeout_sec,
                         double current_rate_limit_rps,
                         double measured_response_ms,
                         int num_workers,
                         char *warnings,
                         size_t warnings_size);

#ifdef __cplusplus
}
#endif

#endif /* SH_CAPACITY_H */
