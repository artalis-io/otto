/*
 * fuzz_protobuf.c - libFuzzer harness for sh_protobuf wire-format reads
 *
 * Catches: truncated and overlong varints, declared lengths that exceed the
 * buffer or wrap when added to the tag width, unknown wire types, fixed-width
 * reads at the end of the buffer.
 *
 * Drives a scanning loop rather than one primitive, because the interesting
 * failures come from a length read in one call being trusted by the next.
 *
 * Build: make -C shared fuzz-protobuf
 * Run:   ./fuzz_protobuf tests/fuzz/corpus_protobuf/ -max_len=65536 -max_total_time=60
 */

#include "sh_protobuf.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > 1024 * 1024) return 0;

    size_t pos = 0;
    int guard = 0;

    while (pos < size && guard++ < 100000) {
        uint32_t field = 0, wire = 0;
        int n = sh_pb_read_tag(data + pos, size - pos, &field, &wire);
        if (n <= 0) break;
        if ((size_t)n > size - pos) abort();   /* read past the end */
        pos += (size_t)n;

        int skipped = sh_pb_skip_field(data + pos, size - pos, wire);
        if (skipped <= 0) break;

        /* The whole point of the length check: a skip may never claim more
         * than the bytes that remain. */
        if ((size_t)skipped > size - pos) abort();
        pos += (size_t)skipped;
    }

    /* Exercise the primitives directly too, at a fuzzer-chosen offset. */
    size_t at = data[0] % size;
    uint64_t u = 0;
    int64_t s = 0;
    uint32_t f32 = 0;
    uint64_t f64 = 0;

    int n = sh_pb_read_varint(data + at, size - at, &u);
    if (n < 0 || (size_t)n > size - at) abort();

    n = sh_pb_read_svarint(data + at, size - at, &s);
    if (n < 0 || (size_t)n > size - at) abort();

    n = sh_pb_read_fixed32(data + at, size - at, &f32);
    if (n < 0 || (size_t)n > size - at) abort();

    n = sh_pb_read_fixed64(data + at, size - at, &f64);
    if (n < 0 || (size_t)n > size - at) abort();

    return 0;
}
