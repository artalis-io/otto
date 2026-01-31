/*
 * lc_normalize.h - Text normalization for geocoding
 *
 * Handles case folding, diacritic removal, and whitespace normalization
 * for UTF-8 text to improve search matching.
 */

#ifndef LC_NORMALIZE_H
#define LC_NORMALIZE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Normalization Functions
 * ============================================================================ */

/*
 * Full normalization pipeline: lowercase + diacritics + whitespace + punctuation
 * Returns normalized string (caller must free) or NULL on error.
 */
char *lc_normalize(const char *input);

/*
 * Normalize into provided buffer. Returns bytes written (excluding null).
 * If buffer is too small, returns required size (caller should retry with larger buffer).
 */
size_t lc_normalize_to(const char *input, char *output, size_t output_size);

/*
 * Convert UTF-8 string to lowercase in place.
 * Handles Latin, Cyrillic, and Greek scripts.
 * Returns the string for chaining.
 */
char *lc_lowercase(char *str);

/*
 * Remove diacritical marks from UTF-8 string.
 * á→a, ö→o, ñ→n, ç→c, etc.
 * Returns new string (caller must free) or NULL on error.
 */
char *lc_remove_diacritics(const char *input);

/*
 * Remove diacritics into provided buffer.
 * Returns bytes written (excluding null).
 */
size_t lc_remove_diacritics_to(const char *input, char *output, size_t output_size);

/*
 * Normalize whitespace: collapse multiple spaces, trim leading/trailing.
 * Modifies string in place. Returns the string for chaining.
 */
char *lc_normalize_whitespace(char *str);

/*
 * Remove punctuation (non-alphanumeric except space).
 * Modifies string in place. Returns the string for chaining.
 */
char *lc_remove_punctuation(char *str);

/*
 * Expand common abbreviations: "st" → "street", "ave" → "avenue", etc.
 * Returns new string (caller must free) or NULL if no expansion needed.
 */
char *lc_expand_abbreviations(const char *input);

/* ============================================================================
 * Character Classification (UTF-8 aware)
 * ============================================================================ */

/*
 * Check if character is alphanumeric (UTF-8 aware).
 * Returns number of bytes consumed (1-4), or 0 if not alphanumeric.
 */
int lc_is_alnum(const uint8_t *str);

/*
 * Check if character is whitespace.
 * Returns number of bytes consumed (1-3), or 0 if not whitespace.
 */
int lc_is_space(const uint8_t *str);

/*
 * Get lowercase version of a UTF-8 character.
 * Writes to output buffer. Returns bytes written (1-4).
 */
int lc_char_to_lower(const uint8_t *input, uint8_t *output);

/*
 * Get base character (no diacritics) of a UTF-8 character.
 * Writes to output buffer. Returns bytes written (1-4).
 */
int lc_char_to_base(const uint8_t *input, uint8_t *output);

#ifdef __cplusplus
}
#endif

#endif /* LC_NORMALIZE_H */
