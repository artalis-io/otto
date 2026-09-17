/*
 * test_inflate.c - sh_inflate zlib/deflate wrappers
 *
 * These had no tests at all. Compressed bytes reach OTTO from PDF streams and
 * OSM PBF blobs, so this decodes untrusted input: the cases that matter are
 * the malformed and the oversized ones, not the happy path.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "sh_inflate.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do {                                   \
    tests_run++;                                              \
    printf("  %-56s", #name);                                 \
    test_##name();                                            \
    printf("[PASS]\n");                                       \
    tests_passed++;                                           \
} while (0)

/* exit(1), not return. RUN_TEST prints [PASS] and counts the test as soon as
 * the function returns, so an assertion that merely returned would be reported
 * as a pass and the process would still exit 0 -- the failure would be visible
 * only to someone reading the log. This matches the other shared test files. */
#define ASSERT(cond) do {                                     \
    if (!(cond)) {                                            \
        printf("[FAIL]\n    %s:%d: %s\n",                     \
               __FILE__, __LINE__, #cond);                    \
        exit(1);                                              \
    }                                                         \
} while (0)

/* ------------------------------------------------------------- roundtrip -- */

TEST(deflate_inflate_roundtrip)
{
    const char *msg = "the quick brown fox jumps over the lazy dog";
    size_t msg_len = strlen(msg);

    uint8_t comp[256];
    size_t comp_len = 0;
    ASSERT(sh_deflate((const uint8_t *)msg, msg_len,
                      comp, sizeof(comp), &comp_len, 6) == SH_OK);
    ASSERT(comp_len > 0);

    uint8_t out[256];
    size_t out_len = 0;
    ASSERT(sh_inflate(comp, comp_len, out, sizeof(out), &out_len) == SH_OK);
    ASSERT(out_len == msg_len);
    ASSERT(memcmp(out, msg, msg_len) == 0);
}

TEST(roundtrip_highly_compressible)
{
    uint8_t src[4096];
    memset(src, 'A', sizeof(src));

    uint8_t comp[4096];
    size_t comp_len = 0;
    ASSERT(sh_deflate(src, sizeof(src), comp, sizeof(comp), &comp_len, 9) == SH_OK);
    /* 4 KB of one byte should compress to far less than it started as. */
    ASSERT(comp_len < sizeof(src) / 10);

    uint8_t out[4096];
    size_t out_len = 0;
    ASSERT(sh_inflate(comp, comp_len, out, sizeof(out), &out_len) == SH_OK);
    ASSERT(out_len == sizeof(src));
    ASSERT(memcmp(out, src, sizeof(src)) == 0);
}

/* --------------------------------------------------------------- refusal -- */

TEST(inflate_rejects_garbage)
{
    const uint8_t garbage[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22 };
    uint8_t out[64];
    size_t out_len = 0;
    ASSERT(sh_inflate(garbage, sizeof(garbage), out, sizeof(out), &out_len)
           != SH_OK);
}

TEST(inflate_rejects_truncated_stream)
{
    const char *msg = "compress me then cut the result in half";
    uint8_t comp[256];
    size_t comp_len = 0;
    ASSERT(sh_deflate((const uint8_t *)msg, strlen(msg),
                      comp, sizeof(comp), &comp_len, 6) == SH_OK);
    ASSERT(comp_len > 4);

    uint8_t out[256];
    size_t out_len = 0;
    /* Half a stream is not a stream. */
    ASSERT(sh_inflate(comp, comp_len / 2, out, sizeof(out), &out_len) != SH_OK);
}

/* A destination too small for the decompressed data must fail rather than
 * write what fits. */
TEST(inflate_rejects_undersized_destination)
{
    uint8_t src[1024];
    memset(src, 'B', sizeof(src));

    uint8_t comp[1024];
    size_t comp_len = 0;
    ASSERT(sh_deflate(src, sizeof(src), comp, sizeof(comp), &comp_len, 6) == SH_OK);

    uint8_t small[16];
    size_t out_len = 0;
    ASSERT(sh_inflate(comp, comp_len, small, sizeof(small), &out_len) != SH_OK);
}

TEST(inflate_rejects_null_arguments)
{
    uint8_t buf[8] = { 0 };
    size_t n = 0;
    ASSERT(sh_inflate(NULL, 1, buf, sizeof(buf), &n) == SH_ERROR_INVALID_PARAM);
    ASSERT(sh_inflate(buf, 1, NULL, sizeof(buf), &n) == SH_ERROR_INVALID_PARAM);
    ASSERT(sh_inflate(buf, 1, buf, sizeof(buf), NULL) == SH_ERROR_INVALID_PARAM);
}

/* avail_in/avail_out are 32-bit inside miniz. Narrowing a larger size_t
 * silently would describe a different buffer than the caller passed, so an
 * oversized length is refused rather than truncated. Only meaningful where
 * size_t is wider than 32 bits. */
TEST(inflate_refuses_lengths_beyond_uint32)
{
    if (sizeof(size_t) <= 4) {
        return; /* nothing to narrow */
    }
    uint8_t buf[8] = { 0 };
    size_t n = 0;
    size_t huge = (size_t)UINT32_MAX + 1;
    ASSERT(sh_inflate(buf, huge, buf, sizeof(buf), &n) == SH_ERROR_INVALID_PARAM);
    ASSERT(sh_inflate(buf, sizeof(buf), buf, huge, &n) == SH_ERROR_INVALID_PARAM);
}

/* ------------------------------------------------------------------- raw -- */

TEST(inflate_raw_matches_inflate)
{
    const char *msg = "raw path should agree with the streaming one";
    uint8_t comp[256];
    size_t comp_len = 0;
    ASSERT(sh_deflate((const uint8_t *)msg, strlen(msg),
                      comp, sizeof(comp), &comp_len, 6) == SH_OK);

    uint8_t out[256];
    size_t out_len = 0;
    ASSERT(sh_inflate_raw(comp, comp_len, out, sizeof(out), &out_len) == SH_OK);
    ASSERT(out_len == strlen(msg));
    ASSERT(memcmp(out, msg, out_len) == 0);
}

/* The failure value is (size_t)-1, not a negative status; the old code stored
 * the byte count in a status enum and read it back as one. */
TEST(inflate_raw_rejects_garbage)
{
    const uint8_t garbage[] = { 0xFF, 0xFE, 0xFD, 0xFC, 0xFB };
    uint8_t out[64];
    size_t out_len = 12345;
    ASSERT(sh_inflate_raw(garbage, sizeof(garbage), out, sizeof(out), &out_len)
           != SH_OK);
}

TEST(inflate_raw_rejects_null_arguments)
{
    uint8_t buf[8] = { 0 };
    size_t n = 0;
    ASSERT(sh_inflate_raw(NULL, 1, buf, sizeof(buf), &n) == SH_ERROR_INVALID_PARAM);
    ASSERT(sh_inflate_raw(buf, 1, NULL, sizeof(buf), &n) == SH_ERROR_INVALID_PARAM);
}

/* ----------------------------------------------------------------- alloc -- */

TEST(inflate_alloc_roundtrip)
{
    const char *msg = "allocate exactly what the caller said to expect";
    size_t msg_len = strlen(msg);

    uint8_t comp[256];
    size_t comp_len = 0;
    ASSERT(sh_deflate((const uint8_t *)msg, msg_len,
                      comp, sizeof(comp), &comp_len, 6) == SH_OK);

    size_t out_len = 0;
    uint8_t *out = sh_inflate_alloc(comp, comp_len, msg_len + 16, &out_len);
    ASSERT(out != NULL);
    ASSERT(out_len == msg_len);
    ASSERT(memcmp(out, msg, msg_len) == 0);
    free(out);
}

TEST(inflate_alloc_returns_null_on_garbage)
{
    const uint8_t garbage[] = { 0x01, 0x02, 0x03, 0x04 };
    size_t out_len = 0;
    uint8_t *out = sh_inflate_alloc(garbage, sizeof(garbage), 64, &out_len);
    ASSERT(out == NULL);
}

TEST(inflate_alloc_rejects_zero_expected)
{
    const uint8_t buf[4] = { 0 };
    size_t out_len = 0;
    ASSERT(sh_inflate_alloc(buf, sizeof(buf), 0, &out_len) == NULL);
}

int main(void)
{
    printf("\nsh_inflate tests\n");
    printf("==================\n");

    RUN_TEST(deflate_inflate_roundtrip);
    RUN_TEST(roundtrip_highly_compressible);
    RUN_TEST(inflate_rejects_garbage);
    RUN_TEST(inflate_rejects_truncated_stream);
    RUN_TEST(inflate_rejects_undersized_destination);
    RUN_TEST(inflate_rejects_null_arguments);
    RUN_TEST(inflate_refuses_lengths_beyond_uint32);
    RUN_TEST(inflate_raw_matches_inflate);
    RUN_TEST(inflate_raw_rejects_garbage);
    RUN_TEST(inflate_raw_rejects_null_arguments);
    RUN_TEST(inflate_alloc_roundtrip);
    RUN_TEST(inflate_alloc_returns_null_on_garbage);
    RUN_TEST(inflate_alloc_rejects_zero_expected);

    printf("\n==================\n");
    printf("%d/%d tests passed\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
