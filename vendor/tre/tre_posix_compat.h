/*
 * tre_posix_compat.h - internal spelling shim for the vendored TRE sources.
 *
 * This is not a public header. Do not include it outside vendor/tre.
 *
 * The engine's .c files are upstream musl, and they say regcomp, regex_t,
 * REG_EXTENDED and so on. The names OTTO exports are prefixed. Rather than
 * rewrite thousands of lines -- which would make every future upstream diff
 * unreadable -- this maps one spelling onto the other, so the engine bodies
 * stay byte-comparable with musl's.
 *
 * The renames are here rather than in the build flags on purpose: compiled
 * without them, the engine would define the bare POSIX symbols and silently
 * displace the platform's regex at link time.
 */

#ifndef OTTO_VENDOR_TRE_POSIX_COMPAT_H
#define OTTO_VENDOR_TRE_POSIX_COMPAT_H

#include "tre_regex.h"

/* Types. */
typedef tre_regoff_t   regoff_t;
typedef tre_regex_t    regex_t;
typedef tre_regmatch_t regmatch_t;

/* Entry points. */
#define regcomp   tre_regcomp
#define regexec   tre_regexec
#define regfree   tre_regfree
#define regerror  tre_regerror

/* Compilation flags. */
#define REG_EXTENDED  TRE_REG_EXTENDED
#define REG_ICASE     TRE_REG_ICASE
#define REG_NEWLINE   TRE_REG_NEWLINE
#define REG_NOSUB     TRE_REG_NOSUB

/* Execution flags. */
#define REG_NOTBOL    TRE_REG_NOTBOL
#define REG_NOTEOL    TRE_REG_NOTEOL

/* Return codes. */
#define REG_OK        TRE_REG_OK
#define REG_NOMATCH   TRE_REG_NOMATCH
#define REG_BADPAT    TRE_REG_BADPAT
#define REG_ECOLLATE  TRE_REG_ECOLLATE
#define REG_ECTYPE    TRE_REG_ECTYPE
#define REG_EESCAPE   TRE_REG_EESCAPE
#define REG_ESUBREG   TRE_REG_ESUBREG
#define REG_EBRACK    TRE_REG_EBRACK
#define REG_EPAREN    TRE_REG_EPAREN
#define REG_EBRACE    TRE_REG_EBRACE
#define REG_BADBR     TRE_REG_BADBR
#define REG_ERANGE    TRE_REG_ERANGE
#define REG_ESPACE    TRE_REG_ESPACE
#define REG_BADRPT    TRE_REG_BADRPT

/* musl's regcomp.c returns this on an unsupported construct. */
#define REG_ENOSYS    (-1)

#endif /* OTTO_VENDOR_TRE_POSIX_COMPAT_H */
