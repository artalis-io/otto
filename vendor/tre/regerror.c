/*
 * regerror.c - error strings for the vendored TRE engine.
 *
 * Not upstream. musl's regerror.c is the one file in this engine that reaches
 * into libc internals: it pulls in locale_impl.h to route each message through
 * musl's message catalogue. Nothing else here touches the locale at all.
 *
 * OTTO does not translate diagnostics, and these strings end up in validation
 * issues that operators and support read, so they are fixed English. That drops
 * the last musl-internal dependency and makes the engine build anywhere.
 *
 * Message wording follows POSIX and matches musl's untranslated strings, so a
 * diagnostic captured on Linux before this change still reads the same.
 */

#include <string.h>

#include "tre_posix_compat.h"

static const char *const messages[] = {
    "No error",                             /* REG_OK       */
    "No match",                             /* REG_NOMATCH  */
    "Invalid regexp",                       /* REG_BADPAT   */
    "Unknown collating element",            /* REG_ECOLLATE */
    "Unknown character class name",         /* REG_ECTYPE   */
    "Trailing backslash",                   /* REG_EESCAPE  */
    "Invalid back reference",               /* REG_ESUBREG  */
    "Missing ']'",                          /* REG_EBRACK   */
    "Missing ')'",                          /* REG_EPAREN   */
    "Missing '}'",                          /* REG_EBRACE   */
    "Invalid contents of {}",                /* REG_BADBR    */
    "Invalid character range",              /* REG_ERANGE   */
    "Out of memory",                        /* REG_ESPACE   */
    "Repetition not preceded by valid expression", /* REG_BADRPT */
};

size_t tre_regerror(int errcode,
                    const tre_regex_t *TRE_RESTRICT preg,
                    char *TRE_RESTRICT errbuf,
                    size_t errbuf_size)
{
    const char *msg;
    size_t needed;

    (void)preg; /* accepted for POSIX compatibility; carries no message state */

    if (errcode >= 0 &&
        (size_t)errcode < sizeof(messages) / sizeof(messages[0])) {
        msg = messages[errcode];
    } else {
        msg = "Unknown error";
    }

    needed = strlen(msg) + 1;

    if (errbuf && errbuf_size) {
        size_t copy = (needed < errbuf_size) ? needed - 1 : errbuf_size - 1;
        memcpy(errbuf, msg, copy);
        errbuf[copy] = '\0';
    }

    return needed;
}
