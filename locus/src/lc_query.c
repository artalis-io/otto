/*
 * lc_query.c - Query parsing implementation
 */

#include "lc_query.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/**
 * Check if a string looks like a house number.
 * Must start with a digit and can contain digits, '/', '-', letters.
 * Examples: "7", "123", "5/A", "12-14", "3B"
 */
bool lc_is_housenumber(const char *str)
{
    if (!str || !*str) return false;

    /* Must start with a digit */
    if (!isdigit((unsigned char)str[0])) return false;

    /* Check rest of string */
    bool has_digit = true;  /* Already confirmed first char is digit */
    for (const char *p = str + 1; *p; p++) {
        if (isdigit((unsigned char)*p)) {
            has_digit = true;
        } else if (*p == '/' || *p == '-') {
            /* Allowed separators */
        } else if (isalpha((unsigned char)*p)) {
            /* Letter suffix (A, B, etc.) - only valid after digits */
            if (!has_digit) return false;
        } else if (isspace((unsigned char)*p)) {
            /* Space ends the house number */
            break;
        } else {
            /* Invalid character */
            return false;
        }
    }

    return has_digit;
}

/**
 * Duplicate a string, trimming whitespace.
 */
static char *strdup_trimmed(const char *start, const char *end)
{
    /* Skip leading whitespace */
    while (start < end && isspace((unsigned char)*start)) start++;

    /* Skip trailing whitespace */
    while (end > start && isspace((unsigned char)*(end - 1))) end--;

    if (start >= end) return NULL;

    size_t len = (size_t)(end - start);
    char *result = malloc(len + 1);
    if (!result) return NULL;

    memcpy(result, start, len);
    result[len] = '\0';
    return result;
}

/* ============================================================================
 * Query Parsing
 * ============================================================================ */

bool lc_parse_address_query(const char *query, LCParsedQuery *result)
{
    if (!query || !result) return false;

    memset(result, 0, sizeof(LCParsedQuery));

    /* Skip leading whitespace */
    while (isspace((unsigned char)*query)) query++;

    if (!*query) return false;

    size_t len = strlen(query);
    const char *end = query + len;

    /* Skip trailing whitespace for length calculation */
    while (end > query && isspace((unsigned char)*(end - 1))) end--;

    if (query >= end) return false;

    /*
     * Strategy: Look for house number at the end (European style) or beginning (US style)
     *
     * European: "Edvi Illés út 7", "Kossuth tér 5/A"
     * US: "123 Main Street"
     */

    /* Try European style first (number at end) */
    const char *last_space = NULL;
    for (const char *p = end - 1; p > query; p--) {
        if (isspace((unsigned char)*p)) {
            last_space = p;
            break;
        }
    }

    if (last_space) {
        const char *potential_number = last_space + 1;
        if (lc_is_housenumber(potential_number)) {
            /* Found house number at end */
            result->street = strdup_trimmed(query, last_space);
            result->housenumber = strdup_trimmed(potential_number, end);
            result->has_housenumber = true;

            if (!result->street) {
                lc_parsed_query_free(result);
                return false;
            }
            return true;
        }
    }

    /* Try US style (number at beginning) */
    const char *first_space = NULL;
    for (const char *p = query; p < end; p++) {
        if (isspace((unsigned char)*p)) {
            first_space = p;
            break;
        }
    }

    if (first_space) {
        char *potential_number = strdup_trimmed(query, first_space);
        if (potential_number && lc_is_housenumber(potential_number)) {
            /* Found house number at beginning */
            result->housenumber = potential_number;
            result->street = strdup_trimmed(first_space + 1, end);
            result->has_housenumber = true;

            if (!result->street) {
                lc_parsed_query_free(result);
                return false;
            }
            return true;
        }
        free(potential_number);
    }

    /* No house number found - entire query is street name */
    result->street = strdup_trimmed(query, end);
    result->housenumber = NULL;
    result->has_housenumber = false;

    return result->street != NULL;
}

void lc_parsed_query_free(LCParsedQuery *pq)
{
    if (!pq) return;

    free(pq->street);
    free(pq->housenumber);

    pq->street = NULL;
    pq->housenumber = NULL;
    pq->has_housenumber = false;
}
