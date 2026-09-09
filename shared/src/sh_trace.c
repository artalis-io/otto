/*
 * sh_trace.c - Trace ID Generation and Propagation Implementation
 */

#include "sh_trace.h"
#include "sh_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sh_pal.h"
#include <time.h>
#include <ctype.h>

/* Entropy comes from sh_pal_random_bytes(); no platform header here.
 * The two arms of the old #ifdef included the same file anyway. */

/* ============================================================================
 * Thread-Local Storage
 *
 * DESIGN NOTE: Thread-local storage keys must be global by definition. The
 * actual trace data is per-thread (not shared). This is an accepted exception
 * to the "no static state in libraries" rule, as TLS keys are inherently global.
 *
 * Thread safety: sh_tls_create is called once via sh_once. TLS access
 * is thread-safe by design.
 * ============================================================================ */

static ShTls s_trace_key;
static ShOnce s_key_once = SH_ONCE_INIT;

static void trace_destructor(void *ptr) {
    free(ptr);
}

static void create_trace_key(void) {
    sh_tls_create(&s_trace_key, trace_destructor);
}

/* ============================================================================
 * Random Number Generation
 * ============================================================================ */

static void get_random_bytes(void *buf, size_t len) {
    /* Entropy through the PAL: getentropy and /dev/urandom do not exist on
     * Windows, where this is BCryptGenRandom. */
    if (sh_pal_random_bytes(buf, len) == 0) {
        return;
    }

    /*
     * Last resort: use time-based PRNG (not cryptographically secure).
     *
     * SECURITY NOTE: This fallback uses a weak LCG PRNG. This is acceptable
     * for trace IDs which only need uniqueness, not unpredictability. Trace
     * IDs are not used for security purposes (auth tokens, session IDs, etc.).
     * If getentropy and /dev/urandom both fail, system is likely misconfigured.
     */
    static unsigned int seed = 0;
    if (seed == 0) {
        /* Process id, not a thread id: `seed` is a single static shared
         * by every thread that reaches this fallback, so mixing in a
         * per-thread value never actually distinguished them. The PAL
         * has no thread-id call and this path does not warrant adding
         * one -- it runs only when the platform CSPRNG has failed. */
        seed = (unsigned int)time(NULL) ^ (unsigned int)sh_pal_pid();
    }
    unsigned char *p = buf;
    for (size_t i = 0; i < len; i++) {
        seed = seed * 1103515245 + 12345;
        p[i] = (seed >> 16) & 0xFF;
    }
}

/* ============================================================================
 * UUID v4 Generation
 * ============================================================================ */

char *sh_trace_generate(char *buf) {
    if (!buf) return NULL;

    unsigned char bytes[16];
    get_random_bytes(bytes, sizeof(bytes));

    /* Set version to 4 (random UUID) */
    bytes[6] = (bytes[6] & 0x0F) | 0x40;

    /* Set variant to RFC 4122 */
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    /* Format as UUID string */
    snprintf(buf, SH_TRACE_ID_LEN,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5],
             bytes[6], bytes[7],
             bytes[8], bytes[9],
             bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);

    return buf;
}

int sh_trace_validate(const char *trace_id) {
    if (!trace_id) return 0;

    size_t len = strlen(trace_id);

    /* Accept UUID format (36 chars with hyphens) */
    if (len == 36) {
        for (size_t i = 0; i < 36; i++) {
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                if (trace_id[i] != '-') return 0;
            } else {
                if (!isxdigit((unsigned char)trace_id[i])) return 0;
            }
        }
        return 1;
    }

    /* Accept short hex IDs (8-64 chars) for compatibility */
    if (len >= 8 && len <= 64) {
        for (size_t i = 0; i < len; i++) {
            if (!isxdigit((unsigned char)trace_id[i])) return 0;
        }
        return 1;
    }

    return 0;
}

/* ============================================================================
 * Thread-Local Trace Context
 * ============================================================================ */

void sh_trace_set(const char *trace_id) {
    sh_once(&s_key_once, create_trace_key);

    /* Free existing trace ID */
    char *old = sh_tls_get(&s_trace_key);
    if (old) {
        free(old);
    }

    if (trace_id && trace_id[0]) {
        char *copy = strdup(trace_id);
        if (copy) {
            sh_tls_set(&s_trace_key, copy);
            sh_log_set_trace_id(copy);
        }
    } else {
        sh_tls_set(&s_trace_key, NULL);
        sh_log_set_trace_id(NULL);
    }
}

const char *sh_trace_get(void) {
    sh_once(&s_key_once, create_trace_key);
    return sh_tls_get(&s_trace_key);
}

const char *sh_trace_new(void) {
    char buf[SH_TRACE_ID_LEN];
    sh_trace_generate(buf);
    sh_trace_set(buf);
    return sh_trace_get();
}

void sh_trace_clear(void) {
    sh_trace_set(NULL);
}

/* ============================================================================
 * HTTP Header Helpers
 * ============================================================================ */

const char *sh_trace_from_headers(sh_trace_header_getter getter, void *ctx) {
    if (!getter) {
        return sh_trace_new();
    }

    /* Try X-Trace-Id first */
    const char *trace_id = getter(SH_TRACE_HEADER_X_TRACE_ID, ctx);
    if (trace_id && sh_trace_validate(trace_id)) {
        sh_trace_set(trace_id);
        return sh_trace_get();
    }

    /* Try X-Request-Id */
    trace_id = getter(SH_TRACE_HEADER_X_REQUEST_ID, ctx);
    if (trace_id && sh_trace_validate(trace_id)) {
        sh_trace_set(trace_id);
        return sh_trace_get();
    }

    /* Try W3C traceparent header */
    const char *traceparent = getter(SH_TRACE_HEADER_TRACEPARENT, ctx);
    if (traceparent) {
        /* Format: version-trace_id-parent_id-flags */
        /* Example: 00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01 */
        if (strlen(traceparent) >= 55 && traceparent[2] == '-') {
            /* Extract trace_id (32 hex chars) */
            char extracted[33];
            memcpy(extracted, traceparent + 3, 32);
            extracted[32] = '\0';
            if (sh_trace_validate(extracted)) {
                sh_trace_set(extracted);
                return sh_trace_get();
            }
        }
    }

    /* No valid trace ID found, generate new one */
    return sh_trace_new();
}

int sh_trace_format_header(char *buf, size_t len) {
    const char *trace_id = sh_trace_get();
    if (!trace_id || !buf || len == 0) {
        if (buf && len > 0) buf[0] = '\0';
        return 0;
    }

    return snprintf(buf, len, "X-Trace-Id: %s\r\n", trace_id);
}

/* ============================================================================
 * Span Support
 * ============================================================================ */

static double get_monotonic_time(void) {
    return (double)sh_monotonic_ns() / 1.0e9;
}

static void generate_span_id(char *buf) {
    unsigned char bytes[8];
    get_random_bytes(bytes, sizeof(bytes));
    snprintf(buf, 17, "%02x%02x%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5], bytes[6], bytes[7]);
}

void sh_trace_span_start(ShTraceSpan *span, const char *operation) {
    if (!span) return;

    memset(span, 0, sizeof(*span));

    /* Get or create trace ID */
    const char *trace_id = sh_trace_get();
    if (!trace_id) {
        trace_id = sh_trace_new();
    }
    strncpy(span->trace_id, trace_id, SH_TRACE_ID_LEN - 1);
    span->trace_id[SH_TRACE_ID_LEN - 1] = '\0';

    /* Generate span ID */
    generate_span_id(span->span_id);

    span->operation = operation;
    span->start_time = get_monotonic_time();

    SH_LOG_DEBUG("Span started",
                 "operation", operation,
                 "span_id", span->span_id);
}

void sh_trace_span_end(ShTraceSpan *span) {
    if (!span || span->start_time == 0) return;

    double duration = get_monotonic_time() - span->start_time;
    char duration_str[32];
    snprintf(duration_str, sizeof(duration_str), "%.3f", duration * 1000);

    SH_LOG_DEBUG("Span completed",
                 "operation", span->operation,
                 "span_id", span->span_id,
                 "duration_ms", duration_str);

    span->start_time = 0;
}
