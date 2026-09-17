/*
 * fuzz_pdf2struc.c - libFuzzer harness for the PDF structure extractor
 *
 * Catches: malformed xref tables and xref streams, /W field widths that do not
 * describe the data, PNG predictor parameters chosen to overflow the stride
 * arithmetic, object streams that point at themselves, streams whose declared
 * /Length disagrees with what is there.
 *
 * Both defects found by reading this file -- the predictor's `row_bytes + 1`
 * and the xref stream's negative /W -- are reachable from a file under 500
 * bytes, which is the size range a fuzzer explores first.
 *
 * Build: make -C shared fuzz-pdf2struc
 * Run:   ./fuzz_pdf2struc tests/fuzz/corpus_pdf2struc/ -max_len=65536 -max_total_time=60
 */

#include "sh_pdf2struc.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

static void collect(void *user, const ShPdf2strucBlock *block)
{
    /* Touch what the extractor handed back, so a block describing memory it
     * does not own is caught here rather than going unnoticed. */
    volatile size_t sink = 0;
    if (!block) return;
    sink += (size_t)block->page_index;
    if (block->text) {
        /* Walk the string the extractor says is null-terminated: if it is not,
         * or points at freed arena memory, ASan says so here. */
        for (const char *p = block->text; *p; p++) sink += (size_t)(unsigned char)*p;
    }
    (void)user;
    (void)sink;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > 4 * 1024 * 1024) return 0;

    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    if (!ctx) return 0;

    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);

    sh_pdf2struc_extract_mem(ctx, data, size, &opt, collect, NULL);

    /* The error string must be readable whatever happened. */
    (void)sh_pdf2struc_last_error(ctx);

    sh_pdf2struc_destroy(ctx);
    return 0;
}
