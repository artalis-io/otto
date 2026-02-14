/*
 * sh_args.c - Shared Argument Parsing for API Servers
 *
 * Common command-line and environment variable parsing.
 */

#include "sh_args.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

/* ============================================================================
 * Environment Variable Prefixes
 * ============================================================================ */

static const char *api_prefixes[] = {
    [SH_API_GENERIC]  = "SH",
    [SH_API_CARTA]    = "CARTA",
    [SH_API_VELO]     = "VELO",
    [SH_API_LOCUS]    = "LOCUS",
    [SH_API_FUELWISE] = "FUELWISE",
    [SH_API_RALPH]    = "RALPH"
};

const char *sh_args_prefix(ShApiType api_type) {
    if (api_type < 0 || api_type > SH_API_RALPH) {
        return "SH";
    }
    return api_prefixes[api_type];
}

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/*
 * Get environment variable with prefix.
 */
static const char *get_env_with_prefix(const char *prefix, const char *name) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s_%s", prefix, name);
    return getenv(buf);
}

/*
 * Parse integer from string, return default on failure.
 */
static int parse_int(const char *str, int default_val) {
    if (!str || !*str) return default_val;
    char *end;
    long val = strtol(str, &end, 10);
    if (end == str || *end != '\0') return default_val;
    return (int)val;
}

/*
 * Parse double from string, return default on failure.
 */
static double parse_double(const char *str, double default_val) {
    if (!str || !*str) return default_val;
    char *end;
    double val = strtod(str, &end);
    if (end == str || *end != '\0') return default_val;
    return val;
}

/*
 * Parse size_t from string, return default on failure.
 */
static size_t parse_size(const char *str, size_t default_val) {
    if (!str || !*str) return default_val;
    char *end;
    unsigned long val = strtoul(str, &end, 10);
    if (end == str || *end != '\0') return default_val;
    return (size_t)val;
}

/* ============================================================================
 * Safe Parsing Utilities
 * ============================================================================ */

int sh_parse_int(const char *str, int default_val, int min_val, int max_val) {
    if (!str || !*str) return default_val;

    char *end;
    long val = strtol(str, &end, 10);

    /* Parse failure: no digits consumed or trailing garbage */
    if (end == str || *end != '\0') return default_val;

    /* Overflow: strtol returns LONG_MIN/LONG_MAX on overflow */
    if (val < (long)min_val) return default_val;
    if (val > (long)max_val) return default_val;

    return (int)val;
}

long sh_parse_long(const char *str, long default_val, long min_val, long max_val) {
    if (!str || !*str) return default_val;

    char *end;
    long val = strtol(str, &end, 10);

    /* Parse failure: no digits consumed or trailing garbage */
    if (end == str || *end != '\0') return default_val;

    /* Bounds check */
    if (val < min_val || val > max_val) return default_val;

    return val;
}

double sh_parse_double(const char *str, double default_val, double min_val, double max_val) {
    if (!str || !*str) return default_val;

    char *end;
    double val = strtod(str, &end);

    /* Parse failure: no digits consumed */
    if (end == str) return default_val;

    /* Reject inf/nan */
    if (!isfinite(val)) return default_val;

    /* Bounds check */
    if (val < min_val || val > max_val) return default_val;

    return val;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void sh_args_init(ShServerConfig *cfg) {
    if (!cfg) return;

    memset(cfg, 0, sizeof(*cfg));

    /* Network defaults */
    cfg->port = 8080;
    strncpy(cfg->host, "0.0.0.0", sizeof(cfg->host) - 1);
    cfg->host[sizeof(cfg->host) - 1] = '\0';

    /* Threading defaults */
    cfg->worker_threads = 0;  /* Auto-detect */

    /* Rate limiting defaults */
    cfg->rate_limit_enabled = 1;
    cfg->rate_limit_rps = 10.0;
    cfg->rate_limit_burst = 100.0;
    cfg->rate_limit_buckets = 1024;

    /* Work queue defaults */
    cfg->work_queue_enabled = 1;
    cfg->work_queue_depth = 100;
    cfg->work_queue_timeout = 10.0;

    /* Adaptive capacity defaults */
    cfg->adaptive_enabled = 0;  /* Off by default */
    cfg->target_utilization = 0.7;
    cfg->client_timeout_ms = 10000.0;
    cfg->burst_tiles = 25;
    cfg->adaptive_window = 1000;
    cfg->adaptive_interval = 1000;
    cfg->adaptive_alpha = 0.1;

    /* Paths */
    strncpy(cfg->static_dir, "./static", sizeof(cfg->static_dir) - 1);
    cfg->static_dir[sizeof(cfg->static_dir) - 1] = '\0';

    /* Logging */
    cfg->verbose = 0;
    cfg->quiet = 0;

    /* Build mode */
    cfg->build_only = 0;
}

void sh_args_load_env(ShServerConfig *cfg, ShApiType api_type) {
    if (!cfg) return;

    const char *prefix = sh_args_prefix(api_type);
    const char *val;

    /* Network */
    if ((val = get_env_with_prefix(prefix, "PORT"))) {
        cfg->port = parse_int(val, cfg->port);
    }
    if ((val = get_env_with_prefix(prefix, "HOST"))) {
        strncpy(cfg->host, val, sizeof(cfg->host) - 1);
        cfg->host[sizeof(cfg->host) - 1] = '\0';
    }

    /* Threading */
    if ((val = get_env_with_prefix(prefix, "THREADS"))) {
        cfg->worker_threads = parse_int(val, cfg->worker_threads);
    }

    /* Rate limiting */
    if ((val = get_env_with_prefix(prefix, "RATE_LIMIT_ENABLED"))) {
        cfg->rate_limit_enabled = parse_int(val, cfg->rate_limit_enabled);
    }
    if ((val = get_env_with_prefix(prefix, "RATE_LIMIT_RPS"))) {
        cfg->rate_limit_rps = parse_double(val, cfg->rate_limit_rps);
    }
    if ((val = get_env_with_prefix(prefix, "RATE_LIMIT_BURST"))) {
        cfg->rate_limit_burst = parse_double(val, cfg->rate_limit_burst);
    }
    if ((val = get_env_with_prefix(prefix, "RATE_LIMIT_BUCKETS"))) {
        cfg->rate_limit_buckets = parse_size(val, cfg->rate_limit_buckets);
    }

    /* Work queue */
    if ((val = get_env_with_prefix(prefix, "WORK_QUEUE_ENABLED"))) {
        cfg->work_queue_enabled = parse_int(val, cfg->work_queue_enabled);
    }
    if ((val = get_env_with_prefix(prefix, "WORK_QUEUE_DEPTH"))) {
        cfg->work_queue_depth = parse_size(val, cfg->work_queue_depth);
    }
    if ((val = get_env_with_prefix(prefix, "WORK_QUEUE_TIMEOUT"))) {
        cfg->work_queue_timeout = parse_double(val, cfg->work_queue_timeout);
    }

    /* Adaptive capacity */
    if ((val = get_env_with_prefix(prefix, "ADAPTIVE_ENABLED"))) {
        cfg->adaptive_enabled = parse_int(val, cfg->adaptive_enabled);
    }
    if ((val = get_env_with_prefix(prefix, "TARGET_UTILIZATION"))) {
        cfg->target_utilization = parse_double(val, cfg->target_utilization);
    }
    if ((val = get_env_with_prefix(prefix, "CLIENT_TIMEOUT"))) {
        cfg->client_timeout_ms = parse_double(val, cfg->client_timeout_ms);
    }
    if ((val = get_env_with_prefix(prefix, "BURST_TILES"))) {
        cfg->burst_tiles = parse_int(val, cfg->burst_tiles);
    }
    if ((val = get_env_with_prefix(prefix, "ADAPTIVE_WINDOW"))) {
        cfg->adaptive_window = parse_size(val, cfg->adaptive_window);
    }
    if ((val = get_env_with_prefix(prefix, "ADAPTIVE_INTERVAL"))) {
        cfg->adaptive_interval = parse_double(val, cfg->adaptive_interval);
    }
    if ((val = get_env_with_prefix(prefix, "ADAPTIVE_ALPHA"))) {
        cfg->adaptive_alpha = parse_double(val, cfg->adaptive_alpha);
    }

    /* Paths */
    if ((val = get_env_with_prefix(prefix, "STATIC_DIR"))) {
        strncpy(cfg->static_dir, val, sizeof(cfg->static_dir) - 1);
        cfg->static_dir[sizeof(cfg->static_dir) - 1] = '\0';
    }
    if ((val = get_env_with_prefix(prefix, "DATA_FILE"))) {
        strncpy(cfg->data_file, val, sizeof(cfg->data_file) - 1);
        cfg->data_file[sizeof(cfg->data_file) - 1] = '\0';
    }

    /* Logging */
    if ((val = get_env_with_prefix(prefix, "VERBOSE"))) {
        cfg->verbose = parse_int(val, cfg->verbose);
    }
    if ((val = get_env_with_prefix(prefix, "QUIET"))) {
        cfg->quiet = parse_int(val, cfg->quiet);
    }
}

int sh_args_parse(ShServerConfig *cfg, int argc, char **argv) {
    if (!cfg || !argv) return -1;

    int i = 1;
    while (i < argc) {
        const char *arg = argv[i];

        /* Check for end of options */
        if (arg[0] != '-') {
            break;
        }

        /* Handle -- */
        if (strcmp(arg, "--") == 0) {
            i++;
            break;
        }

        /* Short options */
        if (strcmp(arg, "-p") == 0 || strcmp(arg, "--port") == 0) {
            if (i + 1 >= argc) return -1;
            cfg->port = parse_int(argv[++i], cfg->port);
        }
        else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--host") == 0) {
            if (i + 1 >= argc) return -1;
            strncpy(cfg->host, argv[++i], sizeof(cfg->host) - 1);
            cfg->host[sizeof(cfg->host) - 1] = '\0';
        }
        else if (strcmp(arg, "-t") == 0 || strcmp(arg, "--threads") == 0) {
            if (i + 1 >= argc) return -1;
            cfg->worker_threads = parse_int(argv[++i], cfg->worker_threads);
        }
        else if (strcmp(arg, "-s") == 0 || strcmp(arg, "--static") == 0) {
            if (i + 1 >= argc) return -1;
            strncpy(cfg->static_dir, argv[++i], sizeof(cfg->static_dir) - 1);
            cfg->static_dir[sizeof(cfg->static_dir) - 1] = '\0';
        }
        else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            cfg->verbose++;
        }
        else if (strcmp(arg, "-q") == 0 || strcmp(arg, "--quiet") == 0) {
            cfg->quiet = 1;
        }
        /* Rate limiting */
        else if (strcmp(arg, "--rate-limit-rps") == 0) {
            if (i + 1 >= argc) return -1;
            cfg->rate_limit_rps = parse_double(argv[++i], cfg->rate_limit_rps);
        }
        else if (strcmp(arg, "--rate-limit-burst") == 0) {
            if (i + 1 >= argc) return -1;
            cfg->rate_limit_burst = parse_double(argv[++i], cfg->rate_limit_burst);
        }
        else if (strcmp(arg, "--rate-limit-off") == 0) {
            cfg->rate_limit_enabled = 0;
        }
        /* Work queue */
        else if (strcmp(arg, "--queue-depth") == 0) {
            if (i + 1 >= argc) return -1;
            cfg->work_queue_depth = parse_size(argv[++i], cfg->work_queue_depth);
        }
        else if (strcmp(arg, "--queue-timeout") == 0) {
            if (i + 1 >= argc) return -1;
            cfg->work_queue_timeout = parse_double(argv[++i], cfg->work_queue_timeout);
        }
        else if (strcmp(arg, "--queue-off") == 0) {
            cfg->work_queue_enabled = 0;
        }
        /* Adaptive capacity */
        else if (strcmp(arg, "--adaptive") == 0) {
            cfg->adaptive_enabled = 1;
        }
        else if (strcmp(arg, "--utilization") == 0) {
            if (i + 1 >= argc) return -1;
            cfg->target_utilization = parse_double(argv[++i], cfg->target_utilization);
        }
        else if (strcmp(arg, "--client-timeout") == 0) {
            if (i + 1 >= argc) return -1;
            cfg->client_timeout_ms = parse_double(argv[++i], cfg->client_timeout_ms);
        }
        else if (strcmp(arg, "--burst-tiles") == 0) {
            if (i + 1 >= argc) return -1;
            cfg->burst_tiles = parse_int(argv[++i], cfg->burst_tiles);
        }
        else if (strcmp(arg, "--adaptive-window") == 0) {
            if (i + 1 >= argc) return -1;
            cfg->adaptive_window = parse_size(argv[++i], cfg->adaptive_window);
        }
        else if (strcmp(arg, "--adaptive-interval") == 0) {
            if (i + 1 >= argc) return -1;
            cfg->adaptive_interval = parse_double(argv[++i], cfg->adaptive_interval);
        }
        else if (strcmp(arg, "--adaptive-alpha") == 0) {
            if (i + 1 >= argc) return -1;
            cfg->adaptive_alpha = parse_double(argv[++i], cfg->adaptive_alpha);
        }
        /* Build mode */
        else if (strcmp(arg, "--build-only") == 0) {
            cfg->build_only = 1;
        }
        /* Help */
        else if (strcmp(arg, "--help") == 0) {
            return -2;  /* Signal to show help */
        }
        else {
            /* Unknown option - stop parsing, let caller handle */
            break;
        }

        i++;
    }

    return i;  /* Index of first non-option argument */
}

void sh_args_print(const ShServerConfig *cfg) {
    if (!cfg) return;

    printf("Server Configuration:\n");
    printf("  Network:\n");
    printf("    Port:            %d\n", cfg->port);
    printf("    Host:            %s\n", cfg->host);
    printf("    Worker threads:  %d%s\n", cfg->worker_threads,
           cfg->worker_threads == 0 ? " (auto)" : "");

    printf("  Rate Limiting:     %s\n", cfg->rate_limit_enabled ? "enabled" : "disabled");
    if (cfg->rate_limit_enabled) {
        printf("    RPS per IP:      %.1f\n", cfg->rate_limit_rps);
        printf("    Burst capacity:  %.0f\n", cfg->rate_limit_burst);
    }

    printf("  Work Queue:        %s\n", cfg->work_queue_enabled ? "enabled" : "disabled");
    if (cfg->work_queue_enabled) {
        printf("    Queue depth:     %zu\n", cfg->work_queue_depth);
        printf("    Timeout:         %.1f s\n", cfg->work_queue_timeout);
    }

    printf("  Adaptive Capacity: %s\n", cfg->adaptive_enabled ? "enabled" : "disabled");
    if (cfg->adaptive_enabled) {
        printf("    Utilization:     %.0f%%\n", cfg->target_utilization * 100.0);
        printf("    Client timeout:  %.0f ms\n", cfg->client_timeout_ms);
        printf("    Sample window:   %zu\n", cfg->adaptive_window);
        printf("    Recalc interval: %.0f requests\n", cfg->adaptive_interval);
        printf("    EMA alpha:       %.2f\n", cfg->adaptive_alpha);
    }

    if (cfg->static_dir[0]) {
        printf("  Static dir:        %s\n", cfg->static_dir);
    }
    if (cfg->data_file[0]) {
        printf("  Data file:         %s\n", cfg->data_file);
    }

    printf("\n");
}

void sh_args_usage(const char *program_name, const char *extra_usage) {
    printf("Usage: %s [options] %s\n\n", program_name,
           extra_usage ? extra_usage : "<data-file>");

    printf("Network:\n");
    printf("  -p, --port PORT           Listen port (default: 8080)\n");
    printf("  -h, --host HOST           Bind address (default: 0.0.0.0)\n");
    printf("  -t, --threads N           Worker threads (default: auto)\n");
    printf("  -s, --static DIR          Static files directory\n");

    printf("\nRate Limiting:\n");
    printf("  --rate-limit-rps N        Requests per second per IP (default: 10)\n");
    printf("  --rate-limit-burst N      Burst capacity (default: 100)\n");
    printf("  --rate-limit-off          Disable rate limiting\n");

    printf("\nWork Queue:\n");
    printf("  --queue-depth N           Max pending requests (default: 100)\n");
    printf("  --queue-timeout N         Request timeout in seconds (default: 10)\n");
    printf("  --queue-off               Disable work queue\n");

    printf("\nAdaptive Capacity:\n");
    printf("  --adaptive                Enable adaptive capacity\n");
    printf("  --utilization N           Target utilization 0.0-1.0 (default: 0.7)\n");
    printf("  --client-timeout N        Client timeout in ms (default: 10000)\n");
    printf("  --burst-tiles N           Tiles in initial view (default: 25)\n");
    printf("  --adaptive-window N       Sample window size (default: 1000)\n");
    printf("  --adaptive-interval N     Recalc interval (default: 1000)\n");
    printf("  --adaptive-alpha N        EMA smoothing 0.0-1.0 (default: 0.1)\n");

    printf("\nLogging:\n");
    printf("  -v, --verbose             Increase verbosity\n");
    printf("  -q, --quiet               Suppress non-error output\n");
    printf("  --help                    Show this help\n");

    printf("\nBuild Mode:\n");
    printf("  --build-only              Exit after building/saving index (no HTTP server)\n");

    printf("\n");
}
