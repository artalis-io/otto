/*
 * lc_ngram.c - N-gram Index for Fuzzy Matching
 *
 * Implements trigram-based fuzzy string matching using Jaccard similarity.
 * Uses O(1) hit counting with a pre-allocated count array for fast search.
 */

#include "lc_ngram.h"
#include <stdlib.h>
#include <string.h>

/* Maximum entities to process per n-gram (skip overly common trigrams) */
#define LC_NGRAM_MAX_POSTING_SIZE 50000

/* ============================================================================
 * N-gram Generation
 * ============================================================================ */

size_t lc_ngram_generate(const char *str, char ngrams[][LC_NGRAM_SIZE + 1], size_t max_ngrams)
{
    if (!str || !ngrams || max_ngrams == 0) return 0;

    size_t len = strlen(str);
    if (len < LC_NGRAM_SIZE) {
        /* String too short - use the whole string as a single "ngram" */
        if (len > 0) {
            strncpy(ngrams[0], str, LC_NGRAM_SIZE);
            ngrams[0][len] = '\0';
            return 1;
        }
        return 0;
    }

    size_t count = 0;
    size_t num_ngrams = len - LC_NGRAM_SIZE + 1;

    for (size_t i = 0; i < num_ngrams && count < max_ngrams; i++) {
        memcpy(ngrams[count], str + i, LC_NGRAM_SIZE);
        ngrams[count][LC_NGRAM_SIZE] = '\0';
        count++;
    }

    return count;
}

/* ============================================================================
 * N-gram Index Management
 * ============================================================================ */

LCNgramIndex *lc_ngram_create(void)
{
    LCNgramIndex *idx = calloc(1, sizeof(LCNgramIndex));
    if (!idx) return NULL;

    idx->capacity = 1024;
    idx->entries = calloc(idx->capacity, sizeof(LCNgramEntry));
    if (!idx->entries) {
        free(idx);
        return NULL;
    }

    idx->memory_used = sizeof(LCNgramIndex) + idx->capacity * sizeof(LCNgramEntry);
    return idx;
}

void lc_ngram_free(LCNgramIndex *idx)
{
    if (!idx) return;

    for (uint32_t i = 0; i < idx->num_entries; i++) {
        free(idx->entries[i].entity_ids);
    }
    free(idx->entries);
    free(idx);
}

static LCNgramEntry *find_or_create_entry(LCNgramIndex *idx, const char *ngram)
{
    /* Linear search for now (will use binary search after build) */
    for (uint32_t i = 0; i < idx->num_entries; i++) {
        if (strcmp(idx->entries[i].ngram, ngram) == 0) {
            return &idx->entries[i];
        }
    }

    /* Create new entry */
    if (idx->num_entries >= idx->capacity) {
        uint32_t new_capacity = idx->capacity * 2;
        LCNgramEntry *new_entries = realloc(idx->entries, new_capacity * sizeof(LCNgramEntry));
        if (!new_entries) return NULL;
        idx->entries = new_entries;
        memset(idx->entries + idx->capacity, 0, (new_capacity - idx->capacity) * sizeof(LCNgramEntry));
        idx->capacity = new_capacity;
        idx->memory_used = sizeof(LCNgramIndex) + idx->capacity * sizeof(LCNgramEntry);
    }

    LCNgramEntry *entry = &idx->entries[idx->num_entries++];
    strncpy(entry->ngram, ngram, LC_NGRAM_SIZE);
    entry->ngram[LC_NGRAM_SIZE] = '\0';
    entry->capacity = 8;
    entry->entity_ids = malloc(entry->capacity * sizeof(uint32_t));
    if (!entry->entity_ids) {
        idx->num_entries--;
        return NULL;
    }
    idx->memory_used += entry->capacity * sizeof(uint32_t);

    return entry;
}

static LCStatus entry_add_entity(LCNgramEntry *entry, uint32_t entity_id)
{
    /* Check if already present */
    for (uint32_t i = 0; i < entry->count; i++) {
        if (entry->entity_ids[i] == entity_id) {
            return LC_OK;
        }
    }

    /* Grow if needed */
    if (entry->count >= entry->capacity) {
        uint32_t new_capacity = entry->capacity * 2;
        uint32_t *new_ids = realloc(entry->entity_ids, new_capacity * sizeof(uint32_t));
        if (!new_ids) return LC_ERROR_OUT_OF_MEMORY;
        entry->entity_ids = new_ids;
        entry->capacity = new_capacity;
    }

    entry->entity_ids[entry->count++] = entity_id;
    return LC_OK;
}

LCStatus lc_ngram_index_name(LCNgramIndex *idx, const char *name, uint32_t entity_id)
{
    if (!idx || !name) return LC_ERROR_INVALID_PARAM;

    /* Track max entity ID for hit buffer sizing */
    if (entity_id >= idx->max_entity_id) {
        idx->max_entity_id = entity_id + 1;
    }

    char ngrams[64][LC_NGRAM_SIZE + 1];
    size_t count = lc_ngram_generate(name, ngrams, 64);

    for (size_t i = 0; i < count; i++) {
        LCNgramEntry *entry = find_or_create_entry(idx, ngrams[i]);
        if (!entry) return LC_ERROR_OUT_OF_MEMORY;

        LCStatus status = entry_add_entity(entry, entity_id);
        if (status != LC_OK) return status;
    }

    return LC_OK;
}

/* Comparator for qsort */
static int entry_compare(const void *a, const void *b)
{
    const LCNgramEntry *ea = (const LCNgramEntry *)a;
    const LCNgramEntry *eb = (const LCNgramEntry *)b;
    return strcmp(ea->ngram, eb->ngram);
}

void lc_ngram_build(LCNgramIndex *idx)
{
    if (!idx || idx->num_entries == 0) return;
    qsort(idx->entries, idx->num_entries, sizeof(LCNgramEntry), entry_compare);
}

/* Binary search for an ngram entry */
static const LCNgramEntry *find_entry(const LCNgramIndex *idx, const char *ngram)
{
    if (!idx || idx->num_entries == 0) return NULL;

    int left = 0;
    int right = (int)idx->num_entries - 1;

    while (left <= right) {
        int mid = (left + right) / 2;
        int cmp = strcmp(idx->entries[mid].ngram, ngram);
        if (cmp == 0) {
            return &idx->entries[mid];
        } else if (cmp < 0) {
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }

    return NULL;
}

/* Comparator for sorting fuzzy matches by score descending */
static int fuzzy_compare(const void *a, const void *b)
{
    const LCFuzzyMatch *ma = (const LCFuzzyMatch *)a;
    const LCFuzzyMatch *mb = (const LCFuzzyMatch *)b;
    if (ma->score > mb->score) return -1;
    if (ma->score < mb->score) return 1;
    return 0;
}

/*
 * Fast fuzzy search using O(1) hit counting.
 *
 * Instead of tracking hits in a dynamic array with O(n) lookup,
 * we use a pre-allocated count array indexed by entity_id.
 * This gives O(1) increment per hit and O(n) final scan.
 */
size_t lc_ngram_search(const LCNgramIndex *idx, const char *query,
                       float threshold, size_t max_results, LCFuzzyMatch *results)
{
    if (!idx || !query || !results || max_results == 0) return 0;
    if (idx->max_entity_id == 0) return 0;

    /* Generate query n-grams */
    char query_ngrams[64][LC_NGRAM_SIZE + 1];
    size_t query_ngram_count = lc_ngram_generate(query, query_ngrams, 64);
    if (query_ngram_count == 0) return 0;

    /* Allocate hit count array - O(1) per entity lookup */
    uint16_t *hit_counts = calloc(idx->max_entity_id, sizeof(uint16_t));
    if (!hit_counts) return 0;

    /* Count hits for each entity - O(total_posting_list_size) */
    size_t total_hits = 0;
    for (size_t i = 0; i < query_ngram_count; i++) {
        const LCNgramEntry *entry = find_entry(idx, query_ngrams[i]);
        if (!entry) continue;

        /* Skip overly common n-grams (low discriminative value) */
        if (entry->count > LC_NGRAM_MAX_POSTING_SIZE) continue;

        for (uint32_t j = 0; j < entry->count; j++) {
            uint32_t eid = entry->entity_ids[j];
            if (eid < idx->max_entity_id) {
                hit_counts[eid]++;
                total_hits++;
            }
        }
    }

    /* Early exit if no hits */
    if (total_hits == 0) {
        free(hit_counts);
        return 0;
    }

    /* Calculate minimum hits needed based on threshold */
    uint16_t min_hits = (uint16_t)(threshold * query_ngram_count);
    if (min_hits == 0) min_hits = 1;

    /* Collect results that meet threshold - O(max_entity_id) */
    size_t result_count = 0;

    /* Use a simple approach: collect up to max_results * 4, then sort and trim */
    size_t collect_limit = max_results * 4;
    if (collect_limit > 1024) collect_limit = 1024;

    LCFuzzyMatch *candidates = malloc(collect_limit * sizeof(LCFuzzyMatch));
    if (!candidates) {
        free(hit_counts);
        return 0;
    }

    size_t candidate_count = 0;
    for (uint32_t eid = 0; eid < idx->max_entity_id && candidate_count < collect_limit; eid++) {
        if (hit_counts[eid] >= min_hits) {
            float score = (float)hit_counts[eid] / (float)query_ngram_count;
            candidates[candidate_count].entity_id = eid;
            candidates[candidate_count].score = score;
            candidate_count++;
        }
    }

    free(hit_counts);

    /* Sort by score descending */
    if (candidate_count > 1) {
        qsort(candidates, candidate_count, sizeof(LCFuzzyMatch), fuzzy_compare);
    }

    /* Copy top results */
    result_count = candidate_count < max_results ? candidate_count : max_results;
    memcpy(results, candidates, result_count * sizeof(LCFuzzyMatch));

    free(candidates);
    return result_count;
}

float lc_ngram_similarity(const char *s1, const char *s2)
{
    if (!s1 || !s2) return 0.0f;

    char ngrams1[64][LC_NGRAM_SIZE + 1];
    char ngrams2[64][LC_NGRAM_SIZE + 1];

    size_t count1 = lc_ngram_generate(s1, ngrams1, 64);
    size_t count2 = lc_ngram_generate(s2, ngrams2, 64);

    if (count1 == 0 || count2 == 0) return 0.0f;

    /* Count intersection */
    size_t intersection = 0;
    for (size_t i = 0; i < count1; i++) {
        for (size_t j = 0; j < count2; j++) {
            if (strcmp(ngrams1[i], ngrams2[j]) == 0) {
                intersection++;
                break;
            }
        }
    }

    /* Jaccard = intersection / union, union = count1 + count2 - intersection */
    size_t union_size = count1 + count2 - intersection;
    return (float)intersection / (float)union_size;
}

uint32_t lc_ngram_entry_count(const LCNgramIndex *idx)
{
    return idx ? idx->num_entries : 0;
}

size_t lc_ngram_memory_usage(const LCNgramIndex *idx)
{
    return idx ? idx->memory_used : 0;
}
