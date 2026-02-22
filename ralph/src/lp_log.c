/*
 * Ralph - LP logging shim implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lp_log.h"

static void trim_trailing_newlines(char *msg)
{
    size_t len;
    if (!msg) return;

    len = strlen(msg);
    while (len > 0) {
        char ch = msg[len - 1];
        if (ch != '\n' && ch != '\r') break;
        msg[len - 1] = '\0';
        len--;
    }
}

void lp_log_emitvf(ShLogLevel level,
                   const char *file,
                   int line,
                   const char *fmt,
                   va_list args)
{
    char stack_buf[1024];
    char *msg = stack_buf;
    size_t cap = sizeof(stack_buf);
    int needed;
    va_list probe;

    if (!fmt) return;

    va_copy(probe, args);
    needed = vsnprintf(stack_buf, cap, fmt, probe);
    va_end(probe);

    if (needed < 0) return;

    if ((size_t)needed >= cap) {
        cap = (size_t)needed + 1;
        msg = (char *)malloc(cap);
        if (!msg) return;
        vsnprintf(msg, cap, fmt, args);
    }

    trim_trailing_newlines(msg);
    if (msg[0] == '\0') {
        if (msg != stack_buf) free(msg);
        return;
    }

    sh_log(level, file, line, msg, NULL);

    if (msg != stack_buf) free(msg);
}

void lp_log_emitf(ShLogLevel level,
                  const char *file,
                  int line,
                  const char *fmt,
                  ...)
{
    va_list args;

    va_start(args, fmt);
    lp_log_emitvf(level, file, line, fmt, args);
    va_end(args);
}

void lp_log_verbosef(int verbose,
                     int min_verbose,
                     ShLogLevel level,
                     const char *file,
                     int line,
                     const char *fmt,
                     ...)
{
    va_list args;

    if (verbose < min_verbose) return;

    va_start(args, fmt);
    lp_log_emitvf(level, file, line, fmt, args);
    va_end(args);
}
