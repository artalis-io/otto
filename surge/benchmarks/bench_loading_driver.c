/* Benchmark-only HGS adapter. Build this SAME source against both revisions.
 * It does not change solver settings or add instrumentation to production. */
#include "sg_internal.h"
#include "sg_parallel.h"
#include "sh_json.h"
#include "sg_api.h"
#include "sh_arena.h"
#include "sh_pal.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

typedef struct {
    SGContext *ctx;
    atomic_int done;
    unsigned delay;
    uint64_t requested_at;
} Cancel;
static void *cancel_after(void *opaque) {
    Cancel *c = opaque;
    uint64_t start = sh_monotonic_ms();
    while (!atomic_load(&c->done)) {
        if (sh_monotonic_ms() - start >= c->delay) {
            c->requested_at = sh_monotonic_ms();
            sg_cancel(c->ctx);
            break;
        }
        sh_sleep_ms(1);
    }
    return NULL;
}
static int output(void *ctx, const char *data, size_t len) {
    return fwrite(data, 1, len, ctx) == len ? 0 : -1;
}
int main(int argc, char **argv) {
    if (argc < 2 || argc > 4) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return 2; }
    long size = ftell(f);
    if (size <= 0 || (size_t)size > (SIZE_MAX - (1u << 20)) / 64) {
        fclose(f); return 2;
    }
    rewind(f);
    char *buf = malloc((size_t)size);
    SHArena *arena = sh_arena_create((size_t)size * 64 + (1u << 20));
    ShJsonValue *root = NULL;
    int parsed = buf && arena && fread(buf, 1, (size_t)size, f) == (size_t)size &&
        sh_json_parse(buf, (size_t)size, arena, &root) == SH_JSON_OK;
    fclose(f); free(buf);
    if (!parsed) { if (arena) sh_arena_free(arena); return 2; }
    const ShJsonValue *cfg = sh_json_get(root, "config");
    long threads = sh_json_as_int(sh_json_get(cfg, "population_threads"), 0);
    long generations = sh_json_as_int(sh_json_get(cfg, "population_generations"), 0);
    if (!sh_json_as_bool(sh_json_get(cfg, "population"), false) ||
        threads < 1 || threads > 8 || generations < 2 || generations > 10) {
        sh_arena_free(arena); return 2;
    }
    SGContext *ctx = sg_create();
    if (!ctx) { sh_arena_free(arena); return 2; }
    SGStatus status = sg_api_build_model(ctx, root);
    sh_arena_free(arena);
    if (status != SG_STATUS_OK) { sg_free(ctx); return 2; }
    Cancel cancel = {0};
    cancel.ctx = ctx;
    atomic_init(&cancel.done, 0);
    ShThread thread;
    int threaded = 0;
    if (argc >= 3) {
        char *end;
        long ms = strtol(argv[2], &end, 10);
        if (*end || ms < 1 || ms > 60000) { sg_free(ctx); return 2; }
        cancel.delay = (unsigned)ms;
        if (sh_thread_create(&thread, cancel_after, &cancel)) { sg_free(ctx); return 2; }
        threaded = 1;
    }
    SGPopulationConfig pop = {0};
    pop.num_threads = (uint32_t)threads;
    pop.num_generations = (uint32_t)generations;
    uint64_t begin = sh_monotonic_ms();
    if (argc < 4 || strcmp(argv[3], "population") == 0)
        status = sg_solve_population(ctx, &pop);
    else if (strcmp(argv[3], "parallel") == 0)
        status = sg_solve_parallel(ctx, (uint32_t)threads);
    else if (strcmp(argv[3], "single") == 0)
        status = sg_solve(ctx);
    else {
        atomic_store(&cancel.done, 1);
        if (threaded) sh_thread_join(&thread, NULL);
        sg_free(ctx); return 2;
    }
    uint64_t finish = sh_monotonic_ms();
    atomic_store(&cancel.done, 1);
    if (threaded) sh_thread_join(&thread, NULL);
    int penalty = ctx->penalty.enabled;
    ctx->penalty.enabled = 0;
    int valid = ctx->final_solution && sg_route_solution_validate(ctx->final_solution, ctx);
    ctx->penalty.enabled = penalty;
    printf("{\"benchmark\":{\"hard_validated\":%s,\"solve_ms\":%llu,\"cancel_requested\":%s,\"cancel_latency_ms\":%llu},\"solution\":",
        valid ? "true" : "false", (unsigned long long)(finish - begin),
        cancel.requested_at ? "true" : "false",
        (unsigned long long)(cancel.requested_at && finish >= cancel.requested_at
            ? finish - cancel.requested_at : 0));
    ShJsonWriter writer;
    sh_json_writer_init(&writer, output, stdout);
    sg_api_write_solution(ctx, &writer, status);
#ifdef SG_LOADING_PROFILE
    SGLoadingProfile *p = &ctx->loading_profile;
    printf(",\"loading_profile\":{\"round_calls\":%llu,\"cache_hits\":%llu,"
           "\"checker_calls\":%llu,\"checker_ns\":%llu,\"insertion_calls\":%llu,\"insertion_ns\":%llu}",
           (unsigned long long)p->round_calls, (unsigned long long)p->cache_hits,
           (unsigned long long)p->checker_calls, (unsigned long long)p->checker_ns,
           (unsigned long long)p->insertion_calls, (unsigned long long)p->insertion_ns);
#endif
    puts("}");
    sg_free(ctx);
    return valid ? 0 : 1;
}
