/*
 * lc_trie.h - Prefix Trie for Autocomplete
 *
 * Implements a memory-efficient trie for fast prefix matching.
 * Used for autocomplete and exact name matching.
 */

#ifndef LC_TRIE_H
#define LC_TRIE_H

#include <stdint.h>
#include <stddef.h>
#include "lc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Trie Configuration
 * ============================================================================ */

/* Character mapping: a-z (26) + 0-9 (10) + space (1) = 37 slots */
#define LC_TRIE_ALPHABET_SIZE 37

/* Default capacity for entity ID lists */
#define LC_TRIE_DEFAULT_CAPACITY 4

/* ============================================================================
 * Trie Node Structure
 * ============================================================================ */

typedef struct LCTrieNode {
    struct LCTrieNode *children[LC_TRIE_ALPHABET_SIZE];
    uint32_t *entity_ids;       /* Entities that match at this prefix */
    uint16_t num_entities;
    uint16_t capacity;
} LCTrieNode;

/* ============================================================================
 * Trie Structure
 * ============================================================================ */

typedef struct {
    LCTrieNode *root;
    uint32_t num_nodes;         /* Total nodes allocated */
    uint32_t num_entries;       /* Total (name, entity_id) pairs */
    size_t memory_used;         /* Approximate memory usage */
} LCTrie;

/* ============================================================================
 * Trie API
 * ============================================================================ */

/*
 * Create a new empty trie.
 * Returns NULL on allocation failure.
 */
LCTrie *lc_trie_create(void);

/*
 * Free trie and all nodes.
 */
void lc_trie_free(LCTrie *trie);

/*
 * Insert a name → entity_id mapping.
 * The name should be normalized (lowercase, no diacritics).
 * Returns LC_OK on success, LC_ERROR_OUT_OF_MEMORY on failure.
 */
LCStatus lc_trie_insert(LCTrie *trie, const char *name, uint32_t entity_id);

/*
 * Search for entities matching a prefix.
 * Results are written to the 'results' array (up to max_results).
 * Returns the number of results found.
 */
size_t lc_trie_search_prefix(const LCTrie *trie, const char *prefix,
                             size_t max_results, uint32_t *results);

/*
 * Search for entities with an exact name match.
 * Results are written to the 'results' array (up to max_results).
 * Returns the number of results found.
 */
size_t lc_trie_search_exact(const LCTrie *trie, const char *name,
                            size_t max_results, uint32_t *results);

/*
 * Get trie statistics.
 */
uint32_t lc_trie_node_count(const LCTrie *trie);
uint32_t lc_trie_entry_count(const LCTrie *trie);
size_t lc_trie_memory_usage(const LCTrie *trie);

/* ============================================================================
 * Character Mapping
 * ============================================================================ */

/*
 * Map a character to trie index (0-36) or -1 if invalid.
 * a-z → 0-25, 0-9 → 26-35, space → 36
 */
int lc_trie_char_index(char c);

/*
 * Map trie index back to character.
 */
char lc_trie_index_char(int idx);

#ifdef __cplusplus
}
#endif

#endif /* LC_TRIE_H */
