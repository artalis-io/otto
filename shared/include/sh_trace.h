/*
 * sh_trace.h - Trace ID Generation and Propagation
 *
 * Provides UUID v4 trace ID generation and HTTP header propagation.
 * Integrates with sh_log for automatic trace ID inclusion in log messages.
 *
 * Usage:
 *   char trace_id[SH_TRACE_ID_LEN];
 *   sh_trace_generate(trace_id);
 *   sh_log_set_trace_id(trace_id);
 *   // ... do work ...
 *   sh_log_set_trace_id(NULL);  // Clear when request completes
 *
 * HTTP Header Integration:
 *   // Extract from incoming request
 *   const char *incoming = get_header("X-Trace-Id");
 *   if (incoming) {
 *       sh_trace_set(incoming);
 *   } else {
 *       sh_trace_new();  // Generate new trace ID
 *   }
 *
 *   // Add to outgoing request
 *   set_header("X-Trace-Id", sh_trace_get());
 */

#ifndef SH_TRACE_H
#define SH_TRACE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/* UUID v4 format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx (36 chars + null) */
#define SH_TRACE_ID_LEN 37

/* Common HTTP header names for trace propagation */
#define SH_TRACE_HEADER_X_TRACE_ID    "X-Trace-Id"
#define SH_TRACE_HEADER_X_REQUEST_ID  "X-Request-Id"
#define SH_TRACE_HEADER_TRACEPARENT   "traceparent"  /* W3C Trace Context */

/* ============================================================================
 * Trace ID Generation
 * ============================================================================ */

/*
 * Generate a new UUID v4 trace ID.
 * Buffer must be at least SH_TRACE_ID_LEN bytes.
 * Returns the buffer pointer for convenience.
 */
char *sh_trace_generate(char *buf);

/*
 * Validate a trace ID string.
 * Returns 1 if valid UUID v4 format, 0 otherwise.
 * Also accepts short IDs (8-64 hex chars) for compatibility.
 */
int sh_trace_validate(const char *trace_id);

/* ============================================================================
 * Thread-Local Trace Context
 * ============================================================================ */

/*
 * Set the current thread's trace ID.
 * Also updates sh_log's trace ID for automatic log correlation.
 * Pass NULL to clear the trace context.
 */
void sh_trace_set(const char *trace_id);

/*
 * Get the current thread's trace ID.
 * Returns NULL if no trace context is set.
 */
const char *sh_trace_get(void);

/*
 * Generate and set a new trace ID for the current thread.
 * Returns pointer to the new trace ID.
 */
const char *sh_trace_new(void);

/*
 * Clear the current thread's trace context.
 * Equivalent to sh_trace_set(NULL).
 */
void sh_trace_clear(void);

/* ============================================================================
 * HTTP Header Helpers
 * ============================================================================ */

/*
 * Extract trace ID from HTTP headers.
 * Checks X-Trace-Id, X-Request-Id, and traceparent headers.
 * If found, sets the thread-local trace context and returns the ID.
 * If not found, generates a new trace ID.
 *
 * header_getter: function to get header value by name
 * ctx: opaque context passed to header_getter
 *
 * Returns the trace ID (either extracted or newly generated).
 */
typedef const char *(*sh_trace_header_getter)(const char *name, void *ctx);

const char *sh_trace_from_headers(sh_trace_header_getter getter, void *ctx);

/*
 * Format trace ID for HTTP response header.
 * Writes "X-Trace-Id: <trace_id>\r\n" to buffer.
 * Returns number of bytes written (excluding null terminator).
 */
int sh_trace_format_header(char *buf, size_t len);

/* ============================================================================
 * Span Support (for distributed tracing)
 * ============================================================================ */

/*
 * A span represents a unit of work within a trace.
 */
typedef struct {
    char trace_id[SH_TRACE_ID_LEN];
    char span_id[17];    /* 64-bit hex ID */
    char parent_span_id[17];
    const char *operation;
    double start_time;   /* Monotonic time in seconds */
} ShTraceSpan;

/*
 * Start a new span under the current trace.
 * If no trace context exists, creates a new trace.
 */
void sh_trace_span_start(ShTraceSpan *span, const char *operation);

/*
 * End a span and log its duration.
 * Logs at DEBUG level with trace_id, span_id, duration_ms.
 */
void sh_trace_span_end(ShTraceSpan *span);

#ifdef __cplusplus
}
#endif

#endif /* SH_TRACE_H */
