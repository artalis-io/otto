/*
 * lc_query.h - Query parsing for structured address searches
 *
 * Parses queries like "Edvi Illés út 7" into:
 *   - street_name: "Edvi Illés út"
 *   - housenumber: "7"
 *
 * Supports patterns:
 *   - "Street Name 123"      (Hungarian/European style)
 *   - "123 Street Name"      (US style)
 *   - "Street Name 123/A"    (with suffix)
 *   - "Street Name 12-14"    (range)
 */

#ifndef LC_QUERY_H
#define LC_QUERY_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Parsed Query Structure
 * ============================================================================ */

typedef struct {
    char *street;           /* Extracted street name (allocated, caller frees) */
    char *housenumber;      /* Extracted house number (allocated, caller frees) */
    bool has_housenumber;   /* True if a house number was found */
} LCParsedQuery;

/* ============================================================================
 * Query Parsing API
 * ============================================================================ */

/**
 * Parse a query string to extract street name and house number.
 *
 * @param query     Input query string (e.g., "Edvi Illés út 7")
 * @param result    Output parsed query (caller must call lc_parsed_query_free)
 * @return          true if parsing succeeded, false on error
 *
 * Examples:
 *   "Edvi Illés út 7"     -> street="Edvi Illés út", housenumber="7"
 *   "123 Main Street"     -> street="Main Street", housenumber="123"
 *   "Budapest"            -> street="Budapest", housenumber=NULL, has_housenumber=false
 *   "Kossuth tér 5/A"     -> street="Kossuth tér", housenumber="5/A"
 */
bool lc_parse_address_query(const char *query, LCParsedQuery *result);

/**
 * Free resources allocated by lc_parse_address_query.
 */
void lc_parsed_query_free(LCParsedQuery *pq);

/**
 * Check if a string looks like a house number.
 * Matches: "7", "123", "5/A", "12-14", "3B", etc.
 *
 * @param str   String to check
 * @return      true if it looks like a house number
 */
bool lc_is_housenumber(const char *str);

#ifdef __cplusplus
}
#endif

#endif /* LC_QUERY_H */
