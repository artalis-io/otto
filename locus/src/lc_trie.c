/*
 * lc_trie.c - Prefix Trie for Autocomplete
 *
 * Implements a memory-efficient trie for fast prefix matching.
 * Uses lazy child allocation and compact entity ID arrays.
 */

#include "lc_trie.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Character Mapping
 * ============================================================================ */

int lc_trie_char_index(char c)
{
    if (c >= 'a' && c <= 'z') return c - 'a';           /* 0-25 */
    if (c >= '0' && c <= '9') return 26 + (c - '0');    /* 26-35 */
    if (c == ' ') return 36;                             /* 36 */
    return -1;
}

char lc_trie_index_char(int idx)
{
    if (idx >= 0 && idx < 26) return 'a' + idx;
    if (idx >= 26 && idx < 36) return '0' + (idx - 26);
    if (idx == 36) return ' ';
    return '?';
}

/* ============================================================================
 * Node Management
 * ============================================================================ */

static LCTrieNode *trie_node_create(void)
{
    LCTrieNode *node = calloc(1, sizeof(LCTrieNode));
    return node;
}

static void trie_node_free(LCTrieNode *node)
{
    if (!node) return;

    /* Free children recursively */
    for (int i = 0; i < LC_TRIE_ALPHABET_SIZE; i++) {
        if (node->children[i]) {
            trie_node_free(node->children[i]);
        }
    }

    /* Free entity ID array */
    free(node->entity_ids);
    free(node);
}

static LCStatus trie_node_add_entity(LCTrieNode *node, uint32_t entity_id)
{
    /* Check if already present */
    for (uint16_t i = 0; i < node->num_entities; i++) {
        if (node->entity_ids[i] == entity_id) {
            return LC_OK;  /* Already exists */
        }
    }

    /* Grow array if needed */
    if (node->num_entities >= node->capacity) {
        uint16_t new_capacity = node->capacity == 0 ?
            LC_TRIE_DEFAULT_CAPACITY : node->capacity * 2;
        uint32_t *new_ids = realloc(node->entity_ids, new_capacity * sizeof(uint32_t));
        if (!new_ids) return LC_ERROR_OUT_OF_MEMORY;
        node->entity_ids = new_ids;
        node->capacity = new_capacity;
    }

    node->entity_ids[node->num_entities++] = entity_id;
    return LC_OK;
}

/* ============================================================================
 * Trie API
 * ============================================================================ */

LCTrie *lc_trie_create(void)
{
    LCTrie *trie = calloc(1, sizeof(LCTrie));
    if (!trie) return NULL;

    trie->root = trie_node_create();
    if (!trie->root) {
        free(trie);
        return NULL;
    }

    trie->num_nodes = 1;
    trie->memory_used = sizeof(LCTrie) + sizeof(LCTrieNode);

    return trie;
}

void lc_trie_free(LCTrie *trie)
{
    if (!trie) return;
    trie_node_free(trie->root);
    free(trie);
}

LCStatus lc_trie_insert(LCTrie *trie, const char *name, uint32_t entity_id)
{
    if (!trie || !name) return LC_ERROR_INVALID_PARAM;

    LCTrieNode *node = trie->root;

    for (const char *p = name; *p; p++) {
        int idx = lc_trie_char_index(*p);
        if (idx < 0) continue;  /* Skip invalid characters */

        if (!node->children[idx]) {
            node->children[idx] = trie_node_create();
            if (!node->children[idx]) {
                return LC_ERROR_OUT_OF_MEMORY;
            }
            trie->num_nodes++;
            trie->memory_used += sizeof(LCTrieNode);
        }
        node = node->children[idx];
    }

    /* Add entity ID to the terminal node */
    LCStatus status = trie_node_add_entity(node, entity_id);
    if (status == LC_OK) {
        trie->num_entries++;
    }

    return status;
}

/* Collect results from a subtree (DFS) */
static size_t collect_results(const LCTrieNode *node, size_t max_results,
                               uint32_t *results, size_t count)
{
    if (!node || count >= max_results) return count;

    /* Add this node's entities */
    for (uint16_t i = 0; i < node->num_entities && count < max_results; i++) {
        /* Check for duplicates (entity might appear multiple times in trie) */
        int found = 0;
        for (size_t j = 0; j < count; j++) {
            if (results[j] == node->entity_ids[i]) {
                found = 1;
                break;
            }
        }
        if (!found) {
            results[count++] = node->entity_ids[i];
        }
    }

    /* Recurse into children */
    for (int i = 0; i < LC_TRIE_ALPHABET_SIZE && count < max_results; i++) {
        if (node->children[i]) {
            count = collect_results(node->children[i], max_results, results, count);
        }
    }

    return count;
}

size_t lc_trie_search_prefix(const LCTrie *trie, const char *prefix,
                             size_t max_results, uint32_t *results)
{
    if (!trie || !results || max_results == 0) return 0;

    const LCTrieNode *node = trie->root;

    /* Navigate to prefix node */
    if (prefix) {
        for (const char *p = prefix; *p; p++) {
            int idx = lc_trie_char_index(*p);
            if (idx < 0) continue;

            if (!node->children[idx]) {
                return 0;  /* Prefix not found */
            }
            node = node->children[idx];
        }
    }

    /* Collect all results from this subtree */
    return collect_results(node, max_results, results, 0);
}

size_t lc_trie_search_exact(const LCTrie *trie, const char *name,
                            size_t max_results, uint32_t *results)
{
    if (!trie || !name || !results || max_results == 0) return 0;

    const LCTrieNode *node = trie->root;

    /* Navigate to exact node */
    for (const char *p = name; *p; p++) {
        int idx = lc_trie_char_index(*p);
        if (idx < 0) continue;

        if (!node->children[idx]) {
            return 0;  /* Name not found */
        }
        node = node->children[idx];
    }

    /* Return only the entities at this exact node (not children) */
    size_t count = 0;
    for (uint16_t i = 0; i < node->num_entities && count < max_results; i++) {
        results[count++] = node->entity_ids[i];
    }

    return count;
}

uint32_t lc_trie_node_count(const LCTrie *trie)
{
    return trie ? trie->num_nodes : 0;
}

uint32_t lc_trie_entry_count(const LCTrie *trie)
{
    return trie ? trie->num_entries : 0;
}

size_t lc_trie_memory_usage(const LCTrie *trie)
{
    return trie ? trie->memory_used : 0;
}
