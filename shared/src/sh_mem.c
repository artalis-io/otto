/*
 * sh_mem.c - Portable memmem.
 *
 * See sh_mem.h for why this exists rather than an #ifdef at each call site.
 */
#include "sh_mem.h"

#include <string.h>

const void *sh_memmem(const void *haystack, size_t haystack_len,
                      const void *needle, size_t needle_len)
{
    const unsigned char *h = (const unsigned char *)haystack;
    const unsigned char *n = (const unsigned char *)needle;
    const unsigned char *last;
    unsigned char first;

    /* An empty needle matches at the start, which is what glibc does. */
    if (needle_len == 0) return haystack;
    if (!haystack || !needle) return NULL;
    if (needle_len > haystack_len) return NULL;

    first = n[0];
    /* Last position a full needle can still start at. */
    last = h + (haystack_len - needle_len);

    while (h <= last) {
        /* memchr does the scanning; it is the part a libc optimises, and it
         * bounds the search so a missing first byte ends the loop at once. */
        const unsigned char *p =
            (const unsigned char *)memchr(h, first, (size_t)(last - h) + 1);
        if (!p) return NULL;
        if (memcmp(p, n, needle_len) == 0) return p;
        h = p + 1;
    }

    return NULL;
}
