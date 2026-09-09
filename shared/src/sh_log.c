/*
 * sh_log.c - Structured Logging Library Implementation
 */

#include "sh_log.h"
#include "sh_args.h"  /* For sh_parse_int */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sh_pal.h"
#include <time.h>
#include <stdarg.h>

/* ============================================================================
 * Thread-Local Storage
 * ============================================================================ */

static ShTls s_trace_id_key;
static ShOnce s_key_once = SH_ONCE_INIT;

/*
 * PTHREAD_MUTEX_INITIALIZER has no portable PAL equivalent, and zeroed storage
 * is not a substitute: glibc's static initialiser happens to be all zeroes,
 * but macOS uses a signature value, so a zero-initialised pthread_mutex_t is
 * not a valid mutex there. The portable form is lazy initialisation through
 * sh_once(), which is why every lock goes through the helper below.
 */
static ShMutex s_log_mutex;

static void trace_id_destructor(void *ptr) {
    free(ptr);
}

/* Both the TLS key and the log mutex, so a single sh_once() covers whichever
 * entry point the process reaches first. */
static void log_init_once(void) {
    sh_mutex_init(&s_log_mutex);
    sh_tls_create(&s_trace_id_key, trace_id_destructor);
}

static void log_lock(void) {
    sh_once(&s_key_once, log_init_once);
    sh_mutex_lock(&s_log_mutex);
}

static void log_unlock(void) {
    sh_mutex_unlock(&s_log_mutex);
}

/* ============================================================================
 * Global State
 *
 * DESIGN NOTE: Logging is an intentional singleton. Multiple logger instances
 * would cause interleaved output and inconsistent configuration. This is an
 * accepted exception to the "no static state in libraries" rule.
 *
 * Thread safety: All access to s_config protected by s_log_mutex.
 * ============================================================================ */

static ShLogConfig s_config = SH_LOG_CONFIG_DEFAULT;
static int s_initialized = 0;

/* ============================================================================
 * ANSI Colors
 * ============================================================================ */

#define ANSI_RESET   "\033[0m"
#define ANSI_GRAY    "\033[90m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_RED     "\033[31m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_BOLD    "\033[1m"

static const char *level_colors[] = {
    ANSI_GRAY,    /* TRACE */
    ANSI_CYAN,    /* DEBUG */
    ANSI_GREEN,   /* INFO */
    ANSI_YELLOW,  /* WARN */
    ANSI_RED,     /* ERROR */
    ANSI_BOLD ANSI_RED, /* FATAL */
};

/* ============================================================================
 * Level/Format Strings
 * ============================================================================ */

static const char *level_names[] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "OFF"
};

static const char *level_names_lower[] = {
    "trace", "debug", "info", "warn", "error", "fatal", "off"
};

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

ShLogLevel sh_log_level_from_string(const char *str) {
    if (!str) return SH_LOG_LEVEL_INFO;

    for (int i = 0; i <= SH_LOG_LEVEL_OFF; i++) {
        if (strcasecmp(str, level_names[i]) == 0 ||
            strcasecmp(str, level_names_lower[i]) == 0) {
            return (ShLogLevel)i;
        }
    }
    return SH_LOG_LEVEL_INFO;
}

const char *sh_log_level_to_string(ShLogLevel level) {
    if (level < 0 || level > SH_LOG_LEVEL_OFF) return "UNKNOWN";
    return level_names[level];
}

ShLogFormat sh_log_format_from_string(const char *str) {
    if (!str) return SH_LOG_FORMAT_TEXT;
    if (strcasecmp(str, "json") == 0) return SH_LOG_FORMAT_JSON;
    return SH_LOG_FORMAT_TEXT;
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

void sh_log_init(const ShLogConfig *config) {
    sh_once(&s_key_once, log_init_once);

    log_lock();

    if (config) {
        s_config = *config;
    } else {
        s_config.level = SH_LOG_LEVEL_INFO;
        s_config.format = SH_LOG_FORMAT_TEXT;
        s_config.color = -1;
        s_config.service = NULL;
        s_config.version = NULL;
    }

    /* Override from environment variables */
    const char *env_level = getenv("SH_LOG_LEVEL");
    if (env_level) {
        s_config.level = sh_log_level_from_string(env_level);
    }

    const char *env_format = getenv("SH_LOG_FORMAT");
    if (env_format) {
        s_config.format = sh_log_format_from_string(env_format);
    }

    const char *env_color = getenv("SH_LOG_COLOR");
    if (env_color) {
        s_config.color = sh_parse_int(env_color, 0, 0, 1);
    }

    /* Auto-detect color if not explicitly set */
    if (s_config.color < 0) {
        s_config.color = sh_stderr_is_tty();
    }

    s_initialized = 1;

    log_unlock();
}

void sh_log_shutdown(void) {
    log_lock();
    s_initialized = 0;
    log_unlock();

    /* Flush stderr */
    fflush(stderr);
}

ShLogLevel sh_log_get_level(void) {
    return s_config.level;
}

void sh_log_set_level(ShLogLevel level) {
    log_lock();
    s_config.level = level;
    log_unlock();
}

int sh_log_enabled(ShLogLevel level) {
    return level >= s_config.level;
}

/* ============================================================================
 * Trace ID Management
 * ============================================================================ */

void sh_log_set_trace_id(const char *trace_id) {
    sh_once(&s_key_once, log_init_once);

    /* Free existing trace ID */
    char *old = sh_tls_get(&s_trace_id_key);
    if (old) {
        free(old);
    }

    if (trace_id) {
        char *copy = strdup(trace_id);
        if (copy) {
            sh_tls_set(&s_trace_id_key, copy);
        }
    } else {
        sh_tls_set(&s_trace_id_key, NULL);
    }
}

const char *sh_log_get_trace_id(void) {
    sh_once(&s_key_once, log_init_once);
    return sh_tls_get(&s_trace_id_key);
}

/* ============================================================================
 * JSON Escaping
 * ============================================================================ */

static void write_json_string(FILE *fp, const char *str) {
    fputc('"', fp);
    if (str) {
        for (const char *p = str; *p; p++) {
            switch (*p) {
                case '"':  fputs("\\\"", fp); break;
                case '\\': fputs("\\\\", fp); break;
                case '\b': fputs("\\b", fp); break;
                case '\f': fputs("\\f", fp); break;
                case '\n': fputs("\\n", fp); break;
                case '\r': fputs("\\r", fp); break;
                case '\t': fputs("\\t", fp); break;
                default:
                    if ((unsigned char)*p < 0x20) {
                        fprintf(fp, "\\u%04x", (unsigned char)*p);
                    } else {
                        fputc(*p, fp);
                    }
            }
        }
    }
    fputc('"', fp);
}

/* ============================================================================
 * Timestamp Generation
 * ============================================================================ */

static void get_timestamp(char *buf, size_t len, int json_format) {
    /* Wall clock through the PAL: gmtime_r does not exist on Windows, and
     * clock_gettime(CLOCK_REALTIME) is patchy there too. */
    uint64_t now_ms = sh_wall_ms();
    struct timespec ts;
    struct tm tm;

    ts.tv_sec  = (time_t)(now_ms / 1000ull);
    ts.tv_nsec = (long)((now_ms % 1000ull) * 1000000ull);

    memset(&tm, 0, sizeof(tm));
    sh_gmtime((int64_t)ts.tv_sec, &tm);

    if (json_format) {
        /* ISO 8601 format for JSON */
        snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec,
                 ts.tv_nsec / 1000000);
    } else {
        /* Compact format for text */
        snprintf(buf, len, "%02d:%02d:%02d.%03ld",
                 tm.tm_hour, tm.tm_min, tm.tm_sec,
                 ts.tv_nsec / 1000000);
    }
}

/* ============================================================================
 * Core Logging Implementation
 * ============================================================================ */

void sh_log_v(ShLogLevel level, const char *file, int line,
              const char *trace_id, const char *message, va_list fields) {
    if (level < s_config.level) return;

    /* Get trace ID from parameter or thread-local */
    if (!trace_id) {
        trace_id = sh_log_get_trace_id();
    }

    /* Get timestamp */
    char timestamp[32];
    get_timestamp(timestamp, sizeof(timestamp), s_config.format == SH_LOG_FORMAT_JSON);

    /* Extract just filename from path */
    const char *filename = file;
    if (file) {
        const char *slash = strrchr(file, '/');
        if (slash) filename = slash + 1;
    }

    log_lock();

    if (s_config.format == SH_LOG_FORMAT_JSON) {
        /* JSON format */
        fprintf(stderr, "{\"timestamp\":\"%s\",\"level\":\"%s\"",
                timestamp, level_names_lower[level]);

        if (s_config.service) {
            fputs(",\"service\":", stderr);
            write_json_string(stderr, s_config.service);
        }

        if (s_config.version) {
            fputs(",\"version\":", stderr);
            write_json_string(stderr, s_config.version);
        }

        if (trace_id) {
            fputs(",\"trace_id\":", stderr);
            write_json_string(stderr, trace_id);
        }

        if (filename) {
            fprintf(stderr, ",\"file\":\"%s\",\"line\":%d", filename, line);
        }

        fputs(",\"message\":", stderr);
        write_json_string(stderr, message);

        /* Write additional fields */
        const char *key;
        while ((key = va_arg(fields, const char *)) != NULL) {
            const char *value = va_arg(fields, const char *);
            if (value) {
                fputc(',', stderr);
                write_json_string(stderr, key);
                fputc(':', stderr);
                write_json_string(stderr, value);
            }
        }

        fputs("}\n", stderr);

    } else {
        /* Text format */
        int use_color = s_config.color && level < SH_LOG_LEVEL_OFF;

        if (use_color) {
            fprintf(stderr, "%s%-5s%s %s%s%s ",
                    level_colors[level], level_names[level], ANSI_RESET,
                    ANSI_GRAY, timestamp, ANSI_RESET);
        } else {
            fprintf(stderr, "%-5s %s ", level_names[level], timestamp);
        }

        if (trace_id) {
            if (use_color) {
                fprintf(stderr, "%s[%.8s]%s ", ANSI_CYAN, trace_id, ANSI_RESET);
            } else {
                fprintf(stderr, "[%.8s] ", trace_id);
            }
        }

        fprintf(stderr, "%s", message ? message : "");

        /* Write additional fields */
        const char *key;
        int first_field = 1;
        while ((key = va_arg(fields, const char *)) != NULL) {
            const char *value = va_arg(fields, const char *);
            if (value) {
                if (first_field) {
                    fputs(" |", stderr);
                    first_field = 0;
                }
                if (use_color) {
                    fprintf(stderr, " %s%s%s=%s", ANSI_CYAN, key, ANSI_RESET, value);
                } else {
                    fprintf(stderr, " %s=%s", key, value);
                }
            }
        }

        if (filename && level >= SH_LOG_LEVEL_WARN) {
            if (use_color) {
                fprintf(stderr, " %s(%s:%d)%s", ANSI_GRAY, filename, line, ANSI_RESET);
            } else {
                fprintf(stderr, " (%s:%d)", filename, line);
            }
        }

        fputc('\n', stderr);
    }

    log_unlock();

    /* Flush immediately for ERROR and FATAL */
    if (level >= SH_LOG_LEVEL_ERROR) {
        fflush(stderr);
    }
}

void sh_log(ShLogLevel level, const char *file, int line,
            const char *message, ...) {
    va_list args;
    va_start(args, message);
    sh_log_v(level, file, line, NULL, message, args);
    va_end(args);
}

void sh_log_with_trace(ShLogLevel level, const char *file, int line,
                       const char *trace_id, const char *message, ...) {
    va_list args;
    va_start(args, message);
    sh_log_v(level, file, line, trace_id, message, args);
    va_end(args);
}
