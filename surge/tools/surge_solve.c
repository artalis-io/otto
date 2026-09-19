/*
 * surge_solve - transport-agnostic CLI: Surge solve request JSON -> solution JSON.
 *
 * Reads a solve request (the same body the HTTP /api/v1/solve endpoint takes),
 * builds the model, solves, and writes the canonical solution JSON
 * (sg_api_write_solution: status, stats, routes[].stops[], unassigned[]) to
 * stdout. This is the missing CLI companion to the HTTP server -- useful for
 * batch solves, scripting, and the route visualizer (surge/scripts/surge_map.py).
 *
 * Usage: surge_solve <request.json>  > solution.json
 *
 * Note: sg_api_build_model_file uses a small parse arena (fsize*2) that is too
 * tight for large travel matrices, so we parse into a generous arena here.
 */
#include "surge.h"
#include "sh_json.h"   /* before sg_api.h: gates the sg_api_write_solution decl */
#include "sg_api.h"
#include "sh_arena.h"
#include <stdio.h>
#include <stdlib.h>

static int write_stdout(void *ctx, const char *data, size_t len) {
    return fwrite(data, 1, len, (FILE *)ctx) == len ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <request.json>  > solution.json\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "surge_solve: cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fprintf(stderr, "surge_solve: empty file\n"); fclose(f); return 1; }
    char *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "surge_solve: read failed\n"); free(buf); fclose(f); return 1;
    }
    fclose(f);

    /* Generous arena (64x file + 1MB) so the parse tree for large travel
     * matrices fits; the stock file loader uses only ~2x. */
    SHArena *arena = sh_arena_create((size_t)sz * 64 + (1u << 20));
    ShJsonValue *root = NULL;
    if (!arena || sh_json_parse(buf, (size_t)sz, arena, &root) != SH_JSON_OK || !root) {
        fprintf(stderr, "surge_solve: JSON parse failed\n");
        free(buf); if (arena) sh_arena_free(arena); return 1;
    }
    free(buf);

    SGContext *ctx = sg_create();
    if (!ctx) { fprintf(stderr, "surge_solve: sg_create failed\n"); sh_arena_free(arena); return 1; }
    SGStatus st = sg_api_build_model(ctx, root);
    sh_arena_free(arena);
    if (st != SG_STATUS_OK) {
        fprintf(stderr, "surge_solve: build_model failed (status=%d)\n", (int)st);
        sg_free(ctx); return 1;
    }

    SGStatus solve_status = sg_solve(ctx);

    /* sg_solve commits its best solution to the context before returning, and
     * sg_api_write_solution now serializes a committed solution under any
     * status (it can return a non-OK status -- e.g. a max_trips=0 multi-trip
     * solve the strict end-of-solve validator rejects -- while a complete plan
     * is present). Pass the raw status so the JSON reports it faithfully; note
     * a non-OK status on stderr. Exit 0 if a solution was emitted. */
    uint32_t route_count = sg_solution_get_route_count(ctx);
    if (solve_status != SG_STATUS_OK && solve_status != SG_STATUS_LIMIT) {
        fprintf(stderr, "surge_solve: sg_solve status=%d (%u routes committed)\n",
                (int)solve_status, route_count);
    }

    ShJsonWriter w;
    sh_json_writer_init(&w, write_stdout, stdout);
    sg_api_write_solution(ctx, &w, solve_status);
    fputc('\n', stdout);

    sg_free(ctx);
    return (solve_status == SG_STATUS_OK || solve_status == SG_STATUS_LIMIT ||
            route_count > 0) ? 0 : 1;
}
