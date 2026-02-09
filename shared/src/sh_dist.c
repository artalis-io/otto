/*
 * Random Number Generation and Probability Distributions
 * Implementation
 *
 * Copyright (c) 2024-2026. All rights reserved.
 */

#include "sh_dist.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ============================================================================
 * RNG State Structures
 * ============================================================================ */

typedef struct {
    uint64_t s[2];
} Xorshift128State;

typedef struct {
    uint64_t state;
    uint64_t inc;
} PCG64State;

typedef struct {
    uint64_t state;
} Splitmix64State;

struct SHRng {
    SHRngType type;
    union {
        Xorshift128State xorshift;
        PCG64State pcg;
        Splitmix64State splitmix;
    } state;
    /* For Box-Muller: cache second normal value */
    double normal_cache;
    int has_cached_normal;
};

/* ============================================================================
 * Splitmix64 (used for seeding other generators)
 * ============================================================================ */

static uint64_t splitmix64_next(uint64_t *state)
{
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* ============================================================================
 * Xorshift128+
 * ============================================================================ */

static uint64_t xorshift128_next(Xorshift128State *s)
{
    uint64_t s1 = s->s[0];
    const uint64_t s0 = s->s[1];
    const uint64_t result = s0 + s1;
    s->s[0] = s0;
    s1 ^= s1 << 23;
    s->s[1] = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5);
    return result;
}

static void xorshift128_seed(Xorshift128State *s, uint64_t seed)
{
    /* Use splitmix64 to generate initial state */
    s->s[0] = splitmix64_next(&seed);
    s->s[1] = splitmix64_next(&seed);
    /* Ensure non-zero state */
    if (s->s[0] == 0 && s->s[1] == 0) {
        s->s[0] = 1;
    }
}

/* ============================================================================
 * PCG64
 * ============================================================================ */

static uint64_t pcg64_next(PCG64State *s)
{
    uint64_t oldstate = s->state;
    s->state = oldstate * 6364136223846793005ULL + s->inc;
    uint64_t xorshifted = ((oldstate >> 18u) ^ oldstate) >> 27u;
    uint64_t rot = oldstate >> 59u;
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

static void pcg64_seed(PCG64State *s, uint64_t seed)
{
    s->state = 0;
    s->inc = (splitmix64_next(&seed) << 1u) | 1u;
    pcg64_next(s);
    s->state += splitmix64_next(&seed);
    pcg64_next(s);
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

SHRng *sh_rng_create(SHRngType type)
{
    SHRng *rng = malloc(sizeof(SHRng));
    if (!rng) {
        return NULL;
    }

    rng->type = type;
    rng->has_cached_normal = 0;
    rng->normal_cache = 0.0;

    /* Seed from time */
    sh_rng_seed_time(rng);

    return rng;
}

SHRng *sh_rng_create_default(void)
{
    return sh_rng_create(SH_RNG_XORSHIFT128);
}

void sh_rng_free(SHRng *rng)
{
    free(rng);
}

/* ============================================================================
 * Seeding
 * ============================================================================ */

void sh_rng_seed(SHRng *rng, uint64_t seed)
{
    if (!rng) return;

    rng->has_cached_normal = 0;

    switch (rng->type) {
        case SH_RNG_XORSHIFT128:
            xorshift128_seed(&rng->state.xorshift, seed);
            break;
        case SH_RNG_PCG64:
            pcg64_seed(&rng->state.pcg, seed);
            break;
        case SH_RNG_SPLITMIX64:
            rng->state.splitmix.state = seed;
            if (seed == 0) rng->state.splitmix.state = 1;
            break;
        case SH_RNG_SYSTEM:
            /* System RNG ignores seed */
            break;
    }
}

void sh_rng_seed_bytes(SHRng *rng, const void *data, size_t len)
{
    if (!rng || !data || len == 0) return;

    /* Hash bytes into a 64-bit seed */
    uint64_t seed = 0;
    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        seed = seed * 31 + bytes[i];
    }
    sh_rng_seed(rng, seed);
}

void sh_rng_seed_time(SHRng *rng)
{
    if (!rng) return;

    uint64_t seed = (uint64_t)time(NULL);
#ifdef _WIN32
    seed ^= (uint64_t)GetCurrentProcessId() << 16;
#else
    seed ^= (uint64_t)getpid() << 16;
#endif
    /* Add some bits from clock */
    seed ^= (uint64_t)clock() << 32;

    sh_rng_seed(rng, seed);
}

/* ============================================================================
 * Core Generation
 * ============================================================================ */

uint64_t sh_rng_next_u64(SHRng *rng)
{
    if (!rng) return 0;

    switch (rng->type) {
        case SH_RNG_XORSHIFT128:
            return xorshift128_next(&rng->state.xorshift);
        case SH_RNG_PCG64:
            return pcg64_next(&rng->state.pcg);
        case SH_RNG_SPLITMIX64:
            return splitmix64_next(&rng->state.splitmix.state);
        case SH_RNG_SYSTEM:
            /* Read from /dev/urandom */
            {
                uint64_t result = 0;
#ifndef _WIN32
                FILE *f = fopen("/dev/urandom", "rb");
                if (f) {
                    size_t n = fread(&result, sizeof(result), 1, f);
                    (void)n;
                    fclose(f);
                }
#endif
                return result;
            }
    }
    return 0;
}

uint32_t sh_rng_next_u32(SHRng *rng)
{
    return (uint32_t)(sh_rng_next_u64(rng) >> 32);
}

double sh_rng_uniform(SHRng *rng)
{
    /* Convert to [0, 1) using 53 bits of precision */
    uint64_t x = sh_rng_next_u64(rng);
    return (x >> 11) * (1.0 / 9007199254740992.0);
}

double sh_rng_uniform_range(SHRng *rng, double a, double b)
{
    return a + sh_rng_uniform(rng) * (b - a);
}

int sh_rng_int_range(SHRng *rng, int a, int b)
{
    if (a > b) {
        int tmp = a; a = b; b = tmp;
    }
    uint64_t range = (uint64_t)(b - a) + 1;
    uint64_t x = sh_rng_next_u64(rng);
    return a + (int)(x % range);
}

/* ============================================================================
 * State Management
 * ============================================================================ */

size_t sh_rng_state_size(const SHRng *rng)
{
    if (!rng) return 0;

    switch (rng->type) {
        case SH_RNG_XORSHIFT128:
            return sizeof(Xorshift128State);
        case SH_RNG_PCG64:
            return sizeof(PCG64State);
        case SH_RNG_SPLITMIX64:
            return sizeof(Splitmix64State);
        case SH_RNG_SYSTEM:
            return 0;  /* No state */
    }
    return 0;
}

void sh_rng_save_state(const SHRng *rng, void *buf)
{
    if (!rng || !buf) return;

    switch (rng->type) {
        case SH_RNG_XORSHIFT128:
            memcpy(buf, &rng->state.xorshift, sizeof(Xorshift128State));
            break;
        case SH_RNG_PCG64:
            memcpy(buf, &rng->state.pcg, sizeof(PCG64State));
            break;
        case SH_RNG_SPLITMIX64:
            memcpy(buf, &rng->state.splitmix, sizeof(Splitmix64State));
            break;
        case SH_RNG_SYSTEM:
            break;
    }
}

void sh_rng_restore_state(SHRng *rng, const void *buf)
{
    if (!rng || !buf) return;

    rng->has_cached_normal = 0;

    switch (rng->type) {
        case SH_RNG_XORSHIFT128:
            memcpy(&rng->state.xorshift, buf, sizeof(Xorshift128State));
            break;
        case SH_RNG_PCG64:
            memcpy(&rng->state.pcg, buf, sizeof(PCG64State));
            break;
        case SH_RNG_SPLITMIX64:
            memcpy(&rng->state.splitmix, buf, sizeof(Splitmix64State));
            break;
        case SH_RNG_SYSTEM:
            break;
    }
}

/* ============================================================================
 * Continuous Distributions
 * ============================================================================ */

double sh_rng_normal(SHRng *rng, double mean, double stddev)
{
    if (!rng) return mean;

    /* Check for cached value from Box-Muller */
    if (rng->has_cached_normal) {
        rng->has_cached_normal = 0;
        return mean + stddev * rng->normal_cache;
    }

    /* Box-Muller transform */
    double u1, u2, s;
    do {
        u1 = 2.0 * sh_rng_uniform(rng) - 1.0;
        u2 = 2.0 * sh_rng_uniform(rng) - 1.0;
        s = u1 * u1 + u2 * u2;
    } while (s >= 1.0 || s == 0.0);

    double mul = sqrt(-2.0 * log(s) / s);
    rng->normal_cache = u2 * mul;
    rng->has_cached_normal = 1;

    return mean + stddev * u1 * mul;
}

double sh_rng_exponential(SHRng *rng, double rate)
{
    if (!rng || rate <= 0) return 0.0;
    double u = sh_rng_uniform(rng);
    /* Avoid log(0) */
    while (u == 0.0) {
        u = sh_rng_uniform(rng);
    }
    return -log(u) / rate;
}

double sh_rng_gamma(SHRng *rng, double shape, double scale)
{
    if (!rng || shape <= 0 || scale <= 0) return 0.0;

    /* Marsaglia and Tsang's method for shape >= 1 */
    double d, c, x, v, u;

    if (shape < 1.0) {
        /* For shape < 1, use shape + 1 and scale the result */
        double g = sh_rng_gamma(rng, shape + 1.0, 1.0);
        return scale * g * pow(sh_rng_uniform(rng), 1.0 / shape);
    }

    d = shape - 1.0 / 3.0;
    c = 1.0 / sqrt(9.0 * d);

    while (1) {
        do {
            x = sh_rng_normal(rng, 0.0, 1.0);
            v = 1.0 + c * x;
        } while (v <= 0.0);

        v = v * v * v;
        u = sh_rng_uniform(rng);

        if (u < 1.0 - 0.0331 * (x * x) * (x * x)) {
            return scale * d * v;
        }

        if (log(u) < 0.5 * x * x + d * (1.0 - v + log(v))) {
            return scale * d * v;
        }
    }
}

double sh_rng_beta(SHRng *rng, double alpha, double beta)
{
    if (!rng || alpha <= 0 || beta <= 0) return 0.0;

    double x = sh_rng_gamma(rng, alpha, 1.0);
    double y = sh_rng_gamma(rng, beta, 1.0);

    return x / (x + y);
}

double sh_rng_lognormal(SHRng *rng, double mu, double sigma)
{
    return exp(sh_rng_normal(rng, mu, sigma));
}

double sh_rng_weibull(SHRng *rng, double shape, double scale)
{
    if (!rng || shape <= 0 || scale <= 0) return 0.0;
    double u = sh_rng_uniform(rng);
    while (u == 0.0) {
        u = sh_rng_uniform(rng);
    }
    return scale * pow(-log(u), 1.0 / shape);
}

/* ============================================================================
 * Discrete Distributions
 * ============================================================================ */

int sh_rng_poisson(SHRng *rng, double lambda)
{
    if (!rng || lambda <= 0) return 0;

    if (lambda < 30) {
        /* Inverse transform for small lambda */
        double L = exp(-lambda);
        int k = 0;
        double p = 1.0;

        do {
            k++;
            p *= sh_rng_uniform(rng);
        } while (p > L);

        return k - 1;
    } else {
        /* Normal approximation for large lambda */
        double result = sh_rng_normal(rng, lambda, sqrt(lambda));
        return (int)(result + 0.5);
    }
}

int sh_rng_binomial(SHRng *rng, int n, double p)
{
    if (!rng || n <= 0 || p <= 0) return 0;
    if (p >= 1) return n;

    /* Simple implementation: sum of n Bernoulli trials */
    /* For large n, could use normal approximation */
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (sh_rng_uniform(rng) < p) {
            count++;
        }
    }
    return count;
}

int sh_rng_geometric(SHRng *rng, double p)
{
    if (!rng || p <= 0 || p > 1) return 1;
    double u = sh_rng_uniform(rng);
    while (u == 0.0) {
        u = sh_rng_uniform(rng);
    }
    return (int)(log(u) / log(1.0 - p)) + 1;
}

/* ============================================================================
 * Array Operations
 * ============================================================================ */

void sh_rng_shuffle(SHRng *rng, void *array, size_t n, size_t elem_size)
{
    if (!rng || !array || n < 2) return;

    char *arr = (char *)array;
    char *tmp = malloc(elem_size);
    if (!tmp) return;

    for (size_t i = n - 1; i > 0; i--) {
        size_t j = sh_rng_next_u64(rng) % (i + 1);
        if (i != j) {
            memcpy(tmp, arr + i * elem_size, elem_size);
            memcpy(arr + i * elem_size, arr + j * elem_size, elem_size);
            memcpy(arr + j * elem_size, tmp, elem_size);
        }
    }

    free(tmp);
}

int sh_rng_choice(SHRng *rng, const double *weights, int n)
{
    if (!rng || !weights || n <= 0) return 0;

    /* Compute total weight */
    double total = 0.0;
    for (int i = 0; i < n; i++) {
        total += weights[i];
    }

    if (total <= 0.0) return 0;

    /* Select random point */
    double r = sh_rng_uniform(rng) * total;
    double cumulative = 0.0;

    for (int i = 0; i < n; i++) {
        cumulative += weights[i];
        if (r < cumulative) {
            return i;
        }
    }

    return n - 1;
}

void sh_rng_sample(SHRng *rng, int n, int k, int *out)
{
    if (!rng || !out || k <= 0 || n <= 0) return;
    if (k > n) k = n;

    /* Reservoir sampling for k << n, else shuffle first k */
    if (k <= n / 2) {
        /* Floyd's algorithm */
        for (int i = 0; i < k; i++) {
            out[i] = -1;
        }
        for (int j = n - k; j < n; j++) {
            int t = sh_rng_int_range(rng, 0, j);
            int found = 0;
            for (int i = 0; i < k && !found; i++) {
                if (out[i] == t) found = 1;
            }
            if (found) {
                for (int i = 0; i < k; i++) {
                    if (out[i] == -1) {
                        out[i] = j;
                        break;
                    }
                }
            } else {
                for (int i = 0; i < k; i++) {
                    if (out[i] == -1) {
                        out[i] = t;
                        break;
                    }
                }
            }
        }
    } else {
        /* Create array 0..n-1, shuffle, take first k */
        int *arr = malloc(n * sizeof(int));
        if (!arr) return;
        for (int i = 0; i < n; i++) {
            arr[i] = i;
        }
        sh_rng_shuffle(rng, arr, n, sizeof(int));
        memcpy(out, arr, k * sizeof(int));
        free(arr);
    }
}

/* ============================================================================
 * PDF/CDF Functions
 * ============================================================================ */

double sh_dist_normal_pdf(double x, double mean, double stddev)
{
    if (stddev <= 0) return 0.0;
    double z = (x - mean) / stddev;
    return exp(-0.5 * z * z) / (stddev * sqrt(2.0 * M_PI));
}

/* Error function approximation */
static double erf_approx(double x)
{
    /* Horner form of approximation */
    double t = 1.0 / (1.0 + 0.5 * fabs(x));
    double tau = t * exp(-x * x - 1.26551223 +
                         t * (1.00002368 +
                         t * (0.37409196 +
                         t * (0.09678418 +
                         t * (-0.18628806 +
                         t * (0.27886807 +
                         t * (-1.13520398 +
                         t * (1.48851587 +
                         t * (-0.82215223 +
                         t * 0.17087277)))))))));
    return x >= 0 ? 1.0 - tau : tau - 1.0;
}

double sh_dist_normal_cdf(double x, double mean, double stddev)
{
    if (stddev <= 0) return 0.0;
    double z = (x - mean) / stddev;
    return 0.5 * (1.0 + erf_approx(z / sqrt(2.0)));
}

double sh_dist_gamma_pdf(double x, double shape, double scale)
{
    if (x < 0 || shape <= 0 || scale <= 0) return 0.0;
    if (x == 0) {
        if (shape < 1) return INFINITY;
        if (shape == 1) return 1.0 / scale;
        return 0.0;
    }
    return pow(x, shape - 1) * exp(-x / scale) / (pow(scale, shape) * tgamma(shape));
}

double sh_dist_exponential_pdf(double x, double rate)
{
    if (x < 0 || rate <= 0) return 0.0;
    return rate * exp(-rate * x);
}

double sh_dist_exponential_cdf(double x, double rate)
{
    if (x < 0 || rate <= 0) return 0.0;
    return 1.0 - exp(-rate * x);
}
