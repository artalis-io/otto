/*
 * nx_slug.c - Slugification for row ID generation
 *
 * Converts arbitrary strings to URL-safe slugs:
 * - Lowercase ASCII letters and digits preserved
 * - Spaces, underscores, dots → hyphens
 * - Consecutive hyphens collapsed to single hyphen
 * - Leading/trailing hyphens removed
 * - Non-ASCII bytes stripped
 */

#include "nx_slug.h"
#include <string.h>
#include <ctype.h>

size_t nx_slugify(const char *input, size_t input_len,
                  char *output, size_t output_cap)
{
    if (!input || !output || output_cap == 0) return 0;

    size_t w = 0;
    int prev_hyphen = 1; /* Start as true to skip leading hyphens */

    for (size_t i = 0; i < input_len && w < output_cap - 1; i++) {
        unsigned char c = (unsigned char)input[i];

        if (c >= 'A' && c <= 'Z') {
            /* Uppercase → lowercase */
            output[w++] = (char)(c + 32);
            prev_hyphen = 0;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            output[w++] = (char)c;
            prev_hyphen = 0;
        } else if (c == ' ' || c == '_' || c == '.' || c == '-' ||
                   c == '/' || c == '\\' || c == '#') {
            /* Replace with hyphen, but collapse consecutive */
            if (!prev_hyphen && w < output_cap - 1) {
                output[w++] = '-';
                prev_hyphen = 1;
            }
        }
        /* All other characters (including non-ASCII) are stripped */
    }

    /* Remove trailing hyphen */
    if (w > 0 && output[w - 1] == '-')
        w--;

    output[w] = '\0';
    return w;
}
