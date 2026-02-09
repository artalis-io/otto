/*
 * Random Number Generation and Probability Distributions
 *
 * Provides pluggable RNG backends and distribution sampling for simulation,
 * benchmarking, and Monte Carlo applications.
 *
 * Copyright (c) 2024-2026. All rights reserved.
 */

#ifndef SH_DIST_H
#define SH_DIST_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * RNG Backend Types
 * ============================================================================ */

typedef enum {
    SH_RNG_XORSHIFT128,     /* Fast, 2^128-1 period, good for simulation */
    SH_RNG_PCG64,           /* Better statistical quality, still fast */
    SH_RNG_SPLITMIX64,      /* Simple, good for seeding other generators */
    SH_RNG_SYSTEM           /* /dev/urandom - secure but slow, not for WASM */
} SHRngType;

/* ============================================================================
 * RNG Handle
 * ============================================================================ */

typedef struct SHRng SHRng;

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/*
 * Create an RNG with the specified backend.
 * Automatically seeds from time + pid.
 *
 * Returns:
 *   Allocated RNG, or NULL on failure.
 *   Caller must call sh_rng_free().
 */
SHRng *sh_rng_create(SHRngType type);

/*
 * Create an RNG with the default backend (XORSHIFT128).
 */
SHRng *sh_rng_create_default(void);

/*
 * Free an RNG.
 */
void sh_rng_free(SHRng *rng);

/* ============================================================================
 * Seeding
 * ============================================================================ */

/*
 * Seed the RNG with a 64-bit value.
 */
void sh_rng_seed(SHRng *rng, uint64_t seed);

/*
 * Seed from arbitrary bytes.
 */
void sh_rng_seed_bytes(SHRng *rng, const void *data, size_t len);

/*
 * Seed from current time + process ID.
 */
void sh_rng_seed_time(SHRng *rng);

/* ============================================================================
 * Core Generation
 * ============================================================================ */

/*
 * Generate a random 64-bit unsigned integer.
 */
uint64_t sh_rng_next_u64(SHRng *rng);

/*
 * Generate a random 32-bit unsigned integer.
 */
uint32_t sh_rng_next_u32(SHRng *rng);

/*
 * Generate a uniform random double in [0, 1).
 */
double sh_rng_uniform(SHRng *rng);

/*
 * Generate a uniform random double in [a, b).
 */
double sh_rng_uniform_range(SHRng *rng, double a, double b);

/*
 * Generate a uniform random integer in [a, b] (inclusive).
 */
int sh_rng_int_range(SHRng *rng, int a, int b);

/* ============================================================================
 * State Management
 * ============================================================================ */

/*
 * Get the size of the RNG state in bytes.
 */
size_t sh_rng_state_size(const SHRng *rng);

/*
 * Save the RNG state to a buffer.
 * Buffer must be at least sh_rng_state_size() bytes.
 */
void sh_rng_save_state(const SHRng *rng, void *buf);

/*
 * Restore the RNG state from a buffer.
 */
void sh_rng_restore_state(SHRng *rng, const void *buf);

/* ============================================================================
 * Continuous Distributions
 * ============================================================================ */

/*
 * Normal (Gaussian) distribution.
 * Uses Box-Muller transform.
 */
double sh_rng_normal(SHRng *rng, double mean, double stddev);

/*
 * Exponential distribution.
 * rate = 1/mean (lambda parameter)
 */
double sh_rng_exponential(SHRng *rng, double rate);

/*
 * Gamma distribution.
 * Uses Marsaglia and Tsang's method.
 */
double sh_rng_gamma(SHRng *rng, double shape, double scale);

/*
 * Beta distribution.
 * Derived from gamma distribution.
 */
double sh_rng_beta(SHRng *rng, double alpha, double beta);

/*
 * Log-normal distribution.
 * mu and sigma are parameters of the underlying normal.
 */
double sh_rng_lognormal(SHRng *rng, double mu, double sigma);

/*
 * Weibull distribution.
 */
double sh_rng_weibull(SHRng *rng, double shape, double scale);

/* ============================================================================
 * Discrete Distributions
 * ============================================================================ */

/*
 * Poisson distribution.
 * Uses inverse transform for small lambda, normal approximation for large.
 */
int sh_rng_poisson(SHRng *rng, double lambda);

/*
 * Binomial distribution.
 * Number of successes in n trials with probability p.
 */
int sh_rng_binomial(SHRng *rng, int n, double p);

/*
 * Geometric distribution.
 * Number of trials until first success.
 */
int sh_rng_geometric(SHRng *rng, double p);

/* ============================================================================
 * Array Operations
 * ============================================================================ */

/*
 * Shuffle an array in place (Fisher-Yates).
 */
void sh_rng_shuffle(SHRng *rng, void *array, size_t n, size_t elem_size);

/*
 * Weighted random choice from n items.
 * Returns index in [0, n-1].
 */
int sh_rng_choice(SHRng *rng, const double *weights, int n);

/*
 * Sample k items from [0, n) without replacement.
 * Results stored in out[0..k-1].
 */
void sh_rng_sample(SHRng *rng, int n, int k, int *out);

/* ============================================================================
 * PDF/CDF Functions (Stateless)
 * ============================================================================ */

/* Normal distribution */
double sh_dist_normal_pdf(double x, double mean, double stddev);
double sh_dist_normal_cdf(double x, double mean, double stddev);

/* Gamma distribution */
double sh_dist_gamma_pdf(double x, double shape, double scale);

/* Exponential distribution */
double sh_dist_exponential_pdf(double x, double rate);
double sh_dist_exponential_cdf(double x, double rate);

#ifdef __cplusplus
}
#endif

#endif /* SH_DIST_H */
