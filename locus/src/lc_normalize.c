/*
 * lc_normalize.c - Text normalization for geocoding
 *
 * Implements UTF-8 aware text normalization including:
 * - Case folding (lowercase)
 * - Diacritic removal (á→a, ö→o, etc.)
 * - Whitespace normalization
 * - Punctuation removal
 * - Abbreviation expansion
 */

#include "lc_normalize.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ============================================================================
 * Diacritic Mapping Tables
 *
 * Maps precomposed characters to their base ASCII equivalent.
 * Covers Latin Extended-A, Latin Extended-B, and Latin-1 Supplement.
 * ============================================================================ */

/* Latin-1 Supplement (U+00C0 - U+00FF) */
static const char latin1_map[64] = {
    /* À-Ï (0xC0-0xCF) */
    'A', 'A', 'A', 'A', 'A', 'A', 'A', 'C',
    'E', 'E', 'E', 'E', 'I', 'I', 'I', 'I',
    /* Ð-ß (0xD0-0xDF) */
    'D', 'N', 'O', 'O', 'O', 'O', 'O', '*',
    'O', 'U', 'U', 'U', 'U', 'Y', 'T', 's',
    /* à-ï (0xE0-0xEF) */
    'a', 'a', 'a', 'a', 'a', 'a', 'a', 'c',
    'e', 'e', 'e', 'e', 'i', 'i', 'i', 'i',
    /* ð-ÿ (0xF0-0xFF) */
    'd', 'n', 'o', 'o', 'o', 'o', 'o', '/',
    'o', 'u', 'u', 'u', 'u', 'y', 't', 'y'
};

/* Latin Extended-A (U+0100 - U+017F) - first 128 characters */
static const char latin_ext_a[128] = {
    'A', 'a', 'A', 'a', 'A', 'a', 'C', 'c',  /* 0100-0107 */
    'C', 'c', 'C', 'c', 'C', 'c', 'D', 'd',  /* 0108-010F */
    'D', 'd', 'E', 'e', 'E', 'e', 'E', 'e',  /* 0110-0117 */
    'E', 'e', 'E', 'e', 'G', 'g', 'G', 'g',  /* 0118-011F */
    'G', 'g', 'G', 'g', 'H', 'h', 'H', 'h',  /* 0120-0127 */
    'I', 'i', 'I', 'i', 'I', 'i', 'I', 'i',  /* 0128-012F */
    'I', 'i', 'I', 'i', 'J', 'j', 'K', 'k',  /* 0130-0137 */
    'k', 'L', 'l', 'L', 'l', 'L', 'l', 'L',  /* 0138-013F */
    'l', 'L', 'l', 'N', 'n', 'N', 'n', 'N',  /* 0140-0147 */
    'n', 'n', 'N', 'n', 'O', 'o', 'O', 'o',  /* 0148-014F */
    'O', 'o', 'O', 'o', 'R', 'r', 'R', 'r',  /* 0150-0157 */
    'R', 'r', 'S', 's', 'S', 's', 'S', 's',  /* 0158-015F */
    'S', 's', 'T', 't', 'T', 't', 'T', 't',  /* 0160-0167 */
    'U', 'u', 'U', 'u', 'U', 'u', 'U', 'u',  /* 0168-016F */
    'U', 'u', 'U', 'u', 'W', 'w', 'Y', 'y',  /* 0170-0177 */
    'Y', 'Z', 'z', 'Z', 'z', 'Z', 'z', 's'   /* 0178-017F */
};

/* Cyrillic lowercase mapping (U+0410-U+044F) - just for case folding */
/* Maps uppercase Cyrillic to lowercase (add 0x20) */

/* ============================================================================
 * UTF-8 Decoding/Encoding Helpers
 * ============================================================================ */

/* Decode a UTF-8 character, return codepoint and bytes consumed */
static int utf8_decode(const uint8_t *str, uint32_t *cp)
{
    if (!str || !str[0]) {
        *cp = 0;
        return 0;
    }

    if ((str[0] & 0x80) == 0) {
        /* ASCII */
        *cp = str[0];
        return 1;
    } else if ((str[0] & 0xE0) == 0xC0) {
        /* 2-byte sequence */
        if ((str[1] & 0xC0) != 0x80) return 0;
        *cp = ((str[0] & 0x1F) << 6) | (str[1] & 0x3F);
        return 2;
    } else if ((str[0] & 0xF0) == 0xE0) {
        /* 3-byte sequence */
        if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80) return 0;
        *cp = ((str[0] & 0x0F) << 12) | ((str[1] & 0x3F) << 6) | (str[2] & 0x3F);
        return 3;
    } else if ((str[0] & 0xF8) == 0xF0) {
        /* 4-byte sequence */
        if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80 || (str[3] & 0xC0) != 0x80) return 0;
        *cp = ((str[0] & 0x07) << 18) | ((str[1] & 0x3F) << 12) | ((str[2] & 0x3F) << 6) | (str[3] & 0x3F);
        return 4;
    }

    return 0;
}

/* Number of bytes in the UTF-8 sequence starting at p, clamped so it never
 * counts past the NUL terminator or a non-continuation byte. A truncated or
 * malformed multi-byte sequence yields 1 (advance one byte). Callers that do
 * `p += utf8_seq_len(p)` are therefore guaranteed never to step past the NUL,
 * even on hostile/truncated input (fixes the OOB read in the normalize loops).
 * p[0] is assumed non-NUL (the loops test *p first); each p[k] is only read
 * after p[k-1] was confirmed a non-NUL continuation byte, so no over-read. */
static int utf8_seq_len(const uint8_t *p)
{
    int len;
    if ((p[0] & 0x80) == 0)         len = 1;
    else if ((p[0] & 0xE0) == 0xC0) len = 2;
    else if ((p[0] & 0xF0) == 0xE0) len = 3;
    else if ((p[0] & 0xF8) == 0xF0) len = 4;
    else                            return 1;  /* stray continuation / invalid lead */

    for (int k = 1; k < len; k++) {
        if ((p[k] & 0xC0) != 0x80) return 1;   /* NUL or non-continuation: truncated */
    }
    return len;
}

/* Encode a codepoint as UTF-8, return bytes written */
static int utf8_encode(uint32_t cp, uint8_t *out)
{
    if (cp < 0x80) {
        out[0] = (uint8_t)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = 0xC0 | (cp >> 6);
        out[1] = 0x80 | (cp & 0x3F);
        return 2;
    } else if (cp < 0x10000) {
        out[0] = 0xE0 | (cp >> 12);
        out[1] = 0x80 | ((cp >> 6) & 0x3F);
        out[2] = 0x80 | (cp & 0x3F);
        return 3;
    } else {
        out[0] = 0xF0 | (cp >> 18);
        out[1] = 0x80 | ((cp >> 12) & 0x3F);
        out[2] = 0x80 | ((cp >> 6) & 0x3F);
        out[3] = 0x80 | (cp & 0x3F);
        return 4;
    }
}

/* ============================================================================
 * Character Classification
 * ============================================================================ */

int lc_is_alnum(const uint8_t *str)
{
    if (!str || !str[0]) return 0;

    uint32_t cp;
    int len = utf8_decode(str, &cp);
    if (len == 0) return 0;

    /* ASCII alphanumeric */
    if ((cp >= 'A' && cp <= 'Z') ||
        (cp >= 'a' && cp <= 'z') ||
        (cp >= '0' && cp <= '9')) {
        return len;
    }

    /* Latin Extended (accented letters) */
    if (cp >= 0x00C0 && cp <= 0x00FF && cp != 0x00D7 && cp != 0x00F7) {
        return len;
    }
    if (cp >= 0x0100 && cp <= 0x017F) {
        return len;
    }

    /* Cyrillic */
    if (cp >= 0x0400 && cp <= 0x04FF) {
        return len;
    }

    /* Greek */
    if (cp >= 0x0370 && cp <= 0x03FF) {
        return len;
    }

    return 0;
}

int lc_is_space(const uint8_t *str)
{
    if (!str || !str[0]) return 0;

    /* Common ASCII whitespace */
    if (str[0] == ' ' || str[0] == '\t' || str[0] == '\n' || str[0] == '\r') {
        return 1;
    }

    /* Non-breaking space (U+00A0) */
    if (str[0] == 0xC2 && str[1] == 0xA0) {
        return 2;
    }

    /* Other Unicode spaces (simplified) */
    if (str[0] == 0xE2 && str[1] == 0x80) {
        /* U+2000-U+200B (various spaces) */
        if (str[2] >= 0x80 && str[2] <= 0x8B) {
            return 3;
        }
    }

    return 0;
}

/* ============================================================================
 * Character Transformations
 * ============================================================================ */

int lc_char_to_lower(const uint8_t *input, uint8_t *output)
{
    uint32_t cp;
    int len = utf8_decode(input, &cp);
    if (len == 0) return 0;

    /* ASCII uppercase */
    if (cp >= 'A' && cp <= 'Z') {
        output[0] = (uint8_t)(cp + 32);
        return 1;
    }

    /* Latin-1 uppercase (À-Þ, except × at 0xD7) */
    if (cp >= 0x00C0 && cp <= 0x00DE && cp != 0x00D7) {
        return utf8_encode(cp + 0x20, output);
    }

    /* Latin Extended-A uppercase */
    if (cp >= 0x0100 && cp <= 0x0137) {
        /* Even codepoints are uppercase */
        if ((cp & 1) == 0) {
            return utf8_encode(cp + 1, output);
        }
    }
    if (cp >= 0x0139 && cp <= 0x0148) {
        /* Odd codepoints are uppercase */
        if ((cp & 1) == 1) {
            return utf8_encode(cp + 1, output);
        }
    }
    if (cp >= 0x014A && cp <= 0x0177) {
        /* Even codepoints are uppercase */
        if ((cp & 1) == 0) {
            return utf8_encode(cp + 1, output);
        }
    }

    /* Cyrillic uppercase (U+0410-U+042F → U+0430-U+044F) */
    if (cp >= 0x0410 && cp <= 0x042F) {
        return utf8_encode(cp + 0x20, output);
    }

    /* Greek uppercase (simplified) */
    if (cp >= 0x0391 && cp <= 0x03A9 && cp != 0x03A2) {
        return utf8_encode(cp + 0x20, output);
    }

    /* German ß remains ß (already lowercase) */
    /* Turkish İ (U+0130) → i (special case) */
    if (cp == 0x0130) {
        output[0] = 'i';
        return 1;
    }

    /* No transformation needed, copy as-is */
    memcpy(output, input, len);
    return len;
}

int lc_char_to_base(const uint8_t *input, uint8_t *output)
{
    uint32_t cp;
    int len = utf8_decode(input, &cp);
    if (len == 0) return 0;

    /* ASCII - no transformation */
    if (cp < 0x80) {
        output[0] = (uint8_t)cp;
        return 1;
    }

    /* Latin-1 Supplement (U+00C0-U+00FF) */
    if (cp >= 0x00C0 && cp <= 0x00FF) {
        char base = latin1_map[cp - 0x00C0];
        if (base != '*' && base != '/') {
            output[0] = (uint8_t)base;
            return 1;
        }
    }

    /* Latin Extended-A (U+0100-U+017F) */
    if (cp >= 0x0100 && cp <= 0x017F) {
        char base = latin_ext_a[cp - 0x0100];
        output[0] = (uint8_t)base;
        return 1;
    }

    /* German ß → ss */
    if (cp == 0x00DF) {
        output[0] = 's';
        output[1] = 's';
        return 2;
    }

    /* Turkish ı (U+0131) → i */
    if (cp == 0x0131) {
        output[0] = 'i';
        return 1;
    }

    /* Turkish İ (U+0130) → i */
    if (cp == 0x0130) {
        output[0] = 'i';
        return 1;
    }

    /* Other characters - copy as-is */
    memcpy(output, input, len);
    return len;
}

/* ============================================================================
 * String Transformations
 * ============================================================================ */

char *lc_lowercase(char *str)
{
    if (!str) return NULL;

    uint8_t *p = (uint8_t *)str;
    uint8_t *w = p;

    while (*p) {
        uint8_t buf[4];
        /* Clamped length: never advances past the NUL on truncated input. */
        int in_len = utf8_seq_len(p);

        int out_len = lc_char_to_lower(p, buf);

        /* In-place transformation (lowercase never increases size in our mappings) */
        if (out_len > 0) {
            memcpy(w, buf, out_len);
            w += out_len;
        }
        p += in_len;
    }
    *w = '\0';

    return str;
}

size_t lc_remove_diacritics_to(const char *input, char *output, size_t output_size)
{
    if (!input || !output || output_size == 0) return 0;

    const uint8_t *p = (const uint8_t *)input;
    uint8_t *w = (uint8_t *)output;
    uint8_t *end = w + output_size - 1;

    while (*p && w < end) {
        uint8_t buf[4];
        /* Clamped length: never advances past the NUL on truncated input. */
        int in_len = utf8_seq_len(p);

        int out_len = lc_char_to_base(p, buf);

        if (out_len > 0 && w + out_len <= end) {
            memcpy(w, buf, out_len);
            w += out_len;
        }
        p += in_len;
    }
    *w = '\0';

    return (size_t)(w - (uint8_t *)output);
}

char *lc_remove_diacritics(const char *input)
{
    if (!input) return NULL;

    /* Allocate buffer (diacritic removal may reduce size, never increase significantly) */
    size_t len = strlen(input);
    size_t buf_size = len + 16;  /* Small buffer for ß→ss expansions */
    char *result = malloc(buf_size);
    if (!result) return NULL;

    lc_remove_diacritics_to(input, result, buf_size);
    return result;
}

char *lc_normalize_whitespace(char *str)
{
    if (!str) return NULL;

    uint8_t *p = (uint8_t *)str;
    uint8_t *w = p;
    int in_space = 1;  /* Start as if previous was space (to trim leading) */

    while (*p) {
        int space_len = lc_is_space(p);
        if (space_len > 0) {
            if (!in_space) {
                *w++ = ' ';
                in_space = 1;
            }
            p += space_len;
        } else {
            /* Copy non-space character (clamped: safe on truncated input). */
            int char_len = utf8_seq_len(p);

            for (int i = 0; i < char_len; i++) {
                *w++ = *p++;
            }
            p -= char_len;  /* We'll advance below */
            p += char_len;
            in_space = 0;
        }
    }

    /* Trim trailing space */
    if (w > (uint8_t *)str && *(w - 1) == ' ') {
        w--;
    }
    *w = '\0';

    return str;
}

char *lc_remove_punctuation(char *str)
{
    if (!str) return NULL;

    uint8_t *p = (uint8_t *)str;
    uint8_t *w = p;

    while (*p) {
        /* Clamped length: never advances past the NUL on truncated input. */
        int char_len = utf8_seq_len(p);

        /* Keep if alphanumeric or space */
        int is_alnum = lc_is_alnum(p);
        int is_space = lc_is_space(p);

        if (is_alnum > 0) {
            for (int i = 0; i < char_len; i++) {
                *w++ = p[i];
            }
        } else if (is_space > 0) {
            *w++ = ' ';
        } else {
            /* Replace punctuation with space (to preserve word boundaries) */
            *w++ = ' ';
        }

        p += char_len;
    }
    *w = '\0';

    return str;
}

/* ============================================================================
 * Abbreviation Expansion
 * ============================================================================ */

typedef struct {
    const char *abbrev;
    const char *expansion;
} Abbreviation;

static const Abbreviation abbreviations[] = {
    {"st", "street"},
    {"str", "street"},
    {"ave", "avenue"},
    {"av", "avenue"},
    {"rd", "road"},
    {"dr", "drive"},
    {"ln", "lane"},
    {"ct", "court"},
    {"pl", "place"},
    {"blvd", "boulevard"},
    {"cir", "circle"},
    {"hwy", "highway"},
    {"pkwy", "parkway"},
    {"sq", "square"},
    {"ter", "terrace"},
    {"n", "north"},
    {"s", "south"},
    {"e", "east"},
    {"w", "west"},
    {"ne", "northeast"},
    {"nw", "northwest"},
    {"se", "southeast"},
    {"sw", "southwest"},
    {"mt", "mount"},
    {"ft", "fort"},
    {NULL, NULL}
};

char *lc_expand_abbreviations(const char *input)
{
    if (!input) return NULL;

    /* Check if any abbreviation matches (whole word) */
    for (const Abbreviation *a = abbreviations; a->abbrev; a++) {
        size_t abbr_len = strlen(a->abbrev);
        size_t input_len = strlen(input);

        /* Check for exact match or word boundary match */
        const char *pos = input;
        while ((pos = strstr(pos, a->abbrev)) != NULL) {
            /* Check word boundaries */
            int at_start = (pos == input || !isalnum((unsigned char)*(pos - 1)));
            int at_end = (pos[abbr_len] == '\0' || !isalnum((unsigned char)pos[abbr_len]));

            if (at_start && at_end) {
                /* Found a match - expand it */
                size_t exp_len = strlen(a->expansion);
                size_t new_len = input_len - abbr_len + exp_len;
                char *result = malloc(new_len + 1);
                if (!result) return NULL;

                size_t prefix_len = pos - input;
                memcpy(result, input, prefix_len);
                memcpy(result + prefix_len, a->expansion, exp_len);
                size_t suffix_len = strlen(pos + abbr_len);
                memcpy(result + prefix_len + exp_len, pos + abbr_len, suffix_len + 1);

                return result;
            }
            pos++;
        }
    }

    return NULL;  /* No expansion needed */
}

/* ============================================================================
 * Full Normalization Pipeline
 * ============================================================================ */

size_t lc_normalize_to(const char *input, char *output, size_t output_size)
{
    if (!input || !output || output_size == 0) return 0;

    /* Step 1: Remove diacritics */
    char *temp = malloc(output_size);
    if (!temp) return 0;

    lc_remove_diacritics_to(input, temp, output_size);

    /* Step 2: Lowercase */
    lc_lowercase(temp);

    /* Step 3: Normalize whitespace */
    lc_normalize_whitespace(temp);

    /* Step 4: Remove punctuation */
    lc_remove_punctuation(temp);

    /* Step 5: Final whitespace cleanup */
    lc_normalize_whitespace(temp);

    size_t len = strlen(temp);
    if (len >= output_size) len = output_size - 1;
    memcpy(output, temp, len);
    output[len] = '\0';

    free(temp);
    return len;
}

char *lc_normalize(const char *input)
{
    if (!input) return NULL;

    size_t len = strlen(input);
    size_t buf_size = len + 64;  /* Extra space for potential expansions */
    char *result = malloc(buf_size);
    if (!result) return NULL;

    lc_normalize_to(input, result, buf_size);
    return result;
}
