/*
 * nx_slug.h - Slugification for row ID generation
 */

#ifndef NX_SLUG_H
#define NX_SLUG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Convert a string to a URL-safe slug.
 *
 * @param input      Input string
 * @param input_len  Length of input
 * @param output     Output buffer
 * @param output_cap Capacity of output buffer (including null terminator)
 * @return Length of slug (excluding null terminator)
 */
size_t nx_slugify(const char *input, size_t input_len,
                  char *output, size_t output_cap);

#ifdef __cplusplus
}
#endif

#endif /* NX_SLUG_H */
