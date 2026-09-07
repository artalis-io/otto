/*
 * sh_mem.h - Memory-block search helpers.
 *
 * memmem() is a GNU extension: glibc and the BSDs have it, Windows does not,
 * and it is not in ISO C or POSIX. Rather than #ifdef around three call sites
 * in the PDF parser, this is one portable implementation used everywhere, so
 * the behaviour is identical on every platform instead of depending on which
 * libc supplied it.
 *
 * Not in sh_pal.h: this is a missing library function, not a platform
 * capability. There is nothing to abstract -- only something to provide.
 */
#ifndef SH_MEM_H
#define SH_MEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Find the first occurrence of `needle` (needle_len bytes) in `haystack`
 * (haystack_len bytes).
 *
 * Returns a pointer into `haystack`, or NULL if not found. An empty needle
 * matches at the start, matching glibc. Binary-safe: embedded NULs are data.
 */
const void *sh_memmem(const void *haystack, size_t haystack_len,
                      const void *needle, size_t needle_len);

#ifdef __cplusplus
}
#endif

#endif /* SH_MEM_H */
