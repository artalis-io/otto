/*
 * fuzz_json.c - libFuzzer harness for sh_json
 *
 * sh_json is the only parser in shared/ fed directly from an HTTP request
 * body -- fuelwise, ralph, surge and velo all call sh_json_parse() on bytes
 * off the wire -- and it had no fuzzer.
 *
 * Catches: the two-pass element count in parse_array/parse_object disagreeing
 * with what the real parse consumes (the count sizes the array, the parse
 * writes into it with no bound on the index), \u escapes and surrogate pairs
 * whose first-pass length differs from what the second pass encodes, deep
 * nesting against SH_JSON_MAX_DEPTH, numbers at the edges of strtod.
 *
 * Arena overflows in here are only reportable because sh_arena_alloc() poisons
 * between allocations; without that a write past one arena slice lands in the
 * next and looks like an ordinary write to live memory.
 *
 * Build: make -C shared fuzz-json
 * Run:   ./fuzz_json work/ tests/fuzz/corpus_json/ -max_total_time=60
 */

#include "sh_json.h"
#include "sh_arena.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Walk everything the parser handed back, so a value describing memory it does
 * not own is caught here rather than going unnoticed. */
static void walk(const ShJsonValue *v, int depth)
{
    volatile size_t sink = 0;

    if (!v) return;
    if (depth > 128) abort();   /* parser must not build past its own limit */

    switch (v->type) {
        case SH_JSON_STRING:
            /* The parser reports a length and promises a terminator. Read the
             * whole span and the byte past it: a length that disagrees with
             * the allocation shows up here. */
            if (v->u.string_val.str) {
                for (size_t i = 0; i < v->u.string_val.len; i++)
                    sink += (unsigned char)v->u.string_val.str[i];
                sink += (unsigned char)v->u.string_val.str[v->u.string_val.len];
                if (v->u.string_val.str[v->u.string_val.len] != '\0') abort();
            }
            break;

        case SH_JSON_ARRAY:
            for (size_t i = 0; i < v->u.array_val.count; i++)
                walk(v->u.array_val.items[i], depth + 1);
            break;

        case SH_JSON_OBJECT:
            for (size_t i = 0; i < v->u.object_val.count; i++) {
                const ShJsonMember *m = &v->u.object_val.members[i];
                if (m->key) {
                    for (size_t k = 0; k < m->key_len; k++)
                        sink += (unsigned char)m->key[k];
                    if (m->key[m->key_len] != '\0') abort();
                }
                walk(m->value, depth + 1);
            }
            break;

        case SH_JSON_NUMBER:
            sink += (size_t)(v->u.num_val == v->u.num_val);  /* NaN check */
            break;

        default:
            break;
    }
    (void)sink;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > 1024 * 1024) return 0;

    /* 1MB matches what the servers allow through: Keel's max_body_size
     * defaults to 1MB, so this is the shape of a real request body. */
    SHArena *arena = sh_arena_create(16 * 1024 * 1024);
    if (!arena) return 0;

    ShJsonValue *root = NULL;
    ShJsonStatus st = sh_json_parse((const char *)data, size, arena, &root);

    if (st == SH_JSON_OK) {
        /* Success must produce a value. */
        if (!root) abort();
        walk(root, 0);
    }

    sh_arena_free(arena);
    return 0;
}
