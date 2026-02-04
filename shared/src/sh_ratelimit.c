/*
 * sh_ratelimit.c - Token Bucket Rate Limiter Implementation
 */

#include "sh_ratelimit.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>

/* ============================================================================
 * Internal Types
 * ============================================================================ */

/* Hash table entry for rate limiting */
typedef struct {
    ShRateLimitAddr addr;  /* Client address (addr.ip4 == 0 && !is_ip6 means empty) */
    double tokens;         /* Current token count */
    double last_update;    /* Timestamp of last update (seconds) */
} RateLimitEntry;

/* Rate limiter instance */
struct ShRateLimiter {
    RateLimitEntry *table;     /* Hash table of entries */
    size_t capacity;           /* Table capacity (power of 2) */
    size_t mask;               /* capacity - 1, for fast modulo */
    double rps;                /* Tokens per second (refill rate) */
    double burst;              /* Maximum tokens (bucket capacity) */
    pthread_mutex_t mutex;     /* Mutex for thread safety */

    /* Statistics (updated under mutex) */
    uint64_t requests_allowed;
    uint64_t requests_denied;
    size_t active_entries;
    size_t evictions;
};

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/* Get current time in seconds with high precision */
static double get_time_seconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

/* Round up to next power of 2 */
static size_t next_power_of_2(size_t n)
{
    if (n == 0) return 1;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
#if SIZE_MAX > 0xFFFFFFFF
    n |= n >> 32;
#endif
    return n + 1;
}

/*
 * Hash function for IP addresses.
 * Uses FNV-1a-inspired mixing for good distribution.
 */
static uint64_t hash_addr(const ShRateLimitAddr *addr)
{
    uint64_t h;

    if (addr->is_ip6) {
        /* Hash both 64-bit halves of IPv6 */
        h = addr->addr.ip6[0];
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= addr->addr.ip6[1];
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
    } else {
        /* Hash IPv4 with mixing */
        h = (uint64_t)addr->addr.ip4;
        h ^= h >> 16;
        h *= 0x85ebca6bULL;
        h ^= h >> 13;
        h *= 0xc2b2ae35ULL;
        h ^= h >> 16;
    }

    return h;
}

/* Check if two addresses are equal */
static int addr_equal(const ShRateLimitAddr *a, const ShRateLimitAddr *b)
{
    if (a->is_ip6 != b->is_ip6) return 0;

    if (a->is_ip6) {
        return a->addr.ip6[0] == b->addr.ip6[0] &&
               a->addr.ip6[1] == b->addr.ip6[1];
    } else {
        return a->addr.ip4 == b->addr.ip4;
    }
}

/* Check if entry is empty (zero address) */
static int entry_is_empty(const RateLimitEntry *entry)
{
    return !entry->addr.is_ip6 && entry->addr.addr.ip4 == 0;
}

/* Mark entry as empty */
static void entry_clear(RateLimitEntry *entry)
{
    memset(entry, 0, sizeof(*entry));
}

/* ============================================================================
 * Public API
 * ============================================================================ */

ShRateLimiter *sh_ratelimit_create(double rps, double burst, size_t table_size)
{
    if (rps <= 0 || burst <= 0) return NULL;

    /* Round table size up to power of 2, minimum 64 */
    if (table_size < 64) table_size = 64;
    table_size = next_power_of_2(table_size);

    ShRateLimiter *limiter = calloc(1, sizeof(ShRateLimiter));
    if (!limiter) return NULL;

    limiter->table = calloc(table_size, sizeof(RateLimitEntry));
    if (!limiter->table) {
        free(limiter);
        return NULL;
    }

    limiter->capacity = table_size;
    limiter->mask = table_size - 1;
    limiter->rps = rps;
    limiter->burst = burst;

    if (pthread_mutex_init(&limiter->mutex, NULL) != 0) {
        free(limiter->table);
        free(limiter);
        return NULL;
    }

    return limiter;
}

void sh_ratelimit_free(ShRateLimiter *limiter)
{
    if (!limiter) return;

    pthread_mutex_destroy(&limiter->mutex);
    free(limiter->table);
    free(limiter);
}

int sh_ratelimit_check(ShRateLimiter *limiter, const ShRateLimitAddr *addr)
{
    if (!limiter || !addr) return 0;  /* Deny if invalid args (security: deny-by-default) */

    /* Don't rate limit zero addresses (invalid) */
    if (!addr->is_ip6 && addr->addr.ip4 == 0) return 1;

    double now = get_time_seconds();
    uint64_t h = hash_addr(addr);
    size_t idx = (size_t)(h & limiter->mask);
    int allowed = 0;

    pthread_mutex_lock(&limiter->mutex);

    /* Linear probing to find existing entry or empty slot */
    RateLimitEntry *entry = NULL;
    RateLimitEntry *empty_slot = NULL;
    double oldest_time = now;
    size_t oldest_idx = idx;

    for (size_t i = 0; i < limiter->capacity; i++) {
        size_t probe_idx = (idx + i) & limiter->mask;
        RateLimitEntry *probe = &limiter->table[probe_idx];

        if (addr_equal(&probe->addr, addr)) {
            entry = probe;
            break;
        }
        if (entry_is_empty(probe) && !empty_slot) {
            empty_slot = probe;
            break;
        }
        /* Track oldest entry for eviction if table is full */
        if (probe->last_update < oldest_time) {
            oldest_time = probe->last_update;
            oldest_idx = probe_idx;
        }
    }

    /* If no entry found and no empty slot, evict oldest */
    if (!entry && !empty_slot) {
        empty_slot = &limiter->table[oldest_idx];
        entry_clear(empty_slot);
        limiter->evictions++;
        limiter->active_entries--;  /* Will be incremented below */
    }

    /* Create new entry if needed */
    if (!entry) {
        entry = empty_slot;
        entry->addr = *addr;
        entry->tokens = limiter->burst;  /* Start with full bucket */
        entry->last_update = now;
        limiter->active_entries++;
    }

    /* Refill tokens based on elapsed time */
    double elapsed = now - entry->last_update;
    if (elapsed > 0) {
        entry->tokens += elapsed * limiter->rps;
        if (entry->tokens > limiter->burst) {
            entry->tokens = limiter->burst;
        }
        entry->last_update = now;
    }

    /* Try to consume a token */
    if (entry->tokens >= 1.0) {
        entry->tokens -= 1.0;
        allowed = 1;
        limiter->requests_allowed++;
    } else {
        limiter->requests_denied++;
    }

    pthread_mutex_unlock(&limiter->mutex);
    return allowed;
}

void sh_ratelimit_stats(ShRateLimiter *limiter, ShRateLimitStats *stats)
{
    if (!limiter || !stats) return;

    pthread_mutex_lock(&limiter->mutex);
    stats->requests_allowed = limiter->requests_allowed;
    stats->requests_denied = limiter->requests_denied;
    stats->active_entries = limiter->active_entries;
    stats->table_capacity = limiter->capacity;
    stats->evictions = limiter->evictions;
    pthread_mutex_unlock(&limiter->mutex);
}

void sh_ratelimit_reset(ShRateLimiter *limiter)
{
    if (!limiter) return;

    pthread_mutex_lock(&limiter->mutex);
    memset(limiter->table, 0, limiter->capacity * sizeof(RateLimitEntry));
    limiter->active_entries = 0;
    pthread_mutex_unlock(&limiter->mutex);
}

void sh_ratelimit_update_rate(ShRateLimiter *limiter, double rps, double burst)
{
    if (!limiter) return;
    if (rps <= 0 || burst <= 0) return;

    pthread_mutex_lock(&limiter->mutex);
    limiter->rps = rps;
    limiter->burst = burst;
    pthread_mutex_unlock(&limiter->mutex);
}
