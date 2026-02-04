/*
 * sh_ratelimit.h - Token Bucket Rate Limiter
 *
 * Thread-safe rate limiting for HTTP APIs using the token bucket algorithm.
 * Supports both IPv4 and IPv6 client addresses.
 *
 * Usage:
 *   // Initialize once at startup
 *   ShRateLimiter *limiter = sh_ratelimit_create(10.0, 100.0, 4096);
 *
 *   // Check each request (thread-safe)
 *   ShRateLimitAddr addr;
 *   sh_ratelimit_addr_ipv4(&addr, client_ipv4);
 *   if (!sh_ratelimit_check(limiter, &addr)) {
 *       // Return HTTP 429 Too Many Requests
 *   }
 *
 *   // Cleanup at shutdown
 *   sh_ratelimit_free(limiter);
 */

#ifndef SH_RATELIMIT_H
#define SH_RATELIMIT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

/*
 * Client address for rate limiting.
 * Supports both IPv4 and IPv6 addresses.
 */
typedef struct {
    union {
        uint32_t ip4;       /* IPv4 address (network byte order) */
        uint64_t ip6[2];    /* IPv6 address (network byte order) */
    } addr;
    uint8_t is_ip6;         /* 1 if IPv6, 0 if IPv4 */
} ShRateLimitAddr;

/*
 * Rate limiter instance.
 * Opaque handle - use sh_ratelimit_create() to construct.
 */
typedef struct ShRateLimiter ShRateLimiter;

/*
 * Rate limiter statistics.
 */
typedef struct {
    uint64_t requests_allowed;   /* Total requests allowed */
    uint64_t requests_denied;    /* Total requests rate-limited */
    size_t active_entries;       /* Current entries in hash table */
    size_t table_capacity;       /* Hash table capacity */
    size_t evictions;            /* Number of entries evicted due to table full */
} ShRateLimitStats;

/* ============================================================================
 * Address Helpers
 * ============================================================================ */

/*
 * Initialize address from IPv4 (uint32_t in network byte order).
 */
static inline void sh_ratelimit_addr_ipv4(ShRateLimitAddr *addr, uint32_t ip4)
{
    /* Note: ip4 and ip6[0] share memory in the union, so we must set ip4 LAST
     * or clear the union first, then set ip4. We clear all to ensure ip6[1] is 0. */
    addr->addr.ip6[0] = 0;
    addr->addr.ip6[1] = 0;
    addr->addr.ip4 = ip4;  /* Set last since it overlaps with ip6[0] */
    addr->is_ip6 = 0;
}

/*
 * Initialize address from IPv6 (two uint64_t in network byte order).
 */
static inline void sh_ratelimit_addr_ipv6(ShRateLimitAddr *addr,
                                          uint64_t ip6_high, uint64_t ip6_low)
{
    addr->addr.ip6[0] = ip6_high;
    addr->addr.ip6[1] = ip6_low;
    addr->is_ip6 = 1;
}

/* ============================================================================
 * Rate Limiter API
 * ============================================================================ */

/*
 * Create a new rate limiter.
 *
 * @param rps        Tokens refilled per second (requests per second)
 * @param burst      Maximum burst capacity (token bucket size)
 * @param table_size Hash table size (should be power of 2, will be rounded up)
 * @return New rate limiter, or NULL on allocation failure
 *
 * OWNERSHIP: Caller owns the returned limiter and must call sh_ratelimit_free().
 */
ShRateLimiter *sh_ratelimit_create(double rps, double burst, size_t table_size);

/*
 * Free a rate limiter and all associated resources.
 * Safe to call with NULL (no-op).
 *
 * @param limiter Rate limiter to free (may be NULL)
 */
void sh_ratelimit_free(ShRateLimiter *limiter);

/*
 * Check if a request from the given address is allowed.
 * Thread-safe - can be called from multiple threads concurrently.
 *
 * @param limiter Rate limiter instance
 * @param addr    Client address
 * @return 1 if request is allowed, 0 if rate limited
 */
int sh_ratelimit_check(ShRateLimiter *limiter, const ShRateLimitAddr *addr);

/*
 * Get rate limiter statistics.
 * Thread-safe - statistics are atomic or protected by mutex.
 *
 * @param limiter Rate limiter instance
 * @param stats   Output statistics (may be NULL to skip)
 */
void sh_ratelimit_stats(ShRateLimiter *limiter, ShRateLimitStats *stats);

/*
 * Reset all rate limit entries (clear the table).
 * Thread-safe.
 *
 * @param limiter Rate limiter instance
 */
void sh_ratelimit_reset(ShRateLimiter *limiter);

#ifdef __cplusplus
}
#endif

#endif /* SH_RATELIMIT_H */
