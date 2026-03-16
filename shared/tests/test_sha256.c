/*
 * test_sha256.c - Unit tests for SHA-256 hash
 *
 * Test vectors from NIST FIPS 180-4 and common references.
 */

#include "sh_hash_sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Test Framework
 * ============================================================================ */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  %-55s ", #name); \
    fflush(stdout); \
    test_##name(); \
    tests_run++; \
    tests_passed++; \
    printf("[PASS]\n"); \
} while (0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("[FAIL]\n    Assertion failed: %s\n", #cond); \
        exit(1); \
    } \
} while (0)

#define ASSERT_STREQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("[FAIL]\n    Expected: %s\n    Got:      %s\n", (b), (a)); \
        exit(1); \
    } \
} while (0)

/* ============================================================================
 * NIST Test Vectors
 * ============================================================================ */

/* SHA-256("") */
TEST(sha256_empty)
{
    char hex[65];
    sh_sha256_hex("", 0, hex);
    ASSERT_STREQ(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

/* SHA-256("abc") — NIST FIPS 180-4 example */
TEST(sha256_abc)
{
    char hex[65];
    sh_sha256_hex("abc", 3, hex);
    ASSERT_STREQ(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

/* SHA-256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
 * NIST two-block message example */
TEST(sha256_two_blocks)
{
    const char *msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    char hex[65];
    sh_sha256_hex(msg, strlen(msg), hex);
    ASSERT_STREQ(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

/* SHA-256(1,000,000 x "a") — NIST long message test */
TEST(sha256_long)
{
    /* Build 1M 'a' characters */
    size_t len = 1000000;
    char *buf = (char *)malloc(len);
    ASSERT(buf != NULL);
    memset(buf, 'a', len);

    char hex[65];
    sh_sha256_hex(buf, len, hex);
    free(buf);

    ASSERT_STREQ(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

/* Incremental update produces same result as single call */
TEST(sha256_incremental)
{
    const char *msg = "The quick brown fox jumps over the lazy dog";
    size_t len = strlen(msg);

    /* Single call */
    char hex1[65];
    sh_sha256_hex(msg, len, hex1);

    /* Incremental: feed one byte at a time */
    ShSha256 ctx;
    sh_sha256_init(&ctx);
    for (size_t i = 0; i < len; i++)
        sh_sha256_update(&ctx, msg + i, 1);
    uint8_t digest[32];
    sh_sha256_final(&ctx, digest);

    /* Convert to hex manually */
    char hex2[65];
    const char *hc = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex2[2*i]     = hc[(digest[i] >> 4) & 0x0f];
        hex2[2*i + 1] = hc[digest[i] & 0x0f];
    }
    hex2[64] = '\0';

    ASSERT_STREQ(hex1, hex2);
}

/* Verify hex output is 64 lowercase chars + null */
TEST(sha256_hex_format)
{
    char hex[65];
    sh_sha256_hex("test", 4, hex);

    ASSERT(strlen(hex) == 64);
    for (int i = 0; i < 64; i++) {
        char c = hex[i];
        ASSERT((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
    ASSERT(hex[64] == '\0');
}

/* Graceful handling of NULL data with len=0 */
TEST(sha256_null_input)
{
    ShSha256 ctx;
    sh_sha256_init(&ctx);
    sh_sha256_update(&ctx, NULL, 0);
    uint8_t digest[32];
    sh_sha256_final(&ctx, digest);

    /* Should match empty string hash */
    char hex[65];
    sh_sha256_hex("", 0, hex);

    char hex2[65];
    const char *hc = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex2[2*i]     = hc[(digest[i] >> 4) & 0x0f];
        hex2[2*i + 1] = hc[digest[i] & 0x0f];
    }
    hex2[64] = '\0';
    ASSERT_STREQ(hex, hex2);
}

/* Single byte */
TEST(sha256_one_byte)
{
    char hex[65];
    sh_sha256_hex("a", 1, hex);
    ASSERT_STREQ(hex, "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb");
}

/* 63 bytes: exactly one byte short of a block */
TEST(sha256_63_bytes)
{
    char buf[63];
    memset(buf, 'x', 63);

    char hex1[65];
    sh_sha256_hex(buf, 63, hex1);

    /* Verify incremental matches */
    ShSha256 ctx;
    sh_sha256_init(&ctx);
    sh_sha256_update(&ctx, buf, 30);
    sh_sha256_update(&ctx, buf + 30, 33);
    uint8_t digest[32];
    sh_sha256_final(&ctx, digest);

    char hex2[65];
    const char *hc = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex2[2*i]     = hc[(digest[i] >> 4) & 0x0f];
        hex2[2*i + 1] = hc[digest[i] & 0x0f];
    }
    hex2[64] = '\0';
    ASSERT_STREQ(hex1, hex2);
}

/* 64 bytes: exactly one block */
TEST(sha256_64_bytes)
{
    char buf[64];
    memset(buf, 'y', 64);

    char hex1[65];
    sh_sha256_hex(buf, 64, hex1);

    /* Verify incremental: two 32-byte chunks */
    ShSha256 ctx;
    sh_sha256_init(&ctx);
    sh_sha256_update(&ctx, buf, 32);
    sh_sha256_update(&ctx, buf + 32, 32);
    uint8_t digest[32];
    sh_sha256_final(&ctx, digest);

    char hex2[65];
    const char *hc = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex2[2*i]     = hc[(digest[i] >> 4) & 0x0f];
        hex2[2*i + 1] = hc[digest[i] & 0x0f];
    }
    hex2[64] = '\0';
    ASSERT_STREQ(hex1, hex2);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
    printf("\nSHA-256 Tests:\n");

    RUN_TEST(sha256_empty);
    RUN_TEST(sha256_abc);
    RUN_TEST(sha256_two_blocks);
    RUN_TEST(sha256_long);
    RUN_TEST(sha256_incremental);
    RUN_TEST(sha256_hex_format);
    RUN_TEST(sha256_null_input);
    RUN_TEST(sha256_one_byte);
    RUN_TEST(sha256_63_bytes);
    RUN_TEST(sha256_64_bytes);

    printf("\nSHA-256: %d passed, %d total\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
