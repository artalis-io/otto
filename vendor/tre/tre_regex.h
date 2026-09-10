/*
 * tre_regex.h - POSIX extended regular expressions, vendored.
 *
 * OTTO uses this engine on every platform, not just the ones whose libc lacks
 * <regex.h>. Validation rules and transform patterns live in customer-authored
 * config; if the engine behind them changed between Linux, macOS and Windows,
 * the same rule could accept a load on one host and reject it on another.
 * A vendored engine makes that class of difference impossible.
 *
 * The API mirrors POSIX regcomp/regexec/regfree, with every name prefixed.
 * The prefix is not decoration: without it, which engine you got would depend
 * on include order and link order, which is exactly the ambiguity this exists
 * to remove.
 *
 * Upstream: musl libc's TRE-derived regex, by Ville Laurikari.
 * Licence:  2-clause BSD. See LICENSE in this directory.
 * See CLAUDE.md for provenance and the list of local changes.
 */

#ifndef OTTO_VENDOR_TRE_REGEX_H
#define OTTO_VENDOR_TRE_REGEX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * musl types this as long, which is 64-bit on LP64 but only 32-bit on Windows
 * LLP64. ptrdiff_t is pointer-sized everywhere, which is what an offset into a
 * string wants to be.
 */
typedef ptrdiff_t tre_regoff_t;

/*
 * musl pads this struct out to glibc's regex_t layout for ABI
 * compatibility. Nothing here is ABI-compatible with anything by design,
 * and the engine only ever touches these two fields, so the padding is
 * gone.
 */
typedef struct {
    size_t re_nsub;     /* number of parenthesised subexpressions */
    void  *opaque;      /* compiled program; owned by the engine */
} tre_regex_t;

typedef struct {
    tre_regoff_t rm_so; /* byte offset of match start, or -1 */
    tre_regoff_t rm_eo; /* byte offset one past match end, or -1 */
} tre_regmatch_t;

/* Compilation flags (tre_regcomp). */
#define TRE_REG_EXTENDED  1  /* POSIX ERE rather than BRE */
#define TRE_REG_ICASE     2  /* case-insensitive */
#define TRE_REG_NEWLINE   4  /* newline is not matched by . or a negated class */
#define TRE_REG_NOSUB     8  /* report match/no-match only; no capture offsets */

/* Execution flags (tre_regexec). */
#define TRE_REG_NOTBOL    1  /* start of string is not start of line */
#define TRE_REG_NOTEOL    2  /* end of string is not end of line */

/* Return codes. */
#define TRE_REG_OK        0
#define TRE_REG_NOMATCH   1
#define TRE_REG_BADPAT    2
#define TRE_REG_ECOLLATE  3
#define TRE_REG_ECTYPE    4
#define TRE_REG_EESCAPE   5
#define TRE_REG_ESUBREG   6
#define TRE_REG_EBRACK    7
#define TRE_REG_EPAREN    8
#define TRE_REG_EBRACE    9
#define TRE_REG_BADBR    10
#define TRE_REG_ERANGE   11
#define TRE_REG_ESPACE   12
#define TRE_REG_BADRPT   13

/*
 * MSVC has __restrict but rejects C99 `restrict`, including the array-parameter
 * form `T p[restrict]`. It is only an aliasing hint, so dropping it there costs
 * optimisation and nothing else.
 */
#if defined(_MSC_VER)
  #define TRE_RESTRICT
#else
  #define TRE_RESTRICT restrict
#endif

/*
 * Compile `pattern` into `preg`. Returns TRE_REG_OK, or one of the error codes
 * above. On success the caller owns `preg` and must release it with
 * tre_regfree(). On failure `preg` must not be passed to tre_regfree().
 */
int tre_regcomp(tre_regex_t *TRE_RESTRICT preg,
                const char *TRE_RESTRICT pattern,
                int cflags);

/*
 * Match `string` against the compiled `preg`. Returns TRE_REG_OK on a match,
 * TRE_REG_NOMATCH if there is none, or TRE_REG_ESPACE if it runs out of memory.
 *
 * On a match, pmatch[0] spans the whole match and pmatch[n] spans the nth
 * capture group; a group that did not participate reports rm_so == -1. Offsets
 * are byte offsets into `string`. Pass nmatch == 0 (and pmatch == NULL) to skip
 * capture reporting, which is also what compiling with TRE_REG_NOSUB does.
 */
int tre_regexec(const tre_regex_t *TRE_RESTRICT preg,
                const char *TRE_RESTRICT string,
                size_t nmatch,
                tre_regmatch_t pmatch[TRE_RESTRICT],
                int eflags);

/* Release everything tre_regcomp() allocated into `preg`. */
void tre_regfree(tre_regex_t *preg);

/*
 * Write a human-readable description of `errcode` into `errbuf`, NUL-terminated
 * and truncated to `errbuf_size`. Returns the length the message would have
 * needed, including the NUL -- so a return greater than errbuf_size means it was
 * truncated. `preg` is accepted for POSIX compatibility and ignored.
 */
size_t tre_regerror(int errcode,
                    const tre_regex_t *TRE_RESTRICT preg,
                    char *TRE_RESTRICT errbuf,
                    size_t errbuf_size);

#ifdef __cplusplus
}
#endif

#endif /* OTTO_VENDOR_TRE_REGEX_H */
