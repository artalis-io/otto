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
#include <stdint.h>

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

/* ============================================================================
 * Adaptive Capacity Tracking
 * ============================================================================ */

/*
 * Default values for adaptive capacity.
 */
#define SH_ADAPTIVE_DEFAULT_WINDOW     1000    /* Sample window for percentiles */
#define SH_ADAPTIVE_DEFAULT_INTERVAL   1000    /* Recalc every N requests */
#define SH_ADAPTIVE_DEFAULT_ALPHA      0.1     /* EMA smoothing factor */
#define SH_ADAPTIVE_MAX_SAMPLE_MS      5000.0  /* Cap samples at 5s */

/*
 * Configuration for adaptive capacity tracker.
 */
typedef struct {
    /* Fixed configuration (from env/args) */
    int num_workers;             /* Worker thread count */
    double target_utilization;   /* Target utilization (0.0-1.0) */
    double client_timeout_ms;    /* Client timeout in milliseconds */
    int burst_tiles;             /* Tiles in initial map view */

    /* Adaptive tuning parameters */
    size_t window_size;          /* Sample window size for percentiles */
    double recalc_interval;      /* Recalculate every N requests */
    double ema_alpha;            /* EMA smoothing factor (0.0-1.0) */
    double max_sample_ms;        /* Cap samples at this value */

    /* Bounds (0 = no bound) */
    double min_rate_limit_rps;   /* Minimum rate limit */
    double max_rate_limit_rps;   /* Maximum rate limit */
    size_t min_queue_depth;      /* Minimum queue depth */
    size_t max_queue_depth;      /* Maximum queue depth */
} ShAdaptiveConfig;

/*
 * Opaque adaptive capacity tracker.
 */
typedef struct ShAdaptiveTracker ShAdaptiveTracker;

/*
 * Percentile statistics from the tracker.
 */
typedef struct {
    double p50_ms;               /* 50th percentile (median) */
    double p90_ms;               /* 90th percentile */
    double p99_ms;               /* 99th percentile */
    double avg_ms;               /* Arithmetic mean */
    double ema_ms;               /* Exponential moving average */
    double min_ms;               /* Minimum observed */
    double max_ms;               /* Maximum observed */
    uint64_t sample_count;       /* Total samples recorded */
    uint64_t recalc_count;       /* Number of recalculations */
} ShAdaptiveStats;

/*
 * Callback invoked when capacity parameters are recalculated.
 *
 * @param params    Newly calculated parameters
 * @param stats     Current statistics
 * @param user_data User-provided context
 */
typedef void (*ShAdaptiveCallback)(const ShCapacityParams *params,
                                   const ShAdaptiveStats *stats,
                                   void *user_data);

/*
 * Create an adaptive capacity tracker.
 *
 * The tracker maintains a sliding window of response time samples,
 * computes percentiles, and periodically recalculates optimal parameters.
 *
 * @param config    Configuration (NULL = use defaults)
 * @return Tracker instance, or NULL on error
 */
ShAdaptiveTracker *sh_adaptive_create(const ShAdaptiveConfig *config);

/*
 * Free an adaptive capacity tracker.
 */
void sh_adaptive_free(ShAdaptiveTracker *tracker);

/*
 * Record a response time sample.
 *
 * Thread-safe. Call this after each request completes.
 * Samples exceeding max_sample_ms are capped.
 *
 * @param tracker       Tracker instance
 * @param response_ms   Response time in milliseconds
 */
void sh_adaptive_record(ShAdaptiveTracker *tracker, double response_ms);

/*
 * Check if recalculation is needed and perform it.
 *
 * Thread-safe. Call periodically (e.g., after each request or on a timer).
 * Returns 1 if recalculation was performed, 0 otherwise.
 *
 * @param tracker   Tracker instance
 * @param params    Output: new parameters (only written if recalculated)
 * @return 1 if recalculated, 0 if not yet time
 */
int sh_adaptive_update(ShAdaptiveTracker *tracker, ShCapacityParams *params);

/*
 * Force immediate recalculation.
 *
 * Thread-safe. Useful for initial setup or manual triggers.
 *
 * @param tracker   Tracker instance
 * @param params    Output: calculated parameters
 * @return 1 on success, 0 if no samples yet
 */
int sh_adaptive_recalculate(ShAdaptiveTracker *tracker, ShCapacityParams *params);

/*
 * Get current statistics.
 *
 * Thread-safe.
 *
 * @param tracker   Tracker instance
 * @param stats     Output: current statistics
 */
void sh_adaptive_stats(ShAdaptiveTracker *tracker, ShAdaptiveStats *stats);

/*
 * Set callback for recalculation events.
 *
 * The callback is invoked (under lock) whenever parameters are recalculated.
 * Useful for applying new rate limits or logging.
 *
 * @param tracker     Tracker instance
 * @param callback    Callback function (NULL to disable)
 * @param user_data   User-provided context passed to callback
 */
void sh_adaptive_set_callback(ShAdaptiveTracker *tracker,
                              ShAdaptiveCallback callback,
                              void *user_data);

/*
 * Get the current calculated parameters without recalculating.
 *
 * Thread-safe.
 *
 * @param tracker   Tracker instance
 * @param params    Output: current parameters
 * @return 1 if parameters available, 0 if never calculated
 */
int sh_adaptive_get_params(ShAdaptiveTracker *tracker, ShCapacityParams *params);

/*
 * Initialize config with default values.
 */
void sh_adaptive_config_init(ShAdaptiveConfig *config);

#ifdef __cplusplus
}
#endif

#endif /* SH_CAPACITY_H */
