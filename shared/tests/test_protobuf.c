/*
 * test_protobuf.c - sh_protobuf wire-format primitives
 *
 * These had no tests at all, which is why the length check in
 * sh_pb_skip_field could wrap without anyone noticing. Protobuf bytes reach
 * OTTO from OSM PBF extracts, so this decodes untrusted input.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include "sh_protobuf.h"

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

/* ---------------------------------------------------------------- varint -- */

TEST(varint_single_byte)
{
    const uint8_t buf[] = { 0x01 };
    uint64_t v = 0;
    ASSERT(sh_pb_read_varint(buf, sizeof(buf), &v) == 1);
    ASSERT(v == 1);
}

TEST(varint_multi_byte)
{
    /* 300 = 0xAC 0x02 */
    const uint8_t buf[] = { 0xAC, 0x02 };
    uint64_t v = 0;
    ASSERT(sh_pb_read_varint(buf, sizeof(buf), &v) == 2);
    ASSERT(v == 300);
}

TEST(varint_max_u64)
{
    const uint8_t buf[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                            0xFF, 0xFF, 0xFF, 0xFF, 0x01 };
    uint64_t v = 0;
    ASSERT(sh_pb_read_varint(buf, sizeof(buf), &v) == 10);
    ASSERT(v == UINT64_MAX);
}

/* A varint whose continuation bit never clears must not read past the end. */
TEST(varint_truncated_is_rejected)
{
    const uint8_t buf[] = { 0x80, 0x80, 0x80 };
    uint64_t v = 0;
    ASSERT(sh_pb_read_varint(buf, sizeof(buf), &v) == 0);
}

/* More continuation bytes than 64 bits can hold. */
TEST(varint_overlong_is_rejected)
{
    const uint8_t buf[] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
                            0x80, 0x80, 0x80, 0x80, 0x80, 0x01 };
    uint64_t v = 0;
    ASSERT(sh_pb_read_varint(buf, sizeof(buf), &v) == 0);
}

TEST(varint_empty_buffer)
{
    uint64_t v = 0;
    ASSERT(sh_pb_read_varint((const uint8_t *)"", 0, &v) == 0);
}

TEST(svarint_zigzag)
{
    const uint8_t zero[]  = { 0x00 };
    const uint8_t minus1[] = { 0x01 };
    const uint8_t one[]   = { 0x02 };
    int64_t v = 0;
    ASSERT(sh_pb_read_svarint(zero, 1, &v) == 1 && v == 0);
    ASSERT(sh_pb_read_svarint(minus1, 1, &v) == 1 && v == -1);
    ASSERT(sh_pb_read_svarint(one, 1, &v) == 1 && v == 1);
}

/* ------------------------------------------------------------------- tag -- */

TEST(tag_splits_field_and_wire_type)
{
    /* field 1, wire type 2 -> (1 << 3) | 2 = 0x0A */
    const uint8_t buf[] = { 0x0A };
    uint32_t field = 0, wire = 0;
    ASSERT(sh_pb_read_tag(buf, 1, &field, &wire) == 1);
    ASSERT(field == 1);
    ASSERT(wire == SH_PB_WIRE_LENGTH_DELIM);
}

/* ------------------------------------------------------------ skip_field -- */

TEST(skip_fixed_widths)
{
    const uint8_t buf[8] = { 0 };
    ASSERT(sh_pb_skip_field(buf, 8, SH_PB_WIRE_FIXED64) == 8);
    ASSERT(sh_pb_skip_field(buf, 4, SH_PB_WIRE_FIXED32) == 4);
    /* Not enough room is a refusal, not a short read. */
    ASSERT(sh_pb_skip_field(buf, 7, SH_PB_WIRE_FIXED64) == 0);
    ASSERT(sh_pb_skip_field(buf, 3, SH_PB_WIRE_FIXED32) == 0);
}

TEST(skip_length_delimited)
{
    /* length 3, then three bytes */
    const uint8_t buf[] = { 0x03, 'a', 'b', 'c' };
    ASSERT(sh_pb_skip_field(buf, sizeof(buf), SH_PB_WIRE_LENGTH_DELIM) == 4);
}

/* A declared length longer than the buffer must be refused. */
TEST(skip_length_beyond_buffer_is_rejected)
{
    const uint8_t buf[] = { 0x10, 'a', 'b' };   /* claims 16 bytes, has 2 */
    ASSERT(sh_pb_skip_field(buf, sizeof(buf), SH_PB_WIRE_LENGTH_DELIM) == 0);
}

/*
 * The one that motivated this file. A length near 2^64 used to be checked with
 * `(size_t)(n + field_len) > len`, which is unsigned 64-bit addition: the sum
 * wraps to something small and passes. It failed closed only because
 * (int)field_len then came out negative.
 */
TEST(skip_length_near_u64_max_is_rejected)
{
    /* varint for UINT64_MAX, so n == 10 and field_len == 2^64-1 */
    const uint8_t buf[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                            0xFF, 0xFF, 0xFF, 0xFF, 0x01,
                            'x', 'y', 'z' };
    ASSERT(sh_pb_skip_field(buf, sizeof(buf), SH_PB_WIRE_LENGTH_DELIM) == 0);
}

/* Every value that wraps `n + field_len` into the buffer must still refuse. */
TEST(skip_length_wrapping_values_are_rejected)
{
    for (uint64_t delta = 0; delta < 24; delta++) {
        uint64_t claim = UINT64_MAX - delta;
        uint8_t buf[32];
        memset(buf, 0, sizeof(buf));

        /* encode `claim` as a varint at the head of the buffer */
        int n = 0;
        uint64_t v = claim;
        do {
            buf[n] = (uint8_t)((v & 0x7F) | ((v >> 7) ? 0x80 : 0));
            v >>= 7;
            n++;
        } while (v && n < 10);

        ASSERT(sh_pb_skip_field(buf, sizeof(buf), SH_PB_WIRE_LENGTH_DELIM) <= 0);
    }
}

TEST(skip_unknown_wire_type_is_rejected)
{
    const uint8_t buf[8] = { 0 };
    ASSERT(sh_pb_skip_field(buf, sizeof(buf), 6) == 0);
    ASSERT(sh_pb_skip_field(buf, sizeof(buf), 7) == 0);
}

/* ----------------------------------------------------------------- fixed -- */

TEST(fixed32_little_endian)
{
    const uint8_t buf[] = { 0x78, 0x56, 0x34, 0x12 };
    uint32_t v = 0;
    ASSERT(sh_pb_read_fixed32(buf, sizeof(buf), &v) == 4);
    ASSERT(v == 0x12345678u);
}

TEST(fixed64_little_endian)
{
    const uint8_t buf[] = { 0xEF, 0xCD, 0xAB, 0x89,
                            0x67, 0x45, 0x23, 0x01 };
    uint64_t v = 0;
    ASSERT(sh_pb_read_fixed64(buf, sizeof(buf), &v) == 8);
    ASSERT(v == 0x0123456789ABCDEFull);
}

TEST(fixed_reads_refuse_short_buffers)
{
    const uint8_t buf[8] = { 0 };
    uint32_t v32 = 0;
    uint64_t v64 = 0;
    ASSERT(sh_pb_read_fixed32(buf, 3, &v32) == 0);
    ASSERT(sh_pb_read_fixed64(buf, 7, &v64) == 0);
}

/* --------------------------------------------------------------- roundtrip -- */

TEST(varint_write_read_roundtrip)
{
    const uint64_t values[] = { 0, 1, 127, 128, 300, 16383, 16384,
                                UINT32_MAX, UINT64_MAX };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        uint8_t buf[16];
        int w = sh_pb_write_varint(buf, sizeof(buf), values[i]);
        ASSERT(w > 0);
        uint64_t back = 0;
        int r = sh_pb_read_varint(buf, (size_t)w, &back);
        ASSERT(r == w);
        ASSERT(back == values[i]);
    }
}

TEST(write_refuses_insufficient_capacity)
{
    uint8_t buf[1];
    /* 300 needs two bytes */
    ASSERT(sh_pb_write_varint(buf, 1, 300) == 0);
}

int main(void)
{
    printf("\nsh_protobuf tests\n");
    printf("==================\n");

    RUN_TEST(varint_single_byte);
    RUN_TEST(varint_multi_byte);
    RUN_TEST(varint_max_u64);
    RUN_TEST(varint_truncated_is_rejected);
    RUN_TEST(varint_overlong_is_rejected);
    RUN_TEST(varint_empty_buffer);
    RUN_TEST(svarint_zigzag);
    RUN_TEST(tag_splits_field_and_wire_type);
    RUN_TEST(skip_fixed_widths);
    RUN_TEST(skip_length_delimited);
    RUN_TEST(skip_length_beyond_buffer_is_rejected);
    RUN_TEST(skip_length_near_u64_max_is_rejected);
    RUN_TEST(skip_length_wrapping_values_are_rejected);
    RUN_TEST(skip_unknown_wire_type_is_rejected);
    RUN_TEST(fixed32_little_endian);
    RUN_TEST(fixed64_little_endian);
    RUN_TEST(fixed_reads_refuse_short_buffers);
    RUN_TEST(varint_write_read_roundtrip);
    RUN_TEST(write_refuses_insufficient_capacity);

    printf("\n==================\n");
    printf("%d/%d tests passed\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
