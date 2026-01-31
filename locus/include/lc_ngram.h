/*
 * lc_ngram.h - N-gram Index for Fuzzy Matching
 *
 * Implements a 3-gram (trigram) index for approximate string matching.
 * Used when exact/prefix matching fails to find results.
 */

#ifndef LC_NGRAM_H
#define LC_NGRAM_H

#include <stdint.h>
#include <stddef.h>
#include "lc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * N-gram Configuration
 * ============================================================================ */

#define LC_NGRAM_SIZE 3  /* Use trigrams */

/* ============================================================================
 * N-gram Entry
 * ============================================================================ */

typedef struct {
    char ngram[LC_NGRAM_SIZE + 1];  /* Null-terminated trigram */
    uint32_t *entity_ids;
    uint32_t count;
    uint32_t capacity;
} LCNgramEntry;

/* ============================================================================
 * N-gram Index
 * ============================================================================ */

typedef struct {
    LCNgramEntry *entries;      /* Sorted by ngram for binary search */
    uint32_t num_entries;
    uint32_t capacity;
    uint32_t max_entity_id;     /* For allocating hit buffer */
    size_t memory_used;
} LCNgramIndex;

/* ============================================================================
 * Fuzzy Search Result (intermediate)
 * ============================================================================ */

typedef struct {
    uint32_t entity_id;
    float score;                /* Jaccard similarity */
} LCFuzzyMatch;

/* ============================================================================
 * N-gram Index API
 * ============================================================================ */

/*
 * Create a new empty n-gram index.
 */
LCNgramIndex *lc_ngram_create(void);

/*
 * Free n-gram index.
 */
void lc_ngram_free(LCNgramIndex *idx);

/*
 * Index a name (add all its n-grams pointing to entity_id).
 * The name should be normalized.
 */
LCStatus lc_ngram_index_name(LCNgramIndex *idx, const char *name, uint32_t entity_id);

/*
 * Build index (sort entries for binary search).
 * Must be called after all insertions, before searching.
 */
void lc_ngram_build(LCNgramIndex *idx);

/*
 * Search for entities matching a query with at least 'threshold' similarity.
 * threshold: 0.0-1.0 (e.g., 0.5 = 50% n-grams must match)
 * Results are written to 'results' (up to max_results), sorted by score desc.
 * Returns number of results found.
 */
size_t lc_ngram_search(const LCNgramIndex *idx, const char *query,
                       float threshold, size_t max_results, LCFuzzyMatch *results);

/*
 * Get index statistics.
 */
uint32_t lc_ngram_entry_count(const LCNgramIndex *idx);
size_t lc_ngram_memory_usage(const LCNgramIndex *idx);

/* ============================================================================
 * N-gram Utilities
 * ============================================================================ */

/*
 * Generate n-grams from a string.
 * Writes up to max_ngrams trigrams to the output buffer.
 * Returns number of n-grams generated.
 */
size_t lc_ngram_generate(const char *str, char ngrams[][LC_NGRAM_SIZE + 1], size_t max_ngrams);

/*
 * Compute Jaccard similarity between two strings based on n-grams.
 * Returns similarity in [0, 1].
 */
float lc_ngram_similarity(const char *s1, const char *s2);

#ifdef __cplusplus
}
#endif

#endif /* LC_NGRAM_H */
