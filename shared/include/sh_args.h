/*
 * sh_args.h - Shared Argument Parsing for API Servers
 *
 * Common command-line and environment variable parsing used by all OTTO APIs.
 * Provides consistent configuration across carta, velo, locus, and fuelwise.
 *
 * Priority (highest to lowest):
 *   1. Command-line arguments
 *   2. Environment variables
 *   3. Default values
 *
 * Example:
 *   ShServerConfig cfg;
 *   sh_args_init(&cfg);
 *   sh_args_parse(&cfg, argc, argv);
 *   // cfg now contains merged config from args + env + defaults
 */

#ifndef SH_ARGS_H
#define SH_ARGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Buffer Size Constants
 * ============================================================================ */

/*
 * DNS hostname max is 253 chars (RFC 1035). Add padding for null + alignment.
 */
#define SH_HOSTNAME_MAX 256

/*
 * Full URL buffer: "http://" (7) + hostname (253) + ":" (1) + port (5) + null (1) = 267
 * Rounded up for alignment.
 */
#define SH_URL_MAX 320

/*
 * Path buffer for filesystem paths.
 */
#define SH_PATH_MAX 512

/* ============================================================================
 * Types
 * ============================================================================ */

/*
 * Common server configuration shared by all APIs.
 */
typedef struct {
    /* Network */
    int port;                    /* Listen port */
    char host[SH_HOSTNAME_MAX];  /* Bind address (DNS max is 253 chars) */

    /* Threading */
    int worker_threads;          /* Worker thread count (0 = auto) */

    /* Rate Limiting */
    int rate_limit_enabled;      /* 1 = enabled, 0 = disabled */
    double rate_limit_rps;       /* Requests per second per IP */
    double rate_limit_burst;     /* Burst capacity (tokens) */
    size_t rate_limit_buckets;   /* Hash table size */

    /* Work Queue */
    int work_queue_enabled;      /* 1 = enabled, 0 = disabled */
    size_t work_queue_depth;     /* Max pending requests */
    double work_queue_timeout;   /* Request timeout in seconds */

    /* Adaptive Capacity */
    int adaptive_enabled;        /* 1 = enabled, 0 = disabled */
    double target_utilization;   /* Target utilization (0.0-1.0) */
    double client_timeout_ms;    /* Client timeout in milliseconds */
    int burst_tiles;             /* Tiles in initial map view */
    size_t adaptive_window;      /* Sample window size for percentiles */
    double adaptive_interval;    /* Recalculation interval (requests) */
    double adaptive_alpha;       /* EMA smoothing factor */

    /* Paths */
    char static_dir[SH_PATH_MAX];  /* Static files directory */
    char data_file[SH_PATH_MAX];   /* Primary data file (PBF, index, etc.) */

    /* Logging */
    int verbose;                 /* Verbosity level (0-3) */
    int quiet;                   /* Suppress non-error output */

    /* Build mode */
    int build_only;              /* Exit after building/saving index (no HTTP server) */
} ShServerConfig;

/*
 * Environment variable prefix for each API.
 * E.g., CARTA_PORT, VELO_PORT, LOCUS_PORT
 */
typedef enum {
    SH_API_GENERIC,   /* No prefix, uses SH_ */
    SH_API_CARTA,     /* CARTA_ prefix */
    SH_API_VELO,      /* VELO_ prefix */
    SH_API_LOCUS,     /* LOCUS_ prefix */
    SH_API_FUELWISE,  /* FUELWISE_ prefix */
    SH_API_RALPH,     /* RALPH_ prefix */
    SH_API_SURGE      /* SURGE_ prefix */
} ShApiType;

/* ============================================================================
 * API
 * ============================================================================ */

/*
 * Initialize config with default values.
 */
void sh_args_init(ShServerConfig *cfg);

/*
 * Parse command-line arguments into config.
 *
 * Recognizes common options:
 *   -p, --port PORT           Listen port
 *   -h, --host HOST           Bind address
 *   -t, --threads N           Worker threads
 *   -s, --static DIR          Static files directory
 *   -v, --verbose             Increase verbosity
 *   -q, --quiet               Suppress output
 *   --rate-limit-rps N        Rate limit (requests/sec)
 *   --rate-limit-burst N      Burst capacity
 *   --rate-limit-off          Disable rate limiting
 *   --queue-depth N           Work queue depth
 *   --queue-timeout N         Queue timeout (seconds)
 *   --queue-off               Disable work queue
 *   --adaptive                Enable adaptive capacity
 *   --utilization N           Target utilization (0.0-1.0)
 *   --client-timeout N        Client timeout (ms)
 *
 * @param cfg     Config to populate
 * @param argc    Argument count
 * @param argv    Argument vector
 * @return Index of first non-option argument, or -1 on error
 */
int sh_args_parse(ShServerConfig *cfg, int argc, char **argv);

/*
 * Load configuration from environment variables.
 *
 * Environment variables (with api_type prefix):
 *   {PREFIX}_PORT              Listen port
 *   {PREFIX}_HOST              Bind address
 *   {PREFIX}_THREADS           Worker threads
 *   {PREFIX}_STATIC_DIR        Static files directory
 *   {PREFIX}_RATE_LIMIT_RPS    Rate limit
 *   {PREFIX}_RATE_LIMIT_BURST  Burst capacity
 *   {PREFIX}_RATE_LIMIT_ENABLED Rate limiting on/off (1/0)
 *   {PREFIX}_WORK_QUEUE_ENABLED Work queue on/off (1/0)
 *   {PREFIX}_WORK_QUEUE_DEPTH  Queue depth
 *   {PREFIX}_WORK_QUEUE_TIMEOUT Queue timeout
 *   {PREFIX}_ADAPTIVE_ENABLED  Adaptive capacity on/off (1/0)
 *   {PREFIX}_TARGET_UTILIZATION Target utilization
 *   {PREFIX}_CLIENT_TIMEOUT    Client timeout (ms)
 *
 * @param cfg       Config to populate
 * @param api_type  API type for prefix selection
 */
void sh_args_load_env(ShServerConfig *cfg, ShApiType api_type);

/*
 * Print configuration summary to stdout.
 */
void sh_args_print(const ShServerConfig *cfg);

/*
 * Print usage/help message.
 *
 * @param program_name  Name of the program (argv[0])
 * @param extra_usage   Additional usage text (NULL for none)
 */
void sh_args_usage(const char *program_name, const char *extra_usage);

/*
 * Get the environment variable prefix for an API type.
 */
const char *sh_args_prefix(ShApiType api_type);

/* ============================================================================
 * Safe Parsing Utilities
 * ============================================================================ */

/*
 * Parse integer from string with bounds validation.
 *
 * Uses strtol() for proper error detection. Returns default_val if:
 * - str is NULL or empty
 * - str is not a valid integer
 * - parsed value is outside [min_val, max_val] range
 *
 * @param str         String to parse
 * @param default_val Value to return on parse failure or out-of-range
 * @param min_val     Minimum acceptable value (inclusive)
 * @param max_val     Maximum acceptable value (inclusive)
 * @return Parsed integer clamped to [min_val, max_val], or default_val on error
 */
int sh_parse_int(const char *str, int default_val, int min_val, int max_val);

/*
 * Parse long from string with bounds validation.
 *
 * Uses strtol() for proper error detection. Returns default_val if:
 * - str is NULL or empty
 * - str is not a valid integer (no digits consumed or trailing garbage)
 * - parsed value is outside [min_val, max_val] range
 *
 * @param str         String to parse
 * @param default_val Value to return on parse failure or out-of-range
 * @param min_val     Minimum acceptable value (inclusive)
 * @param max_val     Maximum acceptable value (inclusive)
 * @return Parsed long clamped to [min_val, max_val], or default_val on error
 */
long sh_parse_long(const char *str, long default_val, long min_val, long max_val);

/*
 * Parse double from string with bounds validation.
 *
 * Uses strtod() for proper error detection. Returns default_val if:
 * - str is NULL or empty
 * - str is not a valid number (no characters consumed)
 * - parsed value is not finite (inf, nan)
 * - parsed value is outside [min_val, max_val] range
 *
 * @param str         String to parse
 * @param default_val Value to return on parse failure or out-of-range
 * @param min_val     Minimum acceptable value (inclusive)
 * @param max_val     Maximum acceptable value (inclusive)
 * @return Parsed double clamped to [min_val, max_val], or default_val on error
 */
double sh_parse_double(const char *str, double default_val, double min_val, double max_val);

#ifdef __cplusplus
}
#endif

#endif /* SH_ARGS_H */
