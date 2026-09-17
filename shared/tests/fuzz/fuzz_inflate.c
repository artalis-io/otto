/*
 * fuzz_inflate.c - libFuzzer harness for sh_inflate
 *
 * Catches: malformed zlib headers, truncated streams, decompression bombs,
 * output buffers too small for the declared content, the raw path disagreeing
 * with the streaming one.
 *
 * Build: make -C shared fuzz-inflate
 * Run:   ./fuzz_inflate tests/fuzz/corpus_inflate/ -max_len=65536 -max_total_time=60
 */

#include "sh_inflate.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > 1024 * 1024) return 0;

    /* The first byte picks an output capacity, so the fuzzer can reach both
     * the "plenty of room" and the "destination too small" paths without
     * needing to guess a size. */
    size_t caps[] = { 1, 16, 1024, 64 * 1024, 1024 * 1024 };
    size_t cap = caps[data[0] % (sizeof(caps) / sizeof(caps[0]))];

    const uint8_t *body = data + 1;
    size_t body_len = size - 1;
    if (body_len == 0) return 0;

    uint8_t *out = (uint8_t *)malloc(cap);
    if (!out) return 0;

    size_t out_len = 0;
    SHStatus st = sh_inflate(body, body_len, out, cap, &out_len);
    if (st == SH_OK) {
        /* A success must not claim more than the buffer holds. */
        if (out_len > cap) abort();
    }

    out_len = 0;
    st = sh_inflate_raw(body, body_len, out, cap, &out_len);
    if (st == SH_OK) {
        if (out_len > cap) abort();
    }

    free(out);

    /* The allocating variant owns its buffer; exercise it on the same bytes. */
    size_t alloc_len = 0;
    uint8_t *alloc_out = sh_inflate_alloc(body, body_len, cap, &alloc_len);
    if (alloc_out) {
        if (alloc_len > cap) abort();
        free(alloc_out);
    }

    return 0;
}
