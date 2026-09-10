/*
 * lint-format-truncation.h - make MinGW reproduce the Linux snprintf checks.
 *
 * Nexus is the only OTTO library built with -Werror, which makes GCC's
 * -Wformat-truncation a build failure rather than a warning. On Linux it fires;
 * on MinGW it never does, because MinGW routes snprintf through
 * __mingw_snprintf, which GCC does not recognise as the builtin and therefore
 * does not analyse. So a Windows developer can build cleanly and still break
 * the Linux build -- which is exactly what happened when nexus was first wired
 * into CI.
 *
 * Force-including this header points snprintf at the builtin, restoring the
 * analysis without changing what any of the code means. It is a lint aid only,
 * never part of a real build.
 *
 *   cd nexus
 *   for f in src/*.c tools/*.c tests/test_*.c; do
 *       gcc -Wall -Wextra -Wrestrict -O3 -std=c11 -D_GNU_SOURCE \
 *           -include scripts/lint-format-truncation.h \
 *           -Iinclude -Itests -I../shared/include \
 *           -isystem ../vendor/miniz -isystem ../vendor/tre \
 *           -c "$f" -o /dev/null
 *   done
 *
 * Caveat: this recovers -Wformat-truncation but NOT -Wrestrict. That one needs
 * glibc's _FORTIFY_SOURCE wrapper (__builtin___snprintf_chk), whose parameters
 * are restrict-qualified where the plain builtin's are not. CI remains the only
 * check for that class.
 */

#include <stdio.h>

#undef snprintf
#define snprintf __builtin_snprintf

#undef vsnprintf
#define vsnprintf __builtin_vsnprintf
