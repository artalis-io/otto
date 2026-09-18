# Reading the API request handlers

The follow-up the previous read pointed at. `docs/analysis/api-server-read.md`
established that the six `api/src/main.c` files are thin marshalling wrappers
and that the parsing of untrusted input lives one call further in, in the
`*_api.c` files:

| file | lines |
|---|---|
| `surge/src/sg_api.c` | 2,247 |
| `fuelwise/src/fw_api.c` | 811 |
| `velo/src/vl_api.c` | 632 |
| `carta/src/ct_api.c` | 580 |
| `locus/src/lc_api.c` | 540 |
| `ralph/src/ralph_api.c` | 527 |

5,337 lines that turn a request body into a model.

## The finding: a 66-byte request that burns 2.8 seconds

`build_exclusion_groups()` in `sg_api.c`:

```c
count = sh_json_as_int(v, 0);
if (count <= 0) return -1;          /* only the lower bound is checked */

for (i = 0; i < count; i++) {
    uint32_t gid;
    if (sg_add_exclusion_group(ctx, &gid) != SG_STATUS_OK) return -1;
}
```

and the callee, in `sg_context.c`:

```c
SGStatus sg_add_exclusion_group(SGContext *ctx, uint32_t *group_id_out) {
    if (!ctx || !group_id_out) return SG_STATUS_INVALID_ARG;
    *group_id_out = ctx->num_exclusion_groups;
    ctx->num_exclusion_groups++;
    return SG_STATUS_OK;                 /* nothing can stop the loop */
}
```

It increments a counter and always succeeds, so the loop runs exactly as many
times as the request asks. Measured against the running server:

| `count` | body | time | status |
|---|---|---|---|
| 1 | 58 B | 53 ms | 200 |
| 100,000,000 | 66 B | 186 ms | 200 |
| 500,000,000 | 66 B | 708 ms | 200 |
| 1,000,000,000 | 67 B | 1,377 ms | 200 |
| 2,147,483,647 | 66 B | **2,822 ms** | **200** |

Linear in the count, so the amplification is whatever the caller types, and the
request is answered `200 OK` rather than rejected. Each one holds a solve-pool
worker for the duration; the pool is bounded, so sustained traffic of these
stalls the service.

The number also multiplies with the vehicle count for the
`construct_exclusion_counts` and `route_exclusion_counts` allocations in
`sg_construct.c`, so an inflated one reaches a `calloc` as well as a loop.

**Fixed** by bounding `sg_add_exclusion_group()` at `SG_MAX_EXCLUSION_GROUPS`
(4096), which is how `sg_add_commodity()` in the same file already bounds
itself at 64. Defending in the callee rather than at the parse site covers
every caller. After the fix, `count` at `INT_MAX` answers 400 in 59 ms.

## Checked and sound

- **Matrix parsing** (`sg_api.c`). Squareness is checked, the two matrices must
  agree in length, and `n > SIZE_MAX / sizeof(double)` guards the allocation
  against wraparound on wasm32. Every error path frees.
- **The other two scalar-count loops.** `commodities.count` drives
  `sg_add_commodity()`, which stops at 64. `setup_times.num_classes` is gated
  on `sh_json_array_len(matrix) == num_classes²`, so reaching the loop requires
  an array of that size and the body limit bounds it.
- **Everything else is array-length driven.** Across all six files, every other
  loop bound and allocation size comes from `sh_json_array_len()` or from
  internal state, not from a scalar the caller supplies. Those cost the
  attacker at least a byte or two of body per iteration, which is the
  difference between amplification and none.
- **fuelwise allocations** all use `calloc(array_len, size)`, which carries its
  own overflow check.
- **Non-finite numbers** cannot enter through a JSON number literal: `sh_json.c`
  rejects them at the parser (a fix from earlier this week), and
  `sh_json_as_double()` returns the supplied default for a non-number.

## One inconsistency, not a defect

`build_travel()` computes `n = location_count * location_count` and calls
`malloc(n * sizeof(double))` without the `n > SIZE_MAX / sizeof(double)` guard
that two other sites in the same file carry. It is not reachable: the array
length must equal `n` before the malloc runs, so `n` is bounded by the body
size. Worth aligning if that code is touched.

## Scope

sg_api.c's model-building path was read in full; all six files were scanned
systematically for the shapes that matter in this kind of code — scalar counts
driving loops, allocations sized from request fields, indices used without
bounds, non-finite numbers — and every hit was read. This is not a line-by-line
read of all 5,337 lines.

The response-building halves of these files, which serialise a solved model
back to JSON, were not examined; they operate on solver output rather than on
request input.
