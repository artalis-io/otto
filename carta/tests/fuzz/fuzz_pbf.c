/*
 * fuzz_pbf.c - Fuzzing harness for PBF parser
 *
 * Compile with:
 *   clang -fsanitize=fuzzer,address,undefined -g -O1 \
 *       -I../../include -I../../../shared/include \
 *       fuzz_pbf.c -L../../ -lcarta -L../../../shared -lshared -lm \
 *       -o fuzz_pbf
 *
 * Run with:
 *   ./fuzz_pbf corpus/ -max_len=1000000
 *
 * The fuzzer will:
 * - Generate random/mutated inputs
 * - Call the PBF parser with each input
 * - Detect crashes, memory errors, undefined behavior
 *
 * Memory limit is set to prevent OOM on huge allocations.
 */

#include "carta.h"
#include "ct_pbf.h"
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

/* LibFuzzer entry point */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* Skip empty inputs */
    if (size == 0) {
        return 0;
    }

    /* Create config with conservative memory limit (64MB) to avoid OOM */
    CTPBFConfig config;
    ct_pbf_config_init(&config);
    config.memory_limit = 64 * 1024 * 1024;  /* 64MB */

    /* Create parsing context with memory limit */
    CTPBFContext *ctx = ct_pbf_context_create_with_config(&config);
    if (!ctx) {
        return 0;  /* Allocation failure - not a bug */
    }

    /*
     * Parse the fuzzed input.
     * This should handle malformed input gracefully without:
     * - Crashing
     * - Memory corruption (ASan will detect)
     * - Undefined behavior (UBSan will detect)
     * - Infinite loops (fuzzer timeout will detect)
     */
    ct_pbf_parse_memory(ctx, data, size);

    /* Clean up */
    ct_pbf_context_free(ctx);

    return 0;
}

/*
 * Optional: Custom mutator for PBF-aware fuzzing.
 * This helps the fuzzer generate more structurally valid PBF inputs.
 * Uncomment if you want to improve fuzzing efficiency.
 */
#if 0
size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size, size_t max_size,
                                unsigned int seed) {
    /* Default mutator - can be enhanced to understand PBF structure */
    return LLVMFuzzerMutate(data, size, max_size);
}
#endif
