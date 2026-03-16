/*
 * Ralph - LP logging shim
 *
 * Bridges legacy printf-style verbose logs to shared sh_log infrastructure.
 */

#ifndef RALPH_LP_LOG_H
#define RALPH_LP_LOG_H

#include <stdarg.h>
#include "sh_log.h"

#if defined(__GNUC__) || defined(__clang__)
#define LP_LOG_PRINTF_ATTR(fmt_idx, args_idx) __attribute__((format(printf, fmt_idx, args_idx)))
#else
#define LP_LOG_PRINTF_ATTR(fmt_idx, args_idx)
#endif

void lp_log_emitf(ShLogLevel level,
                  const char *file,
                  int line,
                  const char *fmt,
                  ...) LP_LOG_PRINTF_ATTR(4, 5);

void lp_log_emitvf(ShLogLevel level,
                   const char *file,
                   int line,
                   const char *fmt,
                   va_list args);

void lp_log_verbosef(int verbose,
                     int min_verbose,
                     ShLogLevel level,
                     const char *file,
                     int line,
                     const char *fmt,
                     ...) LP_LOG_PRINTF_ATTR(6, 7);

/* stdout/stderr compatibility wrappers used by existing solver code paths. */
#define LP_LOG_STDOUT(...) \
    lp_log_emitf(SH_LOG_LEVEL_INFO, __FILE__, __LINE__, __VA_ARGS__)

#define LP_LOG_STDERR(...) \
    lp_log_emitf(SH_LOG_LEVEL_INFO, __FILE__, __LINE__, __VA_ARGS__)

#define LP_LOG_VERBOSE_STDOUT(verbose, min_verbose, ...) \
    lp_log_verbosef((verbose), (min_verbose), SH_LOG_LEVEL_INFO, __FILE__, __LINE__, __VA_ARGS__)

#define LP_LOG_VERBOSE_STDERR(verbose, min_verbose, ...) \
    lp_log_verbosef((verbose), (min_verbose), SH_LOG_LEVEL_INFO, __FILE__, __LINE__, __VA_ARGS__)

#endif /* RALPH_LP_LOG_H */
